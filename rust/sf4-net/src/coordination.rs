//! Room commitment, independent of gameplay links and the native simulation.
//!
//! Storage is process-incarnation scoped: a stopped instance is never restarted
//! with the same voter ID. It must join a live quorum as a new learner instead.
//! Neither snapshots nor RPC errors are logged: checkpoints contain private
//! admission state. C++ owns rule validation; only committed checkpoints escape.
use std::{
    collections::{BTreeMap, BTreeSet, VecDeque},
    fmt::Debug,
    future::Future,
    io::{self, Cursor, Read, Write},
    ops::RangeBounds,
    pin::Pin,
    sync::{Arc, OnceLock},
    time::{Duration, Instant},
};

use flate2::{
    Compression, Decompress, FlushDecompress, Status, read::ZlibDecoder, write::ZlibEncoder,
};
use openraft::{
    BasicNode, ChangeMembers, Config, Entry, EntryPayload, LogId, OptionalSend, Raft, RaftStorage,
    Snapshot, SnapshotMeta, SnapshotPolicy, StorageError, StorageIOError, StoredMembership, Vote,
    error::{InstallSnapshotError, NetworkError, RPCError, RaftError},
    network::{RPCOption, RaftNetwork, RaftNetworkFactory},
    raft::{
        AppendEntriesRequest, AppendEntriesResponse, InstallSnapshotRequest,
        InstallSnapshotResponse, VoteRequest, VoteResponse,
    },
    storage::{Adaptor, LogState, RaftLogReader, RaftSnapshotBuilder},
};
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use serde_json::Value;
use tokio::sync::{Mutex, watch};

mod latency_roster;
mod server_owned;
mod view;
pub use latency_roster::{
    LatencyRoster, LatencyRow, RosterInvalid, committed_latency_roster, latency_roster_of,
};
pub use server_owned::Ownership;
pub use view::envelope_view;

pub const MAX_CHECKPOINT: usize = 1024 * 1024;
pub const MAX_MEMBERS: usize = 16;
/// Applied-membership provenance: at most 128 retired process incarnations
/// plus the maximum 16 live room members. The state machine keeps the most
/// recent `RETAINED_RETIRED_MEMBER_HISTORY` departures and drops older ones as
/// new ones apply, so a long-lived room never saturates; the admission checks
/// against the full bound remain as a fail-closed backstop.
pub const MAX_RETIRED_MEMBER_HISTORY: usize = 128;
pub const RETAINED_RETIRED_MEMBER_HISTORY: usize = MAX_RETIRED_MEMBER_HISTORY - 1;
pub const MAX_MEMBER_HISTORY: usize = MAX_RETIRED_MEMBER_HISTORY + MAX_MEMBERS;
pub const MAX_SNAPSHOT: usize = MAX_CHECKPOINT * 6 + 65536;
pub const SNAPSHOT_FRAGMENT_BYTES: usize = 16 * 1024;
pub const SNAPSHOT_CREDIT_WINDOW: usize = 4;
/// Encoded size bounds of the RPC bodies whose content is small and fixed in
/// shape. Only an append carries a checkpoint and may reach `MAX_SNAPSHOT`.
/// The tests encode the largest body of each kind against these bounds.
pub const MAX_VOTE_REQUEST: usize = 4 * 1024;
pub const MAX_PROPOSE_REQUEST: usize = 16 * 1024;
pub const MAX_SNAPSHOT_REQUEST: usize = 128 * 1024;
pub const MAX_RPC_RESPONSE: usize = 64 * 1024;
const RECENT_REQUESTS: usize = 128;
/// The room coordination wire: the append and snapshot RPC bodies and the
/// snapshot encoding. The coordination ALPN ends with it, and a room proof
/// carries it beside the Sidecar build (`invite::RoomProof`), so helpers of
/// two coordination wires never share a room even when their Sidecar.dll is
/// the same.
pub const COORDINATION_WIRE_VERSION: u8 = 2;
/// Snapshots of coordination wire 2. A snapshot of wire 1 (`sf4rs001`) is
/// not read: snapshots live in memory only and travel only inside one room,
/// whose members share a wire.
const SNAPSHOT_MAGIC: [u8; 8] = *b"sf4rs002";
const SNAPSHOT_HEADER_BYTES: usize = SNAPSHOT_MAGIC.len() + size_of::<u64>() + 32;
const AUTHORITY_CLAIM_COALESCE: Duration = Duration::from_millis(500);
/// The same for the sole voter of a server-owned room; see `authority_claim`.
const HOST_AUTHORITY_CLAIM_COALESCE: Duration = Duration::from_millis(2500);

openraft::declare_raft_types!(pub RoomTypes: D = Proposal, R = Receipt, Node = BasicNode);
pub type RoomRaft = Raft<RoomTypes>;
type StoreError = StorageError<u64>;

fn invalid_body() -> io::Error {
    io::Error::other("invalid room coordination envelope")
}

fn too_large() -> io::Error {
    io::Error::other("room coordination message too large")
}

/// Reads the fields of a binary RPC body in order, never past its end.
struct BodyReader<'a>(&'a [u8]);

impl<'a> BodyReader<'a> {
    fn take(&mut self, length: usize) -> io::Result<&'a [u8]> {
        if length > self.0.len() {
            return Err(invalid_body());
        }
        let (head, rest) = self.0.split_at(length);
        self.0 = rest;
        Ok(head)
    }

    fn u8(&mut self) -> io::Result<u8> {
        Ok(self.take(1)?[0])
    }

    fn u32(&mut self) -> io::Result<u32> {
        let bytes = self.take(size_of::<u32>())?;
        Ok(u32::from_le_bytes(bytes.try_into().map_err(|_| invalid_body())?))
    }

    fn u64(&mut self) -> io::Result<u64> {
        let bytes = self.take(size_of::<u64>())?;
        Ok(u64::from_le_bytes(bytes.try_into().map_err(|_| invalid_body())?))
    }

    fn rest(self) -> &'a [u8] {
        self.0
    }
}

/// The start of a binary RPC body: the coordination wire version, then the
/// JSON header's length (u32, little endian) and the header. `extra` is the
/// room to reserve for what follows it.
fn body_with_header<T: Serialize>(header: &T, extra: usize) -> io::Result<Vec<u8>> {
    let header = serde_json::to_vec(header).map_err(io::Error::other)?;
    if header.len() > MAX_SNAPSHOT {
        return Err(too_large());
    }
    let mut body = Vec::with_capacity(1 + size_of::<u32>() + header.len() + extra);
    body.push(COORDINATION_WIRE_VERSION);
    body.extend_from_slice(&(header.len() as u32).to_le_bytes());
    body.extend_from_slice(&header);
    Ok(body)
}

/// The JSON header of a binary RPC body, and a reader of what follows it.
/// A body of another wire version, such as wire 1's JSON, is refused here.
fn read_header<T: DeserializeOwned>(bytes: &[u8]) -> io::Result<(T, BodyReader<'_>)> {
    let mut reader = BodyReader(bytes);
    if reader.u8()? != COORDINATION_WIRE_VERSION {
        return Err(invalid_body());
    }
    let length = reader.u32()? as usize;
    let header = serde_json::from_slice(reader.take(length)?).map_err(|_| invalid_body())?;
    Ok((header, reader))
}

/// A checkpoint compressed for the wire.
fn compress_checkpoint(text: &str) -> io::Result<Arc<[u8]>> {
    let mut encoder = ZlibEncoder::new(Vec::new(), Compression::fast());
    encoder.write_all(text.as_bytes())?;
    let compressed = encoder.finish()?;
    if compressed.len() > MAX_SNAPSHOT {
        return Err(too_large());
    }
    Ok(compressed.into())
}

/// The checkpoint text `compressed` holds. It must inflate to exactly
/// `length` bytes of UTF-8, and its zlib stream must end, adler32 trailer
/// included, where the bytes do. zlib's checksum covers the inflated bytes
/// only when the stream end is required: a reader that stops at the end of
/// its input accepts a stream whose trailer is cut off. The output never
/// grows past `length` + 1 bytes.
fn inflate_checkpoint(compressed: &[u8], length: u64) -> io::Result<Arc<str>> {
    if length > MAX_SNAPSHOT as u64 {
        return Err(too_large());
    }
    let digest = || io::Error::other("invalid room coordination digest");
    let mut raw = Vec::with_capacity(length as usize + 1);
    let mut inflater = Decompress::new(true);
    loop {
        let progress = (inflater.total_in(), inflater.total_out());
        let input = compressed
            .get(inflater.total_in() as usize..)
            .ok_or_else(digest)?;
        let status = inflater
            .decompress_vec(input, &mut raw, FlushDecompress::Finish)
            .map_err(|error| io::Error::new(io::ErrorKind::InvalidInput, error))?;
        if status == Status::StreamEnd {
            break;
        }
        // Out of room (the stream is longer than declared), or out of input
        // before the stream ended (a cut stream or a missing trailer).
        if raw.len() as u64 > length
            || progress == (inflater.total_in(), inflater.total_out())
        {
            return Err(digest());
        }
    }
    if raw.len() as u64 != length || inflater.total_in() as usize != compressed.len() {
        return Err(digest());
    }
    String::from_utf8(raw)
        .map(Arc::from)
        .map_err(|_| invalid_body())
}

