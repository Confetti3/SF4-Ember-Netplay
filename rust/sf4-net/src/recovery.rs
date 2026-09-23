//! Live room-recovery coordination owned by the helper actor.
//!
//! The native process owns the room checkpoint and effect journal.  This
//! module owns the authenticated Raft admission route, incarnation fencing,
//! bounded checkpoint transfer, and the pre-match route probe.  Gameplay
//! sockets are deliberately outside this state machine and are never closed
//! when coordination is unavailable.

use std::{collections::BTreeSet, io, time::Duration};

use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD};
use iroh::{EndpointAddr, EndpointId};
use openraft::{BasicNode, ChangeMembers};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use tokio::task::JoinHandle;

use crate::{
    coordination::{AdminEntry, Committed, Coordinator, ProbeReservation, Proposal},
    coordination_iroh::IrohRpc,
};

pub const MAX_CHECKPOINT_BYTES: usize = crate::coordination::MAX_CHECKPOINT;
pub const CHECKPOINT_CHUNK_BYTES: usize = 16 * 1024;
pub const CHECKPOINT_WINDOW: usize = 4;
pub const MAX_VOTERS: usize = 5;
pub const PROBE_DURATION: Duration = Duration::from_secs(5);
pub const PROBE_INTERVAL: Duration = Duration::from_millis(50);
pub const PROBE_MIN_SAMPLES: u32 = 80;
/// A quorum read only authorizes native writes while it is fresh. Timing out
/// this read is therefore a safe negative result; it never says that a Raft
/// write did or did not commit.
pub const AUTHORITY_READ_TIMEOUT: Duration = Duration::from_secs(2);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RecoveryPhase {
    Starting,
    Learner,
    Voter,
    Writable,
    QuorumLost,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct AuthorityState {
    pub term: u64,
    pub revision: u64,
    pub incarnation: u64,
    pub leader: String,
    pub writable: bool,
    pub leader_local: bool,
    pub phase: String,
    pub applied_digest: String,
    pub voter_count: usize,
    pub learner_count: usize,
}

#[derive(Clone)]
pub struct RecoverySession {
    pub room: [u8; 16],
    pub incarnation: u64,
    pub rpc: std::sync::Arc<IrohRpc>,
    pub coordinator: std::sync::Arc<Coordinator>,
    pub coordination_endpoint: EndpointId,
    pub coordination_address: EndpointAddr,
    pub primary_endpoint: EndpointId,
    pub phase: RecoveryPhase,
    pub authority_digest: [u8; 32],
    serve_task: std::sync::Arc<tokio::sync::Mutex<Option<JoinHandle<()>>>>,
}

impl RecoverySession {
    /// A new helper process always receives a random Raft node identity.  A
    /// stopped process is therefore admitted as a learner instead of being
    /// allowed to reuse a stale vote.
    pub async fn host(
        room: [u8; 16],
        primary_endpoint: EndpointId,
        relay_only: bool,
    ) -> io::Result<Self> {
        let incarnation = fresh_incarnation();
        let rpc = IrohRpc::bind(room, incarnation, relay_only).await?;
        let coordinator =
            std::sync::Arc::new(Coordinator::new_for_room(room, incarnation, rpc.clone()).await?);
        coordinator
            .raft()
            .initialize(std::collections::BTreeMap::from([(
                incarnation,
                BasicNode::new(rpc.identity().to_string()),
            )]))
            .await
            .map_err(|_| io::Error::other("coordination bootstrap failed"))?;
        let task_rpc = rpc.clone();
        let task_coordinator = coordinator.clone();
        let serve_task = tokio::spawn(async move {
            let _ = task_rpc.serve(task_coordinator).await;
        });
        let session = Self {
            room,
            incarnation,
            coordination_endpoint: rpc.identity(),
            coordination_address: rpc.address(),
            rpc,
            coordinator,
            primary_endpoint,
            phase: RecoveryPhase::Starting,
            authority_digest: [0; 32],
            serve_task: std::sync::Arc::new(tokio::sync::Mutex::new(Some(serve_task))),
        };
        session.wait_for_local_leader().await?;
        Ok(session)
    }

    /// A join starts with a fresh learner and only the committed host route.
    /// The host later receives this process's coordination admission over the
    /// authenticated control stream and adds the learner through Raft.
    pub async fn join(
        room: [u8; 16],
        primary_endpoint: EndpointId,
        host_incarnation: u64,
        host_coordination: EndpointAddr,
        relay_only: bool,
    ) -> io::Result<Self> {
        if host_incarnation == 0 || room == [0; 16] {
            return Err(io::Error::other("invalid coordination admission"));
        }
        let incarnation = fresh_incarnation();
        let rpc = IrohRpc::bind(room, incarnation, relay_only).await?;
        rpc.admit(host_incarnation, host_coordination).await?;
        let coordinator =
            std::sync::Arc::new(Coordinator::new_for_room(room, incarnation, rpc.clone()).await?);
        let task_rpc = rpc.clone();
        let task_coordinator = coordinator.clone();
        let serve_task = tokio::spawn(async move {
            let _ = task_rpc.serve(task_coordinator).await;
        });
        Ok(Self {
            room,
            incarnation,
            coordination_endpoint: rpc.identity(),
            coordination_address: rpc.address(),
            rpc,
            coordinator,
            primary_endpoint,
            phase: RecoveryPhase::Learner,
            authority_digest: [0; 32],
            serve_task: std::sync::Arc::new(tokio::sync::Mutex::new(Some(serve_task))),
        })
    }

    async fn wait_for_local_leader(&self) -> io::Result<()> {
        self.coordinator
            .raft()
            .wait(Some(Duration::from_secs(5)))
            .current_leader(self.incarnation, "coordination bootstrap")
            .await
            .map_err(|_| io::Error::other("coordination leader unavailable"))?;
        Ok(())
    }

    pub async fn stop(&self) {
        self.rpc.close().await;
        if let Some(task) = self.serve_task.lock().await.take() {
            task.abort();
        }
        let _ = self.coordinator.raft().shutdown().await;
    }

    pub async fn advertise(&self) -> Admission {
        let authority_term = self
            .coordinator
            .raft()
            .metrics()
            .borrow()
            .current_term
            .max(1);
        Admission {
            room: self.room,
            incarnation: self.incarnation,
            authority_term,
            coordination_endpoint: self.coordination_endpoint,
            coordination_address: self.coordination_address.clone(),
            primary_endpoint: self.primary_endpoint,
        }
    }

    pub async fn admit(&self, admission: &Admission) -> io::Result<()> {
        let _membership = self.coordinator.membership_operations.lock().await;
        self.validate_admission_history(admission).await?;
        self.admit_unlocked(admission).await
    }

    async fn admit_unlocked(&self, admission: &Admission) -> io::Result<()> {
        if admission.room != self.room
            || admission.incarnation == 0
            || admission.primary_endpoint == self.primary_endpoint
        {
            return Err(io::Error::other("invalid room member binding"));
        }
        self.rpc
            .admit_bound(
                admission.incarnation,
                admission.coordination_address.clone(),
                admission.primary_endpoint,
            )
            .await
    }

    async fn validate_admission_history(&self, admission: &Admission) -> io::Result<()> {
        let (members, history) = self.coordinator.applied_membership_provenance().await;
        if history.contains(&admission.incarnation) && !members.contains(&admission.incarnation) {
            self.rpc.retire(admission.incarnation).await;
            return Err(io::Error::other("retired coordination incarnation"));
        }
        Ok(())
    }

    pub async fn add_learner(&self, admission: &Admission) -> io::Result<()> {
        let _membership = self.coordinator.membership_operations.lock().await;
        self.validate_admission_history(admission).await?;
        let (members, history) = self.coordinator.applied_membership_provenance().await;
        let retired = history.difference(&members).count();
        if !history.contains(&admission.incarnation)
            && (history.len() >= crate::coordination::MAX_MEMBER_HISTORY
                || retired >= crate::coordination::MAX_RETIRED_MEMBER_HISTORY)
        {
            return Err(io::Error::other("coordination membership history full"));
        }
        self.admit_unlocked(admission).await?;
        let node = BasicNode::new(admission.coordination_endpoint.to_string());
        let result = self
            .coordinator
            .raft()
            .add_learner(admission.incarnation, node, true)
            .await
            .map_err(|_| io::Error::other("coordination learner admission failed"));
        result?;
        Ok(())
    }

    pub async fn promote_voters(&self, voters: BTreeSet<u64>) -> io::Result<()> {
        if voters.is_empty() || voters.len() > MAX_VOTERS {
            return Err(io::Error::other("invalid stable voter set"));
        }
        let _membership = self.coordinator.membership_operations.lock().await;
        let applied = self.coordinator.applied_member_ids().await;
        if !voters.is_subset(&applied) {
            return Err(io::Error::other("stable voter is not an applied member"));
        }
        self.coordinator
            .raft()
            .change_membership(voters, true)
            .await
            .map_err(|_| io::Error::other("coordination membership change failed"))?;
        Ok(())
    }

    /// Promote only while the committed checkpoint is still `revision`, whose
    /// roster the caller chose `voters` from. The linearizable read applies
    /// every entry committed so far, including a previous leader's, and the
    /// proposal lock keeps this leader from committing a new roster before
    /// the membership change. Returns false when the roster moved on; the
    /// caller retries from the newer one.
    pub async fn promote_voters_at_revision(
        &self,
        revision: u64,
        voters: BTreeSet<u64>,
    ) -> io::Result<bool> {
        let _proposals = self.coordinator.proposals.lock().await;
        self.coordinator
            .raft()
            .ensure_linearizable()
            .await
            .map_err(|_| io::Error::other("room quorum unavailable"))?;
        if self.committed().await.revision != revision {
            return Ok(false);
        }
        self.promote_voters(voters).await?;
        Ok(true)
    }

    /// Remove departed process incarnations only after the native authority
    /// has committed a checkpoint whose authenticated member set excludes
    /// them. `retain=false` removes departed voters from the active quorum;
    /// learner-only tombstones are removed by `remove_nodes` after that
    /// quorum transition has committed.
    pub async fn remove_members(&self, voters: BTreeSet<u64>) -> io::Result<()> {
        if voters.is_empty() || voters.len() > MAX_VOTERS {
            return Err(io::Error::other("invalid replacement voter set"));
        }
        let _membership = self.coordinator.membership_operations.lock().await;
        self.coordinator
            .raft()
            .change_membership(voters, false)
            .await
            .map_err(|_| io::Error::other("coordination membership removal failed"))?;
        Ok(())
    }

    /// Remove learner-only incarnations after the old quorum has already
    /// committed the voter transition. OpenRaft's retain=false voter change
    /// deliberately leaves removed voters as learners, so a departed native
    /// learner needs this explicit second membership entry.
    pub async fn remove_nodes(&self, nodes: BTreeSet<u64>) -> io::Result<()> {
        if nodes.is_empty() {
            return Ok(());
        }
        let _membership = self.coordinator.membership_operations.lock().await;
        self.coordinator
            .raft()
            .change_membership(ChangeMembers::RemoveNodes(nodes), false)
            .await
            .map_err(|_| io::Error::other("coordination learner removal failed"))?;
        Ok(())
    }

    pub async fn request_self_removal(&self) -> io::Result<()> {
        let leader = self
            .coordinator
            .current_leader()
            .filter(|id| *id != self.incarnation)
            .ok_or_else(|| io::Error::other("coordination successor unavailable"))?;
        self.rpc.request_self_removal(leader).await
    }

    pub async fn state(&self) -> AuthorityState {
        let metrics = self.coordinator.raft().metrics().borrow().clone();
        let committed = self.coordinator.committed().await;
        let leader_id = metrics.current_leader.unwrap_or_default();
        let leader = metrics
            .membership_config
            .membership()
            .get_node(&leader_id)
            .map(|node| node.addr.clone())
            .unwrap_or_default();
        let leader_local = metrics.current_leader == Some(self.incarnation);
        // `writable` means the committed authority is healthy and this helper
        // has a current quorum proof. Native code still gates proposals and
        // control effects on `leader_local`; followers must be able to report
        // ready/recovering state after applying the same committed claim.
        // Route the leader's periodic health check through the same bounded,
        // exact-state authority claim served to followers. This coalesces one
        // linearizable barrier for the room instead of racing a second leader
        // barrier against every follower's authenticated claim after an
        // election. Followers still verify the claim over their own admitted
        // coordination route and compare it with their locally applied state.
        let quorum = if leader_local {
            tokio::time::timeout(AUTHORITY_READ_TIMEOUT, self.coordinator.authority_claim())
                .await
                .is_ok_and(|result| result.is_ok())
        } else {
            tokio::time::timeout(AUTHORITY_READ_TIMEOUT, self.coordinator.read_barrier())
                .await
                .is_ok_and(|result| result.is_ok())
        };
        let writable = quorum && metrics.current_leader.is_some();
        let mut digest = [0; 32];
        if !committed.checkpoint.is_empty() {
            digest = sha256(committed.checkpoint.as_bytes());
        }
        AuthorityState {
            term: metrics.current_term,
            revision: committed.revision,
            incarnation: self.incarnation,
            leader,
            writable,
            leader_local,
            phase: if writable { "writable" } else { "quorum_lost" }.into(),
            applied_digest: hex_digest(&digest),
            voter_count: metrics.membership_config.membership().voter_ids().count(),
            learner_count: metrics.membership_config.membership().learner_ids().count(),
        }
    }

    pub async fn committed(&self) -> Committed {
        self.coordinator.committed().await
    }

    pub fn voter_ids(&self) -> BTreeSet<u64> {
        self.coordinator.voter_ids()
    }

    pub async fn applied_voter_ids(&self) -> BTreeSet<u64> {
        self.coordinator.applied_voter_ids().await
    }

    pub async fn applied_member_ids(&self) -> BTreeSet<u64> {
        self.coordinator.applied_member_ids().await
    }

    pub async fn applied_membership_provenance(&self) -> (BTreeSet<u64>, BTreeSet<u64>) {
        self.coordinator.applied_membership_provenance().await
    }

    /// Confirm that a successor has installed the committed authority claim
    /// before this process closes its coordination and primary routes.
    pub async fn confirm_successor(
        &self,
        successor: u64,
        minimum_term: u64,
        minimum_revision: u64,
    ) -> io::Result<()> {
        let claim = self.rpc.authority_claim(successor).await?;
        if claim.incarnation != successor
            || claim.leader != Some(successor)
            || claim.term < minimum_term
            || claim.revision < minimum_revision
        {
            return Err(io::Error::other("successor authority claim not committed"));
        }
        Ok(())
    }

    /// A removed leader can retain a stale leader view. Query every remaining
    /// admitted voter so a slow follower cannot hide the elected successor.
    pub async fn confirm_available_successor(
        &self,
        minimum_term: u64,
        minimum_revision: u64,
    ) -> io::Result<()> {
        let mut candidates = self.applied_voter_ids().await;
        candidates.remove(&self.incarnation);
        let recovery = self.clone();
        first_confirmed_successor(candidates, move |successor| {
            let recovery = recovery.clone();
            async move {
                recovery
                    .confirm_successor(successor, minimum_term, minimum_revision)
                    .await
            }
        })
        .await
    }

    /// Confirm a follower's committed removal through the current leader.
    /// OpenRaft's retain=false membership change may stop replicating to the
    /// departing process before its local metrics learn that it is no longer
    /// a voter.  The authenticated leader claim is therefore the final proof
    /// used by a healthy non-leader departure.
    pub async fn confirm_departure(
        &self,
        minimum_term: u64,
        minimum_revision: u64,
    ) -> io::Result<()> {
        let successor = self
            .coordinator
            .current_leader()
            .filter(|id| *id != self.incarnation)
            .ok_or_else(|| io::Error::other("successor authority unavailable"))?;
        let claim = self.rpc.authority_claim(successor).await?;
        if claim.incarnation != successor
            || claim.leader != Some(successor)
            || claim.term < minimum_term
            || claim.revision < minimum_revision
            // An empty set is also rejected so a pre-membership/legacy
            // authority response cannot be mistaken for learner exclusion.
            || claim.members.is_empty()
            || claim.members.contains(&self.incarnation)
            || claim.voters.contains(&self.incarnation)
        {
            return Err(io::Error::other("departure remains in committed quorum"));
        }
        Ok(())
    }

    /// Confirm that this helper is the sole committed voter after its
    /// successor has already retired. This covers an explicit final room
    /// teardown where two healthy helpers issue Leave together: the former
    /// leader remains available for a short authority-read grace window, but
    /// there is no successor quorum left to confirm a second departure.
    pub async fn confirm_singleton_departure(
        &self,
        minimum_term: u64,
        minimum_revision: u64,
    ) -> io::Result<()> {
        let successor = self
            .coordinator
            .current_leader()
            .filter(|id| *id != self.incarnation)
            .or_else(|| {
                self.voter_ids()
                    .into_iter()
                    .find(|id| *id != self.incarnation)
            })
            .ok_or_else(|| io::Error::other("former authority route unavailable"))?;
        let claim = self.rpc.authority_claim(successor).await?;
        if claim.term < minimum_term
            || claim.revision < minimum_revision
            || claim.leader != Some(self.incarnation)
            || claim.voters.len() != 1
            || !claim.voters.contains(&self.incarnation)
        {
            return Err(io::Error::other("singleton authority not committed"));
        }
        Ok(())
    }

    pub fn propose(&self, transfer: &CheckpointTransfer) -> io::Result<Proposal> {
        if transfer.room != self.room
            || transfer.bytes.len() > MAX_CHECKPOINT_BYTES
            || transfer.term == 0
            || transfer.revision != transfer.base_revision.saturating_add(1)
        {
            return Err(io::Error::other("invalid checkpoint proposal"));
        }
        let checkpoint = String::from_utf8(transfer.bytes.clone())
            .map_err(|_| io::Error::other("checkpoint must be UTF-8 JSON"))?;
        Ok(Proposal {
            // Preserve the native SessionProposal request as the Raft request
            // identity. Followers use this exact value when replaying the
            // committed checkpoint back to their C++ owner.
            request: transfer.transfer.to_string(),
            dedup_id: format!(
                "native:{}:{}:{}",
                self.incarnation, transfer.term, transfer.transfer
            ),
            term: transfer.term,
            base: transfer.base_revision,
            checkpoint,
            admin: None,
        })
    }

    pub async fn reserve_probe(
        &self,
        source_incarnation: u64,
        target_incarnation: u64,
        request: u64,
        pair_revision: u64,
        term: u64,
        expires: u64,
    ) -> io::Result<()> {
        if source_incarnation == 0
            || target_incarnation == 0
            || request == 0
            || term == 0
            || expires == 0
        {
            return Err(io::Error::other("invalid probe reservation"));
        }
        let receipt = self
            .coordinator
            .propose(Proposal {
                request: format!("probe:{source_incarnation}:{target_incarnation}:{request}"),
                dedup_id: format!(
                    "probe:{}:{}:{}:{}",
                    source_incarnation, target_incarnation, term, request
                ),
                term,
                base: self.committed().await.revision,
                checkpoint: String::new(),
                admin: Some(AdminEntry::ProbeReservation(ProbeReservation {
                    room: self.room,
                    source_incarnation,
                    target_incarnation,
                    request,
                    pair_revision,
                    term,
                    expires,
                })),
            })
            .await?;
        if !receipt.accepted {
            return Err(io::Error::other("probe reservation not committed"));
        }
        Ok(())
    }

    pub async fn probe_reserved(
        &self,
        source_incarnation: u64,
        target_incarnation: u64,
        request: u64,
        pair_revision: u64,
        term: u64,
        expires: u64,
    ) -> bool {
        self.coordinator
            .probe_reserved(
                self.room,
                source_incarnation,
                target_incarnation,
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
        source_endpoint: EndpointId,
        target_endpoint: EndpointId,
        pair_revision: u64,
    ) -> bool {
        self.coordinator
            .probe_pair_bound(
                source_incarnation,
                target_incarnation,
                &source_endpoint.to_string(),
                &target_endpoint.to_string(),
                pair_revision,
            )
            .await
    }
}

impl Drop for RecoverySession {
    fn drop(&mut self) {
        if std::sync::Arc::strong_count(&self.serve_task) == 1
            && let Ok(mut guard) = self.serve_task.try_lock()
            && let Some(task) = guard.take()
        {
            task.abort();
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Admission {
    pub room: [u8; 16],
    pub incarnation: u64,
    #[serde(default)]
    pub authority_term: u64,
    pub coordination_endpoint: EndpointId,
    pub coordination_address: EndpointAddr,
    pub primary_endpoint: EndpointId,
}

#[derive(Clone, Debug)]
pub struct CheckpointTransfer {
    pub room: [u8; 16],
    pub transfer: u64,
    pub term: u64,
    pub base_revision: u64,
    pub revision: u64,
    pub bytes: Vec<u8>,
    pub digest: [u8; 32],
}

impl CheckpointTransfer {
    pub fn new(
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        bytes: Vec<u8>,
    ) -> io::Result<Self> {
        if bytes.is_empty() || bytes.len() > MAX_CHECKPOINT_BYTES {
            return Err(io::Error::other("checkpoint size out of bounds"));
        }
        Ok(Self {
            room,
            transfer,
            term,
            base_revision,
            revision,
            digest: sha256(&bytes),
            bytes,
        })
    }
    pub fn digest_hex(&self) -> String {
        hex_digest(&self.digest)
    }
    pub fn encode_chunk(&self, offset: usize) -> io::Result<String> {
        let end = offset
            .checked_add(CHECKPOINT_CHUNK_BYTES)
            .unwrap_or(self.bytes.len())
            .min(self.bytes.len());
        if offset >= end || offset > self.bytes.len() {
            return Err(io::Error::other("invalid checkpoint chunk offset"));
        }
        Ok(URL_SAFE_NO_PAD.encode(&self.bytes[offset..end]))
    }
}

#[derive(Debug)]
pub struct IncomingTransfer {
    pub room: [u8; 16],
    pub transfer: u64,
    pub term: u64,
    pub base_revision: u64,
    pub revision: u64,
    pub length: usize,
    pub digest: [u8; 32],
    pub bytes: Vec<u8>,
    pub started: tokio::time::Instant,
}

impl IncomingTransfer {
    pub(crate) fn validate_begin(
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        length: usize,
        digest: &str,
    ) -> io::Result<[u8; 32]> {
        if room == [0; 16]
            || transfer == 0
            || term == 0
            || revision != base_revision.saturating_add(1)
            || !(1..=MAX_CHECKPOINT_BYTES).contains(&length)
        {
            return Err(io::Error::other("invalid checkpoint transfer"));
        }
        parse_digest(digest)
    }

    pub fn begin(
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        length: usize,
        digest: &str,
    ) -> io::Result<Self> {
        let digest = Self::validate_begin(
            room,
            transfer,
            term,
            base_revision,
            revision,
            length,
            digest,
        )?;
        Ok(Self {
            room,
            transfer,
            term,
            base_revision,
            revision,
            length,
            digest,
            bytes: Vec::with_capacity(length),
            started: tokio::time::Instant::now(),
        })
    }
    pub fn push(&mut self, offset: usize, encoded: &str) -> io::Result<usize> {
        if encoded.len() > 24 * 1024 {
            return Err(io::Error::other("checkpoint chunks must be contiguous"));
        }
        let decoded = URL_SAFE_NO_PAD
            .decode(encoded)
            .map_err(|_| io::Error::other("invalid checkpoint base64"))?;
        let end = offset
            .checked_add(decoded.len())
            .ok_or_else(|| io::Error::other("checkpoint chunk exceeds bounds"))?;
        if decoded.is_empty() || decoded.len() > CHECKPOINT_CHUNK_BYTES || end > self.length {
            return Err(io::Error::other("checkpoint chunk exceeds bounds"));
        }
        if offset == self.bytes.len() {
            self.bytes.extend_from_slice(&decoded);
        } else if offset > self.bytes.len()
            || end > self.bytes.len()
            || self.bytes[offset..end] != decoded
        {
            return Err(io::Error::other("checkpoint chunks must be contiguous"));
        }
        // Exact retries acknowledge only the replayed chunk end. Returning the
        // retained body's full offset would violate the sender's four-credit
        // in-flight fence when the checkpoint spans more than one window.
        Ok(end)
    }
    pub fn finish(self) -> io::Result<CheckpointTransfer> {
        if self.bytes.len() != self.length || sha256(&self.bytes) != self.digest {
            return Err(io::Error::other("checkpoint digest mismatch"));
        }
        CheckpointTransfer::new(
            self.room,
            self.transfer,
            self.term,
            self.base_revision,
            self.revision,
            self.bytes,
        )
    }
}

pub fn stable_voter_count(member_count: usize) -> usize {
    match member_count {
        0 | 1 => 1,
        2 => 2,
        3 | 4 => 3,
        _ => MAX_VOTERS,
    }
}

pub fn recommended_delay(p95_rtt_us: u64) -> u8 {
    let frames = (p95_rtt_us as f64 / 1000.0 / (2.0 * 16.6667)).ceil() as i64 - 2;
    frames.clamp(0, 10) as u8
}

pub fn sha256(bytes: &[u8]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(bytes);
    hasher.finalize().into()
}

pub fn hex_digest(bytes: &[u8; 32]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn parse_digest(value: &str) -> io::Result<[u8; 32]> {
    if value.len() != 64 || !value.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err(io::Error::other("invalid SHA-256 digest"));
    }
    let mut digest = [0; 32];
    let (pairs, remainder) = value.as_bytes().as_chunks::<2>();
    debug_assert!(remainder.is_empty());
    for (index, pair) in pairs.iter().enumerate() {
        digest[index] = u8::from_str_radix(std::str::from_utf8(pair).unwrap_or(""), 16)
            .map_err(|_| io::Error::other("invalid SHA-256 digest"))?;
    }
    Ok(digest)
}

fn fresh_incarnation() -> u64 {
    loop {
        let value = rand::random::<u64>();
        if value != 0 {
            return value;
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ProbeResult {
    pub sample_count: u32,
    pub loss_count: u32,
    pub p95_rtt_us: u64,
    pub recommended_delay: u8,
    pub status: String,
}

pub fn summarize_probe(samples_us: &[u64], duration: Duration) -> ProbeResult {
    let mut sorted = samples_us.to_vec();
    sorted.sort_unstable();
    let sample_count = sorted.len() as u32;
    let expected = (duration.as_millis() / PROBE_INTERVAL.as_millis()) as u32;
    let loss_count = expected.saturating_sub(sample_count);
    let p95 = if sorted.is_empty() {
        0
    } else {
        let index = ((sorted.len() - 1) * 95).div_ceil(100);
        sorted[index]
    };
    ProbeResult {
        sample_count,
        loss_count,
        p95_rtt_us: p95,
        recommended_delay: recommended_delay(p95),
        status: if sample_count >= expected * 4 / 5 {
            "ready".into()
        } else {
            "unavailable".into()
        },
    }
}

async fn first_confirmed_successor<F, Fut>(candidates: BTreeSet<u64>, confirm: F) -> io::Result<()>
where
    F: Fn(u64) -> Fut,
    Fut: std::future::Future<Output = io::Result<()>> + Send + 'static,
{
    let mut claims = tokio::task::JoinSet::new();
    for candidate in candidates {
        claims.spawn(confirm(candidate));
    }
    while let Some(result) = claims.join_next().await {
        if matches!(result, Ok(Ok(()))) {
            return Ok(());
        }
    }
    Err(io::Error::other("successor authority unavailable"))
}

/// Separate absent replies from packets the local scheduler never sent.
pub fn summarize_datagram_probe(samples_us: &[u64], expected: u32, sent: u32) -> ProbeResult {
    let mut result = summarize_probe(samples_us, Duration::from_millis(u64::from(expected) * 50));
    result.loss_count = sent.saturating_sub(result.sample_count);
    if expected == 0 {
        result.status = "unavailable".into();
    } else if sent < expected {
        result.status = "local_overload".into();
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn elected_successor_is_not_hidden_by_a_stalled_first_voter() {
        tokio::time::timeout(
            Duration::from_millis(200),
            first_confirmed_successor(BTreeSet::from([1, 2, 3]), |candidate| async move {
                match candidate {
                    1 => std::future::pending().await,
                    2 => Err(io::Error::other("not the elected leader")),
                    _ => Ok(()),
                }
            }),
        )
        .await
        .expect("first voter blocked the elected successor")
        .unwrap();
        assert!(
            first_confirmed_successor(BTreeSet::from([1, 2]), |_| async {
                Err(io::Error::other("no quorum claim"))
            })
            .await
            .is_err()
        );
        assert!(
            first_confirmed_successor(BTreeSet::new(), |_| async { Ok(()) })
                .await
                .is_err()
        );
    }

    #[test]
    fn unsent_probe_packets_are_not_reported_as_network_loss() {
        assert_eq!(summarize_datagram_probe(&[], 0, 0).status, "unavailable");
        let result = summarize_datagram_probe(&[1_000; 60], 100, 75);
        assert_eq!(result.loss_count, 15);
        assert_eq!(result.status, "local_overload");
    }

    #[test]
    fn long_probe_requires_eighty_percent_of_the_long_workload() {
        let poor = summarize_probe(&[1_000; 100], Duration::from_secs(30));
        assert_eq!(poor.status, "unavailable");
        assert_eq!(poor.loss_count, 500);
        let enough = summarize_probe(&[1_000; 480], Duration::from_secs(30));
        assert_eq!(enough.status, "ready");
        assert_eq!(enough.loss_count, 120);
    }

    #[test]
    fn stable_voters_skip_four_and_cap_at_five() {
        assert_eq!(stable_voter_count(0), 1);
        assert_eq!(stable_voter_count(1), 1);
        assert_eq!(stable_voter_count(2), 2);
        assert_eq!(stable_voter_count(3), 3);
        assert_eq!(stable_voter_count(4), 3);
        assert_eq!(stable_voter_count(16), 5);
    }

    #[test]
    fn transfer_is_contiguous_and_sha256_bound() {
        let bytes = vec![9; CHECKPOINT_CHUNK_BYTES + 3];
        let source = CheckpointTransfer::new([4; 16], 2, 7, 8, 9, bytes).unwrap();
        let mut incoming = IncomingTransfer::begin(
            source.room,
            source.transfer,
            source.term,
            source.base_revision,
            source.revision,
            source.bytes.len(),
            &source.digest_hex(),
        )
        .unwrap();
        let first = source.encode_chunk(0).unwrap();
        assert_eq!(incoming.push(0, &first).unwrap(), CHECKPOINT_CHUNK_BYTES);
        assert_eq!(
            incoming.push(0, &first).unwrap(),
            CHECKPOINT_CHUNK_BYTES,
            "an exact retained chunk returns only its own end offset"
        );
        assert!(
            incoming
                .push(0, &URL_SAFE_NO_PAD.encode(vec![8; CHECKPOINT_CHUNK_BYTES]))
                .is_err(),
            "a changed retry must not receive credit for retained bytes"
        );
        assert_eq!(
            incoming
                .push(
                    CHECKPOINT_CHUNK_BYTES,
                    &source.encode_chunk(CHECKPOINT_CHUNK_BYTES).unwrap(),
                )
                .unwrap(),
            source.bytes.len()
        );
        assert_eq!(incoming.finish().unwrap().bytes, source.bytes);
    }

    #[test]
    fn probe_summary_enforces_sample_loss_and_delay_boundaries() {
        let below = summarize_probe(&[1_000; 79], PROBE_DURATION);
        assert_eq!(below.sample_count, 79);
        assert_eq!(below.loss_count, 21);
        assert_eq!(below.status, "unavailable");
        assert_eq!(below.recommended_delay, 0);

        let threshold = summarize_probe(&[66_667; 80], PROBE_DURATION);
        assert_eq!(threshold.sample_count, 80);
        assert_eq!(threshold.loss_count, 20);
        assert_eq!(threshold.status, "ready");
        assert_eq!(threshold.recommended_delay, 1);

        let complete = summarize_probe(&[400_000; 100], PROBE_DURATION);
        assert_eq!(complete.sample_count, 100);
        assert_eq!(complete.loss_count, 0);
        assert_eq!(complete.status, "ready");
        assert_eq!(complete.recommended_delay, 10);

        assert_eq!(recommended_delay(66_666), 0);
        assert_eq!(recommended_delay(66_667), 1);
        assert_eq!(recommended_delay(366_667), 9);
        assert_eq!(recommended_delay(366_668), 10);
        assert_eq!(recommended_delay(u64::MAX), 10);
    }
}
