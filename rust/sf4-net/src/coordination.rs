//! Room commitment, independent of gameplay links and the native simulation.
//!
//! Storage is process-incarnation scoped: a stopped instance is never restarted
//! with the same voter ID. It must join a live quorum as a new learner instead.
//! Neither snapshots nor RPC errors are logged: checkpoints contain private
//! admission state. C++ owns rule validation; only committed checkpoints escape.
use std::{
    collections::{BTreeMap, BTreeSet},
    fmt::Debug,
    future::Future,
    io::{self, Cursor, Read, Write},
    ops::RangeBounds,
    pin::Pin,
    sync::Arc,
    time::{Duration, Instant},
};

use base64::{Engine as _, engine::general_purpose::URL_SAFE_NO_PAD};
use flate2::{Compression, read::ZlibDecoder, write::ZlibEncoder};
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

mod server_owned;
pub use server_owned::Ownership;

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
const SNAPSHOT_MAGIC: [u8; 8] = *b"sf4rs001";
const SNAPSHOT_HEADER_BYTES: usize = SNAPSHOT_MAGIC.len() + size_of::<u64>() + 32;
const AUTHORITY_CLAIM_COALESCE: Duration = Duration::from_millis(500);
/// The same for the sole voter of a server-owned room; see `authority_claim`.
const HOST_AUTHORITY_CLAIM_COALESCE: Duration = Duration::from_millis(2500);

openraft::declare_raft_types!(pub RoomTypes: D = Proposal, R = Receipt, Node = BasicNode);
pub type RoomRaft = Raft<RoomTypes>;
type StoreError = StorageError<u64>;

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct CompressedJsonWire {
    version: u8,
    length: u64,
    digest: [u8; 32],
    data: String,
}

impl CompressedJsonWire {
    fn encode<T: Serialize>(value: &T) -> io::Result<Self> {
        let raw = serde_json::to_vec(value).map_err(io::Error::other)?;
        if raw.len() > MAX_SNAPSHOT {
            return Err(io::Error::other("room coordination message too large"));
        }
        let mut encoder = ZlibEncoder::new(Vec::new(), Compression::fast());
        encoder.write_all(&raw)?;
        let compressed = encoder.finish()?;
        if compressed.len() > MAX_SNAPSHOT {
            return Err(io::Error::other("room coordination message too large"));
        }
        Ok(Self {
            version: 1,
            length: raw.len() as u64,
            digest: *blake3::hash(&raw).as_bytes(),
            data: URL_SAFE_NO_PAD.encode(compressed),
        })
    }

    fn decode<T: DeserializeOwned>(self) -> io::Result<T> {
        if self.version != 1 || self.length > MAX_SNAPSHOT as u64 || self.data.len() > MAX_SNAPSHOT
        {
            return Err(io::Error::other("invalid room coordination envelope"));
        }
        let compressed = URL_SAFE_NO_PAD
            .decode(self.data)
            .map_err(|_| io::Error::other("invalid room coordination encoding"))?;
        if compressed.len() > MAX_SNAPSHOT {
            return Err(io::Error::other("room coordination message too large"));
        }
        let mut decoder = ZlibDecoder::new(compressed.as_slice());
        let mut raw = Vec::with_capacity(self.length as usize);
        decoder
            .by_ref()
            .take(MAX_SNAPSHOT as u64 + 1)
            .read_to_end(&mut raw)?;
        if raw.len() as u64 != self.length
            || decoder.total_in() as usize != compressed.len()
            || blake3::hash(&raw).as_bytes() != &self.digest
        {
            return Err(io::Error::other("invalid room coordination digest"));
        }
        serde_json::from_slice(&raw).map_err(io::Error::other)
    }
}

#[derive(Serialize, Deserialize)]
struct InstallSnapshotWire {
    vote: Vote<u64>,
    meta: SnapshotMeta<u64, BasicNode>,
    offset: u64,
    data: String,
    done: bool,
}

impl InstallSnapshotWire {
    fn encode(request: InstallSnapshotRequest<RoomTypes>) -> Self {
        Self {
            vote: request.vote,
            meta: request.meta,
            offset: request.offset,
            data: URL_SAFE_NO_PAD.encode(request.data),
            done: request.done,
        }
    }