/// How many compressed checkpoints a node keeps: as many as the encoded
/// appends it keeps.
const COMPRESSED_CHECKPOINTS: usize = APPEND_BODIES;

/// The checkpoints this node compressed for the wire, each kept beside the
/// text it was compressed from. The leader compresses its proposal once, on
/// the proposing task (`Coordinator::propose`), and every append that carries
/// that entry takes the same bytes. A log entry and its clones share the
/// text, so the text is found by its pointer; holding it here keeps that
/// pointer from naming any other text while it is kept.
#[derive(Default)]
struct CompressedCheckpoints {
    kept: std::sync::Mutex<VecDeque<(Arc<str>, Arc<[u8]>)>>,
    #[cfg(test)]
    compressions: std::sync::atomic::AtomicU64,
}

impl CompressedCheckpoints {
    fn kept(&self) -> std::sync::MutexGuard<'_, VecDeque<(Arc<str>, Arc<[u8]>)>> {
        self.kept
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
    }

    /// The compressed bytes of `text`, compressed now, outside the lock,
    /// when they are not kept already.
    fn compress(&self, text: &Arc<str>) -> io::Result<Arc<[u8]>> {
        let find = |kept: &VecDeque<(Arc<str>, Arc<[u8]>)>| {
            kept.iter()
                .find(|(source, _)| Arc::ptr_eq(source, text))
                .map(|(_, compressed)| compressed.clone())
        };
        if let Some(compressed) = find(&self.kept()) {
            return Ok(compressed);
        }
        #[cfg(test)]
        self.compressions
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let compressed = compress_checkpoint(text)?;
        let mut kept = self.kept();
        if find(&kept).is_none() {
            kept.push_back((text.clone(), compressed.clone()));
            if kept.len() > COMPRESSED_CHECKPOINTS {
                kept.pop_front();
            }
        }
        Ok(compressed)
    }
}

/// An append's RPC body. After the version and header (`body_with_header`),
/// whose JSON is the request with every checkpoint left empty, each normal
/// entry's checkpoint follows in entry order as
/// `text length: u64 | zlib length: u32 | zlib bytes`, little endian; an
/// empty checkpoint is two zero lengths. The checkpoint goes in compressed,
/// once, and is never escaped into the JSON.
fn encode_append(
    request: &AppendEntriesRequest<RoomTypes>,
    checkpoints: &CompressedCheckpoints,
) -> io::Result<Vec<u8>> {
    // Cloning shares each checkpoint; the header's copies are then emptied.
    let mut header = request.clone();
    let mut texts = Vec::new();
    for entry in &mut header.entries {
        if let EntryPayload::Normal(proposal) = &mut entry.payload {
            let text = std::mem::replace(&mut proposal.checkpoint, Arc::from(""));
            let compressed = if text.is_empty() {
                None
            } else {
                Some(checkpoints.compress(&text)?)
            };
            texts.push((text.len() as u64, compressed));
        }
    }
    let extra = texts
        .iter()
        .map(|(_, compressed)| 12 + compressed.as_ref().map_or(0, |bytes| bytes.len()))
        .sum();
    let mut body = body_with_header(&header, extra)?;
    for (length, compressed) in &texts {
        let compressed = compressed.as_deref().unwrap_or_default();
        body.extend_from_slice(&length.to_le_bytes());
        body.extend_from_slice(&(compressed.len() as u32).to_le_bytes());
        body.extend_from_slice(compressed);
    }
    if body.len() > MAX_SNAPSHOT {
        return Err(too_large());
    }
    Ok(body)
}

/// Reads an `encode_append` body, inflating and checking each checkpoint:
/// `read_append`, then `inflate_append`. The RPC's dispatch runs the two
/// itself, before Raft and so outside the store lock, with its own checks
/// between them.
#[cfg(test)]
fn decode_append(bytes: &[u8]) -> io::Result<AppendEntriesRequest<RoomTypes>> {
    let (request, checkpoints) = read_append(bytes)?;
    inflate_append(request, checkpoints)
}

/// The header of an `encode_append` body and each normal entry's declared
/// text length and zlib bytes, in entry order. Every check that needs no
/// inflating runs here, so a body that fails one is refused before any
/// checkpoint is inflated. The declared text lengths together stay within
/// `MAX_SNAPSHOT`, the bound wire 1 put on the whole inflated request.
fn read_append(bytes: &[u8]) -> io::Result<(AppendEntriesRequest<RoomTypes>, Vec<(u64, &[u8])>)> {
    let (request, mut reader) = read_header::<AppendEntriesRequest<RoomTypes>>(bytes)?;
    let mut declared = 0u64;
    let mut checkpoints = Vec::new();
    for entry in &request.entries {
        if let EntryPayload::Normal(proposal) = &entry.payload {
            let length = reader.u64()?;
            let compressed_length = reader.u32()? as usize;
            let compressed = reader.take(compressed_length)?;
            declared = declared.saturating_add(length);
            if !proposal.checkpoint.is_empty() || declared > MAX_SNAPSHOT as u64 {
                return Err(invalid_body());
            }
            if length == 0 && !compressed.is_empty() {
                return Err(invalid_body());
            }
            checkpoints.push((length, compressed));
        }
    }
    if !reader.rest().is_empty() {
        return Err(invalid_body());
    }
    Ok((request, checkpoints))
}

/// Puts each checkpoint `read_append` found back into its entry, inflated.
fn inflate_append(
    mut request: AppendEntriesRequest<RoomTypes>,
    checkpoints: Vec<(u64, &[u8])>,
) -> io::Result<AppendEntriesRequest<RoomTypes>> {
    let mut checkpoints = checkpoints.into_iter();
    for entry in &mut request.entries {
        if let EntryPayload::Normal(proposal) = &mut entry.payload {
            let (length, compressed) = checkpoints.next().ok_or_else(invalid_body)?;
            if length != 0 {
                proposal.checkpoint = inflate_checkpoint(compressed, length)?;
            }
        }
    }
    Ok(request)
}

/// A snapshot RPC's header: everything but its data.
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct InstallSnapshotHeader {
    vote: Vote<u64>,
    meta: SnapshotMeta<u64, BasicNode>,
    offset: u64,
    done: bool,
}

/// A snapshot RPC's body: the version and header (`body_with_header`), then
/// the snapshot data as it is.
fn encode_install_snapshot(request: InstallSnapshotRequest<RoomTypes>) -> io::Result<Vec<u8>> {
    let header = InstallSnapshotHeader {
        vote: request.vote,
        meta: request.meta,
        offset: request.offset,
        done: request.done,
    };
    let mut body = body_with_header(&header, request.data.len())?;
    body.extend_from_slice(&request.data);
    Ok(body)
}

fn decode_install_snapshot(bytes: &[u8]) -> io::Result<InstallSnapshotRequest<RoomTypes>> {
    let (header, reader) = read_header::<InstallSnapshotHeader>(bytes)?;
    let data = reader.rest();
    if data.len() > SNAPSHOT_FRAGMENT_BYTES * SNAPSHOT_CREDIT_WINDOW {
        return Err(io::Error::other("room snapshot window too large"));
    }
    Ok(InstallSnapshotRequest {
        vote: header.vote,
        meta: header.meta,
        offset: header.offset,
        data: data.to_vec(),
        done: header.done,
    })
}

#[derive(Clone, Serialize, Deserialize)]
pub struct Proposal {
    pub request: String,
    /// Stable Raft deduplication namespace. `request` remains the native
    /// transfer identifier exposed to C++ after commitment; this separate
    /// key prevents a fresh process from colliding with an older incarnation
    /// that happened to reuse the same native request number.
    #[serde(default)]
    pub dedup_id: String,
    pub term: u64,
    pub base: u64,
    /// Shared by the log entry, its clones and the commit. An append
    /// carries it compressed beside its JSON header (`encode_append`); as
    /// JSON it is the same string a `String` would be.
    pub checkpoint: Arc<str>,
    #[serde(default)]
    pub admin: Option<AdminEntry>,
}
impl Proposal {
    fn dedup_key(&self) -> &str {
        if self.dedup_id.is_empty() {
            &self.request
        } else {
            &self.dedup_id
        }
    }
    fn digest(&self) -> [u8; 32] {
        let mut hash = blake3::Hasher::new();
        hash.update(self.dedup_key().as_bytes());
        hash.update(&self.base.to_le_bytes());
        hash.update(self.checkpoint.as_bytes());
        *hash.finalize().as_bytes()
    }
}
impl Debug for Proposal {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Proposal")
            .field("term", &self.term)
            .field("base", &self.base)
            .finish_non_exhaustive()
    }
}
#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq, Eq)]
pub struct Receipt {
    pub accepted: bool,
    pub revision: u64,
}
/// The committed checkpoint. The store holds it behind an `Arc` and replaces
/// it whole on each commit, so a reader takes a coherent value without
/// copying the checkpoint, which can weigh a mebibyte.
/// Its fields are private and it has no `Clone`: a value is built whole and
/// never changed, which the cached digest relies on.
#[derive(Default, Serialize, Deserialize)]
pub struct Committed {
    revision: u64,
    checkpoint: Arc<str>,
    request: String,
    term: u64,
    /// The checkpoint's SHA-256, hashed on first use.
    #[serde(skip)]
    digest: OnceLock<[u8; 32]>,
    /// The parts of the checkpoint the helper reads, read on first use
    /// (`view`).
    #[serde(skip)]
    view: OnceLock<Option<Value>>,
}

