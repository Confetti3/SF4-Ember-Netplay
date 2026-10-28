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

mod entry;
mod latency_roster;
mod network;
mod pair_bound;
mod server_owned;
mod store;
mod view;
mod wire;
pub use entry::{AdminEntry, AuthorityClaim, Committed, ProbeReservation, Proposal, Receipt};
pub use network::{Method, Network, Peer, RpcTransport};
use network::*;
pub use pair_bound::committed_probe_pair_bound;
use pair_bound::*;
pub use store::Store;
use store::*;
use wire::*;
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

/// The error `Coordinator::propose` ends with when another proposal holds
/// this leader; `is_proposal_busy` tells it from the others.
#[derive(Debug)]
struct ProposalBusy;

impl std::fmt::Display for ProposalBusy {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("room proposal busy")
    }
}

impl std::error::Error for ProposalBusy {}

pub fn is_proposal_busy(error: &io::Error) -> bool {
    error
        .get_ref()
        .is_some_and(|inner| inner.is::<ProposalBusy>())
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
    /// Tests only: `new_owned` for a private node outside any room.
    #[cfg(test)]
    pub async fn new(incarnation: u64, transport: Arc<dyn RpcTransport>) -> io::Result<Self> {
        Self::new_for_room([0; 16], incarnation, transport).await
    }

    /// Tests only: `new_owned` for a private node of `room`.
    #[cfg(test)]
    pub async fn new_for_room(
        room: [u8; 16],
        incarnation: u64,
        transport: Arc<dyn RpcTransport>,
    ) -> io::Result<Self> {
        Self::new_owned(room, incarnation, transport, Ownership::Private).await
    }

    /// The coordinator of a node of `room` that takes part in it as
    /// `ownership`.
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
            .call(leader, node.addr.clone(), Method::Authority, vec![0])
            .await?;
        let claim: AuthorityClaim = serde_json::from_slice(&bytes)
            .map_err(|_| io::Error::other("invalid authority claim"))?;
        let revision = self.store.committed().await.revision;
        if !claim.is_leader_at(leader, metrics.current_term, revision) {
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
        let revision = self.store.committed().await.revision;
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
        let revision = self.store.committed().await.revision;
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

    /// The room's state machine: the committed checkpoint, the applied
    /// membership and the probe reservations, each read under its lock.
    pub fn store(&self) -> &Store {
        &self.store
    }

    pub async fn propose(&self, proposal: Proposal) -> io::Result<Receipt> {
        if !proposal.well_formed() {
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
                    Method::Propose,
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
            .map_err(|_| io::Error::other(ProposalBusy))?;
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

    pub async fn dispatch(&self, source: u64, method: Method, bytes: &[u8]) -> io::Result<Vec<u8>> {
        let bad = || io::Error::other("invalid room coordination request");
        if source == 0 || bytes.len() > MAX_SNAPSHOT {
            return Err(bad());
        }
        if !self.ownership.serves(source, method) {
            return Err(bad());
        }
        match method {
            Method::Propose => {
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
            Method::Authority => {
                if bytes != [0] {
                    return Err(bad());
                }
                serde_json::to_vec(&self.authority_claim().await?).map_err(|_| bad())
            }
            Method::Elect => {
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
            Method::Remove => {
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
            Method::Append => {
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
            Method::Vote => {
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
            Method::Snapshot => {
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
        }
    }
}

#[cfg(test)]
mod tests;