    fn decode(self) -> io::Result<InstallSnapshotRequest<RoomTypes>> {
        let data = URL_SAFE_NO_PAD
            .decode(self.data)
            .map_err(|_| io::Error::other("invalid room snapshot encoding"))?;
        if data.len() > SNAPSHOT_FRAGMENT_BYTES * SNAPSHOT_CREDIT_WINDOW {
            return Err(io::Error::other("room snapshot window too large"));
        }
        Ok(InstallSnapshotRequest {
            vote: self.vote,
            meta: self.meta,
            offset: self.offset,
            data,
            done: self.done,
        })
    }
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
    pub checkpoint: String,
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
#[derive(Clone, Default, Serialize, Deserialize)]
pub struct Committed {
    pub revision: u64,
    pub checkpoint: String,
    pub request: String,
    pub term: u64,
}

/// What identifies a committed checkpoint without its text: on one node a
/// revision names one checkpoint, so a reader that saw this mark and sees it
/// again saw the same checkpoint.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CommittedMark {
    pub revision: u64,
    pub term: u64,
    pub request: String,
}

impl Committed {
    pub fn mark(&self) -> CommittedMark {
        CommittedMark {
            revision: self.revision,
            term: self.term,
            request: self.request.clone(),
        }
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
    committed: Committed,
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
    /// The SHA-256 of the committed checkpoint at the revision it was taken
    /// for, hashed once per revision for `committed_digest`.
    committed_digest: Option<(u64, [u8; 32])>,
    /// The committed revision, published for `Store::committed_changes`.
    commits: watch::Sender<u64>,
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
            committed_digest: None,
            commits: watch::Sender::new(0),
        }
    }

    /// Tells `committed_changes` readers when the committed revision moved.
    fn publish_commit(&self) {
        let revision = self.machine.committed.revision;
        self.commits.send_if_modified(|published| {
            let moved = *published != revision;
            *published = revision;
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

    pub async fn committed(&self) -> Committed {
        self.0.lock().await.machine.committed.clone()
    }

    /// The committed revision alone. The actor asks on every tick whether it
    /// has moved, and the checkpoint behind it can weigh a mebibyte.
    pub async fn committed_revision(&self) -> u64 {
        self.0.lock().await.machine.committed.revision
    }

    pub async fn committed_mark(&self) -> CommittedMark {
        self.0.lock().await.machine.committed.mark()
    }

    /// The committed mark, with the checkpoint only when the mark is not
    /// `known`: one read, so the checkpoint is always the mark's.
    pub async fn committed_unless(
        &self,
        known: Option<&CommittedMark>,
    ) -> (CommittedMark, Option<String>) {
        let memory = self.0.lock().await;
        let committed = &memory.machine.committed;
        let mark = committed.mark();
        let checkpoint = (known != Some(&mark)).then(|| committed.checkpoint.clone());
        (mark, checkpoint)
    }

    /// Follows the committed revision: it changes when an entry or an
    /// installed snapshot moves it, so a reader can wait for that instead of
    /// asking on a timer.
    pub async fn committed_changes(&self) -> watch::Receiver<u64> {
        self.0.lock().await.commits.subscribe()
    }

    /// The committed revision and the SHA-256 of its checkpoint, hashed once
    /// per revision: the periodic coordination refresh reads it every second.
    pub async fn committed_digest(&self) -> (u64, [u8; 32]) {
        let mut memory = self.0.lock().await;
        let revision = memory.machine.committed.revision;
        if let Some((hashed, digest)) = memory.committed_digest
            && hashed == revision
        {
            return (revision, digest);
        }
        let checkpoint = &memory.machine.committed.checkpoint;
        let digest = if checkpoint.is_empty() {
            [0; 32]
        } else {
            crate::recovery::sha256(checkpoint.as_bytes())
        };
        memory.committed_digest = Some((revision, digest));
        (revision, digest)
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
        let checkpoint = self.committed().await.checkpoint;
        committed_probe_pair_bound(
            &checkpoint,
            source_incarnation,
            target_incarnation,
            source_endpoint,
            target_endpoint,
            pair_revision,
        )
    }
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
        || source_incarnation == 0
        || target_incarnation == 0
        || source_incarnation == target_incarnation
        || source_endpoint.is_empty()
        || target_endpoint.is_empty()
        || source_endpoint == target_endpoint
        || pair_revision == 0
    {
        return false;
    }
    let Ok(envelope) = serde_json::from_str::<Value>(checkpoint) else {
        return false;
    };
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
impl RaftSnapshotBuilder<RoomTypes> for Store {
    async fn build_snapshot(&mut self) -> Result<Snapshot<RoomTypes>, StoreError> {
        let mut memory = self.0.lock().await;
        let data =
            encode_snapshot(&memory.machine).map_err(|e| StorageIOError::read_state_machine(&e))?;
        let meta = SnapshotMeta {
            last_log_id: memory.machine.applied,
            last_membership: memory.machine.membership.clone(),
            snapshot_id: format!("{:?}", memory.machine.applied),
        };
        memory.snapshot = Some((meta.clone(), data.clone()));
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
                        machine.committed = Committed {
                            revision: receipt.revision,
                            checkpoint: proposal.checkpoint.clone(),
                            request: proposal.request.clone(),
                            term: proposal.term,
                        };
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
pub struct Network(pub Arc<dyn RpcTransport>);
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
impl Peer {
    // This error shape is required by OpenRaft's network trait.
    #[allow(clippy::result_large_err)]
    async fn request<T: Serialize, R: DeserializeOwned, E: std::error::Error>(
        &self,
        method: &'static str,
        value: T,
    ) -> Result<R, RPCError<u64, BasicNode, E>> {
        let error = || {
            RPCError::Network(NetworkError::new(&io::Error::other(
                "room coordination RPC failed",
            )))
        };
        let bytes = serde_json::to_vec(&value).map_err(|_| error())?;
        let response = self
            .network
            .0
            .call(self.id, self.address.clone(), method, bytes)
            .await
            .map_err(|_| error())?;
        serde_json::from_slice(&response).map_err(|_| error())
    }
}
impl RaftNetwork<RoomTypes> for Peer {
    async fn append_entries(
        &mut self,
        request: AppendEntriesRequest<RoomTypes>,
        _: RPCOption,
    ) -> Result<AppendEntriesResponse<u64>, RPCError<u64, BasicNode, RaftError<u64>>> {
        let wire = CompressedJsonWire::encode(&request).map_err(|_| {
            RPCError::Network(NetworkError::new(&io::Error::other(
                "room coordination RPC failed",
            )))
        })?;
        self.request("append", wire).await
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
        self.request("snapshot", InstallSnapshotWire::encode(request))
            .await
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
        let (log, machine) = Adaptor::new(store.clone());
        let network = Network(transport);
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
            .0
            .call(leader, node.addr.clone(), "authority", vec![0])
            .await?;
        let claim: AuthorityClaim = serde_json::from_slice(&bytes)
            .map_err(|_| io::Error::other("invalid authority claim"))?;
        let revision = self.store.committed_revision().await;
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
        let revision = self.committed_revision().await;
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
        let revision = self.committed_revision().await;
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
    pub async fn committed(&self) -> Committed {
        self.store.committed().await
    }

    pub async fn committed_revision(&self) -> u64 {
        self.store.committed_revision().await
    }

    pub async fn committed_mark(&self) -> CommittedMark {
        self.store.committed_mark().await
    }

    pub async fn committed_digest(&self) -> (u64, [u8; 32]) {
        self.store.committed_digest().await
    }

    pub async fn committed_unless(
        &self,
        known: Option<&CommittedMark>,
    ) -> (CommittedMark, Option<String>) {
        self.store.committed_unless(known).await
    }

    pub async fn committed_changes(&self) -> watch::Receiver<u64> {
        self.store.committed_changes().await
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
                .0
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
                let request = serde_json::from_slice::<CompressedJsonWire>(bytes)
                    .map_err(|_| bad())?
                    .decode::<AppendEntriesRequest<RoomTypes>>()
                    .map_err(|_| bad())?;
                if request.entries.len() > 1 || request.vote.leader_id.node_id != source {
                    return Err(bad());
                }
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
                let request = serde_json::from_slice::<InstallSnapshotWire>(bytes)
                    .map_err(|_| bad())?
                    .decode()
                    .map_err(|_| bad())?;
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