impl Committed {
    pub fn revision(&self) -> u64 {
        self.revision
    }

    pub fn checkpoint(&self) -> &str {
        &self.checkpoint
    }

    /// The checkpoint text itself, shared rather than copied.
    pub fn checkpoint_shared(&self) -> Arc<str> {
        self.checkpoint.clone()
    }

    pub fn request(&self) -> &str {
        &self.request
    }

    pub fn term(&self) -> u64 {
        self.term
    }

    fn new(revision: u64, checkpoint: Arc<str>, request: String, term: u64) -> Self {
        Self {
            revision,
            checkpoint,
            request,
            term,
            digest: OnceLock::new(),
            view: OnceLock::new(),
        }
    }

    /// The checkpoint's members, room members, tables and tournament
    /// binding, read once per commit (`view::envelope_view`); `None` when
    /// the checkpoint is not JSON. Every reader of the committed roster
    /// takes it from here instead of parsing the whole checkpoint again.
    pub fn view(&self) -> Option<&Value> {
        self.view
            .get_or_init(|| view::envelope_view(&self.checkpoint))
            .as_ref()
    }

    /// The SHA-256 of the checkpoint, all zeroes for none.
    pub fn digest(&self) -> [u8; 32] {
        *self.digest.get_or_init(|| {
            if self.checkpoint.is_empty() {
                [0; 32]
            } else {
                crate::recovery::sha256(self.checkpoint.as_bytes())
            }
        })
    }

    /// The same commit. The store hands out one shared value per commit, so
    /// this is almost always the pointer test; the field comparison covers a
    /// value rebuilt by an installed snapshot.
    pub fn same_commit(&self, other: &Self) -> bool {
        std::ptr::eq(self, other)
            || (self.revision == other.revision
                && self.term == other.term
                && self.request == other.request
                && self.checkpoint == other.checkpoint)
    }
}

/// Small coordination-only entries do not advance the native checkpoint
/// revision. They are still replicated and applied by every admitted helper,
/// which lets a receiver validate a probe against its own committed state.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "type", rename_all = "snake_case", deny_unknown_fields)]
pub enum AdminEntry {
    ProbeReservation(ProbeReservation),
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct ProbeReservation {
    pub room: [u8; 16],
    /// The authenticated primary/coordination identity that initiated the
    /// reservation and the identity that must accept it. Keeping both ends
    /// explicit prevents a remote member from replaying a reservation for a
    /// different process incarnation.
    pub source_incarnation: u64,
    pub target_incarnation: u64,
    pub request: u64,
    pub pair_revision: u64,
    pub term: u64,
    pub expires: u64,
}

/// Small authenticated read-barrier response used when a helper is retiring.
/// It contains no checkpoint bytes: the departing process only needs proof
/// that the successor has the committed term and applied revision.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct AuthorityClaim {
    pub incarnation: u64,
    pub term: u64,
    pub leader: Option<u64>,
    pub revision: u64,
    /// The leader's committed voter set.  A departing follower cannot rely
    /// on its local Raft membership after retain=false removes it from the
    /// old quorum, so graceful leave verifies its absence over this
    /// authenticated leader read instead.
    pub voters: BTreeSet<u64>,
    /// The complete applied membership, including learners.  A native
    /// rejection can leave a coordination-only process as a learner, so a
    /// voter-only claim cannot prove that its authenticated route is gone.
    #[serde(default)]
    pub members: BTreeSet<u64>,
}
#[derive(Clone, Default, Serialize, Deserialize)]
struct Machine {
    applied: Option<LogId<u64>>,
    membership: StoredMembership<u64, BasicNode>,
    /// Replicated proof that an incarnation previously reached applied Raft
    /// membership. A successor can therefore recognize its replay after a
    /// remove entry even if no actor tick observed the intermediate state.
    #[serde(default)]
    member_history: BTreeSet<u64>,
    /// The departed members of `member_history`, oldest departure first: the
    /// order they left the applied membership. Eviction drops from the front,
    /// so the newest departures are the ones kept, and a member that is still
    /// live, or that just left, is never dropped for an older departure.
    /// Members of `member_history` missing from this list were live in the
    /// last applied membership. A snapshot without the field lists none as
    /// departed, so `remember_members` treats all of them as leaving at the
    /// next membership, in ascending incarnation order.
    #[serde(default)]
    departed_order: Vec<u64>,
    committed: Arc<Committed>,
    recent: Vec<(String, [u8; 32], Receipt)>,
    #[serde(default)]
    probes: Vec<ProbeReservation>,
}
impl Machine {
    /// Apply a new membership to the history. Incarnations of the previous
    /// membership that it no longer contains join the departed order, then the
    /// oldest departures beyond the retained window are dropped. The current
    /// members are never dropped. This runs on every replica in log order, so
    /// all of them keep the same window.
    fn remember_members(&mut self, live: &BTreeSet<u64>) {
        // Keep only listed departures that are still remembered, once each,
        // and forget one that is live again.
        let mut listed = BTreeSet::new();
        let history = &self.member_history;
        self.departed_order
            .retain(|id| history.contains(id) && !live.contains(id) && listed.insert(*id));
        // Whatever is not listed as departed was live until now.
        for id in &self.member_history {
            if !live.contains(id) && listed.insert(*id) {
                self.departed_order.push(*id);
            }
        }
        self.member_history.extend(live.iter().copied());
        let excess = self
            .departed_order
            .len()
            .saturating_sub(RETAINED_RETIRED_MEMBER_HISTORY);
        for id in self.departed_order.drain(..excess) {
            self.member_history.remove(&id);
        }
    }
}
struct Memory {
    vote: Option<Vote<u64>>,
    purged: Option<LogId<u64>>,
    log: BTreeMap<u64, Entry<RoomTypes>>,
    machine: Machine,
    snapshot: Option<(SnapshotMeta<u64, BasicNode>, Vec<u8>)>,
    /// The most nodes an installed snapshot's membership may hold.
    max_nodes: usize,
    /// The committed revision, published for `Store::committed_changes`.
    commits: watch::Sender<u64>,
    /// How many times `machine.committed` was replaced: once per accepted
    /// native checkpoint and once per installed snapshot, whatever either
    /// holds. A reader that sees it move by more than one missed a commit.
    observations: u64,
    /// `observations`, published for `Store::observed_changes`.
    observed: watch::Sender<u64>,
}
impl Memory {
    fn new(max_nodes: usize) -> Self {
        Self {
            vote: None,
            purged: None,
            log: BTreeMap::new(),
            machine: Machine::default(),
            snapshot: None,
            max_nodes,
            commits: watch::Sender::new(0),
            observations: 0,
            observed: watch::Sender::new(0),
        }
    }

    /// Tells `committed_changes` readers when the committed revision moved,
    /// and `observed_changes` readers when the committed value was replaced,
    /// which a snapshot at the same revision also does.
    fn publish_commit(&self) {
        let revision = self.machine.committed.revision;
        self.commits.send_if_modified(|published| {
            let moved = *published != revision;
            *published = revision;
            moved
        });
        let observations = self.observations;
        self.observed.send_if_modified(|published| {
            let moved = *published != observations;
            *published = observations;
            moved
        });
    }
}

fn encode_snapshot(machine: &Machine) -> io::Result<Vec<u8>> {
    let raw = serde_json::to_vec(machine).map_err(io::Error::other)?;
    if raw.len() > MAX_SNAPSHOT {
        return Err(io::Error::other("room snapshot too large"));
    }
    let mut encoder = ZlibEncoder::new(Vec::new(), Compression::fast());
    encoder.write_all(&raw)?;
    let compressed = encoder.finish()?;
    let total = SNAPSHOT_HEADER_BYTES
        .checked_add(compressed.len())
        .ok_or_else(|| io::Error::other("room snapshot too large"))?;
    if total > MAX_SNAPSHOT {
        return Err(io::Error::other("room snapshot too large"));
    }
    let mut encoded = Vec::with_capacity(total);
    encoded.extend_from_slice(&SNAPSHOT_MAGIC);
    encoded.extend_from_slice(&(raw.len() as u64).to_le_bytes());
    encoded.extend_from_slice(blake3::hash(&raw).as_bytes());
    encoded.extend_from_slice(&compressed);
    Ok(encoded)
}

fn decode_snapshot(encoded: &[u8]) -> io::Result<Machine> {
    if encoded.len() < SNAPSHOT_HEADER_BYTES || encoded.len() > MAX_SNAPSHOT {
        return Err(io::Error::other("invalid room snapshot envelope"));
    }
    if encoded[..SNAPSHOT_MAGIC.len()] != SNAPSHOT_MAGIC {
        return Err(io::Error::other("invalid room snapshot version"));
    }
    let length_offset = SNAPSHOT_MAGIC.len();
    let digest_offset = length_offset + size_of::<u64>();
    let data_offset = digest_offset + 32;
    let expected_length = u64::from_le_bytes(
        encoded[length_offset..digest_offset]
            .try_into()
            .map_err(|_| io::Error::other("invalid room snapshot length"))?,
    );
    if expected_length > MAX_SNAPSHOT as u64 {
        return Err(io::Error::other("room snapshot too large"));
    }
    let expected_digest = &encoded[digest_offset..data_offset];
    let mut decoder = ZlibDecoder::new(&encoded[data_offset..]);
    let mut raw = Vec::with_capacity(expected_length as usize);
    decoder
        .by_ref()
        .take(MAX_SNAPSHOT as u64 + 1)
        .read_to_end(&mut raw)?;
    if raw.len() as u64 != expected_length
        || decoder.total_in() as usize != encoded.len() - data_offset
        || blake3::hash(&raw).as_bytes() != expected_digest
    {
        return Err(io::Error::other("invalid room snapshot digest"));
    }
    serde_json::from_slice(&raw).map_err(io::Error::other)
}
#[derive(Clone)]
pub struct Store(Arc<Mutex<Memory>>);
impl Default for Store {
    fn default() -> Self {
        Self::with_node_limit(MAX_MEMBERS)
    }
}
impl Store {
    fn with_node_limit(max_nodes: usize) -> Self {
        Self(Arc::new(Mutex::new(Memory::new(max_nodes))))
    }

    /// The committed checkpoint, shared: taking it copies nothing, so the
    /// actor can look on every tick.
    pub async fn committed(&self) -> Arc<Committed> {
        self.0.lock().await.machine.committed.clone()
    }

    /// Follows the committed revision: it changes when an entry or an
    /// installed snapshot moves it, so a reader can wait for that instead of
    /// asking on a timer.
    pub async fn committed_changes(&self) -> watch::Receiver<u64> {
        self.0.lock().await.commits.subscribe()
    }

    /// The committed checkpoint and how many times it has been replaced,
    /// read together under the one lock.
    pub async fn committed_observed(&self) -> (Arc<Committed>, u64) {
        let memory = self.0.lock().await;
        (memory.machine.committed.clone(), memory.observations)
    }

    /// Follows `committed_observed`'s count.
    pub async fn observed_changes(&self) -> watch::Receiver<u64> {
        self.0.lock().await.observed.subscribe()
    }

    /// The membership entry applied by the state machine.  OpenRaft's
    /// metrics expose the effective (possibly joint, merely appended)
    /// configuration; authority and retirement proofs must use this durable
    /// applied view instead.
    pub async fn applied_voters(&self) -> BTreeSet<u64> {
        self.0.lock().await.machine.membership.voter_ids().collect()
    }

    /// The voter set an applied joint configuration is heading to, if the
    /// applied membership is joint. OpenRaft proposes the uniform half only
    /// from the caller of the change; a caller that gave up leaves the group
    /// joint, and the old half then keeps every one of its voters needed.
    pub async fn applied_joint_goal(&self) -> Option<BTreeSet<u64>> {
        let memory = self.0.lock().await;
        let configs = memory.machine.membership.membership().get_joint_config();
        (configs.len() > 1).then(|| configs.last().cloned()).flatten()
    }

    /// Return the durable applied membership, including learners.  Raft's
    /// effective metrics can contain an appended joint configuration; route
    /// retirement must wait for this state-machine view instead.
    pub async fn applied_members(&self) -> BTreeSet<u64> {
        self.0
            .lock()
            .await
            .machine
            .membership
            .nodes()
            .map(|(id, _)| *id)
            .collect()
    }

    /// Read current membership and durable membership history from one state
    /// machine snapshot. Separate reads could straddle an AddLearner commit
    /// and misclassify the newly applied member as retired.
    pub async fn applied_membership_provenance(&self) -> (BTreeSet<u64>, BTreeSet<u64>) {
        let memory = self.0.lock().await;
        (
            memory
                .machine
                .membership
                .nodes()
                .map(|(id, _)| *id)
                .collect(),
            memory.machine.member_history.clone(),
        )
    }

    #[allow(clippy::too_many_arguments)]
    pub async fn probe_reserved(
        &self,
        room: [u8; 16],
        source_incarnation: u64,
        target_incarnation: u64,
        request: u64,
        pair_revision: u64,
        term: u64,
        expires: u64,
    ) -> bool {
        self.0
            .lock()
            .await
            .machine
            .probes
            .iter()
            .any(|reservation| {
                reservation.room == room
                    && reservation.source_incarnation == source_incarnation
                    && reservation.target_incarnation == target_incarnation
                    && reservation.request == request
                    && reservation.pair_revision == pair_revision
                    && reservation.term == term
                    && reservation.expires == expires
            })
    }

    /// Check a probe against the last native checkpoint applied by this
    /// replica.  A Raft membership pair alone is insufficient: a reservation
    /// must name the two members seated at the same table revision, and both
    /// members must still have a committed fighter selection.  The endpoint
    /// strings are checked here as well so a stale endpoint cannot reuse a
    /// valid member id after an incarnation rebind.
    pub async fn probe_pair_bound(
        &self,
        source_incarnation: u64,
        target_incarnation: u64,
        source_endpoint: &str,
        target_endpoint: &str,
        pair_revision: u64,
    ) -> bool {
        let committed = self.committed().await;
        probe_pair_identities_valid(
            source_incarnation,
            target_incarnation,
            source_endpoint,
            target_endpoint,
            pair_revision,
        ) && committed.view().is_some_and(|envelope| {
            probe_pair_bound_in(
                envelope,
                source_incarnation,
                target_incarnation,
                source_endpoint,
                target_endpoint,
                pair_revision,
            )
        })
    }
}

fn probe_pair_identities_valid(
    source_incarnation: u64,
    target_incarnation: u64,
    source_endpoint: &str,
    target_endpoint: &str,
    pair_revision: u64,
) -> bool {
    !(source_incarnation == 0
        || target_incarnation == 0
        || source_incarnation == target_incarnation
        || source_endpoint.is_empty()
        || target_endpoint.is_empty()
        || source_endpoint == target_endpoint
        || pair_revision == 0)
}

/// Validate the native room projection which a committed probe is allowed to
/// use.  This is deliberately a small JSON boundary rather than a native
/// model dependency: the helper must reject malformed or stale projections
/// before opening a GAME_ALPN stream.
pub fn committed_probe_pair_bound(
    checkpoint: &str,
    source_incarnation: u64,
    target_incarnation: u64,
    source_endpoint: &str,
    target_endpoint: &str,
    pair_revision: u64,
) -> bool {
    if checkpoint.is_empty()
        || !probe_pair_identities_valid(
            source_incarnation,
            target_incarnation,
            source_endpoint,
            target_endpoint,
            pair_revision,
        )
    {
        return false;
    }
    let Some(envelope) = envelope_view(checkpoint) else {
        return false;
    };
    probe_pair_bound_in(
        &envelope,
        source_incarnation,
        target_incarnation,
        source_endpoint,
        target_endpoint,
        pair_revision,
    )
}

/// `committed_probe_pair_bound` on a parsed envelope, or its view.
fn probe_pair_bound_in(
    envelope: &Value,
    source_incarnation: u64,
    target_incarnation: u64,
    source_endpoint: &str,
    target_endpoint: &str,
    pair_revision: u64,
) -> bool {
    // Raft stores the complete native SessionProposal JSON.  The portable
    // RecoveryCheckpoint used for pair binding is its nested `checkpoint`
    // member; accepting a flat object here would allow tests or a malformed
    // peer to bypass the proposal identity boundary.
    let Some(root) = envelope.get("checkpoint") else {
        return false;
    };
    let Some(stable_members) = root.get("members").and_then(Value::as_array) else {
        return false;
    };
    let member_for = |incarnation: u64, endpoint: &str| {
        stable_members.iter().find_map(|row| {
            let row_incarnation = row
                .get("incarnation")
                .and_then(Value::as_u64)
                .or_else(|| row.get("data")?.get("incarnation")?.as_u64())?;
            let row_endpoint = row
                .get("data")
                .and_then(|data| {
                    data.get("authenticatedEndpoint")
                        .or_else(|| data.get("authenticated_endpoint"))
                })
                .and_then(Value::as_str)?;
            if row_incarnation == incarnation && row_endpoint == endpoint {
                row.get("member").and_then(Value::as_u64)
            } else {
                None
            }
        })
    };
    let Some(source_member) = member_for(source_incarnation, source_endpoint) else {
        return false;
    };
    let Some(target_member) = member_for(target_incarnation, target_endpoint) else {
        return false;
    };
    if source_member == target_member {
        return false;
    }
    let Some(room_checkpoint) = root.get("room") else {
        return false;
    };
    let Some(room) = room_checkpoint.get("snapshot") else {
        return false;
    };
    let Some(room_members) = room.get("members").and_then(Value::as_array) else {
        return false;
    };
    let member_present = |member: u64| {
        room_members
            .iter()
            .any(|row| row.get("id").and_then(Value::as_u64) == Some(member))
    };
    // Probing measures the authorized seated pair before Ready publishes
    // native fighter selections. Selection is not a network permission.
    if !member_present(source_member) || !member_present(target_member) {
        return false;
    }
    room.get("tables")
        .and_then(Value::as_array)
        .is_some_and(|tables| {
            tables.iter().any(|table| {
                table.get("revision").and_then(Value::as_u64) == Some(pair_revision)
                    && table.get("p1").and_then(Value::as_u64).is_some_and(|p1| {
                        table.get("p2").and_then(Value::as_u64).is_some_and(|p2| {
                            (p1 == source_member && p2 == target_member)
                                || (p1 == target_member && p2 == source_member)
                        })
                    })
            })
        })
}
impl RaftLogReader<RoomTypes> for Store {
    async fn try_get_log_entries<R: RangeBounds<u64> + Clone + Debug + OptionalSend>(
        &mut self,
        range: R,
    ) -> Result<Vec<Entry<RoomTypes>>, StoreError> {
        Ok(self
            .0
            .lock()
            .await
            .log
            .range(range)
            .map(|(_, e)| e.clone())
            .collect())
    }
}
impl Store {
    /// The state machine a snapshot is built from, copied under the lock:
    /// cheap, since the checkpoint itself is shared.
    async fn snapshot_source(&self) -> Machine {
        self.0.lock().await.machine.clone()
    }

    /// Keeps a built snapshot as the current one unless the stored one is
    /// newer. OpenRaft builds snapshots on a task of their own, so an
    /// installed snapshot can land between `snapshot_source` and this.
    /// Returns whether it was kept.
    async fn keep_snapshot(&self, meta: &SnapshotMeta<u64, BasicNode>, data: &[u8]) -> bool {
        let mut memory = self.0.lock().await;
        if memory
            .snapshot
            .as_ref()
            .is_some_and(|(stored, _)| stored.last_log_id > meta.last_log_id)
        {
            return false;
        }
        memory.snapshot = Some((meta.clone(), data.to_vec()));
        true
    }
}

/// The snapshot of `machine`: its metadata and its encoded bytes.
fn snapshot_of(machine: &Machine) -> io::Result<(SnapshotMeta<u64, BasicNode>, Vec<u8>)> {
    let data = encode_snapshot(machine)?;
    let meta = SnapshotMeta {
        last_log_id: machine.applied,
        last_membership: machine.membership.clone(),
        snapshot_id: format!("{:?}", machine.applied),
    };
    Ok((meta, data))
}

impl RaftSnapshotBuilder<RoomTypes> for Store {
    /// Encodes outside the store lock, so apply and the actor's reads do not
    /// wait for it.
    async fn build_snapshot(&mut self) -> Result<Snapshot<RoomTypes>, StoreError> {
        let machine = self.snapshot_source().await;
        let (meta, data) =
            snapshot_of(&machine).map_err(|e| StorageIOError::read_state_machine(&e))?;
        self.keep_snapshot(&meta, &data).await;
        Ok(Snapshot {
            meta,
            snapshot: Box::new(Cursor::new(data)),
        })
    }
}
impl RaftStorage<RoomTypes> for Store {
    type LogReader = Self;
    type SnapshotBuilder = Self;
    async fn save_vote(&mut self, vote: &Vote<u64>) -> Result<(), StoreError> {
        self.0.lock().await.vote = Some(*vote);
        Ok(())
    }
    async fn read_vote(&mut self) -> Result<Option<Vote<u64>>, StoreError> {
        Ok(self.0.lock().await.vote)
    }
    async fn get_log_state(&mut self) -> Result<LogState<RoomTypes>, StoreError> {
        let memory = self.0.lock().await;
        Ok(LogState {
            last_purged_log_id: memory.purged,
            last_log_id: memory
                .log
                .last_key_value()
                .map(|(_, e)| e.log_id)
                .or(memory.purged),
        })
    }
    async fn get_log_reader(&mut self) -> Self {
        self.clone()
    }
    async fn append_to_log<I>(&mut self, entries: I) -> Result<(), StoreError>
    where
        I: IntoIterator<Item = Entry<RoomTypes>> + OptionalSend,
    {
        let mut memory = self.0.lock().await;
        for entry in entries {
            memory.log.insert(entry.log_id.index, entry);
        }
        Ok(())
    }
    async fn delete_conflict_logs_since(&mut self, id: LogId<u64>) -> Result<(), StoreError> {
        self.0.lock().await.log.retain(|index, _| *index < id.index);
        Ok(())
    }
    async fn purge_logs_upto(&mut self, id: LogId<u64>) -> Result<(), StoreError> {
        let mut memory = self.0.lock().await;
        memory.purged = Some(id);
        memory.log.retain(|index, _| *index > id.index);
        Ok(())
    }
    async fn last_applied_state(
        &mut self,
    ) -> Result<(Option<LogId<u64>>, StoredMembership<u64, BasicNode>), StoreError> {
        let memory = self.0.lock().await;
        Ok((memory.machine.applied, memory.machine.membership.clone()))
    }
    async fn apply_to_state_machine(
        &mut self,
        entries: &[Entry<RoomTypes>],
    ) -> Result<Vec<Receipt>, StoreError> {
        let mut memory = self.0.lock().await;
        let machine = &mut memory.machine;
        let mut receipts = Vec::with_capacity(entries.len());
        let mut replaced = 0u64;
        for entry in entries {
            machine.applied = Some(entry.log_id);
            let mut receipt = Receipt {
                accepted: false,
                revision: machine.committed.revision,
            };
            match &entry.payload {
                EntryPayload::Blank => {}
                EntryPayload::Membership(membership) => {
                    let live: BTreeSet<u64> = membership.nodes().map(|(id, _)| *id).collect();
                    machine.remember_members(&live);
                    machine.membership =
                        StoredMembership::new(Some(entry.log_id), membership.clone());
                }
                EntryPayload::Normal(proposal) => {
                    if let Some(AdminEntry::ProbeReservation(reservation)) = &proposal.admin {
                        if proposal.request.len() <= 128
                            && proposal.checkpoint.is_empty()
                            && proposal.term == entry.log_id.leader_id.term
                            && reservation.term == proposal.term
                            && reservation.room != [0; 16]
                            && reservation.source_incarnation != 0
                            && reservation.target_incarnation != 0
                            && reservation.request != 0
                            && reservation.expires != 0
                        {
                            let members: Vec<_> = machine.membership.nodes().collect();
                            let source_member = members
                                .iter()
                                .any(|(id, _)| **id == reservation.source_incarnation);
                            let target_member = members
                                .iter()
                                .any(|(id, _)| **id == reservation.target_incarnation);
                            if !source_member || !target_member {
                                receipts.push(receipt);
                                continue;
                            }
                            // `expires` is a logical watermark carried by
                            // the committed entry. Pruning against it keeps
                            // apply deterministic on every replica while
                            // bounding old process incarnations.
                            machine.probes.retain(|previous| {
                                previous.room != reservation.room
                                    || previous.source_incarnation != reservation.source_incarnation
                                    || previous.target_incarnation != reservation.target_incarnation
                                    || previous.expires >= reservation.expires
                            });
                            if machine.probes.len() >= MAX_MEMBERS {
                                machine.probes.remove(0);
                            }
                            machine.probes.push(reservation.clone());
                            receipt.accepted = true;
                        }
                    } else if let Some((_, digest, previous)) = machine
                        .recent
                        .iter()
                        .find(|(id, _, _)| id == proposal.dedup_key())
                    {
                        // A lost acknowledgement can be retried, but reusing a
                        // request ID for different state must never acknowledge
                        // that different candidate as if it were committed.
                        if *digest == proposal.digest() {
                            receipt = previous.clone();
                        }
                    } else if !proposal.request.is_empty()
                        && !proposal.dedup_key().is_empty()
                        && proposal.dedup_key().len() <= 128
                        && proposal.request.len() <= 128
                        && proposal.checkpoint.len() <= MAX_CHECKPOINT
                        && proposal.term == entry.log_id.leader_id.term
                        && proposal.base == machine.committed.revision
                        && proposal.base < u64::MAX
                    {
                        receipt = Receipt {
                            accepted: true,
                            revision: proposal.base + 1,
                        };
                        machine.committed = Arc::new(Committed::new(
                            receipt.revision,
                            proposal.checkpoint.clone(),
                            proposal.request.clone(),
                            proposal.term,
                        ));
                        replaced += 1;
                        machine.recent.push((
                            proposal.dedup_key().to_owned(),
                            proposal.digest(),
                            receipt.clone(),
                        ));
                        if machine.recent.len() > RECENT_REQUESTS {
                            machine.recent.remove(0);
                        }
                    }
                }
            }
            receipts.push(receipt);
        }
        memory.observations = memory.observations.saturating_add(replaced);
        memory.publish_commit();
        Ok(receipts)
    }
    async fn get_snapshot_builder(&mut self) -> Self {
        self.clone()
    }
    async fn begin_receiving_snapshot(&mut self) -> Result<Box<Cursor<Vec<u8>>>, StoreError> {
        Ok(Box::new(Cursor::new(Vec::new())))
    }
    async fn install_snapshot(
        &mut self,
        meta: &SnapshotMeta<u64, BasicNode>,
        data: Box<Cursor<Vec<u8>>>,
    ) -> Result<(), StoreError> {
        let bytes = data.into_inner();
        let max_nodes = self.0.lock().await.max_nodes;
        if bytes.len() > MAX_SNAPSHOT {
            return Err(StorageIOError::write_state_machine(&io::Error::other(
                "room snapshot too large",
            ))
            .into());
        }
        let machine =
            decode_snapshot(&bytes).map_err(|e| StorageIOError::write_state_machine(&e))?;
        if machine.applied != meta.last_log_id
            || machine.membership != meta.last_membership
            || machine.committed.checkpoint.len() > MAX_CHECKPOINT
            || machine.recent.len() > RECENT_REQUESTS
            || machine.probes.len() > MAX_MEMBERS
            || machine.membership.nodes().count() > max_nodes
            || machine.member_history.len() > MAX_MEMBER_HISTORY
            || machine.departed_order.len() > MAX_RETIRED_MEMBER_HISTORY
            || machine
                .departed_order
                .iter()
                .any(|id| !machine.member_history.contains(id))
            || machine.member_history.contains(&0)
            || machine.recent.iter().any(|(id, _, receipt)| {
                id.is_empty()
                    || id.len() > 128
                    || !receipt.accepted
                    || receipt.revision > machine.committed.revision
            })
            || machine
                .recent
                .iter()
                .enumerate()
                .any(|(index, (id, _, _))| {
                    machine.recent[..index]
                        .iter()
                        .any(|(other, _, _)| other == id)
                })
            || machine.probes.iter().any(|reservation| {
                reservation.room == [0; 16]
                    || reservation.source_incarnation == 0
                    || reservation.target_incarnation == 0
                    || reservation.request == 0
                    || reservation.term == 0
                    || reservation.expires == 0
            })
        {
            return Err(StorageIOError::write_state_machine(&io::Error::other(
                "invalid room checkpoint",
            ))
            .into());
        }
        let mut memory = self.0.lock().await;
        memory.machine = machine;
        memory.snapshot = Some((meta.clone(), bytes));
        memory.observations = memory.observations.saturating_add(1);
        memory.publish_commit();
        Ok(())
    }
    async fn get_current_snapshot(&mut self) -> Result<Option<Snapshot<RoomTypes>>, StoreError> {
        Ok(self
            .0
            .lock()
            .await
            .snapshot
            .as_ref()
            .map(|(meta, data)| Snapshot {
                meta: meta.clone(),
                snapshot: Box::new(Cursor::new(data.clone())),
            }))
    }
}

// Both the Iroh adapter and deterministic partition tests cross this interface.
// RPC serialization errors deliberately do not expose private checkpoint data.
pub trait RpcTransport: Send + Sync + 'static {
    fn call(
        &self,
        target: u64,
        address: String,
        method: &'static str,
        body: Vec<u8>,
    ) -> Pin<Box<dyn Future<Output = io::Result<Vec<u8>>> + Send + '_>>;
}
#[derive(Clone)]
pub struct Network {
    transport: Arc<dyn RpcTransport>,
    /// Shared by every follower's `Peer`.
    appends: Arc<AppendBodies>,
}
impl Network {
    pub fn new(transport: Arc<dyn RpcTransport>) -> Self {
        Self {
            transport,
            appends: Arc::default(),
        }
    }
}

/// An append that carries entries, named by what fixes its content: the
/// leader's vote, the entry before them, the leader's commit and the first
/// and last entry. An entry never changes under its LogId, so two requests
/// with one key encode to the same bytes.
type AppendKey = (
    Vote<u64>,
    Option<LogId<u64>>,
    Option<LogId<u64>>,
    LogId<u64>,
    LogId<u64>,
);
/// How many encoded appends are kept: the newest commit and the one before
/// it, for a follower a step behind.
const APPEND_BODIES: usize = 2;
type AppendBody = Arc<std::sync::OnceLock<Option<Arc<Vec<u8>>>>>;

/// The RPC bodies of the last appends, encoded once for every follower that
/// is sent the same request. Each replication stream runs on its own task, so
/// a stream that finds the key encoding waits for it rather than encoding a
/// second time.
#[derive(Default)]
struct AppendBodies {
    slots: std::sync::Mutex<VecDeque<(AppendKey, AppendBody)>>,
    /// The checkpoints the bodies carry, compressed.
    checkpoints: CompressedCheckpoints,
    #[cfg(test)]
    encodes: std::sync::atomic::AtomicU64,
}
impl AppendBodies {
    /// The encoded body of `request`, `None` when it cannot be encoded.
    fn body(&self, request: &AppendEntriesRequest<RoomTypes>) -> Option<Arc<Vec<u8>>> {
        let encode = || {
            #[cfg(test)]
            self.encodes
                .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            encode_append(request, &self.checkpoints).ok().map(Arc::new)
        };
        // A heartbeat is small; keep the slots for the bodies that are not.
        let (Some(first), Some(last)) = (request.entries.first(), request.entries.last()) else {
            return encode();
        };
        let key = (
            request.vote,
            request.prev_log_id,
            request.leader_commit,
            first.log_id,
            last.log_id,
        );
        let body = {
            let mut slots = self
                .slots
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            match slots.iter().find(|(slot, _)| *slot == key) {
                Some((_, body)) => body.clone(),
                None => {
                    let body = AppendBody::default();
                    slots.push_back((key, body.clone()));
                    if slots.len() > APPEND_BODIES {
                        slots.pop_front();
                    }
                    body
                }
            }
        };
        body.get_or_init(encode).clone()
    }
}

pub struct Peer {
    network: Network,
    id: u64,
    address: String,
}
impl RaftNetworkFactory<RoomTypes> for Network {
    type Network = Peer;
    async fn new_client(&mut self, id: u64, node: &BasicNode) -> Peer {
        Peer {
            network: self.clone(),
            id,
            address: node.addr.clone(),
        }
    }
}
// This error shape is required by OpenRaft's network trait.
fn rpc_failed<E: std::error::Error>() -> RPCError<u64, BasicNode, E> {
    RPCError::Network(NetworkError::new(&io::Error::other(
        "room coordination RPC failed",
    )))
}
impl Peer {
    #[allow(clippy::result_large_err)]
    async fn request<T: Serialize, R: DeserializeOwned, E: std::error::Error>(
        &self,
        method: &'static str,
        value: T,
    ) -> Result<R, RPCError<u64, BasicNode, E>> {
        let bytes = serde_json::to_vec(&value).map_err(|_| rpc_failed())?;
        self.send(method, bytes).await
    }

    #[allow(clippy::result_large_err)]
    async fn send<R: DeserializeOwned, E: std::error::Error>(
        &self,
        method: &'static str,
        bytes: Vec<u8>,
    ) -> Result<R, RPCError<u64, BasicNode, E>> {
        let response = self
            .network
            .transport
            .call(self.id, self.address.clone(), method, bytes)
            .await
            .map_err(|_| rpc_failed())?;
        serde_json::from_slice(&response).map_err(|_| rpc_failed())
    }
}
impl RaftNetwork<RoomTypes> for Peer {
    async fn append_entries(
        &mut self,
        request: AppendEntriesRequest<RoomTypes>,
        _: RPCOption,
    ) -> Result<AppendEntriesResponse<u64>, RPCError<u64, BasicNode, RaftError<u64>>> {
        // Every follower sent this request is sent the same bytes.
        let body = self.network.appends.body(&request).ok_or_else(rpc_failed)?;
        self.send("append", body.as_ref().clone()).await
    }
    async fn vote(
        &mut self,
        request: VoteRequest<u64>,
        _: RPCOption,
    ) -> Result<VoteResponse<u64>, RPCError<u64, BasicNode, RaftError<u64>>> {
        self.request("vote", request).await
    }
    async fn install_snapshot(
        &mut self,
        request: InstallSnapshotRequest<RoomTypes>,
        _: RPCOption,
    ) -> Result<
        InstallSnapshotResponse<u64>,
        RPCError<u64, BasicNode, RaftError<u64, InstallSnapshotError>>,
    > {
        let body = encode_install_snapshot(request).map_err(|_| rpc_failed())?;
        self.send("snapshot", body).await
    }
}

pub struct Coordinator {
    room: [u8; 16],
    incarnation: u64,
    ownership: Ownership,
    raft: RoomRaft,
    store: Store,
    network: Network,
    pub(crate) proposals: Mutex<()>,
    /// A room-wide refresh makes every follower ask the same leader for a
    /// linearizable authority claim at nearly the same time.  Serialize those
    /// requests and share one just-completed barrier while its exact term,
    /// checkpoint revision, and applied membership remain current.
    authority_claims: Mutex<Option<(Instant, AuthorityClaim)>>,
    #[cfg(test)]
    authority_barriers: std::sync::atomic::AtomicU64,
    /// Serialize every Raft membership mutation with its applied-history
    /// validation. A delayed control Admission must not pass a precheck,
    /// straddle an authenticated removal, and re-add that retired process.
    pub(crate) membership_operations: Mutex<()>,
    /// Follows the store's committed revision (`committed_revision`).
    committed_revision: watch::Receiver<u64>,
}
impl Coordinator {
    pub async fn new(incarnation: u64, transport: Arc<dyn RpcTransport>) -> io::Result<Self> {
        Self::new_for_room([0; 16], incarnation, transport).await
    }

    pub async fn new_for_room(
        room: [u8; 16],
        incarnation: u64,
        transport: Arc<dyn RpcTransport>,
    ) -> io::Result<Self> {
        Self::new_owned(room, incarnation, transport, Ownership::Private).await
    }

    /// `new_for_room` for a node that takes part in its room as `ownership`.
    pub async fn new_owned(
        room: [u8; 16],
        incarnation: u64,
        transport: Arc<dyn RpcTransport>,
        ownership: Ownership,
    ) -> io::Result<Self> {
        if incarnation == 0 {
            return Err(io::Error::other("invalid room incarnation"));
        }
        let config = Config {
            // OpenRaft also uses the heartbeat interval as the hard TTL for
            // AppendEntries. A committed private-room checkpoint is commonly
            // 320 KiB and may take more than one second over a healthy relay.
            // Keep the append deadline just below the transport's ten-second
            // bound. Elections complete before the room's 15-second
            // replacement offer when a leader is actually lost.
            heartbeat_interval: 9_000,
            election_timeout_min: 10_000,
            election_timeout_max: 12_000,
            // Snapshot bootstrap uses the same authenticated relay and bounded
            // transport as append. Keep both the sender and per-chunk receiver
            // deadline below that ten-second transport limit; canceling a slow
            // but healthy first chunk otherwise strands a fresh learner.
            install_snapshot_timeout: 9_000,
            max_payload_entries: 1,
            // Every log entry carries a whole checkpoint, so the entries kept
            // between snapshots are most of a helper's memory. The host of a
            // server-owned room is its only voter and its members are
            // learners that are sent a snapshot when they fall behind, so it
            // keeps few; a private room keeps enough to serve a slow voter.
            snapshot_policy: SnapshotPolicy::LogsSinceLast(match ownership {
                Ownership::Host => 3,
                _ => 8,
            }),
            max_in_snapshot_log_to_keep: match ownership {
                Ownership::Host => 1,
                _ => 2,
            },
            // OpenRaft awaits one InstallSnapshot RPC before issuing the next.
            // Batch the same four 16 KiB transport fragments allowed by the
            // checkpoint credit window so relay RTT is paid once per window,
            // while each underlying write and the total snapshot stay bounded.
            snapshot_max_chunk_size: (SNAPSHOT_FRAGMENT_BYTES * SNAPSHOT_CREDIT_WINDOW) as u64,
            enable_elect: ownership.campaigns(),
            ..Config::default()
        }
        .validate()
        .map_err(|_| io::Error::other("invalid room coordination configuration"))?;
        let store = Store::with_node_limit(ownership.max_nodes());
        let committed_revision = store.committed_changes().await;
        let (log, machine) = Adaptor::new(store.clone());
        let network = Network::new(transport);
        let raft = Raft::new(incarnation, Arc::new(config), network.clone(), log, machine)
            .await
            .map_err(|_| io::Error::other("room coordinator startup failed"))?;
        Ok(Self {
            room,
            incarnation,
            ownership,
            raft,
            store,
            network,
            proposals: Mutex::new(()),
            authority_claims: Mutex::new(None),
            #[cfg(test)]
            authority_barriers: std::sync::atomic::AtomicU64::new(0),
            membership_operations: Mutex::new(()),
            committed_revision,
        })
    }
    pub fn raft(&self) -> &RoomRaft {
        &self.raft
    }

    pub fn ownership(&self) -> Ownership {
        self.ownership
    }

    /// A read barrier is required before helper advertises a writable native
    /// authority.  The current term alone is not an authority claim.
    pub async fn read_barrier(&self) -> io::Result<()> {
        let metrics = self.raft.metrics().borrow().clone();
        let Some(leader) = metrics.current_leader else {
            return Err(io::Error::other("room quorum unavailable"));
        };
        if leader == self.incarnation {
            return self
                .raft
                .ensure_linearizable()
                .await
                .map(|_| ())
                .map_err(|_| io::Error::other("room quorum unavailable"));
        }
        let node = metrics
            .membership_config
            .membership()
            .get_node(&leader)
            .ok_or_else(|| io::Error::other("room leader route unavailable"))?;
        let bytes = self
            .network
            .transport
            .call(leader, node.addr.clone(), "authority", vec![0])
            .await?;
        let claim: AuthorityClaim = serde_json::from_slice(&bytes)
            .map_err(|_| io::Error::other("invalid authority claim"))?;
        let revision = self.store.committed().await.revision;
        if claim.incarnation != leader
            || claim.leader != Some(leader)
            || claim.term < metrics.current_term
            || claim.revision < revision
        {
            return Err(io::Error::other("room quorum proof is stale"));
        }
        Ok(())
    }

    pub fn current_term(&self) -> u64 {
        self.raft.metrics().borrow().current_term
    }

    pub fn current_leader(&self) -> Option<u64> {
        self.raft.metrics().borrow().current_leader
    }

    /// Return a leader claim only after this node has established its own
    /// linearizable quorum read. Callers use this over the authenticated
    /// coordination route to confirm a successor before retiring a helper.
    pub async fn authority_claim(&self) -> io::Result<AuthorityClaim> {
        let mut cached = self.authority_claims.lock().await;
        let metrics = self.raft.metrics().borrow().clone();
        let revision = self.committed().await.revision;
        let voters = self.store.applied_voters().await;
        let members = self.store.applied_members().await;
        // The host of a server-owned room is its only voter, so its barrier
        // confirms nothing a follower could contradict; its once-a-second
        // health check reuses a claim for longer.
        let coalesce = if matches!(self.ownership, Ownership::Host) {
            HOST_AUTHORITY_CLAIM_COALESCE
        } else {
            AUTHORITY_CLAIM_COALESCE
        };
        if let Some((completed, claim)) = cached.as_ref()
            && completed.elapsed() <= coalesce
            && claim.term == metrics.current_term
            && claim.leader == metrics.current_leader
            && claim.revision == revision
            && claim.voters == voters
            && claim.members == members
        {
            return Ok(claim.clone());
        }
        #[cfg(test)]
        self.authority_barriers
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        self.read_barrier().await?;
        let metrics = self.raft.metrics().borrow().clone();
        let revision = self.committed().await.revision;
        let claim = AuthorityClaim {
            incarnation: self.incarnation,
            term: metrics.current_term,
            leader: metrics.current_leader,
            revision,
            voters: self.store.applied_voters().await,
            members: self.store.applied_members().await,
        };
        *cached = Some((Instant::now(), claim.clone()));
        Ok(claim)
    }

    pub fn voter_ids(&self) -> std::collections::BTreeSet<u64> {
        self.raft
            .metrics()
            .borrow()
            .membership_config
            .membership()
            .voter_ids()
            .collect()
    }

    pub async fn applied_voter_ids(&self) -> std::collections::BTreeSet<u64> {
        self.store.applied_voters().await
    }

    pub async fn applied_joint_goal(&self) -> Option<std::collections::BTreeSet<u64>> {
        self.store.applied_joint_goal().await
    }

    /// Commit the uniform half of an applied joint configuration that its
    /// caller abandoned. The goal is already fixed by an applied entry, so no
    /// roster read is needed, and none is made: a read would need quorums of
    /// both halves, which is what a stuck joint configuration may have lost.
    /// OpenRaft commits the uniform entry with the goal's own quorum.
    /// Returns whether there was one to finish.
    pub async fn finish_joint_membership(&self) -> io::Result<bool> {
        let _membership = self.membership_operations.lock().await;
        let Some(goal) = self.store.applied_joint_goal().await else {
            return Ok(false);
        };
        self.raft
            .change_membership(goal, true)
            .await
            .map_err(|_| io::Error::other("coordination joint membership not finished"))?;
        Ok(true)
    }

    pub async fn applied_member_ids(&self) -> std::collections::BTreeSet<u64> {
        self.store.applied_members().await
    }

    pub async fn applied_membership_provenance(
        &self,
    ) -> (
        std::collections::BTreeSet<u64>,
        std::collections::BTreeSet<u64>,
    ) {
        self.store.applied_membership_provenance().await
    }

    pub fn learner_ids(&self) -> std::collections::BTreeSet<u64> {
        self.raft
            .metrics()
            .borrow()
            .membership_config
            .membership()
            .learner_ids()
            .collect()
    }
    /// The committed revision, read without the store lock: the value the
    /// store last published for `committed_changes`, which it publishes under
    /// the lock in the same step that replaces the commit. The actor looks
    /// at it every tick and takes the lock only when it moved.
    pub fn committed_revision(&self) -> u64 {
        *self.committed_revision.borrow()
    }

    pub async fn committed(&self) -> Arc<Committed> {
        self.store.committed().await
    }

    pub async fn committed_changes(&self) -> watch::Receiver<u64> {
        self.store.committed_changes().await
    }

    pub async fn committed_observed(&self) -> (Arc<Committed>, u64) {
        self.store.committed_observed().await
    }

    pub async fn observed_changes(&self) -> watch::Receiver<u64> {
        self.store.observed_changes().await
    }

    /// Tests only: the state machine, to apply entries and install snapshots
    /// as Raft would.
    #[cfg(test)]
    pub fn store(&self) -> &Store {
        &self.store
    }
    #[allow(clippy::too_many_arguments)]
    pub async fn probe_reserved(
        &self,
        room: [u8; 16],
        peer: u64,
        incarnation: u64,
        request: u64,
        pair_revision: u64,
        term: u64,
        expires: u64,
    ) -> bool {
        self.store
            .probe_reserved(
                room,
                peer,
                incarnation,
                request,
                pair_revision,
                term,
                expires,
            )
            .await
    }
    pub async fn probe_pair_bound(
        &self,
        source_incarnation: u64,
        target_incarnation: u64,
        source_endpoint: &str,
        target_endpoint: &str,
        pair_revision: u64,
    ) -> bool {
        self.store
            .probe_pair_bound(
                source_incarnation,
                target_incarnation,
                source_endpoint,
                target_endpoint,
                pair_revision,
            )
            .await
    }
    pub async fn propose(&self, proposal: Proposal) -> io::Result<Receipt> {
        if proposal.request.is_empty()
            || proposal.dedup_key().is_empty()
            || proposal.dedup_key().len() > 128
            || proposal.request.len() > 128
            || proposal.checkpoint.len() > MAX_CHECKPOINT
        {
            return Err(io::Error::other("invalid room proposal"));
        }
        if let Some(AdminEntry::ProbeReservation(reservation)) = &proposal.admin
            && self.room != [0; 16]
            && reservation.room != self.room
        {
            return Err(io::Error::other("invalid room reservation"));
        }
        let metrics = self.raft.metrics().borrow().clone();
        let Some(leader) = metrics.current_leader else {
            return Err(io::Error::other("room quorum unavailable"));
        };
        if leader != self.incarnation {
            if proposal.admin.is_none() {
                return Err(io::Error::other("obsolete room authority"));
            }
            let node = metrics
                .membership_config
                .membership()
                .get_node(&leader)
                .ok_or_else(|| io::Error::other("room leader route unavailable"))?;
            let bytes = self
                .network
                .transport
                .call(
                    leader,
                    node.addr.clone(),
                    "propose",
                    serde_json::to_vec(&proposal)
                        .map_err(|_| io::Error::other("room proposal encoding failed"))?,
                )
                .await?;
            return serde_json::from_slice(&bytes)
                .map_err(|_| io::Error::other("invalid room proposal response"));
        }
        let _serial = self
            .proposals
            .try_lock()
            .map_err(|_| io::Error::other("room proposal busy"))?;
        self.raft
            .ensure_linearizable()
            .await
            .map_err(|_| io::Error::other("room quorum unavailable"))?;
        if self.raft.metrics().borrow().current_term != proposal.term {
            return Err(io::Error::other("obsolete room authority"));
        }
        // Compressed once, here on the proposing task rather than on a
        // replication stream; every append carrying the entry takes these
        // bytes. Should it fail, the append encoder compresses it itself.
        if proposal.admin.is_none() && !proposal.checkpoint.is_empty() {
            let _ = self.network.appends.checkpoints.compress(&proposal.checkpoint);
        }
        let response = self
            .raft
            .client_write(proposal)
            .await
            .map_err(|_| io::Error::other("room commitment failed"))?;
        Ok(response.data)
    }
    /// Whether the authenticated source may stand as a candidate here. Only
    /// a voter of this node's latest or applied membership can be elected. A
    /// fresh learner has no membership until its first entry or snapshot
    /// arrives; until then the transport's admitted binding is the only check.
    async fn may_campaign(&self, source: u64) -> bool {
        let (empty, effective) = {
            let metrics = self.raft.metrics();
            let metrics = metrics.borrow();
            let membership = metrics.membership_config.membership();
            (
                membership.nodes().next().is_none(),
                membership.voter_ids().any(|id| id == source),
            )
        };
        empty || effective || self.store.applied_voters().await.contains(&source)
    }

    pub async fn dispatch(&self, source: u64, method: &str, bytes: &[u8]) -> io::Result<Vec<u8>> {
        let bad = || io::Error::other("invalid room coordination request");
        if source == 0 || bytes.len() > MAX_SNAPSHOT {
            return Err(bad());
        }
        if !self.ownership.serves(source, method) {
            return Err(bad());
        }
        match method {
            "propose" => {
                let proposal: Proposal = serde_json::from_slice(bytes).map_err(|_| bad())?;
                // Native checkpoint proposals are created only by the local
                // C++ owner. A remote authenticated coordination RPC may
                // forward bounded admin entries, but it must never become a
                // second native writer. Probe source identity is bound to the
                // authenticated Raft sender as well.
                match &proposal.admin {
                    Some(AdminEntry::ProbeReservation(reservation))
                        if reservation.source_incarnation == source => {}
                    _ => return Err(bad()),
                }
                serde_json::to_vec(&self.propose(proposal).await.map_err(|_| bad())?)
                    .map_err(|_| bad())
            }
            "authority" => {
                if bytes != [0] {
                    return Err(bad());
                }
                serde_json::to_vec(&self.authority_claim().await?).map_err(|_| bad())
            }
            "elect" => {
                // A retiring leader explicitly asks the committed successor
                // to take over before the old coordination route closes.
                // The authenticated Raft source must still be this node's
                // current leader; followers cannot manufacture elections.
                if bytes != [0]
                    || self.raft.metrics().borrow().current_leader != Some(source)
                    || !self
                        .store
                        .applied_voters()
                        .await
                        .contains(&self.incarnation)
                {
                    return Err(bad());
                }
                self.raft.trigger().elect().await.map_err(|_| bad())?;
                Ok(vec![1])
            }
            "remove" => {
                // A member may request only its own departure.  The
                // authenticated RPC source is the identity being removed;
                // the current leader commits the old-quorum transition.
                if bytes != [0]
                    || self.raft.metrics().borrow().current_leader != Some(self.incarnation)
                    || source == self.incarnation
                {
                    return Err(bad());
                }
                let _membership = self.membership_operations.lock().await;
                let voters = self.store.applied_voters().await;
                let members = self.store.applied_members().await;
                if !members.contains(&source) {
                    return Err(bad());
                }
                if voters.contains(&source) {
                    if voters.len() <= 1 {
                        return Err(bad());
                    }
                    // Two members left keep only this leader as a voter. Going
                    // there in one change, keeping the others as learners,
                    // never commits two voters on the way.
                    let staying = members.len().saturating_sub(1);
                    if crate::recovery::stable_voter_count(staying) == 1 {
                        self.raft
                            .change_membership(BTreeSet::from([self.incarnation]), true)
                            .await
                            .map_err(|_| bad())?;
                    } else {
                        let mut replacement = voters;
                        replacement.remove(&source);
                        self.raft
                            .change_membership(replacement, false)
                            .await
                            .map_err(|_| bad())?;
                    }
                }
                // `retain=false` demotes a removed voter to a learner. Commit
                // its exact node removal under the same operation lock before
                // replying, so a delayed Admission observes durable absence.
                self.raft
                    .change_membership(ChangeMembers::RemoveNodes(BTreeSet::from([source])), false)
                    .await
                    .map_err(|_| bad())?;
                Ok(vec![1])
            }
            "append" => {
                // Inflated and checked here, before Raft takes the store lock.
                // The checks that need no inflating come first.
                let (request, checkpoints) = read_append(bytes).map_err(|_| bad())?;
                if request.entries.len() > 1
                    || request.vote.leader_id.node_id != source
                    || checkpoints
                        .iter()
                        .any(|(length, _)| *length > MAX_CHECKPOINT as u64)
                {
                    return Err(bad());
                }
                let request = inflate_append(request, checkpoints).map_err(|_| bad())?;
                for entry in &request.entries {
                    if let EntryPayload::Normal(p) = &entry.payload
                        && (p.checkpoint.len() > MAX_CHECKPOINT
                            || p.request.is_empty()
                            || p.request.len() > 128)
                    {
                        return Err(bad());
                    }
                    if let EntryPayload::Membership(membership) = &entry.payload
                        && membership.nodes().count() > self.ownership.max_nodes()
                    {
                        return Err(bad());
                    }
                }
                let response = self.raft.append_entries(request).await.map_err(|_| bad())?;
                serde_json::to_vec(&response).map_err(|_| bad())
            }
            "vote" => {
                let request: VoteRequest<u64> = serde_json::from_slice(bytes).map_err(|_| bad())?;
                // A candidate always asks with an uncommitted vote. OpenRaft
                // stores the requested vote once the log check passes, so a
                // vote is granted only in the candidate's form.
                if request.vote.leader_id.node_id != source
                    || request.vote.is_committed()
                    || !self.may_campaign(source).await
                {
                    return Err(bad());
                }
                serde_json::to_vec(&self.raft.vote(request).await.map_err(|_| bad())?)
                    .map_err(|_| bad())
            }
            "snapshot" => {
                let request = decode_install_snapshot(bytes).map_err(|_| bad())?;
                if request.vote.leader_id.node_id != source
                    || request.data.len() > SNAPSHOT_FRAGMENT_BYTES * SNAPSHOT_CREDIT_WINDOW
                    || request
                        .offset
                        .checked_add(request.data.len() as u64)
                        .is_none_or(|end| end > MAX_SNAPSHOT as u64)
                    || request.meta.last_membership.nodes().count() > self.ownership.max_nodes()
                {
                    return Err(bad());
                }
                let response = self
                    .raft
                    .install_snapshot(request)
                    .await
                    .map_err(|_| bad())?;
                serde_json::to_vec(&response).map_err(|_| bad())
            }
            _ => Err(bad()),
        }
    }
}

#[cfg(test)]
mod tests;
