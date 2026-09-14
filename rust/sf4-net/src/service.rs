//! Helper actor: process-local IPC owns rooms and match mappings. The actor
//! routes opaque application messages; C++ SessionServer owns all lobby rules.
use std::{
    collections::{BTreeMap, BTreeSet, VecDeque},
    io,
    net::{Ipv4Addr, SocketAddr},
    sync::{Arc, atomic::Ordering},
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD};
use iroh::{
    Endpoint, EndpointAddr, EndpointId,
    endpoint::{Connection, RecvStream, SendStream},
};
use serde::{Deserialize, Serialize};
use tokio::{
    io::{AsyncRead, AsyncWrite, AsyncWriteExt},
    sync::{mpsc, watch},
    task::{AbortHandle, JoinSet},
    time::{Instant, interval, timeout},
};

use crate::{
    bridge::{Bridge, BridgeStats},
    control::ControlWorker,
    invite::Invite,
    recovery::{
        self, Admission, AuthorityState, CHECKPOINT_CHUNK_BYTES, CHECKPOINT_WINDOW,
        CheckpointTransfer, IncomingTransfer,
    },
    transport::{self, ControlChannel, GameAuthorization, GameConnection},
    wire::{self, CONTROL_ALPN, ControlFrame, GAME_ALPN, MAX_CONTROL_PAYLOAD, MatchKey},
};

// A room update may fan out to fifteen controls while match lifecycle events
// are arriving. Keep this burst bounded without coupling room size to failure.
pub const IPC_QUEUE_CAPACITY: usize = 128;
/// A room has one local endpoint and at most fifteen remote members.  Keep
/// control admission and gameplay authorization separate: a room member may
/// lose its control stream while an already-authorized gameplay link remains
/// active, and the two limits must not consume one another's budget.
pub const MAX_CONTROL_PEERS: usize = 15;
pub const MAX_GAME_LINKS: usize = 15;
/// JoinSet workers are bounded independently of the number of room members.
/// The budget covers one pending accept/dial per remote member, all active
/// gameplay bridges, and a small amount of room lifecycle headroom.  Control
/// reader/writer workers are bounded per connection by ControlWorker and are
/// intentionally not detached from the actor's shutdown scope.
pub const MAX_TASKS: usize = MAX_CONTROL_PEERS + MAX_GAME_LINKS + 8;
const CONTROL_POLL_BUDGET: usize = 20;
const LIFECYCLE_EVENT_RESERVE: usize = 8;
const INVITE_LIFETIME: u64 = 3600;
const CHECKPOINT_TRANSFER_TIMEOUT: Duration = Duration::from_secs(15);
const COORDINATION_ADMISSION_TIMEOUT: Duration = Duration::from_secs(12);
/// A single authority read can time out while a relay room is admitting a
/// burst of members. Freeze native mutations immediately, but only ask one
/// deterministic surviving voter to campaign after the same committed
/// leader has remained unwritable for a full election window. This stays
/// ahead of the native replacement-room offer at fifteen seconds.
const RECOVERY_ELECTION_GRACE: Duration = Duration::from_secs(10);
// Retired process incarnations are retained as exact provenance until the
// room closes. Once this bounded tombstone budget is exhausted, admission of
// another incarnation fails closed rather than allowing an old ID to be
// forgotten and replayed on a lagging replica.
const MAX_RETIRED_INCARNATIONS: usize = crate::coordination::MAX_RETIRED_MEMBER_HISTORY;
// One monotonic wire counter covers coordination and native messages. The
// native logical message ID is carried in `NativeControlMessage` so C++ can
// preserve its IPC correlation without sharing the QUIC frame counter.
const TRANSPORT_MESSAGE_ID_BASE: u64 = 2;

// No Debug derive: invitations and match capabilities must not reach logs.
#[derive(Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case", deny_unknown_fields)]
pub enum Command {
    Status,
    Host {
        epoch: u64,
        build: String,
    },
    Join {
        epoch: u64,
        invitation: String,
        build: String,
    },
    Leave {
        epoch: u64,
        /// Replacement-room teardown may retire local state without trying
        /// to mutate the old Raft membership. Normal departure remains the
        /// quorum-confirmed path when this is false.
        #[serde(default)]
        abandon: bool,
    },
    Send {
        epoch: u64,
        peer: EndpointId,
        message_id: u64,
        payload: String,
    },
    CloseControl {
        epoch: u64,
        peer: EndpointId,
    },
    PrepareGame {
        epoch: u64,
        peer: EndpointId,
        room: [u8; 16],
        generation: u64,
        capability: [u8; 32],
        local_port: u16,
        max_packet: usize,
        dial: bool,
    },
    EndMatch {
        epoch: u64,
        generation: u64,
    },
    EndPeer {
        epoch: u64,
        peer: EndpointId,
        generation: u64,
    },
    CheckpointBegin {
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        length: u32,
        digest: String,
    },
    CheckpointChunk {
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        offset: u32,
        data: String,
    },
    CheckpointEnd {
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        length: u32,
        digest: String,
    },
    CheckpointAck {
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        offset: u32,
    },
    ProbeRequest {
        epoch: u64,
        room: [u8; 16],
        peer: EndpointId,
        request: u64,
        pair_revision: u64,
        #[serde(default)]
        benchmark: bool,
    },
    Shutdown,
}

#[derive(Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case", deny_unknown_fields)]
pub enum Event {
    DiscordInvite {
        epoch: u64,
        invitation: String,
        secret: String,
    },
    CoordinationState {
        epoch: u64,
        room: [u8; 16],
        term: u64,
        revision: u64,
        incarnation: u64,
        leader: String,
        writable: bool,
        leader_local: bool,
        voter_count: usize,
        learner_count: usize,
    },
    CheckpointBegin {
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        length: u32,
        digest: String,
    },
    CheckpointChunk {
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        offset: u32,
        data: String,
    },
    CheckpointEnd {
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        length: u32,
        digest: String,
    },
    CheckpointAck {
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        offset: u32,
    },
    CheckpointCommitted {
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        length: u32,
        digest: String,
    },
    ControlRebound {
        epoch: u64,
        room: [u8; 16],
        leader: String,
        members: Vec<String>,
        member_incarnations: BTreeMap<String, u64>,
    },
    ProbeResult {
        epoch: u64,
        room: [u8; 16],
        peer: EndpointId,
        request: u64,
        pair_revision: u64,
        route: String,
        status: String,
        sample_count: u32,
        loss_count: u32,
        p95_rtt_us: u64,
        recommended_delay: i16,
        #[serde(default)]
        metrics: crate::probe::Metrics,
    },
    Status {
        request_id: u64,
        endpoint: EndpointId,
        ip_transports: usize,
        epoch: u64,
        peers: usize,
        games: usize,
    },
    Hosted {
        epoch: u64,
        invitation: String,
        room: [u8; 16],
    },
    Connected {
        epoch: u64,
        peer: EndpointId,
        room: [u8; 16],
    },
    Message {
        epoch: u64,
        peer: EndpointId,
        message_id: u64,
        payload: String,
    },
    ControlClosed {
        epoch: u64,
        peer: EndpointId,
    },
    Sent {
        request_id: u64,
        epoch: u64,
        message_id: u64,
    },
    GameWaiting {
        epoch: u64,
        peer: EndpointId,
        generation: u64,
    },
    GameReady {
        epoch: u64,
        peer: EndpointId,
        generation: u64,
        virtual_port: u16,
        max_packet: usize,
        /// The selected Iroh path at the moment the gameplay bridge is
        /// authorized. This is observational metadata; the committed probe
        /// connection is upgraded in place before GGPO owns its datagrams.
        route: String,
    },
    GameFailed {
        epoch: u64,
        peer: EndpointId,
        generation: u64,
        reason: crate::bridge::Failure,
        route: String,
        max_packet: usize,
        max_datagram: usize,
    },
    GameClosed {
        epoch: u64,
        peer: EndpointId,
        generation: u64,
    },
    Statistics {
        epoch: u64,
        peer: EndpointId,
        generation: u64,
        sent_packets: u64,
        received_packets: u64,
        sent_bytes: u64,
        received_bytes: u64,
        rejected_packets: u64,
        congestion_events: u64,
        local_drops: u64,
        route: String,
    },
    RoomClosed {
        epoch: u64,
    },
    Error {
        request_id: u64,
        epoch: u64,
        #[serde(skip_serializing_if = "Option::is_none")]
        probe_failure: Option<u8>,
        #[serde(skip_serializing_if = "Option::is_none")]
        peer: Option<EndpointId>,
        code: String,
    },
    Stopped,
}

struct Request {
    id: u64,
    command: Command,
}

/// Native C++ payloads are JSON text. Serializing that text as a JSON
/// `String` doubles every quote and can push an otherwise valid near-limit
/// message over the 64 KiB control frame bound. Keep the public Rust field a
/// String for the existing IPC API, but emit valid JSON payloads as a
/// RawValue so the wrapper adds only its small envelope overhead.
struct NativeControlMessage {
    kind: String,
    message_id: u64,
    payload: String,
}

impl Serialize for NativeControlMessage {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        use serde::ser::SerializeStruct;
        let mut state = serializer.serialize_struct("NativeControlMessage", 3)?;
        state.serialize_field("type", &self.kind)?;
        state.serialize_field("message_id", &self.message_id)?;
        if let Ok(raw) = serde_json::value::RawValue::from_string(self.payload.clone()) {
            state.serialize_field("payload", &raw)?;
        } else {
            state.serialize_field("payload", &self.payload)?;
        }
        state.end()
    }
}

impl<'de> Deserialize<'de> for NativeControlMessage {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        #[derive(Deserialize)]
        #[serde(deny_unknown_fields)]
        struct Wire {
            #[serde(rename = "type")]
            kind: String,
            message_id: u64,
            payload: Box<serde_json::value::RawValue>,
        }
        let wire = Wire::deserialize(deserializer)?;
        let encoded = wire.payload.get();
        let payload = if encoded.starts_with('"') {
            serde_json::from_str(encoded).map_err(serde::de::Error::custom)?
        } else {
            encoded.to_owned()
        };
        Ok(Self {
            kind: wire.kind,
            message_id: wire.message_id,
            payload,
        })
    }
}
struct IncomingGame {
    epoch: u64,
    peer: EndpointId,
    connection: Connection,
    generation: Option<u64>,
    probe: Option<(u64, u64)>,
    deadline: tokio::time::Instant,
}
enum Completion {
    GuestControl(u64, Invite, io::Result<ControlChannel>),
    Incoming(u64, io::Result<Connection>),
    ClassifiedGame(IncomingGame, io::Result<transport::GameStream>),
    Hosted(u64, io::Result<Invite>),
    Control(u64, io::Result<ControlChannel>),
    MemberControl(u64, u64, io::Result<ControlChannel>),
    Reconnect(u64, EndpointId, io::Result<ControlChannel>),
    Game(u64, EndpointId, u64, io::Result<GameConnection>),
    BridgeEnded(u64, EndpointId, u64, Option<crate::bridge::Failure>),
    CheckpointProposal(
        CheckpointProposalKey,
        CheckpointTransfer,
        io::Result<crate::coordination::Receipt>,
    ),
    CoordinationRefresh(CoordinationRefreshKey, CoordinationRefresh),
    MembershipReconciliation(
        MembershipOperationKey,
        io::Result<MembershipOperationResult>,
    ),
    Admission(AdmissionOperationKey, io::Result<AdmissionOperationResult>),
    ProbeAuthorization(ProbeAuthorizationKey, io::Result<ProbeAuthorization>),
    Probe(u64, io::Result<ProbeCompletion>),
}

#[derive(Clone, PartialEq, Eq)]
struct CheckpointProposalKey {
    epoch: u64,
    room: [u8; 16],
    incarnation: u64,
    transfer: u64,
    term: u64,
    base_revision: u64,
    revision: u64,
    length: usize,
    digest: [u8; 32],
}

#[derive(Clone, PartialEq, Eq)]
struct CoordinationRefreshKey {
    sequence: u64,
    epoch: u64,
    room: [u8; 16],
    incarnation: u64,
    term: u64,
    leader: Option<u64>,
}

struct CoordinationRefresh {
    state: AuthorityState,
    leader: Option<u64>,
    applied_members: BTreeSet<u64>,
    applied_history: BTreeSet<u64>,
    retired: BTreeSet<u64>,
    committed: crate::coordination::Committed,
}

#[derive(Clone, PartialEq, Eq)]
struct MembershipOperationKey {
    sequence: u64,
    epoch: u64,
    room: [u8; 16],
    incarnation: u64,
    term: u64,
    revision: u64,
}

struct MembershipOperationResult {
    confirmed_retirements: BTreeSet<u64>,
}

#[derive(Clone, PartialEq, Eq)]
struct AdmissionOperationKey {
    sequence: u64,
    epoch: u64,
    room: [u8; 16],
    incarnation: u64,
    term: u64,
    leader: Option<u64>,
    peer: EndpointId,
    fingerprint: Vec<u8>,
    add_member_if_leader: bool,
}

struct AdmissionOperationResult {
    admissions: Vec<Admission>,
}

struct DeferredAdmission {
    peer: EndpointId,
    admissions: Vec<Admission>,
    add_member_if_leader: bool,
    fingerprint: Vec<u8>,
}

#[derive(Clone, PartialEq, Eq)]
struct ProbeAuthorizationKey {
    epoch: u64,
    room: [u8; 16],
    incarnation: u64,
    peer: EndpointId,
    target_incarnation: u64,
    request: u64,
    pair_revision: u64,
    benchmark: bool,
}

struct ProbeAuthorization {
    term: u64,
    leader: Option<u64>,
    revision: u64,
    expires: u64,
}

struct GameSlot {
    auth: GameAuthorization,
    local_port: u16,
    waiting: bool,
    task: Option<AbortHandle>,
    stats: Option<Arc<BridgeStats>>,
    route_connection: Option<Connection>,
    expires: tokio::time::Instant,
    prepare_deadline: Option<watch::Sender<tokio::time::Instant>>,
}

struct OutgoingCheckpoint {
    transfer: CheckpointTransfer,
    next_offset: usize,
    in_flight: BTreeSet<u32>,
    acked_offset: u32,
    end_sent: bool,
    begin_sent: bool,
    started: tokio::time::Instant,
}

#[derive(Clone)]
struct PendingCheckpointCommitted {
    epoch: u64,
    room: [u8; 16],
    transfer: u64,
    term: u64,
    base_revision: u64,
    revision: u64,
    length: u32,
    digest: String,
}

struct ProbeReservation {
    reported: bool,
    connection: Connection,
    request: u64,
    pair_revision: u64,
    route: String,
}

#[derive(Clone)]
struct PendingProbeInvalidation {
    request: u64,
    pair_revision: u64,
    route: String,
}

struct ProbePermission {
    request: u64,
    pair_revision: u64,
    expires: tokio::time::Instant,
}

#[derive(Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case", deny_unknown_fields)]
enum CoordinationControl {
    Admission {
        admission: Admission,
    },
    Membership {
        admissions: Vec<Admission>,
        /// Incarnations whose committed Raft membership no longer contains
        /// the process. This explicit tombstone lets coordination-only
        /// departures revoke every replica even when no native checkpoint
        /// roster ever contained the member.
        #[serde(default)]
        retired: Vec<u64>,
    },
    ProbeReservation {
        room: [u8; 16],
        /// The primary endpoint and process incarnation on the sending side.
        source: EndpointId,
        source_incarnation: u64,
        /// The admitted process incarnation that must accept the reservation.
        target_incarnation: u64,
        request: u64,
        pair_revision: u64,
        term: u64,
        expires: u64,
    },
}

struct ProbeCompletion {
    peer: EndpointId,
    request: u64,
    pair_revision: u64,
    samples_us: Vec<u64>,
    connection: Option<Connection>,
    report: bool,
    route_changed: bool,
    metrics: crate::probe::Metrics,
}
impl Drop for GameSlot {
    fn drop(&mut self) {
        if let Some(task) = &self.task {
            task.abort();
        }
    }
}

fn failed(code: &'static str) -> io::Error {
    io::Error::other(code)
}
fn now() -> io::Result<u64> {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|v| v.as_secs())
        .map_err(|_| failed("clock"))
}

/// Extract the primary Iroh identities from the committed native
/// `SessionProposal` envelope. The C++ side binds these values from its
/// authenticated room-member map; Rust only uses a complete, validated list
/// to reconcile Raft membership after commit.
fn committed_primary_endpoints(bytes: &[u8]) -> Option<BTreeSet<EndpointId>> {
    let envelope: serde_json::Value = serde_json::from_slice(bytes).ok()?;
    let members = envelope.get("checkpoint")?.get("members")?.as_array()?;
    // A recovery checkpoint may retain started spectator tombstones in
    // addition to the sixteen active room members. Bound the total separately
    // from the active roster so a valid 16-member room plus frozen entries is
    // still reconcilable without allowing an unbounded snapshot projection.
    const MAX_FROZEN_MEMBERS: usize = 4 * (MAX_CONTROL_PEERS + 1);
    if members.is_empty() || members.len() > MAX_CONTROL_PEERS + 1 + MAX_FROZEN_MEMBERS {
        return None;
    }
    let mut endpoints = BTreeSet::new();
    let mut active = 0usize;
    for member in members {
        if member.get("frozen").and_then(serde_json::Value::as_bool) == Some(true) {
            continue;
        }
        active += 1;
        if active > MAX_CONTROL_PEERS + 1 {
            return None;
        }
        let data = member.get("data")?;
        let text = data
            .get("authenticatedEndpoint")
            .or_else(|| data.get("authenticated_endpoint"))?
            .as_str()?;
        let endpoint = text.parse::<EndpointId>().ok()?;
        endpoints.insert(endpoint);
    }
    (!endpoints.is_empty()).then_some(endpoints)
}

/// Build the native endpoint-to-incarnation view from the membership this
/// replica has actually applied. Historical admissions can temporarily retain
/// both a retired and a fresh process incarnation for the same stable Iroh
/// endpoint. Publishing every remembered admission would make numeric sort
/// order choose one at random. An ambiguous applied endpoint stays absent
/// until joint consensus and retirement leave one exact incarnation.
fn applied_member_incarnations(
    admissions: &BTreeMap<u64, Admission>,
    applied: &BTreeSet<u64>,
    retiring: &BTreeSet<u64>,
) -> BTreeMap<String, u64> {
    let mut candidates = BTreeMap::<String, Option<u64>>::new();
    for (incarnation, admission) in admissions {
        if !applied.contains(incarnation) || retiring.contains(incarnation) {
            continue;
        }
        candidates
            .entry(admission.primary_endpoint.to_string())
            .and_modify(|current| {
                if *current != Some(*incarnation) {
                    *current = None;
                }
            })
            .or_insert(Some(*incarnation));
    }
    candidates
        .into_iter()
        .filter_map(|(endpoint, incarnation)| incarnation.map(|value| (endpoint, value)))
        .collect()
}

struct Actor {
    endpoint: Endpoint,
    relay_only: bool,
    epoch: u64,
    opening: bool,
    room: Option<[u8; 16]>,
    hosted: Option<Invite>,
    room_invite: Option<Invite>,
    host_address: Option<EndpointAddr>,
    controls: BTreeMap<EndpointId, ControlWorker>,
    games: BTreeMap<EndpointId, GameSlot>,
    closed_generation: u64,
    tasks: JoinSet<Completion>,
    events: mpsc::Sender<Event>,
    recovery: Option<crate::recovery::RecoverySession>,
    admissions: BTreeMap<u64, Admission>,
    admission_order: Vec<u64>,
    /// Incarnations that this actor has observed in its own applied Raft
    /// membership.  Comparing this provenance with the next applied
    /// membership lets a successor derive a retirement even if the former
    /// leader died after committing RemoveNodes but before broadcasting its
    /// local tombstone.
    applied_admission_members: BTreeSet<u64>,
    incoming_transfer: Option<IncomingTransfer>,
    pending_checkpoint_proposal: Option<CheckpointProposalKey>,
    /// An exact native retry of a fully validated proposal needs only a
    /// contiguous cursor. The original body is already owned by the pending
    /// Raft task, so retries never allocate a second near-1 MiB buffer.
    pending_checkpoint_retry: Option<(CheckpointProposalKey, usize)>,
    outgoing_transfer: Option<OutgoingCheckpoint>,
    /// Last native roster that was committed by C++. Admissions arriving
    /// after that checkpoint are not treated as departures until a later
    /// checkpoint explicitly includes them first.
    committed_native_members: Option<BTreeSet<EndpointId>>,
    /// Incarnations omitted by a committed native roster or an authenticated
    /// membership update. Keep these IDs until the local applied Raft
    /// membership proves their exclusion; a leader can die after the native
    /// checkpoint but before its removal entry commits.
    pending_retired_incarnations: BTreeSet<u64>,
    /// Durable retirement evidence broadcast to every later control join.
    /// Pending IDs are removed after local applied-membership proof, but this
    /// set remains so a follower that was disconnected during the transition
    /// cannot reactivate the old incarnation with a replayed Admission.
    retired_incarnations: BTreeSet<u64>,
    last_exported_revision: u64,
    pending_checkpoint_ack: Option<(u64, [u8; 16], u64, u32)>,
    pending_checkpoint_committed: Option<PendingCheckpointCommitted>,
    next_transport_message: u64,
    next_coordination_operation: u64,
    pending_coordination_refresh: Option<CoordinationRefreshKey>,
    pending_membership_operation: Option<MembershipOperationKey>,
    pending_admission_operation: Option<AdmissionOperationKey>,
    deferred_admissions: VecDeque<DeferredAdmission>,
    pending_membership_publications: BTreeSet<EndpointId>,
    last_coordination_state: Option<(u64, u64, bool, bool)>,
    coordination_writable: bool,
    last_control_rebound: Option<(String, Vec<String>, BTreeMap<String, u64>)>,
    unwritable_leader_since: Option<(u64, u64, Instant)>,
    /// At most one committed-leader control rebind may be in flight. Without
    /// this fence a follower that has learned a new leader but has not yet
    /// connected would launch one ten-second dial per state tick.
    reconnect_target: Option<EndpointId>,
    probe_reservations: BTreeMap<EndpointId, ProbeReservation>,
    pending_game_admissions: BTreeMap<EndpointId, (Connection, tokio::task::AbortHandle)>,
    /// Invalidations are retained until the bounded native event queue has
    /// room. Otherwise a closed or migrated reserved connection could leave
    /// native code recommending a route that can no longer be upgraded.
    pending_probe_invalidations: BTreeMap<EndpointId, PendingProbeInvalidation>,
    // A GAME_ALPN connection is a probe reservation until the native side
    // commits gameplay authorization.  This fence prevents a racing
    // PrepareGame accept from consuming the probe handshake.
    probe_peers: BTreeSet<EndpointId>,
    probe_permissions: BTreeMap<EndpointId, ProbePermission>,
    pending_probe_authorizations: BTreeMap<EndpointId, ProbeAuthorizationKey>,
    /// A departing Raft leader keeps its authority RPC alive briefly after
    /// emitting room_closed. A simultaneous follower Leave can then obtain
    /// the committed voter-set proof before the leader's process shuts down.
    retirement_started: Option<Instant>,
}

impl Actor {
    fn remember_admission(&mut self, admission: Admission) {
        let incarnation = admission.incarnation;
        if self.retired_incarnations.contains(&incarnation)
            || self.pending_retired_incarnations.contains(&incarnation)
        {
            return;
        }
        self.admissions.insert(incarnation, admission);
        if !self.admission_order.contains(&incarnation) {
            self.admission_order.push(incarnation);
        }
    }

    #[cfg(test)]
    fn stable_voters(&self, current: BTreeSet<u64>, desired: usize) -> BTreeSet<u64> {
        let mut voters: BTreeSet<_> = current
            .into_iter()
            .filter(|id| {
                self.admissions.contains_key(id)
                    && !self.pending_retired_incarnations.contains(id)
                    && !self.retired_incarnations.contains(id)
            })
            .collect();
        for id in self
            .admission_order
            .iter()
            .copied()
            .chain(self.admissions.keys().copied())
        {
            if voters.len() >= desired {
                break;
            }
            if self.admissions.contains_key(&id)
                && !self.pending_retired_incarnations.contains(&id)
                && !self.retired_incarnations.contains(&id)
            {
                voters.insert(id);
            }
        }
        voters
    }

    fn apply_confirmed_retirements(&mut self, confirmed: BTreeSet<u64>) {
        for incarnation in confirmed {
            // Keep the exact retirement fence bounded. Once the room has
            // accumulated the configured number of historical incarnations,
            // leave this departure pending and fail closed: the RPC binding
            // is already read-only, and evicting the ID from the pending set
            // would let a lagging peer replay the old Admission after a later
            // control reconnect.
            self.admissions.remove(&incarnation);
            self.admission_order.retain(|id| *id != incarnation);
            if !self.retired_incarnations.contains(&incarnation)
                && self.retired_incarnations.len() >= MAX_RETIRED_INCARNATIONS
            {
                continue;
            }
            self.retired_incarnations.insert(incarnation);
            self.pending_retired_incarnations.remove(&incarnation);
        }
    }

    #[cfg(test)]
    async fn apply_pending_retirements(&mut self) -> io::Result<()> {
        let Some(recovery) = self.recovery.clone() else {
            return Ok(());
        };
        let applied = recovery.applied_member_ids().await;
        let confirmed: Vec<u64> = self
            .pending_retired_incarnations
            .iter()
            .copied()
            .filter(|incarnation| !applied.contains(incarnation))
            .collect();
        for incarnation in &confirmed {
            recovery.rpc.retire(*incarnation).await;
        }
        self.apply_confirmed_retirements(confirmed.into_iter().collect());
        Ok(())
    }

    fn emit(&self, event: Event) -> io::Result<()> {
        self.events
            .try_send(event)
            .map_err(|_| failed("IPC event queue full or closed"))
    }
    fn error(&self, request_id: u64, code: &str) -> io::Result<()> {
        self.emit(Event::Error {
            probe_failure: None,
            request_id,
            epoch: self.epoch,
            peer: None,
            code: code.into(),
        })
    }

    // Additive, allowlisted diagnostics: keep the existing error code so older
    // native clients still treat a rejected probe as a nonfatal operation.
    fn probe_error(&self, reason: u8) -> io::Result<()> {
        self.emit(Event::Error {
            request_id: 0,
            epoch: self.epoch,
            peer: None,
            code: "probe_unavailable".into(),
            probe_failure: Some(reason),
        })
    }

    fn peer_error(&self, request_id: u64, peer: EndpointId, code: &str) -> io::Result<()> {
        self.emit(Event::Error {
            probe_failure: None,
            request_id,
            epoch: self.epoch,
            peer: Some(peer),
            code: code.into(),
        })
    }

    fn emit_bulk(&self, event: Event) -> bool {
        if self.events.capacity() <= LIFECYCLE_EVENT_RESERVE {
            return false;
        }
        self.events.try_send(event).is_ok()
    }

    async fn setup_host_recovery(&mut self, invite: Invite) -> io::Result<Invite> {
        let session = crate::recovery::RecoverySession::host(
            invite.room(),
            self.endpoint.id(),
            self.relay_only,
        )
        .await?;
        let authority = session.state().await;
        let invite = invite.with_authority(
            session.coordination_endpoint,
            authority.term.max(1),
            session.incarnation,
        )?;
        let own = session.advertise().await;
        self.remember_admission(own);
        self.recovery = Some(session);
        Ok(invite)
    }

    async fn setup_join_recovery(&mut self, invite: &Invite) -> io::Result<()> {
        let coordination_address = invite
            .coordination_address()
            .ok_or_else(|| failed("invitation has no coordination route"))?;
        let session = crate::recovery::RecoverySession::join(
            invite.room(),
            self.endpoint.id(),
            invite.authority_incarnation(),
            coordination_address,
            self.relay_only,
        )
        .await?;
        let own = session.advertise().await;
        self.remember_admission(own);
        self.remember_admission(Admission {
            room: invite.room(),
            incarnation: invite.authority_incarnation(),
            authority_term: invite.authority_term(),
            coordination_endpoint: invite
                .coordination_endpoint()
                .ok_or_else(|| failed("invitation coordination identity missing"))?,
            coordination_address: invite
                .coordination_address()
                .ok_or_else(|| failed("invitation coordination route missing"))?,
            primary_endpoint: invite.endpoint(),
        });
        self.recovery = Some(session);
        Ok(())
    }

    async fn emit_coordination_state(&mut self) -> io::Result<()> {
        if self.retirement_started.is_some() {
            return Ok(());
        }
        let Some(recovery) = self.recovery.clone() else {
            self.coordination_writable = false;
            return Ok(());
        };
        if self.pending_coordination_refresh.is_some() || self.tasks.len() >= MAX_TASKS {
            return Ok(());
        }
        let Some(room) = self.room else {
            return Ok(());
        };
        let sequence = self.next_coordination_operation;
        self.next_coordination_operation = self.next_coordination_operation.saturating_add(1);
        let key = CoordinationRefreshKey {
            sequence,
            epoch: self.epoch,
            room,
            incarnation: recovery.incarnation,
            term: recovery.coordinator.current_term(),
            leader: recovery.coordinator.current_leader(),
        };
        let retired_candidates: Vec<u64> = self
            .admissions
            .keys()
            .copied()
            .filter(|incarnation| *incarnation != recovery.incarnation)
            .collect();
        self.pending_coordination_refresh = Some(key.clone());
        self.tasks.spawn(async move {
            let (applied_members, applied_history) = recovery.applied_membership_provenance().await;
            let mut retired = BTreeSet::new();
            for incarnation in retired_candidates {
                if recovery.rpc.is_retired(incarnation).await {
                    retired.insert(incarnation);
                }
            }
            let state = recovery.state().await;
            let committed = recovery.committed().await;
            let leader = recovery.coordinator.current_leader();
            Completion::CoordinationRefresh(
                key,
                CoordinationRefresh {
                    state,
                    leader,
                    applied_members,
                    applied_history,
                    retired,
                    committed,
                },
            )
        });
        Ok(())
    }

    fn apply_coordination_refresh(&mut self, refresh: CoordinationRefresh) -> io::Result<()> {
        let Some(recovery) = self.recovery.clone() else {
            return Ok(());
        };
        let CoordinationRefresh {
            mut state,
            applied_members,
            applied_history,
            retired,
            committed,
            ..
        } = refresh;
        // Derive retirement provenance from the locally applied membership as
        // well as the explicit native/membership tombstone path. This closes
        // the leader-loss window without waiting in the IPC actor.
        let departed = self
            .applied_admission_members
            .iter()
            .chain(applied_history.iter())
            .copied()
            .filter(|incarnation| {
                *incarnation != recovery.incarnation && !applied_members.contains(incarnation)
            });
        self.pending_retired_incarnations.extend(departed);
        self.pending_retired_incarnations.extend(retired);
        self.applied_admission_members
            .extend(applied_history.iter().copied());
        let retired_for_broadcast = self.pending_retired_incarnations.clone();
        if let Some(retained) = committed_primary_endpoints(committed.checkpoint.as_bytes()) {
            self.schedule_membership_reconciliation(retained, state.term, committed.revision);
        } else {
            self.schedule_pending_membership_reconciliation(state.term, committed.revision);
        }
        let became_local_leader = state.leader_local
            && self
                .last_coordination_state
                .is_none_or(|marker| !marker.3 || marker.0 != state.term);
        if state.leader_local && (!retired_for_broadcast.is_empty() || became_local_leader) {
            self.queue_membership_publication();
        }
        // A relay dial or learner catch-up can fail after the authenticated
        // Admission control frame has been consumed. Keep that binding private
        // until it appears in applied membership and retry it from each fresh
        // leader read; otherwise one transient add_learner failure strands the
        // native join forever on an otherwise healthy control connection.
        if state.leader_local {
            let unapplied = self
                .admissions
                .values()
                .filter(|admission| {
                    admission.incarnation != recovery.incarnation
                        && !applied_members.contains(&admission.incarnation)
                        && !applied_history.contains(&admission.incarnation)
                        && !retired_for_broadcast.contains(&admission.incarnation)
                        && !self.retired_incarnations.contains(&admission.incarnation)
                        && self.controls.contains_key(&admission.primary_endpoint)
                })
                .cloned()
                .collect::<Vec<_>>();
            for admission in unapplied {
                if self
                    .queue_admission_operation(admission.primary_endpoint, vec![admission], true)
                    .is_err()
                {
                    break;
                }
            }
        }
        let leader_coord = state.leader.clone();
        let leader_admission = self
            .admissions
            .values()
            .find(|admission| admission.coordination_endpoint.to_string() == leader_coord)
            .cloned();
        let leader = leader_admission
            .as_ref()
            .map(|admission| admission.primary_endpoint.to_string())
            .unwrap_or_default();
        state.leader = leader.clone();
        // Raft may report a leader before this helper has received its
        // authenticated primary endpoint binding. Never leak the coordination
        // endpoint as a gameplay route or advertise native writability until
        // the committed leader is routable.
        if state.leader.is_empty() {
            state.writable = false;
        }
        self.coordination_writable = state.writable;
        // Refresh the compact/full invitation only after the new authority is
        // visible through the committed Raft leader binding. This keeps old
        // leader references from being used for a future join while retaining
        // the room capability and expiry.
        if let Some(admission) = leader_admission.as_ref()
            && let Some(current) = self.room_invite.clone()
            && (current.endpoint() != admission.primary_endpoint
                || current.authority_term() != state.term
                || current.authority_incarnation() != admission.incarnation)
            && let Ok(updated) = current.with_authority_route(
                admission.primary_endpoint,
                admission.coordination_endpoint,
                state.term.max(1),
                admission.incarnation,
            )
            && let (Ok(invitation), Ok(secret)) = (updated.encode(), updated.encode_discord())
            && self.emit_bulk(Event::DiscordInvite {
                epoch: self.epoch,
                invitation,
                secret,
            })
        {
            self.room_invite = Some(updated.clone());
            if self.hosted.is_some() {
                self.hosted = Some(updated);
            }
        }
        let marker = (
            state.term,
            state.revision,
            state.writable,
            state.leader_local,
        );
        if self.last_coordination_state != Some(marker) {
            if !self.emit_bulk(Event::CoordinationState {
                epoch: self.epoch,
                room: recovery.room,
                term: state.term,
                revision: state.revision,
                incarnation: state.incarnation,
                leader: state.leader.clone(),
                writable: state.writable,
                leader_local: state.leader_local,
                voter_count: state.voter_count,
                learner_count: state.learner_count,
            }) {
                return Ok(());
            }
            self.last_coordination_state = Some(marker);
        }
        let connected: BTreeSet<String> = self
            .controls
            .keys()
            .map(ToString::to_string)
            .chain(std::iter::once(self.endpoint.id().to_string()))
            .collect();
        let member_incarnations =
            applied_member_incarnations(&self.admissions, &applied_members, &retired_for_broadcast);
        let members = member_incarnations
            .keys()
            .filter(|endpoint| connected.contains(*endpoint))
            .cloned()
            .collect::<Vec<_>>();
        // ControlRebound is a routing event. Defer it while a new leader is
        // only present in admissions; the native router must retain its live
        // endpoint until the replacement control socket is ready.
        let leader_ready = !state.leader.is_empty()
            && (state.leader == self.endpoint.id().to_string()
                || self
                    .controls
                    .keys()
                    .any(|peer| peer.to_string() == state.leader));
        if state.leader.is_empty() {
            return Ok(());
        }
        if !leader_ready {
            if let Some(admission) = leader_admission {
                self.reconnect_control(admission.primary_endpoint);
            }
            return Ok(());
        }
        let rebound_marker = (
            state.leader.clone(),
            members.clone(),
            member_incarnations.clone(),
        );
        // ControlRebound is an idempotent native routing snapshot.  Repeat
        // the current snapshot on the one-second coordination cadence even
        // when the marker is unchanged: a joining native owner may not have
        // installed its IPC reader when the first snapshot was emitted, and
        // caching that send would strand it in Joining forever.  Bulk queue
        // reserve still bounds retries during checkpoint pressure.
        if !self.emit_bulk(Event::ControlRebound {
            epoch: self.epoch,
            room: recovery.room,
            leader: state.leader,
            members,
            member_incarnations,
        }) {
            return Ok(());
        }
        self.last_control_rebound = Some(rebound_marker);
        Ok(())
    }

    fn send_coordination_control(&mut self, peer: EndpointId) {
        let Some(recovery) = self.recovery.clone() else {
            return;
        };
        let leader_local = recovery.coordinator.current_leader() == Some(recovery.incarnation);
        let own = self.admissions.values().find(|admission| {
            admission.primary_endpoint == self.endpoint.id()
                || admission.coordination_endpoint == recovery.coordination_endpoint
        });
        let Some(own) = own else {
            return;
        };
        let message = CoordinationControl::Admission {
            admission: own.clone(),
        };
        let Ok(payload) = serde_json::to_string(&message) else {
            return;
        };
        let id = self.next_transport_message;
        self.next_transport_message = self.next_transport_message.saturating_add(1);
        if let Some(control) = self.controls.get(&peer) {
            let _ = control.try_send(ControlFrame {
                message_id: id,
                payload: payload.into_bytes(),
            });
        }
        // A fresh control route may have missed an earlier retirement
        // broadcast.  Replay the durable tombstone set only from the
        // committed leader; followers must not impersonate membership
        // authority while they establish their reciprocal Admission.
        if leader_local {
            self.pending_membership_publications.insert(peer);
            self.pump_membership_publications();
        }
    }

    fn control_rejoin_member(&self, peer: EndpointId) -> Option<u64> {
        if !self
            .committed_native_members
            .as_ref()
            .is_some_and(|members| members.contains(&peer))
        {
            return None;
        }
        self.admissions
            .values()
            .find(|admission| {
                self.room == Some(admission.room)
                    && admission.primary_endpoint == peer
                    && admission.incarnation != 0
                    && !self.retired_incarnations.contains(&admission.incarnation)
                    && !self
                        .pending_retired_incarnations
                        .contains(&admission.incarnation)
            })
            .map(|admission| admission.incarnation)
    }

    fn reconnect_control(&mut self, peer: EndpointId) {
        if self.reconnect_target.is_some()
            || self.controls.contains_key(&peer)
            || self.tasks.len() >= MAX_TASKS
            || self.room.is_none()
        {
            return;
        }
        let Some(invite) = self.room_invite.clone() else {
            return;
        };
        let target = self.current_leader_primary().unwrap_or(peer);
        if target == self.endpoint.id() || self.controls.contains_key(&target) {
            return;
        }
        let endpoint = self.endpoint.clone();
        let epoch = self.epoch;
        let address = invite
            .address()
            .relay_urls()
            .next()
            .cloned()
            .map(|relay| EndpointAddr::new(target).with_relay_url(relay))
            .unwrap_or_else(|| EndpointAddr::new(target));
        self.reconnect_target = Some(target);
        self.tasks.spawn(async move {
            tokio::time::sleep(Duration::from_millis(250)).await;
            Completion::Reconnect(
                epoch,
                target,
                transport::connect_control_to(&endpoint, address, &invite).await,
            )
        });
    }

    /// A replacement control from the same admitted endpoint is safe only
    /// after the previous transport has actually closed.  The bounded worker
    /// is removed on the next poll tick in the usual case; this helper closes
    /// the small race where a reconnect arrives between transport completion
    /// and that tick.  An open worker is never replaced, preserving the
    /// endpoint-to-incarnation binding and its message ordering.
    fn remove_closed_control(&mut self, peer: EndpointId) {
        if self
            .controls
            .get(&peer)
            .is_some_and(|control| control.is_closed())
        {
            self.controls.remove(&peer);
        }
    }

    fn current_leader_primary(&self) -> Option<EndpointId> {
        let recovery = self.recovery.as_ref()?;
        let leader = recovery.coordinator.current_leader()?;
        self.admissions
            .get(&leader)
            .map(|admission| admission.primary_endpoint)
    }

    /// Ask one deterministic surviving applied voter to campaign after a
    /// sustained failure of the committed leader. OpenRaft still requires the
    /// existing committed quorum before authority can change.
    async fn trigger_recovery_election_for_leader(&self, lost_leader: u64) {
        let Some(recovery) = self.recovery.as_ref() else {
            return;
        };
        if lost_leader == recovery.incarnation
            || recovery.coordinator.current_leader() != Some(lost_leader)
        {
            return;
        }
        let voters = recovery.applied_voter_ids().await;
        let candidate = voters.into_iter().find(|incarnation| {
            *incarnation != lost_leader
                && !self.pending_retired_incarnations.contains(incarnation)
                && !self.retired_incarnations.contains(incarnation)
        });
        if candidate == Some(recovery.incarnation) {
            let _ = recovery.coordinator.raft().trigger().elect().await;
        }
    }

    fn send_membership_control(
        &mut self,
        peer: EndpointId,
        retired_before_retry: &BTreeSet<u64>,
    ) -> bool {
        let mut retired = retired_before_retry.clone();
        retired.extend(self.pending_retired_incarnations.iter().copied());
        retired.extend(self.retired_incarnations.iter().copied());
        let admissions = self
            .admissions
            .values()
            .filter(|admission| !retired.contains(&admission.incarnation))
            .cloned()
            .collect();
        let Ok(payload) = serde_json::to_string(&CoordinationControl::Membership {
            admissions,
            retired: retired.into_iter().collect(),
        }) else {
            return false;
        };
        let id = self.next_transport_message;
        let sent = self.controls.get(&peer).is_some_and(|control| {
            control
                .try_send(ControlFrame {
                    message_id: id,
                    payload: payload.into_bytes(),
                })
                .is_ok()
        });
        if sent {
            self.next_transport_message = self.next_transport_message.saturating_add(1);
        }
        sent
    }

    fn queue_membership_publication(&mut self) {
        self.pending_membership_publications
            .extend(self.controls.keys().copied());
        self.pump_membership_publications();
    }

    fn pump_membership_publications(&mut self) {
        let leader_local = self.recovery.as_ref().is_some_and(|recovery| {
            recovery.coordinator.current_leader() == Some(recovery.incarnation)
        });
        if !leader_local {
            // A retained send belongs to the authority that queued it. After
            // an election, drop that work rather than impersonating the new
            // leader; a local-leader refresh queues a current full snapshot.
            self.pending_membership_publications.clear();
            return;
        }
        let peers = self
            .pending_membership_publications
            .iter()
            .copied()
            .collect::<Vec<_>>();
        for peer in peers {
            if !self.controls.contains_key(&peer) {
                self.pending_membership_publications.remove(&peer);
                continue;
            }
            if self.send_membership_control(peer, &BTreeSet::new()) {
                self.pending_membership_publications.remove(&peer);
            }
        }
    }

    fn send_probe_reservation(
        &mut self,
        peer: EndpointId,
        room: [u8; 16],
        request: u64,
        pair_revision: u64,
        term: u64,
        expires: u64,
    ) {
        let Some((source_incarnation, target_incarnation)) = self
            .recovery
            .as_ref()
            .map(|recovery| recovery.incarnation)
            .zip(
                self.admissions
                    .values()
                    .find(|admission| admission.primary_endpoint == peer)
                    .map(|admission| admission.incarnation),
            )
        else {
            return;
        };
        let Ok(payload) = serde_json::to_string(&CoordinationControl::ProbeReservation {
            room,
            source: self.endpoint.id(),
            source_incarnation,
            target_incarnation,
            request,
            pair_revision,
            term,
            expires,
        }) else {
            return;
        };
        let id = self.next_transport_message;
        self.next_transport_message = self.next_transport_message.saturating_add(1);
        if let Some(control) = self.controls.get(&peer) {
            let _ = control.try_send(ControlFrame {
                message_id: id,
                payload: payload.into_bytes(),
            });
        }
    }

    async fn accept_coordination_control(
        &mut self,
        peer: EndpointId,
        payload: &str,
    ) -> io::Result<bool> {
        let Ok(message) = serde_json::from_str::<CoordinationControl>(payload) else {
            return Ok(false);
        };
        match message {
            CoordinationControl::Admission { admission } => {
                let Some(recovery) = self.recovery.clone() else {
                    // sf4e2/emd2 carries only the authenticated primary room
                    // ticket. The host's Admission response on that same
                    // control stream is the authority lookup: it supplies the
                    // committed coordination endpoint and process incarnation
                    // without asking C++ to know an Iroh identity.
                    let Some(invite) = self.room_invite.clone() else {
                        return Err(failed("coordination admission unavailable"));
                    };
                    if admission.room != invite.room()
                        || admission.room != self.room.unwrap_or([0; 16])
                        || admission.primary_endpoint != peer
                        || admission.coordination_endpoint == self.endpoint.id()
                        || admission.incarnation == 0
                        || admission.authority_term == 0
                    {
                        return Err(failed("invalid coordination authority response"));
                    }
                    let session = crate::recovery::RecoverySession::join(
                        admission.room,
                        self.endpoint.id(),
                        admission.incarnation,
                        admission.coordination_address.clone(),
                        self.relay_only,
                    )
                    .await?;
                    self.remember_admission(admission.clone());
                    self.remember_admission(session.advertise().await);
                    self.recovery = Some(session);
                    self.last_coordination_state = None;
                    self.last_control_rebound = None;
                    // Complete the reciprocal authenticated binding now that
                    // the guest has a live coordination endpoint.
                    self.send_coordination_control(peer);
                    self.emit_coordination_state().await?;
                    return Ok(true);
                };
                if admission.room != recovery.room || admission.primary_endpoint != peer {
                    return Err(failed("invalid coordination member binding"));
                }
                if self.retired_incarnations.contains(&admission.incarnation)
                    || self
                        .pending_retired_incarnations
                        .contains(&admission.incarnation)
                    || (!self.admissions.contains_key(&admission.incarnation)
                        && self.retired_incarnations.len() >= MAX_RETIRED_INCARNATIONS)
                {
                    return Err(failed("retired coordination incarnation"));
                }
                let (applied, applied_history) = recovery.applied_membership_provenance().await;
                if admission.incarnation != recovery.incarnation
                    && !applied.contains(&admission.incarnation)
                    && (self
                        .applied_admission_members
                        .contains(&admission.incarnation)
                        || applied_history.contains(&admission.incarnation))
                {
                    // The actor has previously observed this incarnation in
                    // applied membership, so an Admission after its applied
                    // exclusion is a replay even if the former leader died
                    // before its local tombstone broadcast.
                    self.pending_retired_incarnations
                        .insert(admission.incarnation);
                    return Err(failed("retired coordination incarnation"));
                }
                let historical_retirements = applied_history.difference(&applied).count();
                if !applied_history.contains(&admission.incarnation)
                    && (applied_history.len() >= crate::coordination::MAX_MEMBER_HISTORY
                        || historical_retirements >= MAX_RETIRED_INCARNATIONS)
                {
                    return Err(failed("coordination membership history full"));
                }
                let add_and_promote =
                    recovery.coordinator.current_leader() == Some(recovery.incarnation);
                self.remember_admission(admission.clone());
                self.queue_admission_operation(peer, vec![admission], add_and_promote)?;
                Ok(true)
            }
            CoordinationControl::Membership {
                admissions,
                retired,
            } => {
                let Some(recovery) = self.recovery.clone() else {
                    return Ok(false);
                };
                let (applied, applied_history) = recovery.applied_membership_provenance().await;
                let leader_primary = if let Some(leader) = recovery.coordinator.current_leader() {
                    self.admissions
                        .get(&leader)
                        .map(|admission| admission.primary_endpoint)
                } else if applied.is_empty() && applied_history.is_empty() {
                    // A fresh learner has no local leader until its first Raft
                    // membership arrives. During only that empty bootstrap
                    // state, trust the authority cryptographically bound by
                    // the room invitation and authenticated primary control.
                    self.room_invite
                        .as_ref()
                        .and_then(|invite| {
                            self.admissions.values().find(|admission| {
                                admission.room == recovery.room
                                    && admission.primary_endpoint == invite.endpoint()
                                    && (invite.authority_incarnation() == 0
                                        || admission.incarnation == invite.authority_incarnation())
                            })
                        })
                        .map(|admission| admission.primary_endpoint)
                } else {
                    None
                };
                if leader_primary != Some(peer) {
                    return Err(failed("membership authority is not current leader"));
                }
                for incarnation in retired {
                    if incarnation != recovery.incarnation {
                        if self.retired_incarnations.len() < MAX_RETIRED_INCARNATIONS {
                            self.retired_incarnations.insert(incarnation);
                        }
                        self.pending_retired_incarnations.insert(incarnation);
                    }
                }
                let historical_retirements = applied_history.difference(&applied).count();
                let mut accepted = Vec::new();
                for admission in admissions {
                    if admission.room != recovery.room
                        || admission.primary_endpoint == self.endpoint.id()
                        || self.retired_incarnations.contains(&admission.incarnation)
                        || self
                            .pending_retired_incarnations
                            .contains(&admission.incarnation)
                    {
                        continue;
                    }
                    if applied_history.contains(&admission.incarnation)
                        && !applied.contains(&admission.incarnation)
                    {
                        self.pending_retired_incarnations
                            .insert(admission.incarnation);
                        continue;
                    }
                    if !applied_history.contains(&admission.incarnation)
                        && (applied_history.len() >= crate::coordination::MAX_MEMBER_HISTORY
                            || historical_retirements >= MAX_RETIRED_INCARNATIONS)
                    {
                        return Err(failed("coordination membership history full"));
                    }
                    accepted.push(admission);
                }
                self.last_coordination_state = None;
                self.last_control_rebound = None;
                self.emit_coordination_state().await?;
                self.queue_admission_operation(peer, accepted, false)?;
                Ok(true)
            }
            CoordinationControl::ProbeReservation {
                room,
                source: claimed_source,
                source_incarnation,
                target_incarnation,
                request,
                pair_revision,
                term,
                expires,
            } => {
                let Some(recovery) = self.recovery.clone() else {
                    return Ok(false);
                };
                let state = recovery.state().await;
                if room != recovery.room
                    || claimed_source != peer
                    || request == 0
                    || state.term != term
                    || !state.writable
                    || expires < now().unwrap_or(u64::MAX)
                    || !self.admissions.values().any(|admission| {
                        admission.room == room
                            && admission.primary_endpoint == peer
                            && admission.incarnation == source_incarnation
                    })
                    || !self.admissions.values().any(|admission| {
                        admission.room == room
                            && admission.primary_endpoint == self.endpoint.id()
                            && admission.incarnation == target_incarnation
                    })
                    || !recovery
                        .probe_pair_bound(
                            source_incarnation,
                            target_incarnation,
                            peer,
                            self.endpoint.id(),
                            pair_revision,
                        )
                        .await
                {
                    return Err(failed("invalid probe reservation"));
                }
                let applied = timeout(Duration::from_secs(5), async {
                    loop {
                        if recovery
                            .probe_reserved(
                                source_incarnation,
                                target_incarnation,
                                request,
                                pair_revision,
                                term,
                                expires,
                            )
                            .await
                        {
                            break true;
                        }
                        tokio::time::sleep(Duration::from_millis(25)).await;
                    }
                })
                .await
                .unwrap_or(false);
                if !applied {
                    return Err(failed("probe reservation not applied"));
                }
                self.probe_permissions.insert(
                    peer,
                    ProbePermission {
                        request,
                        pair_revision,
                        expires: tokio::time::Instant::now()
                            + Duration::from_secs(expires.saturating_sub(now().unwrap_or(expires))),
                    },
                );
                self.probe_peers.insert(peer);
                Ok(true)
            }
        }
    }

    fn start_outgoing_checkpoint(&mut self, transfer: CheckpointTransfer) {
        // One native receiver owns one reassembly buffer. Never replace an
        // export already in flight; the newly committed revision remains in
        // the replicated state machine and the watcher starts it after the
        // current export completes or times out.
        if self.outgoing_transfer.is_some() {
            return;
        }
        self.outgoing_transfer = Some(OutgoingCheckpoint {
            transfer,
            next_offset: 0,
            in_flight: BTreeSet::new(),
            acked_offset: 0,
            end_sent: false,
            begin_sent: false,
            started: tokio::time::Instant::now(),
        });
        // A completed transfer is not visible to native code until its marker
        // arrives. Preserve revision order when a new commit becomes available
        // while that marker is waiting for IPC capacity.
        self.pump_pending_checkpoint_committed();
        self.pump_outgoing_checkpoint();
    }

    #[allow(clippy::too_many_arguments)]
    async fn checkpoint_begin(
        &mut self,
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        length: u32,
        digest: String,
    ) -> io::Result<()> {
        if !self.matches(epoch) || self.room != Some(room) {
            return self.error(0, "checkpoint_transfer_busy");
        }
        if self.outgoing_transfer.is_some() {
            return self.error(0, "checkpoint_transfer_busy_outgoing");
        }
        let candidate_digest = IncomingTransfer::validate_begin(
            room,
            transfer,
            term,
            base_revision,
            revision,
            length as usize,
            &digest,
        )?;
        if let Some(pending) = self.pending_checkpoint_proposal.as_ref() {
            let exact_retry = pending.epoch == epoch
                && pending.room == room
                && pending.transfer == transfer
                && pending.term == term
                && pending.base_revision == base_revision
                && pending.revision == revision
                && pending.length == length as usize
                && pending.digest == candidate_digest;
            if !exact_retry {
                return self.error(0, "checkpoint_transfer_busy_proposal");
            }
            // The pending Raft task already owns the validated body. Track
            // only a bounded cursor for an exact native retry, rather than
            // allocating another checkpoint-sized reassembly buffer.
            self.pending_checkpoint_retry = Some((pending.clone(), 0));
            return Ok(());
        }
        if let Some(incoming) = self.incoming_transfer.as_mut() {
            let exact_retry = incoming.room == room
                && incoming.transfer == transfer
                && incoming.term == term
                && incoming.base_revision == base_revision
                && incoming.revision == revision
                && incoming.length == length as usize
                && incoming.digest == candidate_digest;
            if !exact_retry {
                return self.error(0, "checkpoint_transfer_busy_incoming");
            }
            // Retain the validated prefix and renew only an identical native
            // retry. Replayed chunks must byte-match that prefix before they
            // receive fresh sender credit.
            incoming.started = tokio::time::Instant::now();
            return Ok(());
        }
        self.incoming_transfer = Some(IncomingTransfer::begin(
            room,
            transfer,
            term,
            base_revision,
            revision,
            length as usize,
            &digest,
        )?);
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    async fn checkpoint_chunk(
        &mut self,
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        offset: u32,
        data: String,
    ) -> io::Result<()> {
        let mut retry = false;
        let next_offset = if let Some(incoming) = self.incoming_transfer.as_mut() {
            if incoming.room != room
                || incoming.transfer != transfer
                || incoming.term != term
                || incoming.base_revision != base_revision
                || incoming.revision != revision
            {
                Err(failed("invalid checkpoint identity"))
            } else {
                incoming
                    .push(offset as usize, &data)
                    .map(|acknowledged| acknowledged as u32)
            }
        } else if let Some((pending, cursor)) = self.pending_checkpoint_retry.as_mut() {
            retry = true;
            let decoded = URL_SAFE_NO_PAD
                .decode(data.as_bytes())
                .map_err(|_| failed("invalid checkpoint retry chunk"));
            decoded.and_then(|decoded| {
                let end = (*cursor).saturating_add(decoded.len());
                if pending.epoch != epoch
                    || pending.room != room
                    || pending.transfer != transfer
                    || pending.term != term
                    || pending.base_revision != base_revision
                    || pending.revision != revision
                    || offset as usize != *cursor
                    || decoded.is_empty()
                    || decoded.len() > CHECKPOINT_CHUNK_BYTES
                    || end > pending.length
                {
                    return Err(failed("invalid checkpoint retry chunk"));
                }
                *cursor = end;
                Ok(end as u32)
            })
        } else {
            Err(failed("checkpoint transfer missing"))
        };
        let Ok(next_offset) = next_offset else {
            if retry {
                self.pending_checkpoint_retry = None;
            } else {
                self.incoming_transfer = None;
            }
            return self.error(0, "invalid_checkpoint_chunk");
        };
        let pending_ack = (epoch, room, transfer, next_offset);
        self.pending_checkpoint_ack = None;
        if !self.emit_bulk(Event::CheckpointAck {
            epoch,
            room,
            transfer,
            // The acknowledgement is the next contiguous byte offset. This
            // is the sender's credit and lets a four-chunk window advance.
            offset: next_offset,
        }) {
            // Keep the latest cumulative credit and retry it on the next
            // actor turn. Dropping this event would permanently stall the
            // sender's bounded four-chunk window.
            self.pending_checkpoint_ack = Some(pending_ack);
        }
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    async fn checkpoint_end(
        &mut self,
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        length: u32,
        digest: String,
    ) -> io::Result<()> {
        if let Some(pending) = self.pending_checkpoint_proposal.as_ref() {
            let retry_complete = self
                .pending_checkpoint_retry
                .as_ref()
                .is_none_or(|(key, cursor)| key == pending && *cursor == pending.length);
            let exact = pending.epoch == epoch
                && pending.room == room
                && pending.transfer == transfer
                && pending.term == term
                && pending.base_revision == base_revision
                && pending.revision == revision
                && pending.length == length as usize
                && recovery::hex_digest(&pending.digest) == digest;
            if exact && retry_complete {
                self.pending_checkpoint_retry = None;
                return Ok(());
            }
            return self.error(0, "checkpoint_transfer_busy_proposal");
        }
        if self.tasks.len() >= MAX_TASKS {
            return self.error(0, "checkpoint_transfer_busy");
        }
        let Some(incoming) = self.incoming_transfer.take() else {
            return self.error(0, "checkpoint_transfer_missing");
        };
        if !self.matches(epoch)
            || self.room != Some(room)
            || incoming.room != room
            || incoming.transfer != transfer
            || incoming.term != term
            || incoming.base_revision != base_revision
            || incoming.revision != revision
            || incoming.length != length as usize
            || recovery::hex_digest(&incoming.digest) != digest
        {
            return self.error(0, "invalid_checkpoint_end");
        }
        let transfer = incoming.finish()?;
        let Some(recovery) = self.recovery.clone() else {
            return self.error(0, "coordination_unavailable");
        };
        let proposal = recovery.propose(&transfer)?;
        let key = CheckpointProposalKey {
            epoch,
            room,
            incarnation: recovery.incarnation,
            transfer: transfer.transfer,
            term,
            base_revision,
            revision,
            length: transfer.bytes.len(),
            digest: transfer.digest,
        };
        self.pending_checkpoint_proposal = Some(key.clone());
        self.tasks.spawn(async move {
            let result = recovery.coordinator.propose(proposal).await;
            Completion::CheckpointProposal(key, transfer, result)
        });
        Ok(())
    }

    fn schedule_membership_reconciliation(
        &mut self,
        retained: BTreeSet<EndpointId>,
        term: u64,
        revision: u64,
    ) {
        if retained.is_empty() || !retained.contains(&self.endpoint.id()) {
            return;
        }
        let roster_changed = self.committed_native_members.as_ref() != Some(&retained);
        if roster_changed {
            let previous = self.committed_native_members.clone().unwrap_or_default();
            let removed = self
                .admissions
                .values()
                .filter(|admission| {
                    previous.contains(&admission.primary_endpoint)
                        && !retained.contains(&admission.primary_endpoint)
                })
                .map(|admission| admission.incarnation);
            // The roster is already committed. Fence every omitted identity
            // before any asynchronous membership write can stall or lose its
            // waiter during a leadership change.
            self.pending_retired_incarnations.extend(removed);
            self.committed_native_members = Some(retained.clone());
        }
        if !roster_changed && self.pending_retired_incarnations.is_empty() {
            return;
        }
        self.spawn_membership_operation(retained, term, revision);
    }

    fn schedule_pending_membership_reconciliation(&mut self, term: u64, revision: u64) {
        if self.pending_retired_incarnations.is_empty() {
            return;
        }
        let retained = self.committed_native_members.clone().unwrap_or_else(|| {
            self.admissions
                .values()
                .map(|admission| admission.primary_endpoint)
                .chain(std::iter::once(self.endpoint.id()))
                .collect()
        });
        self.spawn_membership_operation(retained, term, revision);
    }

    fn spawn_membership_operation(
        &mut self,
        retained: BTreeSet<EndpointId>,
        term: u64,
        revision: u64,
    ) {
        if self.pending_membership_operation.is_some() || self.tasks.len() >= MAX_TASKS {
            return;
        }
        let Some(recovery) = self.recovery.clone() else {
            return;
        };
        let sequence = self.next_coordination_operation;
        self.next_coordination_operation = self.next_coordination_operation.saturating_add(1);
        let key = MembershipOperationKey {
            sequence,
            epoch: self.epoch,
            room: recovery.room,
            incarnation: recovery.incarnation,
            term,
            revision,
        };
        let pending = self.pending_retired_incarnations.clone();
        let admissions = self.admissions.values().cloned().collect::<Vec<_>>();
        self.pending_membership_operation = Some(key.clone());
        self.tasks.spawn(async move {
            let result = async {
                if recovery.coordinator.current_term() != term {
                    return Err(failed("obsolete membership operation"));
                }
                if recovery.coordinator.current_leader() == Some(recovery.incarnation) {
                    let applied_voters = recovery.applied_voter_ids().await;
                    let voter_removals = pending
                        .intersection(&applied_voters)
                        .copied()
                        .collect::<BTreeSet<_>>();
                    if !voter_removals.is_empty() {
                        let replacement = applied_voters
                            .difference(&voter_removals)
                            .copied()
                            .collect::<BTreeSet<_>>();
                        if replacement.is_empty() {
                            return Err(failed("empty replacement membership"));
                        }
                        recovery.remove_members(replacement).await?;
                    }
                    if recovery.coordinator.current_term() != term {
                        return Err(failed("obsolete membership operation"));
                    }
                    let voters_after = recovery.applied_voter_ids().await;
                    for admission in admissions.iter().filter(|admission| {
                        retained.contains(&admission.primary_endpoint)
                            && !voters_after.contains(&admission.incarnation)
                    }) {
                        recovery.add_learner(admission).await?;
                    }
                    let members = recovery.applied_member_ids().await;
                    let learner_removals = pending
                        .intersection(&members)
                        .filter(|incarnation| !voters_after.contains(incarnation))
                        .copied()
                        .collect::<BTreeSet<_>>();
                    recovery.remove_nodes(learner_removals).await?;
                }
                let members = recovery.applied_member_ids().await;
                let confirmed_retirements = pending
                    .difference(&members)
                    .copied()
                    .collect::<BTreeSet<_>>();
                for incarnation in &confirmed_retirements {
                    recovery.rpc.retire(*incarnation).await;
                }
                Ok(MembershipOperationResult {
                    confirmed_retirements,
                })
            }
            .await;
            Completion::MembershipReconciliation(key, result)
        });
    }

    fn queue_admission_operation(
        &mut self,
        peer: EndpointId,
        admissions: Vec<Admission>,
        add_member_if_leader: bool,
    ) -> io::Result<()> {
        if admissions.is_empty() {
            return Ok(());
        }
        let fingerprint = serde_json::to_vec(&admissions)
            .map_err(|_| failed("coordination admission encoding failed"))?;
        let duplicate_active = self
            .pending_admission_operation
            .as_ref()
            .is_some_and(|key| {
                key.peer == peer
                    && key.fingerprint == fingerprint
                    && key.add_member_if_leader == add_member_if_leader
            });
        let duplicate_deferred = self.deferred_admissions.iter().any(|deferred| {
            deferred.peer == peer
                && deferred.fingerprint == fingerprint
                && deferred.add_member_if_leader == add_member_if_leader
        });
        if duplicate_active || duplicate_deferred {
            return Ok(());
        }
        if self.deferred_admissions.len() >= MAX_CONTROL_PEERS {
            return Err(failed("coordination admission busy"));
        }
        self.deferred_admissions.push_back(DeferredAdmission {
            peer,
            admissions,
            add_member_if_leader,
            fingerprint,
        });
        self.start_next_admission_operation();
        Ok(())
    }

    fn start_next_admission_operation(&mut self) {
        if self.pending_admission_operation.is_some() || self.tasks.len() >= MAX_TASKS {
            return;
        }
        let Some(deferred) = self.deferred_admissions.pop_front() else {
            return;
        };
        let Some(recovery) = self.recovery.clone() else {
            self.deferred_admissions.clear();
            return;
        };
        let sequence = self.next_coordination_operation;
        self.next_coordination_operation = self.next_coordination_operation.saturating_add(1);
        let key = AdmissionOperationKey {
            sequence,
            epoch: self.epoch,
            room: recovery.room,
            incarnation: recovery.incarnation,
            term: recovery.coordinator.current_term(),
            leader: recovery.coordinator.current_leader(),
            peer: deferred.peer,
            fingerprint: deferred.fingerprint,
            add_member_if_leader: deferred.add_member_if_leader,
        };
        let admissions = deferred.admissions;
        let add_and_promote =
            deferred.add_member_if_leader && key.leader == Some(recovery.incarnation);
        let pending_retired = self.pending_retired_incarnations.clone();
        let retired = self.retired_incarnations.clone();
        let mut candidates = self.admission_order.clone();
        for admission in self.admissions.values().chain(admissions.iter()) {
            if !candidates.contains(&admission.incarnation) {
                candidates.push(admission.incarnation);
            }
        }
        let desired = crate::recovery::stable_voter_count(
            self.admissions
                .keys()
                .copied()
                .chain(admissions.iter().map(|admission| admission.incarnation))
                .collect::<BTreeSet<_>>()
                .len(),
        );
        self.pending_admission_operation = Some(key.clone());
        self.tasks.spawn(async move {
            let operation = async {
                if recovery.coordinator.current_term() != key.term
                    || recovery.coordinator.current_leader() != key.leader
                {
                    return Err(failed("obsolete coordination admission"));
                }
                if add_and_promote {
                    for admission in &admissions {
                        recovery.add_learner(admission).await?;
                    }
                    if recovery.coordinator.current_term() == key.term
                        && recovery.coordinator.current_leader() == key.leader
                    {
                        let current = recovery.applied_voter_ids().await;
                        let mut voters = current
                            .into_iter()
                            .filter(|id| !pending_retired.contains(id) && !retired.contains(id))
                            .collect::<BTreeSet<_>>();
                        for id in candidates {
                            if voters.len() >= desired {
                                break;
                            }
                            if !pending_retired.contains(&id) && !retired.contains(&id) {
                                voters.insert(id);
                            }
                        }
                        if !voters.is_empty() && voters.len() <= crate::recovery::MAX_VOTERS {
                            // Admission is already durable once AddLearner
                            // succeeds. Preserve that authenticated binding if
                            // a later voter promotion loses authority or fails;
                            // the serialized next refresh retries promotion.
                            let _ = recovery.promote_voters(voters).await;
                        }
                    }
                } else {
                    for admission in &admissions {
                        recovery.admit(admission).await?;
                    }
                }
                let (members, history) = recovery.applied_membership_provenance().await;
                if admissions.iter().any(|admission| {
                    history.contains(&admission.incarnation)
                        && !members.contains(&admission.incarnation)
                }) {
                    return Err(failed("retired coordination incarnation"));
                }
                Ok(AdmissionOperationResult { admissions })
            };
            let result = timeout(COORDINATION_ADMISSION_TIMEOUT, operation)
                .await
                .unwrap_or_else(|_| Err(failed("coordination admission timeout")));
            Completion::Admission(key, result)
        });
    }

    /// Every Raft replica must replay each newly applied committed checkpoint
    /// to its own native process. This watcher is intentionally independent of
    /// leader status: followers become usable replicas after applying the
    /// committed log, while native writes remain gated by `leader_local`.
    async fn pump_committed_checkpoint(&mut self) -> io::Result<()> {
        let Some(recovery) = self.recovery.clone() else {
            return Ok(());
        };
        if self.incoming_transfer.is_some()
            || self.outgoing_transfer.is_some()
            || self.pending_checkpoint_committed.is_some()
        {
            return Ok(());
        }
        let committed = recovery.committed().await;
        if committed.revision == 0 || committed.revision <= self.last_exported_revision {
            return Ok(());
        }
        let Ok(transfer_id) = committed.request.parse::<u64>() else {
            // Only helper proposals created from a native SessionProposal are
            // replayable. Keep the revision pending so an invalid candidate
            // cannot be mistaken for a committed native checkpoint.
            return Ok(());
        };
        if transfer_id == 0 || committed.term == 0 {
            return Ok(());
        }
        let Some(base_revision) = committed.revision.checked_sub(1) else {
            return Ok(());
        };
        let transfer = CheckpointTransfer::new(
            recovery.room,
            transfer_id,
            committed.term,
            base_revision,
            committed.revision,
            committed.checkpoint.into_bytes(),
        )?;
        if let Some(retained) = committed_primary_endpoints(&transfer.bytes) {
            self.schedule_membership_reconciliation(retained, committed.term, committed.revision);
        }
        if self
            .pending_checkpoint_proposal
            .as_ref()
            .is_some_and(|key| {
                key.epoch == self.epoch
                    && key.room == transfer.room
                    && key.incarnation == recovery.incarnation
                    && key.transfer == transfer.transfer
                    && key.term == transfer.term
                    && key.base_revision == transfer.base_revision
                    && key.revision == transfer.revision
                    && key.length == transfer.bytes.len()
                    && key.digest == transfer.digest
            })
        {
            // Durable local application, rather than the client-write waiter,
            // completes proposal ownership. A late worker result is fenced by
            // the missing key and cannot replay the effect twice.
            self.pending_checkpoint_proposal = None;
            self.pending_checkpoint_retry = None;
        }
        self.start_outgoing_checkpoint(transfer);
        Ok(())
    }

    fn pump_pending_checkpoint_ack(&mut self) {
        let Some((epoch, room, transfer, offset)) = self.pending_checkpoint_ack else {
            return;
        };
        if self.emit_bulk(Event::CheckpointAck {
            epoch,
            room,
            transfer,
            offset,
        }) {
            self.pending_checkpoint_ack = None;
        }
    }

    fn pump_pending_checkpoint_committed(&mut self) {
        let Some(marker) = self.pending_checkpoint_committed.clone() else {
            return;
        };
        if self.emit_bulk(Event::CheckpointCommitted {
            epoch: marker.epoch,
            room: marker.room,
            transfer: marker.transfer,
            term: marker.term,
            base_revision: marker.base_revision,
            revision: marker.revision,
            length: marker.length,
            digest: marker.digest,
        }) {
            self.pending_checkpoint_committed = None;
            self.last_exported_revision = marker.revision;
        }
    }

    fn expire_checkpoint_transfers(&mut self) {
        let now = tokio::time::Instant::now();
        if self.incoming_transfer.as_ref().is_some_and(|transfer| {
            now.duration_since(transfer.started) > CHECKPOINT_TRANSFER_TIMEOUT
        }) {
            self.incoming_transfer = None;
            self.pending_checkpoint_ack = None;
            let _ = self.emit_bulk(Event::Error {
                probe_failure: None,
                request_id: 0,
                epoch: self.epoch,
                peer: None,
                code: "checkpoint_receive_timeout".into(),
            });
        }
        if self.outgoing_transfer.as_ref().is_some_and(|transfer| {
            now.duration_since(transfer.started) > CHECKPOINT_TRANSFER_TIMEOUT
        }) {
            let revision = self
                .outgoing_transfer
                .as_ref()
                .map(|transfer| transfer.transfer.revision)
                .unwrap_or_default();
            self.outgoing_transfer = None;
            // A timed-out transfer must be replayed from the applied commit;
            // do not let a previously observed revision suppress retry.
            if revision != 0 && self.last_exported_revision >= revision {
                self.last_exported_revision = revision.saturating_sub(1);
            }
            let _ = self.emit_bulk(Event::Error {
                probe_failure: None,
                request_id: 0,
                epoch: self.epoch,
                peer: None,
                code: "checkpoint_send_timeout".into(),
            });
        }
    }

    fn pump_probe_invalidations(&mut self) {
        let pending: Vec<_> = self
            .pending_probe_invalidations
            .iter()
            .map(|(peer, invalidation)| (*peer, invalidation.clone()))
            .collect();
        for (peer, invalidation) in pending {
            if !self.emit_bulk(Event::ProbeResult {
                epoch: self.epoch,
                room: self.room.unwrap_or([0; 16]),
                peer,
                request: invalidation.request,
                pair_revision: invalidation.pair_revision,
                route: invalidation.route,
                status: "invalidated".into(),
                sample_count: 0,
                loss_count: 100,
                p95_rtt_us: 0,
                recommended_delay: -1,
                metrics: crate::probe::Metrics::default(),
            }) {
                break;
            }
            self.pending_probe_invalidations.remove(&peer);
        }
    }

    fn queue_probe_invalidation(
        &mut self,
        peer: EndpointId,
        request: u64,
        pair_revision: u64,
        route: String,
    ) {
        self.pending_probe_invalidations.insert(
            peer,
            PendingProbeInvalidation {
                request,
                pair_revision,
                route,
            },
        );
        self.pump_probe_invalidations();
    }

    fn invalidate_changed_probe_routes(&mut self) {
        self.pump_probe_invalidations();
        let invalidated: Vec<_> = self
            .probe_reservations
            .iter()
            .filter_map(|(peer, reservation)| {
                let current = selected_probe_route(&reservation.connection);
                let closed = reservation.connection.close_reason().is_some();
                (closed || current != reservation.route || current == "unavailable").then_some((
                    *peer,
                    reservation.request,
                    reservation.pair_revision,
                    if closed {
                        "unavailable".into()
                    } else {
                        current
                    },
                ))
            })
            .collect();
        for (peer, request, pair_revision, route) in invalidated {
            if let Some(mut reservation) = self.probe_reservations.remove(&peer) {
                let reported = reservation.reported;
                if reservation.connection.close_reason().is_none() {
                    reservation.route = route.clone();
                    reservation.reported = false;
                    self.probe_reservations.insert(peer, reservation);
                }
                if reported {
                    self.queue_probe_invalidation(peer, request, pair_revision, route);
                }
            }
        }
    }

    fn expire_probe_permissions(&mut self) {
        let now = tokio::time::Instant::now();
        let expired: Vec<_> = self
            .probe_permissions
            .iter()
            .filter_map(|(peer, permission)| (permission.expires <= now).then_some(*peer))
            .collect();
        for peer in expired {
            self.probe_permissions.remove(&peer);
            self.probe_peers.remove(&peer);
        }
    }

    async fn graceful_leave(&mut self, epoch: u64, abandon: bool) -> io::Result<()> {
        if !self.matches(epoch) {
            return self.error(0, "stale_epoch");
        }
        // Leave can be retried while the native adapter drains its closing
        // event. Preserve the original authority proof window instead of
        // clearing the coordination route on the duplicate command.
        if self.retirement_started.is_some() {
            return self.emit(Event::RoomClosed { epoch });
        }
        if abandon {
            self.clear_room();
            return self.emit(Event::RoomClosed { epoch });
        }
        let retain_departure_authority = self.recovery.as_ref().is_some_and(|recovery| {
            recovery.coordinator.current_leader() == Some(recovery.incarnation)
        });
        if let Some(recovery) = self.recovery.clone() {
            let state = recovery.state().await;
            let voters = recovery.applied_voter_ids().await;
            let applied_members = recovery.applied_member_ids().await;
            let coordination_only_departure = self.hosted.is_none()
                && self.committed_native_members.is_none()
                && applied_members.contains(&recovery.incarnation)
                && !state.leader_local;
            let coordination_only_learner =
                coordination_only_departure && !voters.contains(&recovery.incarnation);
            if coordination_only_departure {
                // The primary/native Join may be rejected after the helper
                // has already been admitted to Raft. There is no native
                // checkpoint roster to reconcile in that case, so ask the
                // committed leader to remove this authenticated voter before
                // running the normal successor proof below.
                let (removed_or_promoted, committed_self_removal) =
                    timeout(Duration::from_secs(5), async {
                        loop {
                            if recovery.coordinator.current_leader().is_none()
                                || recovery.coordinator.current_leader()
                                    == Some(recovery.incarnation)
                            {
                                break (true, false);
                            }
                            if recovery.request_self_removal().await.is_ok() {
                                // The leader's authenticated remove RPC does not
                                // reply until both the voter transition and exact
                                // RemoveNodes entry are committed. A removed
                                // follower may never receive that final entry, so
                                // this response is its durable exclusion proof.
                                break (true, true);
                            }
                            tokio::time::sleep(Duration::from_millis(25)).await;
                        }
                    })
                    .await
                    .unwrap_or((false, false));
                if !removed_or_promoted {
                    self.error(0, "leave_membership_failed")?;
                    return Ok(());
                }
                if committed_self_removal {
                    self.clear_room();
                    return self.emit(Event::RoomClosed { epoch });
                }
                // A learner does not participate in the successor election,
                // but it must remain live until the explicit RemoveNodes
                // entry has applied locally. Closing earlier leaves its old
                // authenticated route active on lagging replicas.
                if coordination_only_learner {
                    let coordination_member_removed = timeout(Duration::from_secs(5), async {
                        loop {
                            if !recovery
                                .applied_member_ids()
                                .await
                                .contains(&recovery.incarnation)
                            {
                                break true;
                            }
                            // OpenRaft may commit RemoveNodes on the leader
                            // and stop replicating to the learner before the
                            // departing process applies that final entry.
                            // The authenticated leader claim includes its
                            // complete applied membership, so it is the
                            // equivalent committed exclusion proof for this
                            // one-way departure route.
                            if recovery
                                .confirm_departure(state.term, recovery.committed().await.revision)
                                .await
                                .is_ok()
                            {
                                break true;
                            }
                            tokio::time::sleep(Duration::from_millis(25)).await;
                        }
                    })
                    .await
                    .unwrap_or(false);
                    if !coordination_member_removed {
                        self.error(0, "leave_membership_unconfirmed")?;
                        return Ok(());
                    }
                }
            }
            if voters.contains(&recovery.incarnation) && voters.len() > 1 {
                if state.leader_local {
                    let mut successor_voters = voters.clone();
                    successor_voters.remove(&recovery.incarnation);
                    // OpenRaft commits joint old/new membership through the
                    // old quorum before the helper retires its route.
                    let successor = successor_voters.iter().next().copied();
                    let promoted = match timeout(
                        Duration::from_secs(5),
                        recovery.promote_voters(successor_voters),
                    )
                    .await
                    {
                        Ok(Ok(())) => true,
                        Ok(Err(_)) | Err(_) => {
                            // A simultaneous non-native learner Leave may
                            // have committed the complementary singleton
                            // transition first. Treat that durable applied
                            // membership as the handoff proof instead of
                            // tearing down the actor with a race-specific
                            // failure.
                            let singleton = timeout(Duration::from_secs(2), async {
                                loop {
                                    let applied = recovery.applied_voter_ids().await;
                                    if applied.len() == 1 && applied.contains(&recovery.incarnation)
                                    {
                                        break true;
                                    }
                                    tokio::time::sleep(Duration::from_millis(25)).await;
                                }
                            })
                            .await
                            .unwrap_or(false);
                            if singleton {
                                true
                            } else {
                                self.error(0, "leave_membership_failed")?;
                                false
                            }
                        }
                    };
                    if !promoted {
                        return Ok(());
                    }
                    if let Some(successor) = successor {
                        // OpenRaft may retain the old leader as a learner
                        // after the joint transition. Trigger the election
                        // on the committed successor so the retiring leader
                        // can obtain a real successor authority claim before
                        // it tears down its control route.
                        let _ = recovery.rpc.request_election(successor).await;
                    }
                }
                // A non-leader departure is committed by the native room
                // authority first. In both cases wait until this incarnation
                // is absent from the committed voter set instead of tearing
                // down a still-authoritative helper. A retain=false change
                // can stop replicating to the departing follower before its
                // local metrics learn that fact, so followers also query the
                // current leader's authenticated voter claim.
                let minimum_revision = recovery.committed().await.revision;
                let mut leader_confirmed_departure = false;
                let removed = timeout(Duration::from_secs(5), async {
                    loop {
                        if !recovery
                            .applied_voter_ids()
                            .await
                            .contains(&recovery.incarnation)
                        {
                            break true;
                        }
                        // A simultaneous two-helper Leave can reach the
                        // successor after the old leader has committed the
                        // singleton membership but before the successor's
                        // command is handled.  There is then no remote
                        // successor left to query: the local committed
                        // membership and leader proof are the handoff.
                        if recovery.applied_voter_ids().await.len() == 1
                            && recovery.coordinator.current_leader() == Some(recovery.incarnation)
                        {
                            leader_confirmed_departure = true;
                            break true;
                        }
                        if recovery
                            .confirm_singleton_departure(state.term, minimum_revision)
                            .await
                            .is_ok()
                        {
                            leader_confirmed_departure = true;
                            break true;
                        }
                        if !state.leader_local
                            && recovery
                                .confirm_departure(state.term, minimum_revision)
                                .await
                                .is_ok()
                        {
                            leader_confirmed_departure = true;
                            break true;
                        }
                        tokio::time::sleep(Duration::from_millis(25)).await;
                    }
                })
                .await
                .unwrap_or(false);
                if !removed {
                    return self.error(0, "leave_successor_unconfirmed");
                }
                let minimum_term = state.term;
                let confirmed = if leader_confirmed_departure {
                    true
                } else {
                    timeout(Duration::from_secs(5), async {
                        loop {
                            if recovery
                                .confirm_available_successor(minimum_term, minimum_revision)
                                .await
                                .is_ok()
                            {
                                break true;
                            }
                            tokio::time::sleep(Duration::from_millis(50)).await;
                        }
                    })
                    .await
                    .unwrap_or(false)
                };
                if !confirmed {
                    // Keep the endpoint and gameplay links alive if no
                    // successor has a committed authority claim yet.
                    return self.error(0, "leave_successor_unconfirmed");
                }
                self.last_coordination_state = None;
                self.last_control_rebound = None;
                let _ = self.emit_coordination_state().await;
            }
            // A native RoomAction Leave may have committed the membership
            // removal before this helper receives the explicit Leave.  The
            // coordination-only learner path above has its own applied-member
            // exclusion proof; a native departure is confirmed by the
            // successor proof (or the already-established retirement grace).
            // Do not turn an unused local flag into a second, weaker proof.
        }
        // A simultaneous follower Leave can become the committed singleton
        // successor while this future is running. Keep that newly acquired
        // authority route alive for the same proof window as the old leader,
        // so the retiring peer can authenticate the handoff before shutdown.
        let became_successor = self.recovery.as_ref().is_some_and(|recovery| {
            recovery.coordinator.current_leader() == Some(recovery.incarnation)
        });
        if retain_departure_authority || became_successor {
            self.enter_departure_grace();
            self.emit(Event::RoomClosed { epoch })
        } else {
            self.clear_room();
            self.emit(Event::RoomClosed { epoch })
        }
    }

    /// Run the potentially slow membership handoff without starving the IPC
    /// command loop.  A partition can keep OpenRaft's membership commit
    /// pending until the five-second bounded operation timeout; status,
    /// shutdown, and an explicit abandon must still be serviceable during
    /// that interval.  The leave future owns the actor borrow, so these
    /// lifecycle responses use the captured event sender and immutable
    /// endpoint snapshot until the operation is cancelled.
    async fn leave_command(
        &mut self,
        epoch: u64,
        abandon: bool,
        commands: &mut mpsc::Receiver<Request>,
        failed_ipc: &mut watch::Receiver<bool>,
    ) -> io::Result<bool> {
        let events = self.events.clone();
        let endpoint = self.endpoint.clone();
        let status_epoch = self.epoch;
        let status_peers = self.controls.len();
        let status_games = self.games.len();
        let mut leave = Box::pin(self.graceful_leave(epoch, abandon));
        loop {
            tokio::select! {
                result = &mut leave => {
                    result?;
                    return Ok(true);
                }
                changed = failed_ipc.changed() => {
                    drop(leave);
                    return match changed {
                        Ok(()) => Err(failed("IPC disconnected")),
                        Err(_) => Err(failed("IPC failure watcher closed")),
                    };
                }
                request = commands.recv() => {
                    let Some(request) = request else {
                        drop(leave);
                        return Err(failed("IPC disconnected"));
                    };
                    match request.command {
                        Command::Status => {
                            let _ = events.try_send(Event::Status {
                                request_id: request.id,
                                endpoint: endpoint.id(),
                                ip_transports: endpoint.bound_sockets().len(),
                                epoch: status_epoch,
                                peers: status_peers,
                                games: status_games,
                            });
                        }
                        Command::Shutdown => {
                            drop(leave);
                            return Ok(false);
                        }
                        Command::Leave { epoch: requested_epoch, abandon: true } => {
                            if requested_epoch != epoch || requested_epoch == 0 {
                                let _ = events.try_send(Event::Error {
                                    probe_failure: None,
                                    request_id: request.id,
                                    epoch: status_epoch,
                                    peer: None,
                                    code: "stale_epoch".into(),
                                });
                                continue;
                            }
                            drop(leave);
                            self.clear_room();
                            self.emit(Event::RoomClosed { epoch })?;
                            return Ok(true);
                        }
                        Command::Leave { epoch: requested_epoch, abandon: false } => {
                            let event = if requested_epoch == epoch && requested_epoch != 0 {
                                // The original leave future still owns the
                                // membership handoff.  A duplicate must not
                                // manufacture RoomClosed before the old
                                // quorum has committed our exclusion and a
                                // successor has proved its authority.
                                Event::Error {
                                    probe_failure: None,
                                    request_id: request.id,
                                    epoch: status_epoch,
                                    peer: None,
                                    code: "leave_in_progress".into(),
                                }
                            } else {
                                Event::Error {
                                    probe_failure: None,
                                    request_id: request.id,
                                    epoch: status_epoch,
                                    peer: None,
                                    code: "stale_epoch".into(),
                                }
                            };
                            let _ = events.try_send(event);
                        }
                        _ => {
                            let _ = events.try_send(Event::Error {
                                probe_failure: None,
                                request_id: request.id,
                                epoch: status_epoch,
                                peer: None,
                                code: "leave_in_progress".into(),
                            });
                        }
                    }
                }
            }
        }
    }

    /// Close native/control state while retaining only the coordination
    /// authority read route for a bounded handoff window. This is needed when
    /// two helpers issue healthy Leave commands together: the new follower
    /// can still query the departing leader after the leader has reported its
    /// own room_closed event, then both processes may shut down normally.
    fn enter_departure_grace(&mut self) {
        self.hosted = None;
        self.room_invite = None;
        self.host_address = None;
        self.opening = false;
        self.controls.clear();
        self.games.clear();
        self.tasks.abort_all();
        self.admissions.clear();
        self.admission_order.clear();
        self.applied_admission_members.clear();
        self.incoming_transfer = None;
        self.pending_checkpoint_proposal = None;
        self.pending_checkpoint_retry = None;
        self.outgoing_transfer = None;
        self.committed_native_members = None;
        self.pending_retired_incarnations.clear();
        self.retired_incarnations.clear();
        self.pending_checkpoint_ack = None;
        self.pending_checkpoint_committed = None;
        self.pending_coordination_refresh = None;
        self.pending_membership_operation = None;
        self.pending_admission_operation = None;
        self.deferred_admissions.clear();
        self.pending_membership_publications.clear();
        self.last_coordination_state = None;
        self.last_control_rebound = None;
        self.unwritable_leader_since = None;
        self.reconnect_target = None;
        self.probe_reservations.clear();
        self.pending_game_admissions.clear();
        self.pending_probe_invalidations.clear();
        self.probe_peers.clear();
        self.probe_permissions.clear();
        self.pending_probe_authorizations.clear();
        self.retirement_started = Some(Instant::now());
    }

    fn expire_departure_grace(&mut self) {
        if self
            .retirement_started
            .is_some_and(|started| started.elapsed() >= Duration::from_secs(30))
        {
            self.clear_room();
        }
    }

    fn checkpoint_ack(
        &mut self,
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        offset: u32,
    ) -> io::Result<()> {
        if !self.matches(epoch) || self.room != Some(room) {
            return self.error(0, "stale_checkpoint_ack");
        }
        let Some(outgoing) = self.outgoing_transfer.as_mut() else {
            return self.error(0, "checkpoint_transfer_missing");
        };
        if outgoing.transfer.transfer != transfer {
            return self.error(0, "stale_checkpoint_ack");
        }
        if offset as usize > outgoing.transfer.bytes.len() {
            return self.error(0, "invalid_checkpoint_ack");
        }
        if offset as usize == outgoing.transfer.bytes.len() && outgoing.end_sent {
            let finished = outgoing.transfer.transfer;
            let transfer = outgoing.transfer.clone();
            self.outgoing_transfer = None;
            self.pending_checkpoint_committed = Some(PendingCheckpointCommitted {
                epoch,
                room,
                transfer: finished,
                term: transfer.term,
                base_revision: transfer.base_revision,
                revision: transfer.revision,
                length: transfer.bytes.len() as u32,
                digest: transfer.digest_hex(),
            });
            self.pump_pending_checkpoint_committed();
            return Ok(());
        }
        if offset <= outgoing.acked_offset {
            return Ok(());
        }
        // Credit advances only to an exact chunk end which this sender has
        // placed in flight. An arbitrary byte offset must not manufacture a
        // fifth credit or skip bytes the receiver never observed.
        if !outgoing.in_flight.contains(&offset) {
            return self.error(0, "invalid_checkpoint_ack");
        }
        outgoing.in_flight.retain(|end| *end > offset);
        outgoing.acked_offset = offset;
        self.pump_outgoing_checkpoint();
        Ok(())
    }

    async fn spawn_probe(
        &mut self,
        epoch: u64,
        room: [u8; 16],
        peer: EndpointId,
        request: u64,
        pair_revision: u64,
        benchmark: bool,
    ) -> io::Result<()> {
        if !self.matches(epoch) || self.room != Some(room) || request == 0 {
            return self.probe_error(1);
        }
        if self.tasks.len() >= MAX_TASKS {
            return self.probe_error(2);
        }
        if !self.controls.contains_key(&peer) {
            return self.probe_error(3);
        }
        if self.probe_peers.contains(&peer) {
            return self.probe_error(4);
        }
        let Some(recovery) = self.recovery.clone() else {
            return self.probe_error(5);
        };
        let Some(target_incarnation) = self
            .admissions
            .values()
            .find(|admission| admission.primary_endpoint == peer && admission.room == room)
            .map(|admission| admission.incarnation)
        else {
            return self.probe_error(6);
        };
        let key = ProbeAuthorizationKey {
            epoch,
            room,
            incarnation: recovery.incarnation,
            peer,
            target_incarnation,
            request,
            pair_revision,
            benchmark,
        };
        self.probe_peers.insert(peer);
        self.pending_probe_authorizations.insert(peer, key.clone());
        let source = self.endpoint.id();
        self.tasks.spawn(async move {
            let result = async {
                let state = recovery.state().await;
                if !state.writable
                    || recovery.coordinator.current_term() != state.term
                    || recovery.coordinator.current_leader().is_none()
                {
                    return Err(failed("probe authority unavailable"));
                }
                if !recovery
                        .probe_pair_bound(
                            recovery.incarnation,
                            target_incarnation,
                            source,
                            peer,
                            pair_revision,
                        )
                        .await
                {
                    return Err(failed("probe pair unavailable"));
                }
                let expires = now()
                    .unwrap_or_default()
                    .saturating_add(crate::probe::duration(benchmark).as_secs() + 5)
                    .saturating_add(transport::HANDSHAKE_TIMEOUT.as_secs());
                recovery
                    .reserve_probe(
                        recovery.incarnation,
                        target_incarnation,
                        request,
                        pair_revision,
                        state.term,
                        expires,
                    )
                    .await?;
                let committed = recovery.committed().await;
                Ok(ProbeAuthorization {
                    term: state.term,
                    leader: recovery.coordinator.current_leader(),
                    revision: committed.revision,
                    expires,
                })
            }
            .await;
            Completion::ProbeAuthorization(key, result)
        });
        Ok(())
    }

    fn pump_outgoing_checkpoint(&mut self) {
        // The receiver cannot make a newer revision visible before the
        // completion marker for the preceding transfer. Keep this guard at
        // the emission seam: IPC capacity can become available after the
        // marker pump failed but before this pump runs again.
        if self.pending_checkpoint_committed.is_some() {
            return;
        }
        let Some(mut outgoing) = self.outgoing_transfer.take() else {
            return;
        };
        let transfer = &outgoing.transfer;
        if !outgoing.begin_sent {
            if !self.emit_bulk(Event::CheckpointBegin {
                epoch: self.epoch,
                room: transfer.room,
                transfer: transfer.transfer,
                term: transfer.term,
                base_revision: transfer.base_revision,
                revision: transfer.revision,
                length: transfer.bytes.len() as u32,
                digest: transfer.digest_hex(),
            }) {
                self.outgoing_transfer = Some(outgoing);
                return;
            }
            outgoing.begin_sent = true;
        }
        while outgoing.in_flight.len() < CHECKPOINT_WINDOW
            && outgoing.next_offset < transfer.bytes.len()
        {
            let offset = outgoing.next_offset;
            let end = (offset + CHECKPOINT_CHUNK_BYTES).min(transfer.bytes.len());
            let data = URL_SAFE_NO_PAD.encode(&transfer.bytes[offset..end]);
            if !self.emit_bulk(Event::CheckpointChunk {
                epoch: self.epoch,
                room: transfer.room,
                transfer: transfer.transfer,
                term: transfer.term,
                base_revision: transfer.base_revision,
                revision: transfer.revision,
                offset: offset as u32,
                data,
            }) {
                self.outgoing_transfer = Some(outgoing);
                return;
            }
            // In-flight keys are cumulative end offsets. The receiver ACKs
            // its next contiguous offset, so one ACK may retire several
            // chunks under backpressure.
            outgoing.in_flight.insert(end as u32);
            outgoing.next_offset = end;
        }
        if outgoing.next_offset == transfer.bytes.len()
            && outgoing.in_flight.is_empty()
            && !outgoing.end_sent
            && self.emit_bulk(Event::CheckpointEnd {
                epoch: self.epoch,
                room: transfer.room,
                transfer: transfer.transfer,
                term: transfer.term,
                base_revision: transfer.base_revision,
                revision: transfer.revision,
                length: transfer.bytes.len() as u32,
                digest: transfer.digest_hex(),
            })
        {
            outgoing.end_sent = true;
        }
        self.outgoing_transfer = Some(outgoing);
    }
    fn clear_room(&mut self) {
        if let Some(recovery) = self.recovery.take() {
            tokio::spawn(async move {
                recovery.stop().await;
            });
        }
        self.hosted = None;
        self.room_invite = None;
        self.host_address = None;
        self.room = None;
        self.opening = false;
        self.controls.clear();
        self.games.clear();
        self.tasks.abort_all();
        self.closed_generation = 0;
        self.admissions.clear();
        self.admission_order.clear();
        self.applied_admission_members.clear();
        self.incoming_transfer = None;
        self.pending_checkpoint_proposal = None;
        self.pending_checkpoint_retry = None;
        self.outgoing_transfer = None;
        self.committed_native_members = None;
        self.pending_retired_incarnations.clear();
        self.retired_incarnations.clear();
        self.last_exported_revision = 0;
        self.pending_checkpoint_ack = None;
        self.pending_checkpoint_committed = None;
        self.pending_coordination_refresh = None;
        self.pending_membership_operation = None;
        self.pending_admission_operation = None;
        self.deferred_admissions.clear();
        self.pending_membership_publications.clear();
        self.last_coordination_state = None;
        self.last_control_rebound = None;
        self.unwritable_leader_since = None;
        self.reconnect_target = None;
        self.probe_reservations.clear();
        self.pending_game_admissions.clear();
        self.pending_probe_invalidations.clear();
        self.probe_peers.clear();
        self.probe_permissions.clear();
        self.pending_probe_authorizations.clear();
        self.retirement_started = None;
    }
    fn begin(&mut self, epoch: u64, build: &str) -> bool {
        if self.retirement_started.is_some() {
            self.clear_room();
        }
        if epoch <= self.epoch
            || epoch > i64::MAX as u64
            || self.room.is_some()
            || self.opening
            || !self.games.is_empty()
            || build.is_empty()
            || build.len() > 128
        {
            return false;
        }
        self.epoch = epoch;
        self.opening = true;
        true
    }
    fn matches(&self, epoch: u64) -> bool {
        epoch == self.epoch && epoch != 0
    }

    fn command(&mut self, request: Request) -> io::Result<bool> {
        let id = request.id;
        match request.command {
            Command::Status => self.emit(Event::Status {
                request_id: id,
                endpoint: self.endpoint.id(),
                ip_transports: self.endpoint.bound_sockets().len(),
                epoch: self.epoch,
                peers: self.controls.len(),
                games: self.games.len(),
            })?,
            Command::Shutdown => return Ok(false),
            Command::Host { epoch, build } => {
                if !self.begin(epoch, &build) {
                    self.error(id, "invalid_room_state")?;
                    return Ok(true);
                }
                let endpoint = self.endpoint.clone();
                self.tasks.spawn(async move {
                    let result = timeout(transport::HANDSHAKE_TIMEOUT, async {
                        endpoint.online().await;
                        let relay = endpoint
                            .addr()
                            .relay_urls()
                            .next()
                            .cloned()
                            .ok_or_else(|| failed("relay_unavailable"))?;
                        Invite::create(endpoint.id(), relay, build, now()?, INVITE_LIFETIME)
                    })
                    .await
                    .unwrap_or_else(|_| Err(failed("relay_unavailable")));
                    Completion::Hosted(epoch, result)
                });
            }
            Command::Join {
                epoch,
                invitation,
                build,
            } => {
                // Consume an accepted room epoch before parsing the invitation.
                // Rejection then belongs to the caller's attempt and Leave can
                // acknowledge cancellation even though no connection was made.
                if !self.begin(epoch, &build) {
                    self.error(id, "invalid_room_state")?;
                    return Ok(true);
                }
                let invite =
                    now().and_then(|time| Invite::parse_for_build(&invitation, time, &build));
                let invite = match invite {
                    Ok(invite) if invite.build() == build => invite,
                    _ => {
                        self.clear_room();
                        self.error(id, "invalid_or_incompatible_invitation")?;
                        return Ok(true);
                    }
                };
                self.room = Some(invite.room());
                self.host_address = Some(invite.address());
                let endpoint = self.endpoint.clone();
                self.tasks.spawn(async move {
                    let result = transport::connect_control(&endpoint, &invite).await;
                    Completion::GuestControl(epoch, invite, result)
                });
            }
            Command::Leave { epoch, abandon } => {
                let _ = abandon;
                if !self.matches(epoch) {
                    self.error(id, "stale_epoch")?;
                    return Ok(true);
                }
                self.clear_room();
                self.emit(Event::RoomClosed { epoch })?;
            }
            Command::Send {
                epoch,
                peer,
                message_id,
                payload,
            } => {
                if !self.matches(epoch) {
                    self.error(id, "stale_epoch")?;
                    return Ok(true);
                }
                if payload.is_empty() || payload.len() > MAX_CONTROL_PAYLOAD {
                    self.error(id, "invalid_control_size")?;
                    return Ok(true);
                }
                let Ok(wire_payload) = serde_json::to_string(&NativeControlMessage {
                    kind: "native_control".into(),
                    message_id,
                    payload,
                }) else {
                    self.error(id, "control_send_failed")?;
                    return Ok(true);
                };
                if wire_payload.len() > MAX_CONTROL_PAYLOAD {
                    self.error(id, "invalid_control_size")?;
                    return Ok(true);
                }
                let transport_id = self.next_transport_message;
                self.next_transport_message = self.next_transport_message.saturating_add(1);
                let result = self.controls.get(&peer).ok_or(()).and_then(|control| {
                    control
                        .try_send(ControlFrame {
                            message_id: transport_id,
                            payload: wire_payload.into_bytes(),
                        })
                        .map_err(|_| ())
                });
                if result.is_err() {
                    self.peer_error(id, peer, "control_send_failed")?;
                } else {
                    self.emit(Event::Sent {
                        request_id: id,
                        epoch,
                        message_id,
                    })?;
                }
            }
            Command::CloseControl { epoch, peer } => {
                if !self.matches(epoch) {
                    self.error(id, "stale_epoch")?;
                    return Ok(true);
                }
                if self.controls.remove(&peer).is_some() {
                    self.emit(Event::ControlClosed { epoch, peer })?;
                }
                // Never closes separately authorized gameplay connections.
            }
            Command::PrepareGame {
                epoch,
                peer,
                room,
                generation,
                capability,
                local_port,
                max_packet,
                dial,
            } => {
                if !self.matches(epoch)
                    || self.room != Some(room)
                    || generation <= self.closed_generation
                    || generation > i64::MAX as u64
                    || room == [0; 16]
                    || capability == [0; 32]
                    || local_port == 0
                    || max_packet == 0
                    || max_packet > wire::MAX_UDP_PAYLOAD
                    || peer == self.endpoint.id()
                    || self.games.contains_key(&peer)
                    || self.games.len() >= MAX_GAME_LINKS
                    || self
                        .games
                        .values()
                        .any(|game| game.auth.key.generation != generation)
                    || self.tasks.len() >= MAX_TASKS
                {
                    self.error(id, "invalid_game_registration")?;
                    return Ok(true);
                }
                let auth = GameAuthorization {
                    peer,
                    key: MatchKey { room, generation },
                    capability,
                    max_packet,
                };
                let reserved = self
                    .probe_reservations
                    .remove(&peer)
                    .map(|reservation| reservation.connection);
                let mut slot = GameSlot {
                    auth: auth.clone(),
                    local_port,
                    waiting: !dial && reserved.is_none(),
                    task: None,
                    stats: None,
                    route_connection: None,
                    expires: tokio::time::Instant::now() + transport::PREPARED_GAME_TIMEOUT,
                    prepare_deadline: None,
                };
                if let Some(connection) = reserved {
                    let reserved_auth = auth.clone();
                    let deadline = if dial {
                        None
                    } else {
                        let (sender, receiver) = watch::channel(slot.expires);
                        slot.prepare_deadline = Some(sender);
                        Some(receiver)
                    };
                    slot.task = Some(self.tasks.spawn(async move {
                        let result = if dial {
                            transport::connect_game_on(connection, reserved_auth).await
                        } else {
                            transport::accept_game_on(
                                connection,
                                reserved_auth,
                                deadline.expect("prepared listener deadline"),
                            )
                            .await
                        };
                        Completion::Game(epoch, peer, generation, result)
                    }));
                } else if dial {
                    let endpoint = self.endpoint.clone();
                    let address = self
                        .host_address
                        .as_ref()
                        .filter(|a| a.id == peer)
                        .cloned()
                        .unwrap_or_else(|| EndpointAddr::new(peer));
                    slot.task = Some(self.tasks.spawn(async move {
                        let result = transport::connect_game(&endpoint, address, auth).await;
                        Completion::Game(epoch, peer, generation, result)
                    }));
                }
                self.games.insert(peer, slot);
                self.emit(Event::GameWaiting {
                    epoch,
                    peer,
                    generation,
                })?;
            }
            Command::EndMatch { epoch, generation } => {
                if !self.matches(epoch)
                    || generation == 0
                    || generation > i64::MAX as u64
                    || self
                        .games
                        .values()
                        .any(|game| game.auth.key.generation != generation)
                {
                    self.error(id, "stale_match")?;
                    return Ok(true);
                }
                self.closed_generation = self.closed_generation.max(generation);
                let peers: Vec<_> = self.games.keys().copied().collect();
                self.games.clear();
                for peer in peers {
                    if let Some((connection, task)) = self.pending_game_admissions.remove(&peer) {
                        connection.close(0u32.into(), b"match ended during admission");
                        task.abort();
                    }
                    self.emit(Event::GameClosed {
                        epoch,
                        peer,
                        generation,
                    })?;
                }
            }
            Command::EndPeer {
                epoch,
                peer,
                generation,
            } => {
                if !self.matches(epoch)
                    || generation == 0
                    || generation > i64::MAX as u64
                    || generation <= self.closed_generation
                {
                    self.error(id, "stale_match")?;
                    return Ok(true);
                }
                // Per-link teardown is intentionally idempotent.  A bridge
                // may have already observed the remote close before the
                // authority's game_peer_end message reaches this helper.
                let Some(slot) = self.games.get(&peer) else {
                    return Ok(true);
                };
                if slot.auth.key.generation != generation {
                    self.error(id, "stale_match")?;
                    return Ok(true);
                }
                self.games.remove(&peer);
                if let Some((connection, task)) = self.pending_game_admissions.remove(&peer) {
                    connection.close(0u32.into(), b"peer ended during admission");
                    task.abort();
                }
                self.emit(Event::GameClosed {
                    epoch,
                    peer,
                    generation,
                })?;
            }
            Command::CheckpointBegin { .. }
            | Command::CheckpointChunk { .. }
            | Command::CheckpointEnd { .. }
            | Command::CheckpointAck { .. }
            | Command::ProbeRequest { .. } => {
                self.error(id, "async_command_not_dispatched")?;
            }
        }
        Ok(true)
    }

    async fn completed(&mut self, completion: Completion) -> io::Result<()> {
        let (completion, joined_invite) = match completion {
            Completion::GuestControl(epoch, invite, result) => {
                (Completion::Control(epoch, result), Some(invite))
            }
            Completion::MemberControl(epoch, incarnation, result) => {
                if let Ok(channel) = &result {
                    let peer = channel.connection.remote_id();
                    let current = epoch == self.epoch
                        && self.control_rejoin_member(peer) == Some(incarnation);
                    let active = if let Some(recovery) = &self.recovery {
                        recovery.applied_member_ids().await.contains(&incarnation)
                    } else {
                        false
                    };
                    if !current || !active {
                        channel.connection.close(1u32.into(), b"member retired");
                        return Ok(());
                    }
                }
                (Completion::Control(epoch, result), None)
            }
            Completion::Reconnect(epoch, peer, result) => {
                if epoch == self.epoch {
                    self.reconnect_target = None;
                }
                // A transient coordination bind/relay failure can close the
                // initial primary control before this helper has a RecoverySession.
                // Keep the authenticated invitation attached to the replacement
                // control so the fresh learner bootstrap is retried as well.
                let retrying_join = self
                    .recovery
                    .is_none()
                    .then(|| self.room_invite.clone())
                    .flatten();
                if result.is_err() {
                    self.reconnect_control(peer);
                } else if let Some(invite) = self.room_invite.as_ref()
                    && let Some(relay) = invite.address().relay_urls().next().cloned()
                {
                    self.host_address = Some(EndpointAddr::new(peer).with_relay_url(relay));
                }
                (Completion::Control(epoch, result), retrying_join)
            }
            other => (other, None),
        };
        match completion {
            Completion::GuestControl(..) | Completion::MemberControl(..) => unreachable!(),
            Completion::Reconnect(..) => unreachable!(),
            Completion::CheckpointProposal(key, transfer, _waiter_result) => {
                if self.pending_checkpoint_proposal.as_ref() != Some(&key) {
                    return Ok(());
                }
                self.pending_checkpoint_proposal = None;
                self.pending_checkpoint_retry = None;
                let Some(recovery) = self.recovery.clone() else {
                    return Ok(());
                };
                if key.epoch != self.epoch
                    || self.room != Some(key.room)
                    || recovery.room != key.room
                    || recovery.incarnation != key.incarnation
                {
                    return Ok(());
                }
                // A waiter error is not proof that a Raft write failed. Only
                // the locally applied committed record authorizes native
                // replay and membership effects; the periodic watcher uses
                // this same durable observation if the waiter never returns.
                let committed = recovery.committed().await;
                let committed_exact = committed.revision == key.revision
                    && committed.term == key.term
                    && committed.request == key.transfer.to_string()
                    && committed.checkpoint.as_bytes() == transfer.bytes
                    && transfer.room == key.room
                    && transfer.transfer == key.transfer
                    && transfer.base_revision == key.base_revision
                    && transfer.revision == key.revision
                    && transfer.bytes.len() == key.length
                    && transfer.digest == key.digest;
                if committed_exact {
                    if let Some(retained) = committed_primary_endpoints(&transfer.bytes) {
                        self.schedule_membership_reconciliation(
                            retained,
                            committed.term,
                            committed.revision,
                        );
                    }
                    if committed.revision > self.last_exported_revision
                        && self.outgoing_transfer.is_none()
                        && self.pending_checkpoint_committed.is_none()
                    {
                        self.start_outgoing_checkpoint(transfer);
                    }
                }
                self.last_coordination_state = None;
                self.emit_coordination_state().await?;
            }
            Completion::CoordinationRefresh(key, refresh) => {
                if self.pending_coordination_refresh.as_ref() != Some(&key) {
                    return Ok(());
                }
                self.pending_coordination_refresh = None;
                let Some(recovery) = self.recovery.clone() else {
                    return Ok(());
                };
                let committed_now = recovery.committed().await;
                let current_term = recovery.coordinator.current_term();
                let current_leader = recovery.coordinator.current_leader();
                let current = key.epoch == self.epoch
                    && self.room == Some(key.room)
                    && recovery.room == key.room
                    && recovery.incarnation == key.incarnation
                    && key.term == refresh.state.term
                    && key.leader == refresh.leader
                    && current_term == key.term
                    && current_leader == key.leader
                    && refresh.state.incarnation == key.incarnation
                    && refresh.state.revision == refresh.committed.revision
                    && committed_now.revision == refresh.committed.revision
                    && committed_now.term == refresh.committed.term
                    && committed_now.request == refresh.committed.request
                    && committed_now.checkpoint == refresh.committed.checkpoint;
                let failed_leader = if current && !refresh.state.writable {
                    current_leader
                        .filter(|leader| *leader != recovery.incarnation)
                        .and_then(|leader| {
                            let now = Instant::now();
                            match self.unwritable_leader_since {
                                Some((term, failed, since))
                                    if term == current_term && failed == leader =>
                                {
                                    if now.duration_since(since) >= RECOVERY_ELECTION_GRACE {
                                        // Rate-limit repeated triggers while an election is in
                                        // progress. A later committed term or healthy proof
                                        // clears this marker.
                                        self.unwritable_leader_since =
                                            Some((current_term, leader, now));
                                        Some(leader)
                                    } else {
                                        None
                                    }
                                }
                                _ => {
                                    self.unwritable_leader_since =
                                        Some((current_term, leader, now));
                                    None
                                }
                            }
                        })
                } else {
                    if current {
                        self.unwritable_leader_since = None;
                    }
                    None
                };
                if current {
                    self.apply_coordination_refresh(refresh)?;
                    if let Some(leader) = failed_leader {
                        self.trigger_recovery_election_for_leader(leader).await;
                    }
                } else {
                    // A read that crossed a leadership or applied-revision
                    // change can only withdraw writability. A fresh worker
                    // will establish the next positive claim.
                    self.coordination_writable = false;
                    self.last_coordination_state = None;
                    self.last_control_rebound = None;
                    self.unwritable_leader_since = None;
                    self.emit_coordination_state().await?;
                }
            }
            Completion::MembershipReconciliation(key, result) => {
                if self.pending_membership_operation.as_ref() != Some(&key) {
                    return Ok(());
                }
                self.pending_membership_operation = None;
                let Some(recovery) = self.recovery.clone() else {
                    return Ok(());
                };
                let committed = recovery.committed().await;
                let current = key.epoch == self.epoch
                    && self.room == Some(key.room)
                    && recovery.room == key.room
                    && recovery.incarnation == key.incarnation
                    && recovery.coordinator.current_term() == key.term
                    && committed.revision == key.revision;
                if current && let Ok(result) = result {
                    self.apply_confirmed_retirements(result.confirmed_retirements);
                }
                // Stale completions leave the pre-write retirement fences in
                // place. The next exact committed-roster refresh retries the
                // operation through the current authority.
                self.last_coordination_state = None;
                self.last_control_rebound = None;
                self.emit_coordination_state().await?;
            }
            Completion::Admission(key, result) => {
                if self.pending_admission_operation.as_ref() != Some(&key) {
                    return Ok(());
                }
                self.pending_admission_operation = None;
                let Some(recovery) = self.recovery.clone() else {
                    self.deferred_admissions.clear();
                    return Ok(());
                };
                let current_room = key.epoch == self.epoch
                    && self.room == Some(key.room)
                    && recovery.room == key.room
                    && recovery.incarnation == key.incarnation;
                let mut published_roster_changed = false;
                if current_room && let Ok(result) = result {
                    let (members, history) = recovery.applied_membership_provenance().await;
                    for admission in result.admissions {
                        if self.retired_incarnations.contains(&admission.incarnation)
                            || self
                                .pending_retired_incarnations
                                .contains(&admission.incarnation)
                            || (history.contains(&admission.incarnation)
                                && !members.contains(&admission.incarnation))
                        {
                            self.pending_retired_incarnations
                                .insert(admission.incarnation);
                            continue;
                        }
                        self.remember_admission(admission);
                        published_roster_changed = true;
                    }
                }
                if published_roster_changed
                    && recovery.coordinator.current_leader() == Some(recovery.incarnation)
                {
                    // Admission is asynchronous, so the old receive-site
                    // broadcast observes the pre-admission roster. Publish
                    // only after the authenticated binding is installed, and
                    // retain every destination until its bounded worker queue
                    // accepts the current full snapshot.
                    self.queue_membership_publication();
                }
                self.last_coordination_state = None;
                self.last_control_rebound = None;
                self.emit_coordination_state().await?;
                self.start_next_admission_operation();
            }
            Completion::ProbeAuthorization(key, result) => {
                if self.pending_probe_authorizations.get(&key.peer) != Some(&key) {
                    return Ok(());
                }
                self.pending_probe_authorizations.remove(&key.peer);
                let Some(recovery) = self.recovery.clone() else {
                    self.probe_peers.remove(&key.peer);
                    return Ok(());
                };
                let wall_now = now().unwrap_or_default();
                let valid = if let Ok(authorization) = &result {
                    let committed = recovery.committed().await;
                    key.epoch == self.epoch
                        && self.room == Some(key.room)
                        && recovery.room == key.room
                        && recovery.incarnation == key.incarnation
                        && recovery.coordinator.current_term() == authorization.term
                        && recovery.coordinator.current_leader() == authorization.leader
                        && authorization.leader.is_some()
                        && authorization.expires > wall_now
                        && committed.revision == authorization.revision
                        && !self.games.contains_key(&key.peer)
                        && self.admissions.values().any(|admission| {
                            admission.room == key.room
                                && admission.primary_endpoint == key.peer
                                && admission.incarnation == key.target_incarnation
                        })
                        && recovery
                            .probe_pair_bound(
                                key.incarnation,
                                key.target_incarnation,
                                self.endpoint.id(),
                                key.peer,
                                key.pair_revision,
                            )
                            .await
                } else {
                    false
                };
                let authorization = match result {
                    Ok(authorization) => authorization,
                    Err(error) => {
                        let reason = match error.to_string().as_str() {
                            "probe authority unavailable" => 7,
                            "probe pair unavailable" => 8,
                            "room proposal busy" => 11,
                            _ => 9,
                        };
                        self.probe_peers.remove(&key.peer);
                        self.probe_error(reason)?;
                        return Ok(());
                    }
                };
                if !valid {
                    self.probe_peers.remove(&key.peer);
                    self.probe_error(10)?;
                    return Ok(());
                }
                self.pending_probe_invalidations.remove(&key.peer);
                let address = self
                    .host_address
                    .as_ref()
                    .filter(|address| address.id == key.peer)
                    .cloned()
                    .unwrap_or_else(|| EndpointAddr::new(key.peer));
                let remaining = Duration::from_secs(authorization.expires.saturating_sub(wall_now));
                self.probe_permissions.insert(
                    key.peer,
                    ProbePermission {
                        request: key.request,
                        pair_revision: key.pair_revision,
                        expires: tokio::time::Instant::now() + remaining,
                    },
                );
                self.send_probe_reservation(
                    key.peer,
                    key.room,
                    key.request,
                    key.pair_revision,
                    authorization.term,
                    authorization.expires,
                );
                let endpoint = self.endpoint.clone();
                self.tasks.spawn(async move {
                    let result = run_probe(
                        endpoint,
                        address,
                        key.room,
                        key.peer,
                        key.request,
                        key.pair_revision,
                        key.benchmark,
                    )
                    .await
                    .unwrap_or(ProbeCompletion {
                        peer: key.peer,
                        request: key.request,
                        pair_revision: key.pair_revision,
                        samples_us: Vec::new(),
                        connection: None,
                        report: true,
                        route_changed: false,
                        metrics: crate::probe::Metrics::default(),
                    });
                    Completion::Probe(key.epoch, Ok(result))
                });
            }
            Completion::Hosted(epoch, result) => {
                if epoch != self.epoch || !self.opening {
                    return Ok(());
                }
                self.opening = false;
                match result {
                    Ok(invite) => {
                        let invite = match self.setup_host_recovery(invite).await {
                            Ok(invite) => invite,
                            Err(_) => {
                                self.clear_room();
                                self.error(0, "coordination_unavailable")?;
                                return Ok(());
                            }
                        };
                        self.room = Some(invite.room());
                        self.emit(Event::Hosted {
                            epoch,
                            invitation: invite.encode()?,
                            room: invite.room(),
                        })?;
                        self.emit(Event::DiscordInvite {
                            epoch,
                            invitation: invite.encode()?,
                            secret: invite.encode_discord()?,
                        })?;
                        self.hosted = Some(invite);
                        self.room_invite = self.hosted.clone();
                    }
                    Err(_) => self.error(0, "host_unavailable")?,
                }
            }
            Completion::Incoming(epoch, result) => {
                if let Ok(connection) = result {
                    if epoch != self.epoch || self.tasks.len() >= MAX_TASKS {
                        connection.close(1u32.into(), b"obsolete room");
                        return Ok(());
                    }
                    let peer = connection.remote_id();
                    if connection.alpn() == CONTROL_ALPN {
                        self.remove_closed_control(peer);
                        if let Some(invite) = self
                            .room_invite
                            .clone()
                            .or_else(|| self.hosted.clone())
                            .filter(|_| {
                                self.controls.len() < MAX_CONTROL_PEERS
                                    || self.controls.contains_key(&peer)
                            })
                        {
                            let member = self.control_rejoin_member(peer);
                            let recovery = self.recovery.clone();
                            self.tasks.spawn(async move {
                                if let (Some(incarnation), Some(recovery)) = (member, recovery) {
                                    let result = if recovery
                                        .applied_member_ids()
                                        .await
                                        .contains(&incarnation)
                                    {
                                        transport::accept_control_member(connection, &invite, peer)
                                            .await
                                    } else {
                                        connection.close(1u32.into(), b"member retired");
                                        Err(failed("member retired"))
                                    };
                                    Completion::MemberControl(epoch, incarnation, result)
                                } else {
                                    Completion::Control(
                                        epoch,
                                        transport::accept_control(connection, &invite).await,
                                    )
                                }
                            });
                        } else {
                            connection.close(1u32.into(), b"room unavailable");
                        }
                    } else if connection.alpn() == GAME_ALPN {
                        let generation = self
                            .games
                            .get(&peer)
                            .filter(|slot| slot.waiting)
                            .map(|slot| slot.auth.key.generation);
                        let probe = self
                            .probe_permissions
                            .get(&peer)
                            .filter(|p| p.expires > tokio::time::Instant::now())
                            .map(|p| (p.request, p.pair_revision));
                        if (generation.is_none() && probe.is_none())
                            || self.pending_game_admissions.contains_key(&peer)
                        {
                            connection.close(1u32.into(), b"gameplay not authorized");
                            return Ok(());
                        }
                        let admission_connection = connection.clone();
                        let incoming = IncomingGame {
                            epoch,
                            peer,
                            connection,
                            generation,
                            probe,
                            deadline: tokio::time::Instant::now() + transport::HANDSHAKE_TIMEOUT,
                        };
                        let task = self.tasks.spawn(async move {
                            let result = transport::accept_game_stream_until(
                                &incoming.connection,
                                incoming.deadline,
                            )
                            .await;
                            Completion::ClassifiedGame(incoming, result)
                        });
                        self.pending_game_admissions
                            .insert(peer, (admission_connection, task));
                    } else {
                        connection.close(1u32.into(), b"unsupported protocol");
                    }
                }
            }
            Completion::ClassifiedGame(incoming, result) => {
                let IncomingGame {
                    epoch,
                    peer,
                    connection,
                    generation,
                    probe,
                    deadline,
                } = incoming;
                if epoch != self.epoch {
                    connection.close(1u32.into(), b"obsolete room");
                    return Ok(());
                }
                if !self
                    .pending_game_admissions
                    .get(&peer)
                    .is_some_and(|(pending, _)| pending.stable_id() == connection.stable_id())
                {
                    connection.close(1u32.into(), b"superseded game admission");
                    return Ok(());
                }
                self.pending_game_admissions.remove(&peer);
                let Ok(mode) = result else {
                    return Ok(());
                };
                let authorized = tokio::time::Instant::now() < deadline
                    && match &mode {
                        transport::GameStream::Gameplay(_, _) => {
                            self.games.get(&peer).is_some_and(|slot| {
                                slot.waiting && Some(slot.auth.key.generation) == generation
                            })
                        }
                        transport::GameStream::Probe(_, _) => {
                            self.probe_permissions.get(&peer).is_some_and(|p| {
                                p.expires > tokio::time::Instant::now()
                                    && Some((p.request, p.pair_revision)) == probe
                            })
                        }
                    };
                if !authorized {
                    connection.close(1u32.into(), b"obsolete game admission");
                    return Ok(());
                }
                match mode {
                    transport::GameStream::Probe(send, recv)
                        if self.probe_peers.contains(&peer) =>
                    {
                        let room = self.room.unwrap_or([0; 16]);
                        let (request, pair_revision) = self
                            .probe_permissions
                            .get(&peer)
                            .filter(|permission| permission.expires > tokio::time::Instant::now())
                            .map(|permission| (permission.request, permission.pair_revision))
                            .unwrap_or((0, 0));
                        self.tasks.spawn(async move {
                            let result = match serve_probe(
                                connection,
                                send,
                                recv,
                                room,
                                request,
                                pair_revision,
                            )
                            .await
                            {
                                Ok(connection) => Ok(ProbeCompletion {
                                    peer,
                                    request,
                                    pair_revision,
                                    samples_us: Vec::new(),
                                    connection: Some(connection),
                                    report: false,
                                    route_changed: false,
                                    metrics: crate::probe::Metrics::default(),
                                }),
                                Err(_) => Ok(ProbeCompletion {
                                    peer,
                                    request,
                                    pair_revision,
                                    samples_us: Vec::new(),
                                    connection: None,
                                    report: false,
                                    route_changed: false,
                                    metrics: crate::probe::Metrics::default(),
                                }),
                            };
                            Completion::Probe(epoch, result)
                        });
                    }
                    transport::GameStream::Gameplay(send, recv) => {
                        if let Some(slot) = self.games.get_mut(&peer).filter(|slot| slot.waiting) {
                            slot.waiting = false;
                            let auth = slot.auth.clone();
                            let generation = auth.key.generation;
                            slot.task = Some(self.tasks.spawn(async move {
                                Completion::Game(
                                    epoch,
                                    peer,
                                    generation,
                                    transport::accept_game_stream_with_until(
                                        connection, auth, send, recv, deadline,
                                    )
                                    .await,
                                )
                            }));
                        } else {
                            connection.close(1u32.into(), b"gameplay not authorized");
                        }
                    }
                    transport::GameStream::Probe(send, recv) => {
                        drop(send);
                        drop(recv);
                        connection.close(1u32.into(), b"probe not reserved");
                    }
                }
            }
            Completion::Control(epoch, result) => {
                match result {
                    Ok(channel) => {
                        let peer = channel.connection.remote_id();
                        self.remove_closed_control(peer);
                        let replacing = self.controls.contains_key(&peer);
                        if epoch != self.epoch
                            || self.room.is_none()
                            || (!replacing && self.controls.len() >= MAX_CONTROL_PEERS)
                        {
                            channel.connection.close(1u32.into(), b"room unavailable");
                            return Ok(());
                        }
                        // The QUIC identity and room proof authenticate this
                        // as a new connection from the same helper endpoint.
                        // Supersede the old worker even if its remote close has
                        // not propagated yet; coordination admission still
                        // fences stale process incarnations independently.
                        if replacing {
                            self.controls.remove(&peer);
                        }
                        self.opening = false;
                        self.controls.insert(peer, ControlWorker::start(channel));
                        if let Some(invite) = joined_invite.as_ref() {
                            self.room_invite = Some(invite.clone());
                            // sf4e2/emd2 intentionally omits the private
                            // coordination endpoint. The authenticated host
                            // Admission frame on this primary control stream
                            // performs the authority lookup before recovery
                            // starts. sf4e3/full invites can bootstrap
                            // directly from their committed route.
                            let setup = invite.coordination_address().is_some();
                            if setup && self.setup_join_recovery(invite).await.is_err() {
                                self.controls.remove(&peer);
                                let _ = self.emit_bulk(Event::ControlClosed { epoch, peer });
                                self.reconnect_control(peer);
                                self.error(0, "coordination_unavailable")?;
                                return Ok(());
                            }
                        }
                        self.emit(Event::Connected {
                            epoch,
                            peer,
                            room: self.room.unwrap(),
                        })?;
                        if let Some(invite) = joined_invite {
                            self.emit(Event::DiscordInvite {
                                epoch,
                                invitation: invite.encode()?,
                                secret: invite.encode_discord()?,
                            })?;
                        }
                        self.send_coordination_control(peer);
                        self.last_coordination_state = None;
                        self.last_control_rebound = None;
                        self.emit_coordination_state().await?;
                    }
                    Err(_) if epoch == self.epoch && self.opening => {
                        self.clear_room();
                        self.error(0, "join_failed")?;
                    }
                    Err(_) => (), // Rejected inbound peer: keep the host's room alive.
                }
            }
            Completion::Game(epoch, peer, generation, result) => {
                let valid = epoch == self.epoch
                    && self.games.get(&peer).is_some_and(|slot| {
                        slot.auth.key.generation == generation && slot.stats.is_none()
                    });
                if !valid {
                    return Ok(());
                } // GameConnection drop closes stale results.
                let local_port = self.games[&peer].local_port;
                let bridge = match result {
                    Ok(game) => {
                        let route = selected_probe_route(&game.connection);
                        let route_connection = game.connection.clone();
                        Bridge::bind(game, SocketAddr::from((Ipv4Addr::LOCALHOST, local_port)))
                            .await
                            .map(|bridge| (bridge, route, route_connection))
                    }
                    Err(error) => Err(error),
                };
                match bridge {
                    Ok((bridge, route, route_connection)) => {
                        let virtual_port = bridge.local_addr()?.port();
                        let slot = self.games.get_mut(&peer).unwrap();
                        slot.prepare_deadline = None;
                        slot.stats = Some(bridge.stats.clone());
                        slot.route_connection = Some(route_connection);
                        let max_packet = slot.auth.max_packet;
                        slot.task = Some(self.tasks.spawn(async move {
                            let (_keep_running, stop) = watch::channel(false);
                            let failure = bridge.run(stop).await.err();
                            Completion::BridgeEnded(epoch, peer, generation, failure)
                        }));
                        self.emit(Event::GameReady {
                            epoch,
                            peer,
                            generation,
                            virtual_port,
                            max_packet,
                            route,
                        })?;
                    }
                    Err(_) => {
                        self.games.remove(&peer);
                        self.error(0, "gameplay_prepare_failed")?;
                        self.emit(Event::GameClosed {
                            epoch,
                            peer,
                            generation,
                        })?;
                    }
                }
            }
            Completion::BridgeEnded(epoch, peer, generation, failure) => {
                if epoch == self.epoch
                    && self
                        .games
                        .get(&peer)
                        .is_some_and(|slot| slot.auth.key.generation == generation)
                {
                    if let Some(slot) = self.games.remove(&peer) {
                        let route = slot
                            .route_connection
                            .as_ref()
                            .map(selected_probe_route)
                            .unwrap_or_else(|| "unavailable".into());
                        if let Some(stats) = slot.stats.as_ref() {
                            self.emit(Event::Statistics {
                                epoch,
                                peer,
                                generation,
                                sent_packets: stats.sent_packets.load(Ordering::Relaxed),
                                received_packets: stats.received_packets.load(Ordering::Relaxed),
                                sent_bytes: stats.sent_bytes.load(Ordering::Relaxed),
                                received_bytes: stats.received_bytes.load(Ordering::Relaxed),
                                rejected_packets: stats.rejected_packets.load(Ordering::Relaxed),
                                congestion_events: stats.congestion_events.load(Ordering::Relaxed),
                                local_drops: stats.local_drops.load(Ordering::Relaxed),
                                route: route.clone(),
                            })?;
                        }
                        if let Some(reason) = failure {
                            self.emit(Event::GameFailed {
                                epoch,
                                peer,
                                generation,
                                reason,
                                route,
                                max_packet: slot.auth.max_packet,
                                max_datagram: slot
                                    .route_connection
                                    .as_ref()
                                    .and_then(Connection::max_datagram_size)
                                    .unwrap_or(0),
                            })?;
                        }
                    }
                    self.emit(Event::GameClosed {
                        epoch,
                        peer,
                        generation,
                    })?;
                }
            }
            Completion::Probe(epoch, result) => {
                if epoch != self.epoch {
                    return Ok(());
                }
                if let Ok(probe) = result {
                    let current =
                        self.probe_permissions
                            .get(&probe.peer)
                            .is_some_and(|permission| {
                                permission.request == probe.request
                                    && permission.pair_revision == probe.pair_revision
                            });
                    if !current {
                        if let Some(connection) = probe.connection {
                            connection.close(1u32.into(), b"obsolete probe completion");
                        }
                        return Ok(());
                    }
                    self.probe_peers.remove(&probe.peer);
                    self.probe_permissions.remove(&probe.peer);
                    self.pending_probe_invalidations.remove(&probe.peer);
                    // A check initiated by the other player may replace our
                    // measured connection. Retire its recommendation explicitly.
                    if let Some(previous) = self.probe_reservations.remove(&probe.peer) {
                        if previous.reported && !probe.report {
                            self.queue_probe_invalidation(
                                probe.peer,
                                previous.request,
                                previous.pair_revision,
                                probe
                                    .connection
                                    .as_ref()
                                    .map(selected_probe_route)
                                    .unwrap_or_default(),
                            );
                        }
                    }
                    if probe.report {
                        let route = probe
                            .connection
                            .as_ref()
                            .map(selected_probe_route)
                            .unwrap_or_else(|| "unavailable".into());
                        if probe.route_changed {
                            if let Some(connection) = probe.connection {
                                if connection.close_reason().is_none() {
                                    self.probe_reservations.insert(
                                        probe.peer,
                                        ProbeReservation {
                                            reported: false,
                                            connection,
                                            request: probe.request,
                                            pair_revision: probe.pair_revision,
                                            route: route.clone(),
                                        },
                                    );
                                }
                            }
                            self.queue_probe_invalidation(
                                probe.peer,
                                probe.request,
                                probe.pair_revision,
                                route,
                            );
                            return Ok(());
                        }
                        let summary = recovery::summarize_datagram_probe(
                            &probe.samples_us,
                            probe.metrics.expected,
                            probe.metrics.sent,
                        );
                        if summary.status == "ready" {
                            if let Some(connection) = probe.connection {
                                self.probe_reservations.insert(
                                    probe.peer,
                                    ProbeReservation {
                                        reported: true,
                                        connection,
                                        request: probe.request,
                                        pair_revision: probe.pair_revision,
                                        route: route.clone(),
                                    },
                                );
                            }
                        } else if let Some(connection) = probe.connection {
                            connection.close(1u32.into(), b"probe unavailable");
                        }
                        self.emit(Event::ProbeResult {
                            epoch,
                            room: self.room.unwrap_or([0; 16]),
                            peer: probe.peer,
                            request: probe.request,
                            pair_revision: probe.pair_revision,
                            route,
                            status: summary.status.clone(),
                            sample_count: summary.sample_count,
                            loss_count: summary.loss_count,
                            p95_rtt_us: summary.p95_rtt_us,
                            recommended_delay: if summary.status == "ready" {
                                i16::from(summary.recommended_delay)
                            } else {
                                -1
                            },
                            metrics: probe.metrics,
                        })?;
                    } else if let Some(connection) = probe.connection {
                        self.probe_reservations.insert(
                            probe.peer,
                            ProbeReservation {
                                reported: false,
                                route: selected_probe_route(&connection),
                                connection,
                                request: probe.request,
                                pair_revision: probe.pair_revision,
                            },
                        );
                    }
                }
            }
        }
        Ok(())
    }

    async fn poll_controls(&mut self) -> io::Result<()> {
        let peers: Vec<_> = self.controls.keys().copied().collect();
        for peer in peers {
            for _ in 0..CONTROL_POLL_BUDGET {
                // Leave space for terminal/control lifecycle events. A remote
                // flood backpressures this peer's bounded worker instead of
                // exhausting the global queue and killing healthy gameplay.
                if self.events.capacity() <= LIFECYCLE_EVENT_RESERVE {
                    break;
                }
                let frame = self
                    .controls
                    .get_mut(&peer)
                    .and_then(|control| control.try_receive());
                if let Some(frame) = frame {
                    match String::from_utf8(frame.payload) {
                        Ok(payload) => {
                            let coordination =
                                self.accept_coordination_control(peer, &payload).await;
                            if coordination.is_err() {
                                // A stale or unauthenticated coordination
                                // control is scoped to this peer. Close that
                                // route while retaining independent gameplay
                                // bridges and the rest of the room.
                                let retry_join = self.recovery.is_none()
                                    && self
                                        .room_invite
                                        .as_ref()
                                        .is_some_and(|invite| invite.endpoint() == peer);
                                self.controls.remove(&peer);
                                if retry_join {
                                    self.reconnect_control(peer);
                                }
                                let _ = self.emit_bulk(Event::ControlClosed {
                                    epoch: self.epoch,
                                    peer,
                                });
                                break;
                            }
                            if coordination.unwrap_or(false) {
                                // Any roster mutation is published by its
                                // authenticated asynchronous completion.
                            } else if let Ok(native) =
                                serde_json::from_str::<NativeControlMessage>(&payload)
                                && native.kind == "native_control"
                                && native.message_id > 1
                                && !native.payload.is_empty()
                            {
                                self.emit(Event::Message {
                                    epoch: self.epoch,
                                    peer,
                                    message_id: native.message_id,
                                    payload: native.payload,
                                })?;
                            } else {
                                self.emit(Event::Message {
                                    epoch: self.epoch,
                                    peer,
                                    message_id: frame.message_id,
                                    payload,
                                })?;
                            }
                        }
                        Err(_) => {
                            self.controls.remove(&peer);
                            self.reconnect_control(peer);
                            self.emit(Event::ControlClosed {
                                epoch: self.epoch,
                                peer,
                            })?;
                            break;
                        }
                    }
                } else {
                    break;
                }
            }
            if self
                .controls
                .get(&peer)
                .is_some_and(|control| control.is_closed())
            {
                self.controls.remove(&peer);
                self.reconnect_control(peer);
                self.emit(Event::ControlClosed {
                    epoch: self.epoch,
                    peer,
                })?;
            }
        }
        // A pending native game authorization belongs to the committed room
        // state. During a coordinated quorum outage keep its bounded listener
        // alive and let the new authority either authorize it or commit its
        // cancellation. Prepared listeners retain a renewable 60-second
        // pre-stream wait; once a gameplay stream arrives, the transport
        // keeps its separate ten-second marker/proof handshake deadline.
        if self.recovery.is_some() && !self.coordination_writable {
            let deadline = tokio::time::Instant::now() + transport::PREPARED_GAME_TIMEOUT;
            for slot in self
                .games
                .values_mut()
                .filter(|slot| slot.waiting || slot.prepare_deadline.is_some())
            {
                if slot.expires < deadline {
                    slot.expires = deadline;
                    if let Some(sender) = slot.prepare_deadline.as_ref() {
                        sender.send_replace(deadline);
                    }
                }
            }
        }
        let expired: Vec<_> = self
            .games
            .iter()
            .filter(|(_, slot)| {
                (slot.waiting || slot.prepare_deadline.is_some())
                    && tokio::time::Instant::now() >= slot.expires
            })
            .map(|(peer, slot)| (*peer, slot.auth.key.generation))
            .collect();
        for (peer, generation) in expired {
            self.games.remove(&peer);
            self.emit(Event::GameClosed {
                epoch: self.epoch,
                peer,
                generation,
            })?;
        }
        Ok(())
    }

    async fn run(
        &mut self,
        mut commands: mpsc::Receiver<Request>,
        mut failed_ipc: watch::Receiver<bool>,
    ) -> io::Result<()> {
        let mut tick = interval(Duration::from_millis(2));
        tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        let mut statistics = interval(Duration::from_secs(1));
        loop {
            tokio::select! {
                _ = failed_ipc.changed() => return Err(failed("IPC disconnected")),
                request = commands.recv() => match request {
                    Some(request) => {
                        let id = request.id;
                        match request.command {
                            Command::CheckpointBegin { epoch, room, transfer, term, base_revision, revision, length, digest } =>
                                if self.checkpoint_begin(epoch, room, transfer, term, base_revision, revision, length, digest).await.is_err() {
                                    self.error(id, "checkpoint_begin_rejected")?;
                                },
                            Command::CheckpointChunk { epoch, room, transfer, term, base_revision, revision, offset, data } =>
                                if self.checkpoint_chunk(epoch, room, transfer, term, base_revision, revision, offset, data).await.is_err() {
                                    self.error(id, "checkpoint_chunk_rejected")?;
                                },
                            Command::CheckpointEnd { epoch, room, transfer, term, base_revision, revision, length, digest } =>
                                if self.checkpoint_end(epoch, room, transfer, term, base_revision, revision, length, digest).await.is_err() {
                                    self.error(id, "checkpoint_not_committed")?;
                                },
                            Command::CheckpointAck { epoch, room, transfer, offset } =>
                                if self.checkpoint_ack(epoch, room, transfer, offset).is_err() {
                                    self.error(id, "checkpoint_ack_rejected")?;
                                },
                            Command::ProbeRequest { epoch, room, peer, request, pair_revision, benchmark } => {
                                self.spawn_probe(epoch, room, peer, request, pair_revision, benchmark).await?;
                            }
                            Command::Leave { epoch, abandon } => {
                                if !self.leave_command(epoch, abandon, &mut commands, &mut failed_ipc).await? {
                                    return Ok(());
                                }
                            }
                            command => if !self.command(Request { id, command })? { return Ok(()); },
                        }
                    },
                    None => return Err(failed("IPC disconnected")),
                },
                incoming = self.endpoint.accept() => {
                    let Some(incoming) = incoming else { return Err(failed("endpoint closed")); };
                    if self.tasks.len() >= MAX_TASKS
                        || (self.hosted.is_none()
                            && self.room_invite.is_none()
                            && !self.games.values().any(|slot| slot.waiting)
                            && self.probe_peers.is_empty())
                    {
                        incoming.refuse();
                        continue;
                    }
                    let epoch = self.epoch;
                    self.tasks.spawn(async move {
                        let result = timeout(transport::HANDSHAKE_TIMEOUT, incoming).await
                            .map_err(|_| failed("handshake timeout")).and_then(|r| r.map_err(|_| failed("handshake rejected")));
                        Completion::Incoming(epoch, result)
                    });
                }
                result = self.tasks.join_next(), if !self.tasks.is_empty() => {
                    match result { Some(Ok(completion)) => self.completed(completion).await?,
                        Some(Err(error)) if error.is_cancelled() => (), _ => return Err(failed("helper worker failed")) }
                }
                _ = tick.tick() => {
                    self.expire_departure_grace();
                    self.start_next_admission_operation();
                    self.pump_membership_publications();
                    self.poll_controls().await?;
                    self.pump_pending_checkpoint_ack();
                    self.pump_pending_checkpoint_committed();
                    self.expire_checkpoint_transfers();
                    self.expire_probe_permissions();
                    self.invalidate_changed_probe_routes();
                    self.pump_outgoing_checkpoint();
                    self.pump_committed_checkpoint().await?;
                },
                _ = statistics.tick() => {
                    self.emit_coordination_state().await?;
                    for (peer, slot) in &self.games {
                        if self.events.capacity() <= LIFECYCLE_EVENT_RESERVE { break; }
                        if let Some(stats) = &slot.stats {
                            self.emit(Event::Statistics { epoch: self.epoch, peer: *peer, generation: slot.auth.key.generation,
                                sent_packets: stats.sent_packets.load(Ordering::Relaxed), received_packets: stats.received_packets.load(Ordering::Relaxed),
                                sent_bytes: stats.sent_bytes.load(Ordering::Relaxed), received_bytes: stats.received_bytes.load(Ordering::Relaxed),
                                rejected_packets: stats.rejected_packets.load(Ordering::Relaxed), congestion_events: stats.congestion_events.load(Ordering::Relaxed),
                                local_drops: stats.local_drops.load(Ordering::Relaxed),
                                route: slot.route_connection.as_ref().map(selected_probe_route).unwrap_or_else(|| "unavailable".into()) })?;
                        }
                    }
                }
            }
        }
    }
}

async fn run_probe(
    endpoint: Endpoint,
    address: EndpointAddr,
    room: [u8; 16],
    peer: EndpointId,
    request: u64,
    pair_revision: u64,
    benchmark: bool,
) -> io::Result<ProbeCompletion> {
    let (connection, send, recv) = connect_probe_stream(&endpoint, &address).await?;
    let measurement = crate::probe::measure(
        &connection,
        send,
        recv,
        room,
        request,
        pair_revision,
        benchmark,
    )
    .await?;
    Ok(ProbeCompletion {
        peer,
        request,
        pair_revision,
        samples_us: measurement.samples,
        connection: Some(connection),
        report: true,
        route_changed: measurement.route_changed,
        metrics: measurement.metrics,
    })
}

async fn connect_probe_stream(
    endpoint: &Endpoint,
    address: &EndpointAddr,
) -> io::Result<(Connection, SendStream, RecvStream)> {
    let deadline = tokio::time::Instant::now() + transport::HANDSHAKE_TIMEOUT;
    loop {
        let now = tokio::time::Instant::now();
        if now >= deadline {
            return Err(failed("probe timeout"));
        }
        let remaining = deadline - now;
        let connection =
            match timeout(remaining, endpoint.connect(address.clone(), GAME_ALPN)).await {
                Ok(Ok(connection)) => connection,
                Ok(Err(_)) => {
                    tokio::time::sleep(Duration::from_millis(25)).await;
                    continue;
                }
                Err(_) => return Err(failed("probe timeout")),
            };
        let stream = timeout(
            deadline.saturating_duration_since(tokio::time::Instant::now()),
            connection.open_bi(),
        )
        .await;
        let Ok(Ok((mut send, mut recv))) = stream else {
            connection.close(1u32.into(), b"probe stream unavailable");
            continue;
        };
        let ready = async {
            send.write_all(&transport::GAME_PROBE_MAGIC).await?;
            send.flush().await?;
            let mut acknowledgment = [0u8; transport::GAME_PROBE_MAGIC.len()];
            recv.read_exact(&mut acknowledgment)
                .await
                .map_err(|_| failed("probe acknowledgment"))?;
            (acknowledgment == transport::GAME_PROBE_MAGIC)
                .then_some(())
                .ok_or_else(|| failed("invalid probe acknowledgment"))
        };
        let acknowledgment_timeout = deadline
            .saturating_duration_since(tokio::time::Instant::now())
            .min(Duration::from_millis(500));
        if matches!(timeout(acknowledgment_timeout, ready).await, Ok(Ok(()))) {
            return Ok((connection, send, recv));
        }
        // A committed reservation and its control notification use a different
        // connection from GAME_ALPN. The first gameplay connection may win that
        // race; retry only until the existing handshake deadline proves the
        // recipient has applied the reservation and acknowledged this stream.
        connection.close(1u32.into(), b"probe reservation pending");
        tokio::time::sleep(Duration::from_millis(25)).await;
    }
}

async fn serve_probe(
    connection: Connection,
    mut send: SendStream,
    recv: RecvStream,
    room: [u8; 16],
    request: u64,
    pair_revision: u64,
) -> io::Result<Connection> {
    send.write_all(&transport::GAME_PROBE_MAGIC)
        .await
        .map_err(|_| failed("probe acknowledgment"))?;
    crate::probe::respond(&connection, send, recv, room, request, pair_revision).await?;
    Ok(connection)
}

fn selected_probe_route(connection: &Connection) -> String {
    let paths = connection.paths();
    paths
        .iter()
        .find(|path| path.is_selected())
        .map(|path| path.remote_addr().to_string())
        .unwrap_or_else(|| "unavailable".into())
}

pub async fn run<S: AsyncRead + AsyncWrite + Unpin + Send + 'static>(
    stream: S,
    endpoint: Endpoint,
    relay_only: bool,
) -> io::Result<()> {
    let (mut reader, mut writer) = tokio::io::split(stream);
    let (commands, receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (events, mut outbound) = mpsc::channel::<Event>(IPC_QUEUE_CAPACITY);
    let (failed_ipc, failure) = watch::channel(false);
    let writer_failed = failed_ipc.clone();
    let reader_task = tokio::spawn(async move {
        let mut last_id = 1;
        while let Ok(frame) = wire::read_ipc(&mut reader).await {
            if frame.message_id <= last_id {
                break;
            }
            last_id = frame.message_id;
            let Ok(command) = serde_json::from_slice(&frame.payload) else {
                break;
            };
            if commands
                .send(Request {
                    id: frame.message_id,
                    command,
                })
                .await
                .is_err()
            {
                break;
            }
        }
        let _ = failed_ipc.send(true);
    });
    let writer_task = tokio::spawn(async move {
        let mut message_id = 2;
        while let Some(event) = outbound.recv().await {
            let payload = serde_json::to_vec(&event).map_err(|_| failed("IPC serialize"))?;
            wire::write_ipc(
                &mut writer,
                &ControlFrame {
                    message_id,
                    payload,
                },
            )
            .await?;
            if matches!(event, Event::Stopped) {
                return Ok::<(), io::Error>(());
            }
            message_id += 1;
        }
        Err(failed("IPC writer ended"))
    });
    let writer_abort = writer_task.abort_handle();
    // Watch the writer independently: a blocked reader must not hide its death.
    let writer_watch = tokio::spawn(async move {
        let result = writer_task.await;
        if !matches!(result, Ok(Ok(()))) {
            let _ = writer_failed.send(true);
        }
    });
    let _task_scope = TaskScope(vec![
        reader_task.abort_handle(),
        writer_abort,
        writer_watch.abort_handle(),
    ]);
    let mut actor = Actor {
        endpoint: endpoint.clone(),
        relay_only,
        epoch: 0,
        opening: false,
        room: None,
        hosted: None,
        room_invite: None,
        host_address: None,
        controls: BTreeMap::new(),
        games: BTreeMap::new(),
        closed_generation: 0,
        tasks: JoinSet::new(),
        events,
        recovery: None,
        admissions: BTreeMap::new(),
        admission_order: Vec::new(),
        applied_admission_members: BTreeSet::new(),
        incoming_transfer: None,
        pending_checkpoint_proposal: None,
        pending_checkpoint_retry: None,
        outgoing_transfer: None,
        committed_native_members: None,
        pending_retired_incarnations: BTreeSet::new(),
        retired_incarnations: BTreeSet::new(),
        last_exported_revision: 0,
        pending_checkpoint_ack: None,
        pending_checkpoint_committed: None,
        next_transport_message: TRANSPORT_MESSAGE_ID_BASE,
        next_coordination_operation: 1,
        pending_coordination_refresh: None,
        pending_membership_operation: None,
        pending_admission_operation: None,
        deferred_admissions: VecDeque::new(),
        pending_membership_publications: BTreeSet::new(),
        last_coordination_state: None,
        coordination_writable: false,
        last_control_rebound: None,
        unwritable_leader_since: None,
        reconnect_target: None,
        probe_reservations: BTreeMap::new(),
        pending_game_admissions: BTreeMap::new(),
        pending_probe_invalidations: BTreeMap::new(),
        probe_peers: BTreeSet::new(),
        probe_permissions: BTreeMap::new(),
        pending_probe_authorizations: BTreeMap::new(),
        retirement_started: None,
    };
    let result = actor.run(receiver, failure).await;
    actor.clear_room();
    actor.tasks.shutdown().await;
    endpoint.close().await;
    if result.is_ok() {
        let _ = actor.emit(Event::Stopped);
    }
    drop(actor);
    reader_task.abort();
    // A client that never reads cannot hold shutdown open indefinitely.
    let abort = writer_watch.abort_handle();
    if timeout(Duration::from_secs(2), writer_watch).await.is_err() {
        abort.abort();
    }
    result
}

// Dropping a cancelled service future must not detach pipe reader/writer tasks.
struct TaskScope(Vec<AbortHandle>);
impl Drop for TaskScope {
    fn drop(&mut self) {
        for task in &self.0 {
            task.abort();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn native_wrapper_keeps_quote_heavy_json_below_frame_bound() {
        let payload = serde_json::json!({ "text": "\"".repeat(32_000) }).to_string();
        let wire = serde_json::to_string(&NativeControlMessage {
            kind: "native_control".into(),
            message_id: 19,
            payload: payload.clone(),
        })
        .unwrap();
        assert!(wire.len() < MAX_CONTROL_PAYLOAD);
        assert!(wire.contains(r#""payload":{"text":"#));
        let decoded: NativeControlMessage = serde_json::from_str(&wire).unwrap();
        assert_eq!(decoded.payload, payload);
    }
    use iroh::endpoint::{PortmapperConfig, presets};
    use tokio::net::UdpSocket;

    async fn endpoint() -> Endpoint {
        Endpoint::builder(presets::Minimal)
            .clear_ip_transports()
            .bind_addr((Ipv4Addr::LOCALHOST, 0))
            .unwrap()
            .portmapper_config(PortmapperConfig::Disabled)
            .alpns(vec![CONTROL_ALPN.to_vec(), GAME_ALPN.to_vec()])
            .bind()
            .await
            .unwrap()
    }
    fn address(endpoint: &Endpoint) -> EndpointAddr {
        EndpointAddr::new(endpoint.id()).with_ip_addr(endpoint.bound_sockets()[0])
    }
    async fn next(events: &mut mpsc::Receiver<Event>, kind: &str) -> Event {
        loop {
            let event = events.recv().await.unwrap();
            let value = serde_json::to_value(&event).unwrap();
            if value["type"] == kind {
                return event;
            }
            assert_ne!(
                value["type"], "error",
                "unexpected helper error: {}",
                value["code"]
            );
        }
    }

    fn test_actor(endpoint: Endpoint, events: mpsc::Sender<Event>) -> Actor {
        Actor {
            endpoint,
            relay_only: false,
            epoch: 0,
            opening: false,
            room: None,
            hosted: None,
            room_invite: None,
            host_address: None,
            controls: BTreeMap::new(),
            games: BTreeMap::new(),
            closed_generation: 0,
            tasks: JoinSet::new(),
            events,
            recovery: None,
            admissions: BTreeMap::new(),
            admission_order: Vec::new(),
            applied_admission_members: BTreeSet::new(),
            incoming_transfer: None,
            pending_checkpoint_proposal: None,
            pending_checkpoint_retry: None,
            outgoing_transfer: None,
            committed_native_members: None,
            pending_retired_incarnations: BTreeSet::new(),
            retired_incarnations: BTreeSet::new(),
            last_exported_revision: 0,
            pending_checkpoint_ack: None,
            pending_checkpoint_committed: None,
            next_transport_message: TRANSPORT_MESSAGE_ID_BASE,
            next_coordination_operation: 1,
            pending_coordination_refresh: None,
            pending_membership_operation: None,
            pending_admission_operation: None,
            deferred_admissions: VecDeque::new(),
            pending_membership_publications: BTreeSet::new(),
            last_coordination_state: None,
            coordination_writable: false,
            last_control_rebound: None,
            unwritable_leader_since: None,
            reconnect_target: None,
            probe_reservations: BTreeMap::new(),
            pending_game_admissions: BTreeMap::new(),
            pending_probe_invalidations: BTreeMap::new(),
            probe_peers: BTreeSet::new(),
            probe_permissions: BTreeMap::new(),
            pending_probe_authorizations: BTreeMap::new(),
            retirement_started: None,
        }
    }

    #[tokio::test]
    async fn near_limit_checkpoint_obeys_four_credit_window_and_cumulative_acks() {
        timeout(Duration::from_secs(10), async {
            let source_endpoint = endpoint().await;
            let target_endpoint = endpoint().await;
            let (source_events, mut source_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (target_events, mut target_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let mut source = test_actor(source_endpoint.clone(), source_events);
            let mut target = test_actor(target_endpoint.clone(), target_events);
            let room = [43; 16];
            source.epoch = 1;
            source.room = Some(room);
            target.epoch = 1;
            target.room = Some(room);
            let bytes: Vec<u8> = (0..recovery::MAX_CHECKPOINT_BYTES - 13)
                .map(|index| (index.wrapping_mul(73) & 255) as u8)
                .collect();
            let transfer = CheckpointTransfer::new(room, 17, 3, 8, 9, bytes.clone()).unwrap();
            source.start_outgoing_checkpoint(transfer.clone());

            match source_rx.recv().await.unwrap() {
                Event::CheckpointBegin {
                    transfer,
                    term,
                    base_revision,
                    revision,
                    length,
                    digest,
                    ..
                } => {
                    target
                        .checkpoint_begin(
                            1,
                            room,
                            transfer,
                            term,
                            base_revision,
                            revision,
                            length,
                            digest,
                        )
                        .await
                        .unwrap();
                }
                _ => panic!("checkpoint begin must precede chunks"),
            }

            let mut received = 0usize;
            let mut window = 0usize;
            while received < bytes.len() {
                let chunks = CHECKPOINT_WINDOW
                    .min((bytes.len() - received).div_ceil(CHECKPOINT_CHUNK_BYTES));
                let mut cumulative_ack = 0;
                for _ in 0..chunks {
                    match source_rx.recv().await.unwrap() {
                        Event::CheckpointChunk {
                            transfer,
                            term,
                            base_revision,
                            revision,
                            offset,
                            data,
                            ..
                        } => {
                            assert_eq!(offset as usize, received);
                            let decoded = URL_SAFE_NO_PAD.decode(&data).unwrap();
                            received += decoded.len();
                            target
                                .checkpoint_chunk(
                                    1,
                                    room,
                                    transfer,
                                    term,
                                    base_revision,
                                    revision,
                                    offset,
                                    data,
                                )
                                .await
                                .unwrap();
                            match target_rx.recv().await.unwrap() {
                                Event::CheckpointAck { offset, .. } => {
                                    cumulative_ack = offset;
                                }
                                _ => panic!("each received chunk returns cumulative credit"),
                            }
                        }
                        _ => panic!("only four checkpoint chunks may occupy a window"),
                    }
                }
                assert!(source_rx.try_recv().is_err());

                if window == 0 {
                    source
                        .checkpoint_ack(1, room, transfer.transfer, cumulative_ack - 1)
                        .unwrap();
                    assert!(matches!(
                        source_rx.recv().await,
                        Some(Event::Error { code, .. }) if code == "invalid_checkpoint_ack"
                    ));
                    assert_eq!(source.outgoing_transfer.as_ref().unwrap().acked_offset, 0);
                }
                source
                    .checkpoint_ack(1, room, transfer.transfer, cumulative_ack)
                    .unwrap();
                if received < bytes.len() && window == 0 {
                    let next_offset = source.outgoing_transfer.as_ref().unwrap().next_offset;
                    source
                        .checkpoint_ack(1, room, transfer.transfer, cumulative_ack)
                        .unwrap();
                    assert_eq!(
                        source.outgoing_transfer.as_ref().unwrap().next_offset,
                        next_offset,
                        "duplicate cumulative ACK must not create credit"
                    );
                }
                window += 1;
            }

            let (length, digest) = match source_rx.recv().await.unwrap() {
                Event::CheckpointEnd { length, digest, .. } => (length, digest),
                _ => panic!("checkpoint end must follow acknowledged chunks"),
            };
            assert_eq!(length as usize, bytes.len());
            assert_eq!(digest, transfer.digest_hex());
            assert_eq!(
                target
                    .incoming_transfer
                    .take()
                    .unwrap()
                    .finish()
                    .unwrap()
                    .bytes,
                bytes
            );
            source
                .checkpoint_ack(1, room, transfer.transfer, length)
                .unwrap();
            assert!(matches!(
                source_rx.recv().await,
                Some(Event::CheckpointCommitted {
                    transfer: 17,
                    revision: 9,
                    ..
                })
            ));
            assert!(source.outgoing_transfer.is_none());
            source_endpoint.close().await;
            target_endpoint.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn committed_marker_precedes_the_next_revision_export() {
        let endpoint = endpoint().await;
        let (events, mut receiver) = mpsc::channel(LIFECYCLE_EVENT_RESERVE + 1);
        let mut actor = test_actor(endpoint.clone(), events);
        let room = [44; 16];
        actor.epoch = 1;
        actor.room = Some(room);
        assert!(actor.emit_bulk(Event::CheckpointAck {
            epoch: 1,
            room,
            transfer: 16,
            offset: 4,
        }));
        actor.pending_checkpoint_committed = Some(PendingCheckpointCommitted {
            epoch: 1,
            room,
            transfer: 17,
            term: 3,
            base_revision: 7,
            revision: 8,
            length: 4,
            digest: recovery::hex_digest(&recovery::sha256(b"old")),
        });
        let next = CheckpointTransfer::new(room, 18, 3, 8, 9, b"next".to_vec()).unwrap();

        actor.start_outgoing_checkpoint(next);

        assert!(matches!(
            receiver.recv().await,
            Some(Event::CheckpointAck { transfer: 16, .. })
        ));
        // Capacity became available after the marker pump failed. Pumping the
        // newer export alone must still leave that slot for the old marker.
        actor.pump_outgoing_checkpoint();
        assert!(receiver.try_recv().is_err());
        actor.pump_pending_checkpoint_committed();
        assert!(matches!(
            receiver.recv().await,
            Some(Event::CheckpointCommitted {
                transfer: 17,
                revision: 8,
                ..
            })
        ));
        actor.pump_outgoing_checkpoint();
        assert!(matches!(
            receiver.recv().await,
            Some(Event::CheckpointBegin {
                transfer: 18,
                base_revision: 8,
                revision: 9,
                ..
            })
        ));
        endpoint.close().await;
    }

    #[tokio::test]
    async fn a_new_revision_does_not_replace_an_export_in_flight() {
        let endpoint = endpoint().await;
        let (events, _receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(endpoint.clone(), events);
        let room = [45; 16];
        actor.epoch = 1;
        actor.room = Some(room);
        let current = CheckpointTransfer::new(room, 17, 3, 7, 8, vec![7; 96 * 1024]).unwrap();
        let newer = CheckpointTransfer::new(room, 18, 3, 8, 9, vec![8; 96 * 1024]).unwrap();

        actor.start_outgoing_checkpoint(current);
        actor.start_outgoing_checkpoint(newer);

        let outgoing = actor.outgoing_transfer.as_ref().unwrap();
        assert_eq!(outgoing.transfer.transfer, 17);
        assert_eq!(outgoing.transfer.revision, 8);
        endpoint.close().await;
    }

    #[tokio::test]
    async fn completed_receiver_accepts_exact_retry_before_delayed_commit_marker() {
        timeout(Duration::from_secs(5), async {
            let source_endpoint = endpoint().await;
            let target_endpoint = endpoint().await;
            let (source_events, mut source_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (target_events, mut target_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let mut source = test_actor(source_endpoint.clone(), source_events);
            let mut target = test_actor(target_endpoint.clone(), target_events);
            let room = [46; 16];
            source.epoch = 1;
            source.room = Some(room);
            target.epoch = 1;
            target.room = Some(room);
            let bytes: Vec<u8> = (0..CHECKPOINT_CHUNK_BYTES * (CHECKPOINT_WINDOW + 2) + 37)
                .map(|index| (index.wrapping_mul(41) & 255) as u8)
                .collect();
            let transfer = CheckpointTransfer::new(room, 19, 4, 9, 10, bytes.clone()).unwrap();
            source.start_outgoing_checkpoint(transfer.clone());

            // Complete the receiver's body, but delay its final ACK long enough
            // for the sender to expire. This pins the exact production retry
            // state: more than one four-credit window is retained while the
            // matching committed marker has not arrived yet.
            loop {
                match source_rx.recv().await.unwrap() {
                    Event::CheckpointBegin {
                        transfer: transfer_id,
                        term,
                        base_revision,
                        revision,
                        length,
                        digest,
                        ..
                    } => {
                        target
                            .checkpoint_begin(
                                1,
                                room,
                                transfer_id,
                                term,
                                base_revision,
                                revision,
                                length,
                                digest,
                            )
                            .await
                            .unwrap();
                    }
                    Event::CheckpointChunk {
                        transfer: transfer_id,
                        term,
                        base_revision,
                        revision,
                        offset,
                        data,
                        ..
                    } => {
                        target
                            .checkpoint_chunk(
                                1,
                                room,
                                transfer_id,
                                term,
                                base_revision,
                                revision,
                                offset,
                                data,
                            )
                            .await
                            .unwrap();
                        let Event::CheckpointAck { offset, .. } = target_rx.recv().await.unwrap()
                        else {
                            panic!("valid checkpoint chunk must receive cumulative credit")
                        };
                        source.checkpoint_ack(1, room, transfer_id, offset).unwrap();
                    }
                    Event::CheckpointEnd { .. } => break,
                    _ => panic!("unexpected checkpoint event before delayed marker"),
                }
            }
            assert_eq!(target.incoming_transfer.as_ref().unwrap().bytes, bytes);
            source.outgoing_transfer.as_mut().unwrap().started = tokio::time::Instant::now()
                - CHECKPOINT_TRANSFER_TIMEOUT
                - Duration::from_millis(1);
            source.expire_checkpoint_transfers();
            assert!(matches!(
                source_rx.recv().await,
                Some(Event::Error { code, .. }) if code == "checkpoint_send_timeout"
            ));

            target
                .checkpoint_begin(
                    1,
                    room,
                    transfer.transfer + 1,
                    transfer.term,
                    transfer.base_revision,
                    transfer.revision,
                    transfer.bytes.len() as u32,
                    transfer.digest_hex(),
                )
                .await
                .unwrap();
            assert!(matches!(
                target_rx.recv().await,
                Some(Event::Error { code, .. }) if code == "checkpoint_transfer_busy_incoming"
            ));
            assert_eq!(
                target.incoming_transfer.as_ref().unwrap().transfer,
                transfer.transfer,
                "a mismatched retry must not replace the retained body"
            );

            source.start_outgoing_checkpoint(transfer.clone());
            let Event::CheckpointBegin {
                transfer: transfer_id,
                term,
                base_revision,
                revision,
                length,
                digest,
                ..
            } = source_rx.recv().await.unwrap()
            else {
                panic!("retry must restart with its exact checkpoint begin")
            };
            target
                .checkpoint_begin(
                    1,
                    room,
                    transfer_id,
                    term,
                    base_revision,
                    revision,
                    length,
                    digest,
                )
                .await
                .unwrap();
            assert!(
                target_rx.try_recv().is_err(),
                "exact retry begin needs no credit"
            );

            loop {
                match source_rx.recv().await.unwrap() {
                    Event::CheckpointChunk {
                        transfer: transfer_id,
                        term,
                        base_revision,
                        revision,
                        offset,
                        data,
                        ..
                    } => {
                        target
                            .checkpoint_chunk(
                                1,
                                room,
                                transfer_id,
                                term,
                                base_revision,
                                revision,
                                offset,
                                data,
                            )
                            .await
                            .unwrap();
                        let Event::CheckpointAck { offset, .. } = target_rx.recv().await.unwrap()
                        else {
                            panic!("retained exact chunk must return its chunk-end credit")
                        };
                        source.checkpoint_ack(1, room, transfer_id, offset).unwrap();
                    }
                    Event::CheckpointEnd { length, .. } => {
                        assert_eq!(target.incoming_transfer.as_ref().unwrap().bytes, bytes);
                        source
                            .checkpoint_ack(1, room, transfer.transfer, length)
                            .unwrap();
                        break;
                    }
                    _ => panic!("unexpected checkpoint retry event"),
                }
            }
            assert!(matches!(
                source_rx.recv().await,
                Some(Event::CheckpointCommitted {
                    transfer: 19,
                    revision: 10,
                    ..
                })
            ));
            source_endpoint.close().await;
            target_endpoint.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn bulk_checkpoint_events_preserve_lifecycle_capacity() {
        let endpoint = endpoint().await;
        let (events, mut receiver) = mpsc::channel(LIFECYCLE_EVENT_RESERVE + 1);
        let mut actor = test_actor(endpoint.clone(), events);
        actor.epoch = 1;
        actor.room = Some([49; 16]);
        assert!(actor.emit_bulk(Event::CheckpointAck {
            epoch: 1,
            room: [49; 16],
            transfer: 1,
            offset: 0,
        }));
        assert!(!actor.emit_bulk(Event::CheckpointAck {
            epoch: 1,
            room: [49; 16],
            transfer: 1,
            offset: 1,
        }));
        actor.emit(Event::RoomClosed { epoch: 1 }).unwrap();
        assert!(matches!(
            receiver.recv().await,
            Some(Event::CheckpointAck { .. })
        ));
        assert!(matches!(
            receiver.recv().await,
            Some(Event::RoomClosed { epoch: 1 })
        ));
        endpoint.close().await;
    }

    #[tokio::test]
    async fn actor_replaces_authenticated_control_from_same_endpoint() {
        timeout(Duration::from_secs(15), async {
            let host = endpoint().await;
            let remote = endpoint().await;
            let relay = iroh::defaults::prod::default_relay_map()
                .urls::<Vec<_>>()
                .remove(0);
            let invite =
                Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600)
                    .unwrap();
            let room = invite.room();
            let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (_commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (_fault, failure) = watch::channel(false);
            let mut actor = test_actor(host.clone(), events_tx);
            actor.epoch = 1;
            actor.room = Some(room);
            actor.hosted = Some(invite.clone());
            actor.room_invite = Some(invite.clone());
            let service = tokio::spawn(async move {
                let result = actor.run(command_rx, failure).await;
                actor.clear_room();
                actor.tasks.shutdown().await;
                result
            });
            let _service_scope = TaskScope(vec![service.abort_handle()]);

            let first_connection = remote.connect(address(&host), CONTROL_ALPN).await.unwrap();
            let _first = transport::connect_control_on(first_connection, &invite)
                .await
                .unwrap();
            let _ = next(&mut events, "connected").await;

            // A clean helper Leave closes the old control at the remote end,
            // but QUIC close delivery can race the next Join. The replacement
            // is authenticated by the same endpoint identity and room proof,
            // so it must supersede the stale worker without waiting for a
            // close notification tick.
            let second_connection = remote.connect(address(&host), CONTROL_ALPN).await.unwrap();
            let mut second = transport::connect_control_on(second_connection, &invite)
                .await
                .unwrap();
            let _ = next(&mut events, "connected").await;
            second
                .sender
                .send(&ControlFrame {
                    message_id: 2,
                    payload: b"replacement control".to_vec(),
                })
                .await
                .unwrap();
            assert!(matches!(
                next(&mut events, "message").await,
                Event::Message { payload, .. } if payload == "replacement control"
            ));
            host.close().await;
            remote.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn rejoin_expiry_exception_requires_current_native_membership() {
        let local = endpoint().await;
        let remote = endpoint().await;
        let (events, _receiver) = mpsc::channel(128);
        let mut actor = test_actor(local.clone(), events);
        actor.room = Some([7; 16]);
        actor.remember_admission(Admission {
            room: [7; 16],
            incarnation: 9,
            authority_term: 1,
            coordination_endpoint: remote.id(),
            coordination_address: address(&remote),
            primary_endpoint: remote.id(),
        });
        assert_eq!(actor.control_rejoin_member(remote.id()), None);
        actor.committed_native_members = Some(BTreeSet::from([remote.id()]));
        assert_eq!(actor.control_rejoin_member(remote.id()), Some(9));
        assert_eq!(actor.control_rejoin_member(local.id()), None);
        actor.pending_retired_incarnations.insert(9);
        assert_eq!(actor.control_rejoin_member(remote.id()), None);
        actor.pending_retired_incarnations.clear();
        actor.retired_incarnations.insert(9);
        assert_eq!(actor.control_rejoin_member(remote.id()), None);
        actor.retired_incarnations.clear();
        actor.room = Some([8; 16]);
        assert_eq!(actor.control_rejoin_member(remote.id()), None);
        actor.room = Some([7; 16]);
        actor.committed_native_members.as_mut().unwrap().clear();
        assert_eq!(actor.control_rejoin_member(remote.id()), None);
        local.close().await;
        remote.close().await;
    }

    #[tokio::test]
    async fn incomplete_game_marker_does_not_block_actor_commands() {
        let local = endpoint().await;
        let remote = endpoint().await;
        let (client, server) = tokio::join!(remote.connect(address(&local), GAME_ALPN), async {
            local.accept().await.unwrap().await
        });
        let client = client.unwrap();
        let server = server.unwrap();
        let retired_connection = server.clone();
        let (events, mut receiver) = mpsc::channel(128);
        let mut actor = test_actor(local.clone(), events);
        actor.epoch = 7;
        actor.room = Some([71; 16]);
        actor.games.insert(
            remote.id(),
            GameSlot {
                auth: GameAuthorization {
                    peer: remote.id(),
                    key: crate::wire::MatchKey {
                        room: [71; 16],
                        generation: 3,
                    },
                    capability: [1; 32],
                    max_packet: 1024,
                },
                local_port: 9,
                waiting: true,
                task: None,
                stats: None,
                route_connection: None,
                expires: tokio::time::Instant::now() + Duration::from_secs(60),
                prepare_deadline: None,
            },
        );
        actor.probe_peers.insert(remote.id());
        actor.probe_permissions.insert(
            remote.id(),
            ProbePermission {
                request: 1,
                pair_revision: 1,
                expires: tokio::time::Instant::now() + Duration::from_secs(20),
            },
        );
        timeout(
            Duration::from_millis(200),
            actor.completed(Completion::Incoming(7, Ok(server))),
        )
        .await
        .expect("remote marker blocked the actor")
        .unwrap();
        assert!(
            actor
                .command(Request {
                    id: 17,
                    command: Command::Status
                })
                .unwrap()
        );
        assert!(matches!(
            receiver.recv().await,
            Some(Event::Status { request_id: 17, .. })
        ));
        assert!(
            actor
                .command(Request {
                    id: 19,
                    command: Command::EndMatch {
                        epoch: 7,
                        generation: 3
                    }
                })
                .unwrap()
        );
        assert!(actor.pending_game_admissions.is_empty());
        // Cancellation releases the preliminary connection immediately, so
        // a new match from this peer need not wait for the old deadline.
        let _ = actor.tasks.join_next().await;
        timeout(Duration::from_secs(1), client.closed())
            .await
            .unwrap();
        let (retry_client, retry_server) =
            tokio::join!(remote.connect(address(&local), GAME_ALPN), async {
                local.accept().await.unwrap().await
            });
        let retry_client = retry_client.unwrap();
        let retry_server = retry_server.unwrap();
        let retry_id = retry_server.stable_id();
        actor
            .completed(Completion::Incoming(7, Ok(retry_server)))
            .await
            .unwrap();
        actor
            .completed(Completion::ClassifiedGame(
                IncomingGame {
                    epoch: 7,
                    peer: remote.id(),
                    connection: retired_connection,
                    generation: Some(3),
                    probe: Some((1, 1)),
                    deadline: tokio::time::Instant::now() + Duration::from_secs(10),
                },
                Err(failed("old completion queued before cancellation")),
            ))
            .await
            .unwrap();
        assert_eq!(
            actor
                .pending_game_admissions
                .get(&remote.id())
                .unwrap()
                .0
                .stable_id(),
            retry_id
        );
        drop(retry_client);

        assert!(
            !actor
                .command(Request {
                    id: 18,
                    command: Command::Shutdown
                })
                .unwrap()
        );
        drop(client);
        local.close().await;
        remote.close().await;
    }

    #[tokio::test]
    async fn completed_probe_keeps_connection_open_for_gameplay_upgrade() {
        timeout(Duration::from_secs(15), async {
            let host = endpoint().await;
            let guest = endpoint().await;
            let room = [47; 16];
            let request = 9;
            let pair_revision = 23;
            let (host_probe, guest_connection) = tokio::join!(
                run_probe(
                    host.clone(),
                    address(&guest),
                    room,
                    guest.id(),
                    request,
                    pair_revision,
                    false,
                ),
                async {
                    let connection = guest.accept().await.unwrap().await.unwrap();
                    match transport::accept_game_stream(&connection).await.unwrap() {
                        transport::GameStream::Probe(send, recv) => {
                            serve_probe(connection, send, recv, room, request, pair_revision).await
                        }
                        transport::GameStream::Gameplay(_, _) => {
                            Err(failed("expected probe stream"))
                        }
                    }
                }
            );
            let host_probe = host_probe.unwrap();
            let host_connection = host_probe.connection.unwrap();
            let guest_connection = guest_connection.unwrap();
            assert!(host_connection.close_reason().is_none());
            assert!(guest_connection.close_reason().is_none());
            assert_eq!(
                recovery::summarize_datagram_probe(
                    &host_probe.samples_us,
                    host_probe.metrics.expected,
                    host_probe.metrics.sent
                )
                .status,
                "ready"
            );

            let key = MatchKey {
                room,
                generation: 1,
            };
            let host_auth = GameAuthorization {
                peer: guest.id(),
                key,
                capability: [71; 32],
                max_packet: 1024,
            };
            let guest_auth = GameAuthorization {
                peer: host.id(),
                ..host_auth.clone()
            };
            let (_deadline_sender, deadline) =
                watch::channel(tokio::time::Instant::now() + transport::PREPARED_GAME_TIMEOUT);
            let (host_game, guest_game) = tokio::join!(
                transport::accept_game_on(host_connection, host_auth, deadline),
                transport::connect_game_on(guest_connection, guest_auth),
            );
            assert!(host_game.is_ok());
            assert!(guest_game.is_ok());
            host.close().await;
            guest.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn closed_probe_invalidation_retries_after_bulk_backpressure() {
        timeout(Duration::from_secs(10), async {
            let local = endpoint().await;
            let remote = endpoint().await;
            let (connection, remote_connection) =
                tokio::join!(local.connect(address(&remote), GAME_ALPN), async {
                    remote.accept().await.unwrap().await
                });
            let connection = connection.unwrap();
            let remote_connection = remote_connection.unwrap();
            let original_route = selected_probe_route(&connection);
            assert_ne!(original_route, "unavailable");

            let (events, mut receiver) = mpsc::channel(LIFECYCLE_EVENT_RESERVE + 1);
            let mut actor = test_actor(local.clone(), events);
            actor.epoch = 7;
            actor.room = Some([59; 16]);
            actor.probe_reservations.insert(
                remote.id(),
                ProbeReservation {
                    reported: true,
                    connection: connection.clone(),
                    request: 23,
                    pair_revision: 37,
                    route: original_route.clone(),
                },
            );
            assert!(actor.emit_bulk(Event::CheckpointAck {
                epoch: 7,
                room: [59; 16],
                transfer: 1,
                offset: 0,
            }));

            remote_connection.close(1u32.into(), b"closed probe regression");
            connection.closed().await;
            assert!(connection.close_reason().is_some());
            assert_eq!(selected_probe_route(&connection), original_route);
            actor.invalidate_changed_probe_routes();
            assert!(!actor.probe_reservations.contains_key(&remote.id()));
            assert!(actor.pending_probe_invalidations.contains_key(&remote.id()));
            assert!(matches!(
                receiver.recv().await,
                Some(Event::CheckpointAck { .. })
            ));

            actor.invalidate_changed_probe_routes();
            assert!(!actor.pending_probe_invalidations.contains_key(&remote.id()));
            assert!(matches!(
                receiver.recv().await,
                Some(Event::ProbeResult {
                    peer,
                    request: 23,
                    pair_revision: 37,
                    route,
                    status,
                    ..
                }) if peer == remote.id() && route == "unavailable" && status == "invalidated"
            ));
            local.close().await;
            remote.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn obsolete_probe_completion_cannot_replace_newer_peer_state() {
        timeout(Duration::from_secs(10), async {
            let local = endpoint().await;
            let remote = endpoint().await;
            let (new_connection, new_remote) =
                tokio::join!(local.connect(address(&remote), GAME_ALPN), async {
                    remote.accept().await.unwrap().await
                });
            let (old_connection, old_remote) =
                tokio::join!(local.connect(address(&remote), GAME_ALPN), async {
                    remote.accept().await.unwrap().await
                });
            let new_connection = new_connection.unwrap();
            let old_connection = old_connection.unwrap();
            let old_observer = old_connection.clone();
            let (events, _receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let mut actor = test_actor(local.clone(), events);
            actor.epoch = 5;
            actor.room = Some([61; 16]);
            actor.probe_peers.insert(remote.id());
            actor.probe_permissions.insert(
                remote.id(),
                ProbePermission {
                    request: 29,
                    pair_revision: 41,
                    expires: tokio::time::Instant::now() + Duration::from_secs(5),
                },
            );
            actor.probe_reservations.insert(
                remote.id(),
                ProbeReservation {
                    reported: true,
                    route: selected_probe_route(&new_connection),
                    connection: new_connection,
                    request: 29,
                    pair_revision: 41,
                },
            );
            actor.pending_probe_invalidations.insert(
                remote.id(),
                PendingProbeInvalidation {
                    request: 29,
                    pair_revision: 41,
                    route: "newer route".into(),
                },
            );

            actor
                .completed(Completion::Probe(
                    5,
                    Ok(ProbeCompletion {
                        peer: remote.id(),
                        request: 11,
                        pair_revision: 17,
                        samples_us: Vec::new(),
                        connection: Some(old_connection),
                        report: false,
                        route_changed: false,
                        metrics: crate::probe::Metrics::default(),
                    }),
                ))
                .await
                .unwrap();

            assert!(actor.probe_peers.contains(&remote.id()));
            assert!(
                actor
                    .probe_permissions
                    .get(&remote.id())
                    .is_some_and(
                        |permission| permission.request == 29 && permission.pair_revision == 41
                    )
            );
            assert!(
                actor
                    .probe_reservations
                    .get(&remote.id())
                    .is_some_and(
                        |reservation| reservation.request == 29 && reservation.pair_revision == 41
                    )
            );
            assert!(
                actor
                    .pending_probe_invalidations
                    .get(&remote.id())
                    .is_some_and(|invalidation| invalidation.request == 29
                        && invalidation.pair_revision == 41)
            );
            old_observer.closed().await;
            assert!(old_observer.close_reason().is_some());
            drop(new_remote);
            drop(old_remote);
            local.close().await;
            remote.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn stable_voters_never_reselect_pending_or_retired_incarnations() {
        let local = endpoint().await;
        let (events, _events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(local.clone(), events);
        let room = [17; 16];
        for incarnation in 1..=4 {
            actor.admissions.insert(
                incarnation,
                Admission {
                    room,
                    incarnation,
                    authority_term: 1,
                    coordination_endpoint: local.id(),
                    coordination_address: address(&local),
                    primary_endpoint: local.id(),
                },
            );
            actor.admission_order.push(incarnation);
        }
        actor.pending_retired_incarnations.insert(2);
        actor.retired_incarnations.insert(3);

        assert_eq!(
            actor.stable_voters(BTreeSet::from([1, 2, 3]), 3),
            BTreeSet::from([1, 4])
        );
        local.close().await;
    }

    #[tokio::test]
    async fn native_rebound_uses_only_the_applied_unambiguous_incarnation() {
        let primary = endpoint().await;
        let old_coordination = endpoint().await;
        let fresh_coordination = endpoint().await;
        let room = [18; 16];
        let admission = |incarnation, coordination: &Endpoint| Admission {
            room,
            incarnation,
            authority_term: 1,
            coordination_endpoint: coordination.id(),
            coordination_address: address(coordination),
            primary_endpoint: primary.id(),
        };
        // Incarnation identifiers are random. The retired value is
        // deliberately numerically larger than the fresh value so collection
        // order would select the wrong process without applied-membership
        // filtering.
        let admissions = BTreeMap::from([
            (900, admission(900, &old_coordination)),
            (100, admission(100, &fresh_coordination)),
        ]);
        assert_eq!(
            applied_member_incarnations(
                &admissions,
                &BTreeSet::from([100]),
                &BTreeSet::from([900]),
            ),
            BTreeMap::from([(primary.id().to_string(), 100)])
        );
        assert!(
            applied_member_incarnations(
                &admissions,
                &BTreeSet::from([100, 900]),
                &BTreeSet::new(),
            )
            .is_empty()
        );
        primary.close().await;
        old_coordination.close().await;
        fresh_coordination.close().await;
    }

    #[tokio::test]
    async fn fresh_learner_accepts_membership_only_from_invitation_authority() {
        timeout(Duration::from_secs(20), async {
            let host_primary = endpoint().await;
            let guest_primary = endpoint().await;
            let unrelated_primary = endpoint().await;
            let relay = iroh::defaults::prod::default_relay_map()
                .urls::<Vec<_>>()
                .remove(0);
            let seed = Invite::create(
                host_primary.id(),
                relay,
                "test-build".into(),
                now().unwrap(),
                3600,
            )
            .unwrap();
            let room = seed.room();
            let host = crate::recovery::RecoverySession::host(room, host_primary.id(), false)
                .await
                .unwrap();
            let guest = crate::recovery::RecoverySession::join(
                room,
                guest_primary.id(),
                host.incarnation,
                host.coordination_address.clone(),
                false,
            )
            .await
            .unwrap();
            assert_eq!(guest.coordinator.current_leader(), None);
            assert_eq!(
                guest.applied_membership_provenance().await,
                (BTreeSet::new(), BTreeSet::new())
            );

            let host_admission = host.advertise().await;
            let guest_admission = guest.advertise().await;
            let (events, _events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let mut actor = test_actor(guest_primary.clone(), events);
            actor.epoch = 1;
            actor.room = Some(room);
            // The compact sf4e2/emd2 invitation intentionally carries no
            // coordination incarnation. Its authenticated Admission bootstrap
            // has already bound the host's process to invite.endpoint().
            let compact = Invite::parse_for_build(
                &seed.encode_discord().unwrap(),
                now().unwrap(),
                "test-build",
            )
            .unwrap();
            assert_eq!(compact.authority_incarnation(), 0);
            actor.room_invite = Some(compact);
            actor.recovery = Some(guest.clone());
            actor.remember_admission(host_admission.clone());
            actor.remember_admission(guest_admission.clone());
            let payload = serde_json::to_string(&CoordinationControl::Membership {
                admissions: vec![host_admission, guest_admission],
                retired: Vec::new(),
            })
            .unwrap();

            assert!(
                actor
                    .accept_coordination_control(unrelated_primary.id(), &payload)
                    .await
                    .is_err()
            );
            assert!(
                actor
                    .accept_coordination_control(host_primary.id(), &payload)
                    .await
                    .unwrap()
            );

            guest.stop().await;
            host.stop().await;
            host_primary.close().await;
            guest_primary.close().await;
            unrelated_primary.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn delayed_admission_cannot_readd_authenticated_removed_voter() {
        timeout(Duration::from_secs(25), async {
            let host_primary = endpoint().await;
            let departing_primary = endpoint().await;
            let room = [29; 16];
            let host = crate::recovery::RecoverySession::host(room, host_primary.id(), false)
                .await
                .unwrap();
            let departing = crate::recovery::RecoverySession::join(
                room,
                departing_primary.id(),
                host.incarnation,
                host.coordination_address.clone(),
                false,
            )
            .await
            .unwrap();
            let admission = departing.advertise().await;
            host.add_learner(&admission).await.unwrap();
            host.promote_voters(BTreeSet::from([host.incarnation, admission.incarnation]))
                .await
                .unwrap();

            // This is the service Admission precheck before its await. The
            // authenticated method-7 removal then commits in that window.
            let (precheck_members, precheck_history) = host.applied_membership_provenance().await;
            assert!(precheck_members.contains(&admission.incarnation));
            assert!(precheck_history.contains(&admission.incarnation));
            assert_eq!(
                host.coordinator
                    .dispatch(admission.incarnation, "remove", &[0])
                    .await
                    .unwrap(),
                vec![1]
            );
            let (members, history) = host.applied_membership_provenance().await;
            assert_eq!(members, BTreeSet::from([host.incarnation]));
            assert!(history.contains(&admission.incarnation));

            assert!(host.add_learner(&admission).await.is_err());
            assert!(host.rpc.is_retired(admission.incarnation).await);
            assert_eq!(
                host.applied_member_ids().await,
                BTreeSet::from([host.incarnation])
            );
            assert_eq!(
                host.applied_voter_ids().await,
                BTreeSet::from([host.incarnation])
            );

            departing.stop().await;
            host.stop().await;
            host_primary.close().await;
            departing_primary.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn successor_rejects_replayed_admission_from_applied_membership_history() {
        timeout(Duration::from_secs(20), async {
            let host = endpoint().await;
            let departed_primary = endpoint().await;
            let relay = iroh::defaults::prod::default_relay_map()
                .urls::<Vec<_>>()
                .remove(0);
            let seed = Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600)
                .unwrap();
            let room = seed.room();
            let (events, _events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let mut actor = test_actor(host.clone(), events);
            let invite = actor.setup_host_recovery(seed).await.unwrap();
            actor.epoch = 1;
            actor.room = Some(room);
            actor.hosted = Some(invite.clone());
            actor.room_invite = Some(invite.clone());
            let recovery = actor.recovery.clone().unwrap();
            let departed = crate::recovery::RecoverySession::join(
                room,
                departed_primary.id(),
                recovery.incarnation,
                recovery.coordination_address.clone(),
                false,
            )
            .await
            .unwrap();
            let admission = departed.advertise().await;
            recovery.add_learner(&admission).await.unwrap();
            recovery
                .remove_nodes(BTreeSet::from([admission.incarnation]))
                .await
                .unwrap();
            assert!(
                !recovery
                    .applied_member_ids()
                    .await
                    .contains(&admission.incarnation)
            );

            // Model a successor actor that receives both membership entries
            // before its one-second state tick. Its actor-local observation is
            // empty, while the replicated state machine has seen the member.
            actor.remember_admission(admission.clone());
            assert!(actor.applied_admission_members.is_empty());
            let payload = serde_json::to_string(&CoordinationControl::Admission {
                admission: admission.clone(),
            })
            .unwrap();
            assert!(
                actor
                    .accept_coordination_control(departed_primary.id(), &payload)
                    .await
                    .is_err()
            );
            assert!(
                actor
                    .pending_retired_incarnations
                    .contains(&admission.incarnation)
            );
            assert!(
                !recovery
                    .applied_member_ids()
                    .await
                    .contains(&admission.incarnation)
            );

            departed.stop().await;
            recovery.stop().await;
            host.close().await;
            departed_primary.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn retirement_cap_fences_rpc_and_admission_before_exact_history_saturates() {
        let host = endpoint().await;
        let departed_primary = endpoint().await;
        let relay = iroh::defaults::prod::default_relay_map()
            .urls::<Vec<_>>()
            .remove(0);
        let seed =
            Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600).unwrap();
        let room = seed.room();
        let (events, _events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(host.clone(), events);
        let _invite = actor.setup_host_recovery(seed).await.unwrap();
        let recovery = actor.recovery.clone().unwrap();
        let incarnation = u64::MAX - 1;
        let admission = Admission {
            room,
            incarnation,
            authority_term: 1,
            coordination_endpoint: departed_primary.id(),
            coordination_address: address(&departed_primary),
            primary_endpoint: departed_primary.id(),
        };
        recovery
            .rpc
            .admit_bound(
                incarnation,
                admission.coordination_address.clone(),
                admission.primary_endpoint,
            )
            .await
            .unwrap();
        actor.remember_admission(admission);
        actor
            .retired_incarnations
            .extend(1..=MAX_RETIRED_INCARNATIONS as u64);
        actor.pending_retired_incarnations.insert(incarnation);

        actor.apply_pending_retirements().await.unwrap();
        assert!(recovery.rpc.is_retired(incarnation).await);
        assert!(!actor.admissions.contains_key(&incarnation));
        assert!(!actor.admission_order.contains(&incarnation));
        assert!(actor.pending_retired_incarnations.contains(&incarnation));
        assert_eq!(actor.retired_incarnations.len(), MAX_RETIRED_INCARNATIONS);

        recovery.stop().await;
        host.close().await;
        departed_primary.close().await;
    }

    #[tokio::test]
    async fn held_checkpoint_and_admission_writes_do_not_block_actor_or_import_stale_completion() {
        timeout(Duration::from_secs(20), async {
            let host_primary = endpoint().await;
            let voter_primary = endpoint().await;
            let joining_primary = endpoint().await;
            let relay = iroh::defaults::prod::default_relay_map()
                .urls::<Vec<_>>()
                .remove(0);
            let seed = Invite::create(
                host_primary.id(),
                relay,
                "test-build".into(),
                now().unwrap(),
                3600,
            )
            .unwrap();
            let room = seed.room();
            let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let mut actor = test_actor(host_primary.clone(), events_tx);
            let invite = actor.setup_host_recovery(seed).await.unwrap();
            actor.epoch = 1;
            actor.room = Some(room);
            actor.hosted = Some(invite.clone());
            actor.room_invite = Some(invite.clone());
            let recovery = actor.recovery.clone().unwrap();

            let voter = crate::recovery::RecoverySession::join(
                room,
                voter_primary.id(),
                recovery.incarnation,
                recovery.coordination_address.clone(),
                false,
            )
            .await
            .unwrap();
            let voter_admission = voter.advertise().await;
            recovery.add_learner(&voter_admission).await.unwrap();
            recovery
                .promote_voters(BTreeSet::from([
                    recovery.incarnation,
                    voter.incarnation,
                ]))
                .await
                .unwrap();
            actor.remember_admission(voter_admission);

            let joining = crate::recovery::RecoverySession::join(
                room,
                joining_primary.id(),
                recovery.incarnation,
                recovery.coordination_address.clone(),
                false,
            )
            .await
            .unwrap();
            let joining_admission = joining.advertise().await;
            let connecting = async {
                let connection = joining_primary
                    .connect(address(&host_primary), CONTROL_ALPN)
                    .await
                    .unwrap();
                transport::connect_control_on(connection, &invite)
                    .await
                    .unwrap()
            };
            let accepting = async {
                let connection = host_primary.accept().await.unwrap().await.unwrap();
                transport::accept_control(connection, &invite).await.unwrap()
            };
            let (mut joining_control, host_control) = tokio::join!(connecting, accepting);
            actor.controls.insert(
                joining_primary.id(),
                ControlWorker::start(host_control),
            );

            // Hold the exact membership serialization lock and remove the
            // second voter. The checkpoint proposal now waits for quorum,
            // while an authenticated Admission waits behind this lock.
            let membership_gate = recovery.coordinator.membership_operations.lock().await;
            voter.stop().await;
            let term = recovery.coordinator.current_term();
            assert_eq!(recovery.coordinator.current_leader(), Some(recovery.incarnation));

            let (commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (_failed_tx, failed_rx) = watch::channel(false);
            let service = tokio::spawn(async move {
                let result = actor.run(command_rx, failed_rx).await;
                (actor, result)
            });
            let body = serde_json::to_vec(&serde_json::json!({
                "version": 1,
                "request": 41,
                "term": term,
                "base_revision": 0,
                "checkpoint": {"room": "held-quorum"},
                "effects": [],
                "effects_digest": "4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945"
            }))
            .unwrap();
            let transfer = CheckpointTransfer::new(room, 41, term, 0, 1, body.clone()).unwrap();
            let digest = transfer.digest_hex();
            let send = |id, command| Request { id, command };
            commands
                .send(send(
                    2,
                    Command::CheckpointBegin {
                        epoch: 1,
                        room,
                        transfer: 41,
                        term,
                        base_revision: 0,
                        revision: 1,
                        length: body.len() as u32,
                        digest: digest.clone(),
                    },
                ))
                .await
                .unwrap();
            commands
                .send(send(
                    3,
                    Command::CheckpointChunk {
                        epoch: 1,
                        room,
                        transfer: 41,
                        term,
                        base_revision: 0,
                        revision: 1,
                        offset: 0,
                        data: URL_SAFE_NO_PAD.encode(&body),
                    },
                ))
                .await
                .unwrap();
            commands
                .send(send(
                    4,
                    Command::CheckpointEnd {
                        epoch: 1,
                        room,
                        transfer: 41,
                        term,
                        base_revision: 0,
                        revision: 1,
                        length: body.len() as u32,
                        digest: digest.clone(),
                    },
                ))
                .await
                .unwrap();

            joining_control
                .sender
                .send(&ControlFrame {
                    message_id: 2,
                    payload: serde_json::to_vec(&CoordinationControl::Admission {
                        admission: joining_admission,
                    })
                    .unwrap(),
                })
                .await
                .unwrap();
            joining_control
                .sender
                .send(&ControlFrame {
                    message_id: 3,
                    payload: serde_json::to_vec(&NativeControlMessage {
                        kind: "native_control".into(),
                        message_id: 77,
                        payload: "lifecycle-after-held-admission".into(),
                    })
                    .unwrap(),
                })
                .await
                .unwrap();

            // An exact retry while the write waiter is held owns only a cursor
            // and still returns cumulative ACK credit through the actor loop.
            commands
                .send(send(
                    5,
                    Command::CheckpointBegin {
                        epoch: 1,
                        room,
                        transfer: 41,
                        term,
                        base_revision: 0,
                        revision: 1,
                        length: body.len() as u32,
                        digest: digest.clone(),
                    },
                ))
                .await
                .unwrap();
            commands
                .send(send(
                    6,
                    Command::CheckpointChunk {
                        epoch: 1,
                        room,
                        transfer: 41,
                        term,
                        base_revision: 0,
                        revision: 1,
                        offset: 0,
                        data: URL_SAFE_NO_PAD.encode(&body),
                    },
                ))
                .await
                .unwrap();
            commands
                .send(send(7, Command::Status))
                .await
                .unwrap();

            let deadline = tokio::time::Instant::now() + Duration::from_secs(6);
            let mut acknowledgements = 0;
            let mut saw_message = false;
            let mut saw_status = false;
            let mut saw_quorum_lost = false;
            while acknowledgements < 2 || !saw_message || !saw_status || !saw_quorum_lost {
                let event = tokio::select! {
                    event = events.recv() => event.unwrap(),
                    _ = tokio::time::sleep_until(deadline) => panic!("actor stalled behind coordination write"),
                };
                match event {
                    Event::CheckpointAck { transfer: 41, offset, .. } => {
                        assert_eq!(offset as usize, body.len());
                        acknowledgements += 1;
                    }
                    Event::Message { message_id: 77, payload, .. } => {
                        assert_eq!(payload, "lifecycle-after-held-admission");
                        saw_message = true;
                    }
                    Event::Status { request_id: 7, .. } => saw_status = true,
                    Event::CoordinationState { writable: false, .. } => {
                        saw_quorum_lost = true;
                    }
                    Event::Error { code, .. }
                        if code == "checkpoint_not_committed"
                            || code == "checkpoint_transfer_busy_proposal" => {}
                    _ => {}
                }
            }

            commands
                .send(send(8, Command::Shutdown))
                .await
                .unwrap();
            let (mut actor, result) = timeout(Duration::from_secs(1), service)
                .await
                .expect("responsive shutdown")
                .unwrap();
            result.unwrap();
            let stale = CheckpointProposalKey {
                epoch: 1,
                room,
                incarnation: recovery.incarnation,
                transfer: transfer.transfer,
                term: transfer.term,
                base_revision: transfer.base_revision,
                revision: transfer.revision,
                length: transfer.bytes.len(),
                digest: transfer.digest,
            };
            actor.pending_checkpoint_proposal = Some(stale.clone());
            actor.epoch = 2;
            actor
                .completed(Completion::CheckpointProposal(
                    stale,
                    transfer,
                    Ok(crate::coordination::Receipt {
                        accepted: true,
                        revision: 1,
                    }),
                ))
                .await
                .unwrap();
            assert!(actor.outgoing_transfer.is_none());
            assert_eq!(actor.last_exported_revision, 0);
            actor.tasks.abort_all();
            drop(membership_gate);
            joining.stop().await;
            recovery.stop().await;
            joining_primary.close().await;
            voter_primary.close().await;
            host_primary.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn overlapping_admissions_are_serialized_and_reach_current_voter_default() {
        timeout(Duration::from_secs(25), async {
            let host_primary = endpoint().await;
            let first_primary = endpoint().await;
            let second_primary = endpoint().await;
            let relay = iroh::defaults::prod::default_relay_map()
                .urls::<Vec<_>>()
                .remove(0);
            let seed = Invite::create(
                host_primary.id(),
                relay,
                "test-build".into(),
                now().unwrap(),
                3600,
            )
            .unwrap();
            let room = seed.room();
            let (events, _events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let mut actor = test_actor(host_primary.clone(), events);
            let invite = actor.setup_host_recovery(seed).await.unwrap();
            actor.epoch = 1;
            actor.room = Some(room);
            actor.hosted = Some(invite.clone());
            actor.room_invite = Some(invite);
            let recovery = actor.recovery.clone().unwrap();
            let first = crate::recovery::RecoverySession::join(
                room,
                first_primary.id(),
                recovery.incarnation,
                recovery.coordination_address.clone(),
                false,
            )
            .await
            .unwrap();
            let second = crate::recovery::RecoverySession::join(
                room,
                second_primary.id(),
                recovery.incarnation,
                recovery.coordination_address.clone(),
                false,
            )
            .await
            .unwrap();
            let first_admission = first.advertise().await;
            let second_admission = second.advertise().await;

            actor
                .queue_admission_operation(first_primary.id(), vec![first_admission.clone()], true)
                .unwrap();
            actor
                .queue_admission_operation(first_primary.id(), vec![first_admission], true)
                .unwrap();
            assert!(actor.deferred_admissions.is_empty());
            actor
                .queue_admission_operation(
                    second_primary.id(),
                    vec![second_admission.clone()],
                    true,
                )
                .unwrap();
            assert_eq!(actor.deferred_admissions.len(), 1);

            while !actor.admissions.contains_key(&first.incarnation)
                || !actor.admissions.contains_key(&second.incarnation)
                || recovery.applied_voter_ids().await.len() != 3
            {
                let completion = timeout(Duration::from_secs(10), actor.tasks.join_next())
                    .await
                    .expect("serialized admission completion")
                    .expect("admission worker exists")
                    .unwrap();
                actor.completed(completion).await.unwrap();
            }
            assert_eq!(
                recovery.applied_voter_ids().await,
                BTreeSet::from([recovery.incarnation, first.incarnation, second.incarnation])
            );
            actor.tasks.abort_all();
            first.stop().await;
            second.stop().await;
            recovery.stop().await;
            host_primary.close().await;
            first_primary.close().await;
            second_primary.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn async_admission_completion_publishes_third_member_to_existing_follower() {
        timeout(Duration::from_secs(25), async {
            let host_primary = endpoint().await;
            let follower_primary = endpoint().await;
            let newcomer_primary = endpoint().await;
            let relay = iroh::defaults::prod::default_relay_map()
                .urls::<Vec<_>>()
                .remove(0);
            let seed = Invite::create(
                host_primary.id(),
                relay,
                "test-build".into(),
                now().unwrap(),
                3600,
            )
            .unwrap();
            let room = seed.room();
            let (events, _events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let mut actor = test_actor(host_primary.clone(), events);
            let invite = actor.setup_host_recovery(seed).await.unwrap();
            actor.epoch = 1;
            actor.room = Some(room);
            actor.hosted = Some(invite.clone());
            actor.room_invite = Some(invite.clone());
            let recovery = actor.recovery.clone().unwrap();

            let follower = crate::recovery::RecoverySession::join(
                room,
                follower_primary.id(),
                recovery.incarnation,
                recovery.coordination_address.clone(),
                false,
            )
            .await
            .unwrap();
            let follower_admission = follower.advertise().await;
            recovery.add_learner(&follower_admission).await.unwrap();
            recovery
                .promote_voters(BTreeSet::from([recovery.incarnation, follower.incarnation]))
                .await
                .unwrap();
            actor.remember_admission(follower_admission);

            // This established follower route predates the third admission.
            // It therefore receives the new binding only if the asynchronous
            // Admission completion publishes the post-commit roster.
            let connecting = async {
                let connection = follower_primary
                    .connect(address(&host_primary), CONTROL_ALPN)
                    .await
                    .unwrap();
                transport::connect_control_on(connection, &invite)
                    .await
                    .unwrap()
            };
            let accepting = async {
                let connection = host_primary.accept().await.unwrap().await.unwrap();
                transport::accept_control(connection, &invite)
                    .await
                    .unwrap()
            };
            let (mut follower_control, host_control) = tokio::join!(connecting, accepting);
            actor
                .controls
                .insert(follower_primary.id(), ControlWorker::start(host_control));

            let newcomer = crate::recovery::RecoverySession::join(
                room,
                newcomer_primary.id(),
                recovery.incarnation,
                recovery.coordination_address.clone(),
                false,
            )
            .await
            .unwrap();
            let newcomer_admission = newcomer.advertise().await;
            actor
                .queue_admission_operation(
                    newcomer_primary.id(),
                    vec![newcomer_admission.clone()],
                    true,
                )
                .unwrap();

            while !actor.admissions.contains_key(&newcomer.incarnation) {
                let completion = timeout(Duration::from_secs(10), actor.tasks.join_next())
                    .await
                    .expect("third admission completion")
                    .expect("admission worker exists")
                    .unwrap();
                actor.completed(completion).await.unwrap();
            }

            let frame = timeout(Duration::from_secs(5), follower_control.receiver.receive())
                .await
                .expect("post-admission membership publication")
                .unwrap();
            let published: CoordinationControl = serde_json::from_slice(&frame.payload).unwrap();
            let CoordinationControl::Membership {
                admissions,
                retired,
            } = published
            else {
                panic!("expected membership publication");
            };
            assert!(retired.is_empty());
            assert!(admissions.iter().any(|admission| {
                admission.incarnation == newcomer.incarnation
                    && admission.primary_endpoint == newcomer_primary.id()
            }));
            assert!(admissions.iter().any(|admission| {
                admission.incarnation == follower.incarnation
                    && admission.primary_endpoint == follower_primary.id()
            }));
            assert!(actor.pending_membership_publications.is_empty());

            actor.controls.clear();
            actor.tasks.abort_all();
            newcomer.stop().await;
            follower.stop().await;
            recovery.stop().await;
            newcomer_primary.close().await;
            follower_primary.close().await;
            host_primary.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn expired_probe_authorization_completion_never_installs_or_dials() {
        let host_primary = endpoint().await;
        let peer_primary = endpoint().await;
        let relay = iroh::defaults::prod::default_relay_map()
            .urls::<Vec<_>>()
            .remove(0);
        let seed = Invite::create(
            host_primary.id(),
            relay,
            "test-build".into(),
            now().unwrap(),
            3600,
        )
        .unwrap();
        let room = seed.room();
        let (events, mut events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(host_primary.clone(), events);
        let _invite = actor.setup_host_recovery(seed).await.unwrap();
        actor.epoch = 1;
        actor.room = Some(room);
        let recovery = actor.recovery.clone().unwrap();
        let target_incarnation = u64::MAX - 9;
        actor.remember_admission(Admission {
            room,
            incarnation: target_incarnation,
            authority_term: recovery.coordinator.current_term(),
            coordination_endpoint: peer_primary.id(),
            coordination_address: address(&peer_primary),
            primary_endpoint: peer_primary.id(),
        });
        let term = recovery.coordinator.current_term();
        let checkpoint = serde_json::to_vec(&serde_json::json!({
            "version": 1,
            "request": 50,
            "term": term,
            "base_revision": 0,
            "checkpoint": {
                "members": [
                    {"member": 1, "incarnation": recovery.incarnation, "data": {"authenticatedEndpoint": host_primary.id().to_string()}},
                    {"member": 2, "incarnation": target_incarnation, "data": {"authenticatedEndpoint": peer_primary.id().to_string()}}
                ],
                "room": {
                    "version": 2,
                    "snapshot": {
                        "members": [{"id": 1, "fighter": 3}, {"id": 2, "fighter": 7}],
                        "tables": [{"revision": 7, "p1": 1, "p2": 2}]
                    }
                }
            },
            "effects": [],
            "effects_digest": "4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945"
        }))
        .unwrap();
        let transfer = CheckpointTransfer::new(room, 50, term, 0, 1, checkpoint).unwrap();
        let receipt = recovery
            .coordinator
            .propose(recovery.propose(&transfer).unwrap())
            .await
            .unwrap();
        assert!(receipt.accepted);
        assert!(
            recovery
                .probe_pair_bound(
                    recovery.incarnation,
                    target_incarnation,
                    host_primary.id(),
                    peer_primary.id(),
                    7,
                )
                .await,
            "every completion fence except wall-clock expiry must be valid"
        );
        let key = ProbeAuthorizationKey {
            epoch: 1,
            room,
            incarnation: recovery.incarnation,
            peer: peer_primary.id(),
            target_incarnation,
            request: 51,
            pair_revision: 7,
            benchmark: false,
        };
        actor
            .pending_probe_authorizations
            .insert(peer_primary.id(), key.clone());
        actor.probe_peers.insert(peer_primary.id());
        let task_count = actor.tasks.len();
        actor
            .completed(Completion::ProbeAuthorization(
                key,
                Ok(ProbeAuthorization {
                    term: recovery.coordinator.current_term(),
                    leader: recovery.coordinator.current_leader(),
                    revision: recovery.committed().await.revision,
                    expires: now().unwrap().saturating_sub(1),
                }),
            ))
            .await
            .unwrap();
        assert!(!actor.probe_permissions.contains_key(&peer_primary.id()));
        assert!(!actor.probe_peers.contains(&peer_primary.id()));
        assert_eq!(actor.tasks.len(), task_count);
        assert!(matches!(
            events_rx.recv().await,
            Some(Event::Error { code, .. }) if code == "probe_unavailable"
        ));
        recovery.stop().await;
        host_primary.close().await;
        peer_primary.close().await;
    }

    #[tokio::test]
    async fn service_replays_committed_checkpoint_to_each_native_owner() {
        timeout(Duration::from_secs(35), async {
            let host = endpoint().await;
            let guest = endpoint().await;
            let relay = iroh::defaults::prod::default_relay_map()
                .urls::<Vec<_>>()
                .remove(0);
            let seed = Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600)
                .unwrap();
            let room = seed.room();
            let (host_events_tx, mut host_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (host_commands, host_command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (guest_events_tx, mut guest_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (guest_commands, guest_command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (_host_fault, host_failure) = watch::channel(false);
            let (_guest_fault, guest_failure) = watch::channel(false);
            let make_actor = |endpoint: Endpoint, events: mpsc::Sender<Event>| Actor {
                endpoint,
                relay_only: false,
                epoch: 0,
                opening: false,
                room: None,
                hosted: None,
                room_invite: None,
                host_address: None,
                controls: BTreeMap::new(),
                games: BTreeMap::new(),
                closed_generation: 0,
                tasks: JoinSet::new(),
                events,
                recovery: None,
                admissions: BTreeMap::new(),
                admission_order: Vec::new(),
                applied_admission_members: BTreeSet::new(),
                incoming_transfer: None,
                pending_checkpoint_proposal: None,
                pending_checkpoint_retry: None,
                outgoing_transfer: None,
                committed_native_members: None,
                pending_retired_incarnations: BTreeSet::new(),
                retired_incarnations: BTreeSet::new(),
                last_exported_revision: 0,
                pending_checkpoint_ack: None,
                pending_checkpoint_committed: None,
                next_transport_message: TRANSPORT_MESSAGE_ID_BASE,
                next_coordination_operation: 1,
                pending_coordination_refresh: None,
                pending_membership_operation: None,
                pending_admission_operation: None,
            deferred_admissions: VecDeque::new(),
            pending_membership_publications: BTreeSet::new(),
                last_coordination_state: None,
                coordination_writable: false,
                last_control_rebound: None,
                unwritable_leader_since: None,
                reconnect_target: None,
                probe_reservations: BTreeMap::new(),
                pending_game_admissions: BTreeMap::new(),
                pending_probe_invalidations: BTreeMap::new(),
                probe_peers: BTreeSet::new(),
                probe_permissions: BTreeMap::new(),
                pending_probe_authorizations: BTreeMap::new(),
                retirement_started: None,
            };
            let mut host_actor = make_actor(host.clone(), host_events_tx);
            let invite = host_actor.setup_host_recovery(seed).await.unwrap();
            host_actor.epoch = 1;
            host_actor.room = Some(room);
            host_actor.hosted = Some(invite.clone());
            let host_service = tokio::spawn(async move {
                let result = host_actor.run(host_command_rx, host_failure).await;
                host_actor.clear_room();
                host_actor.tasks.shutdown().await;
                result
            });
            let mut guest_actor = make_actor(guest.clone(), guest_events_tx);
            // Use an explicit loopback connection for this deterministic
            // service fixture; production Join still uses the authenticated
            // invite address (relay or direct route).
            guest_actor.epoch = 1;
            guest_actor.opening = true;
            guest_actor.room = Some(room);
            guest_actor.host_address = Some(address(&host));
            let guest_connection = guest
                .connect(address(&host), CONTROL_ALPN)
                .await
                .unwrap();
            let guest_control = transport::connect_control_on(guest_connection, &invite)
                .await
                .unwrap();
            guest_actor
                .completed(Completion::GuestControl(1, invite.clone(), Ok(guest_control)))
                .await
                .unwrap();
            let guest_service = tokio::spawn(async move {
                let result = guest_actor.run(guest_command_rx, guest_failure).await;
                guest_actor.clear_room();
                guest_actor.tasks.shutdown().await;
                result
            });
            let _services = TaskScope(vec![
                host_service.abort_handle(),
                guest_service.abort_handle(),
            ]);
            // Wait until the host has both authenticated control routes before
            // submitting a proposal. This exercises the live admission path,
            // rather than a standalone Coordinator fixture.
            let mut term = 0;
            let mut host_has_guest_route = false;
            while !host_has_guest_route || term == 0 {
                match host_events.recv().await.unwrap() {
                    Event::CoordinationState {
                        term: state_term,
                        writable: true,
                        leader_local: true,
                        ..
                    } => term = term.max(state_term),
                    Event::ControlRebound { members, .. } if members.len() >= 2 => {
                        host_has_guest_route = true;
                    }
                    Event::Error { code, .. } => panic!("host helper error: {code}"),
                    _ => {}
                }
            }
            guest_commands
                .send(Request {
                    id: 10,
                    command: Command::Send {
                        epoch: 1,
                        peer: host.id(),
                        message_id: 71,
                        payload: "cpp hello alongside admission".into(),
                    },
                })
                .await
                .unwrap();
            loop {
                match host_events.recv().await.unwrap() {
                    Event::Message {
                        message_id,
                        payload,
                        ..
                    } => {
                        assert_eq!(message_id, 71);
                        assert_eq!(payload, "cpp hello alongside admission");
                        break;
                    }
                    Event::Error { code, .. } => panic!("host helper error: {code}"),
                    _ => {}
                }
            }
            tokio::time::sleep(Duration::from_millis(500)).await;
            let body = serde_json::json!({
                "version": 1,
                "request": 7,
                "term": term,
                "base_revision": 0,
                "checkpoint": {"room": "recovery-test"},
                "effects": [],
                "effects_digest": "4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945"
            });
            let bytes = serde_json::to_vec(&body).unwrap();
            let digest = recovery::hex_digest(&recovery::sha256(&bytes));
            host_commands
                .send(Request {
                    id: 2,
                    command: Command::CheckpointBegin {
                        epoch: 1,
                        room,
                        transfer: 7,
                        term,
                        base_revision: 0,
                        revision: 1,
                        length: bytes.len() as u32,
                        digest: digest.clone(),
                    },
                })
                .await
                .unwrap();
            for (offset, chunk) in bytes.chunks(CHECKPOINT_CHUNK_BYTES).enumerate() {
                let offset = offset * CHECKPOINT_CHUNK_BYTES;
                host_commands
                    .send(Request {
                        id: 3 + offset as u64,
                        command: Command::CheckpointChunk {
                            epoch: 1,
                            room,
                            transfer: 7,
                            term,
                            base_revision: 0,
                            revision: 1,
                            offset: offset as u32,
                            data: URL_SAFE_NO_PAD.encode(chunk),
                        },
                    })
                    .await
                    .unwrap();
            }
            host_commands
                .send(Request {
                    id: 4,
                    command: Command::CheckpointEnd {
                        epoch: 1,
                        room,
                        transfer: 7,
                        term,
                        base_revision: 0,
                        revision: 1,
                        length: bytes.len() as u32,
                        digest,
                    },
                })
                .await
                .unwrap();

            let deadline = tokio::time::Instant::now() + Duration::from_secs(15);
            let mut guest_committed = false;
            let mut guest_started = false;
            while !guest_committed {
                tokio::select! {
                    event = host_events.recv() => match event.unwrap() {
                        Event::CheckpointBegin { transfer, .. } => {
                            host_commands.send(Request { id: 20, command: Command::CheckpointAck { epoch: 1, room, transfer, offset: 0 } }).await.unwrap();
                        }
                        Event::CheckpointChunk { transfer, offset, data, .. } => {
                            let next = offset + URL_SAFE_NO_PAD.decode(data).unwrap().len() as u32;
                            host_commands.send(Request { id: 21, command: Command::CheckpointAck { epoch: 1, room, transfer, offset: next } }).await.unwrap();
                        }
                        Event::CheckpointEnd { transfer, length, .. } => {
                            host_commands.send(Request { id: 22, command: Command::CheckpointAck { epoch: 1, room, transfer, offset: length } }).await.unwrap();
                        }
                        Event::Error { code, .. } => panic!("host helper error: {code}"),
                        _ => {},
                    },
                    event = guest_events.recv() => match event.unwrap() {
                        Event::CheckpointBegin { transfer, .. } => {
                            guest_started = true;
                            guest_commands.send(Request { id: 30, command: Command::CheckpointAck { epoch: 1, room, transfer, offset: 0 } }).await.unwrap();
                        }
                        Event::CheckpointChunk { transfer, offset, data, .. } => {
                            let next = offset + URL_SAFE_NO_PAD.decode(data).unwrap().len() as u32;
                            guest_commands.send(Request { id: 31, command: Command::CheckpointAck { epoch: 1, room, transfer, offset: next } }).await.unwrap();
                        }
                        Event::CheckpointEnd { transfer, length, .. } => {
                            guest_commands.send(Request { id: 32, command: Command::CheckpointAck { epoch: 1, room, transfer, offset: length } }).await.unwrap();
                        }
                        Event::CheckpointCommitted { transfer, revision, .. } => {
                            assert_eq!(transfer, 7);
                            assert_eq!(revision, 1);
                            guest_committed = true;
                        }
                        Event::Error { code, .. } => panic!("guest helper error: {code}"),
                        _ => {},
                    },
                    _ = tokio::time::sleep_until(deadline) => panic!("committed checkpoint was not replayed"),
                }
            }
            assert!(guest_started);
            host.close().await;
            guest.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn closing_room_does_not_poison_new_room_on_same_endpoint() {
        let host = endpoint().await;
        let relay = iroh::defaults::prod::default_relay_map()
            .urls::<Vec<_>>()
            .remove(0);
        let create = || {
            Invite::create(
                host.id(),
                relay.clone(),
                "test-build".into(),
                now().unwrap(),
                3600,
            )
            .unwrap()
        };
        let old = create();
        let replacement = create();
        assert_eq!(old.endpoint(), replacement.endpoint());
        assert_ne!(old.room(), replacement.room());
        assert!(replacement.admit(&old.proof(), now().unwrap()).is_err());
        replacement
            .admit(
                &Invite::parse(&replacement.encode().unwrap(), now().unwrap())
                    .unwrap()
                    .proof(),
                now().unwrap(),
            )
            .unwrap();
        let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = Actor {
            endpoint: host.clone(),
            relay_only: false,
            epoch: 1,
            opening: false,
            room: Some(old.room()),
            hosted: Some(old.clone()),
            room_invite: Some(old.clone()),
            host_address: None,
            controls: BTreeMap::new(),
            games: BTreeMap::new(),
            closed_generation: 99,
            tasks: JoinSet::new(),
            events: events_tx,
            recovery: None,
            admissions: BTreeMap::new(),
            admission_order: Vec::new(),
            applied_admission_members: BTreeSet::new(),
            incoming_transfer: None,
            pending_checkpoint_proposal: None,
            pending_checkpoint_retry: None,
            outgoing_transfer: None,
            committed_native_members: None,
            pending_retired_incarnations: BTreeSet::new(),
            retired_incarnations: BTreeSet::new(),
            last_exported_revision: 0,
            pending_checkpoint_ack: None,
            pending_checkpoint_committed: None,
            next_transport_message: TRANSPORT_MESSAGE_ID_BASE,
            next_coordination_operation: 1,
            pending_coordination_refresh: None,
            pending_membership_operation: None,
            pending_admission_operation: None,
            deferred_admissions: VecDeque::new(),
            pending_membership_publications: BTreeSet::new(),
            last_coordination_state: None,
            coordination_writable: false,
            last_control_rebound: None,
            unwritable_leader_since: None,
            reconnect_target: None,
            probe_reservations: BTreeMap::new(),
            pending_game_admissions: BTreeMap::new(),
            pending_probe_invalidations: BTreeMap::new(),
            probe_peers: BTreeSet::new(),
            probe_permissions: BTreeMap::new(),
            pending_probe_authorizations: BTreeMap::new(),
            retirement_started: None,
        };
        actor
            .command(Request {
                id: 1,
                command: Command::Leave {
                    epoch: 1,
                    abandon: false,
                },
            })
            .unwrap();
        let _ = next(&mut events, "room_closed").await;
        assert!(actor.begin(2, "test-build"));
        actor
            .completed(Completion::Hosted(2, Ok(replacement.clone())))
            .await
            .unwrap();
        let _ = next(&mut events, "hosted").await;
        let _ = next(&mut events, "discord_invite").await;
        // An old leave request and a late old-host completion cannot close or
        // replace the new room, even though its endpoint is unchanged.
        actor
            .command(Request {
                id: 2,
                command: Command::Leave {
                    epoch: 1,
                    abandon: false,
                },
            })
            .unwrap();
        assert!(
            matches!(events.recv().await, Some(Event::Error { code, .. }) if code == "stale_epoch")
        );
        actor
            .completed(Completion::Hosted(1, Ok(old.clone())))
            .await
            .unwrap();
        assert_eq!(actor.room, Some(replacement.room()));
        assert_eq!(actor.closed_generation, 0);
        actor
            .hosted
            .as_ref()
            .unwrap()
            .admit(&replacement.proof(), now().unwrap())
            .unwrap();
        assert!(
            actor
                .hosted
                .as_ref()
                .unwrap()
                .admit(&old.proof(), now().unwrap())
                .is_err()
        );
        actor.clear_room();
        host.close().await;
    }

    #[tokio::test]
    async fn actor_routes_cpp_control_and_keeps_gameplay_alive_when_control_closes() {
        timeout(Duration::from_secs(25), async {
            let host = endpoint().await;
            let remote = endpoint().await;
            let relay = iroh::defaults::prod::default_relay_map()
                .urls::<Vec<_>>()
                .remove(0);
            let invite =
                Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600)
                    .unwrap();
            let room = invite.room();
            let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (_fault, failure) = watch::channel(false);
            let mut actor = Actor {
                endpoint: host.clone(),
                relay_only: false,
                epoch: 1,
                opening: false,
                room: Some(room),
                hosted: Some(invite.clone()),
                room_invite: Some(invite.clone()),
                host_address: None,
                controls: BTreeMap::new(),
                games: BTreeMap::new(),
                closed_generation: 0,
                tasks: JoinSet::new(),
                events: events_tx,
                recovery: None,
                admissions: BTreeMap::new(),
                admission_order: Vec::new(),
                applied_admission_members: BTreeSet::new(),
                incoming_transfer: None,
                pending_checkpoint_proposal: None,
                pending_checkpoint_retry: None,
                outgoing_transfer: None,
                committed_native_members: None,
                pending_retired_incarnations: BTreeSet::new(),
                retired_incarnations: BTreeSet::new(),
                last_exported_revision: 0,
                pending_checkpoint_ack: None,
                pending_checkpoint_committed: None,
                next_transport_message: TRANSPORT_MESSAGE_ID_BASE,
                next_coordination_operation: 1,
                pending_coordination_refresh: None,
                pending_membership_operation: None,
                pending_admission_operation: None,
                deferred_admissions: VecDeque::new(),
                pending_membership_publications: BTreeSet::new(),
                last_coordination_state: None,
                coordination_writable: false,
                last_control_rebound: None,
                unwritable_leader_since: None,
                reconnect_target: None,
                probe_reservations: BTreeMap::new(),
                pending_game_admissions: BTreeMap::new(),
                pending_probe_invalidations: BTreeMap::new(),
                probe_peers: BTreeSet::new(),
                probe_permissions: BTreeMap::new(),
                pending_probe_authorizations: BTreeMap::new(),
                retirement_started: None,
            };
            let service = tokio::spawn(async move {
                let result = actor.run(command_rx, failure).await;
                actor.clear_room();
                actor.tasks.shutdown().await;
                result
            });
            let _service_scope = TaskScope(vec![service.abort_handle()]);
            let connection = remote.connect(address(&host), CONTROL_ALPN).await.unwrap();
            let mut control = transport::connect_control_on(connection, &invite)
                .await
                .unwrap();
            let _ = next(&mut events, "connected").await;
            control
                .sender
                .send(&ControlFrame {
                    message_id: 42,
                    payload: br#"{"type":"lobby_ready"}"#.to_vec(),
                })
                .await
                .unwrap();
            match next(&mut events, "message").await {
                Event::Message {
                    epoch,
                    peer,
                    message_id,
                    payload,
                } => {
                    assert_eq!(epoch, 1);
                    assert_eq!(peer, remote.id());
                    assert_eq!(message_id, 42);
                    assert_eq!(payload, r#"{"type":"lobby_ready"}"#);
                }
                _ => unreachable!(),
            }
            commands
                .send(Request {
                    id: 2,
                    command: Command::Send {
                        epoch: 1,
                        peer: remote.id(),
                        message_id: 51,
                        payload: "cpp authoritative reply".into(),
                    },
                })
                .await
                .unwrap();
            let native_frame = control.receiver.receive().await.unwrap();
            let native: NativeControlMessage =
                serde_json::from_slice(&native_frame.payload).unwrap();
            assert_eq!(native.kind, "native_control");
            assert_eq!(native.message_id, 51);
            assert_eq!(native.payload, "cpp authoritative reply");
            let _ = next(&mut events, "sent").await;

            let local_host = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap();
            let local_remote = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap();
            commands
                .send(Request {
                    id: 3,
                    command: Command::PrepareGame {
                        epoch: 1,
                        peer: remote.id(),
                        room,
                        generation: 1,
                        capability: [8; 32],
                        local_port: local_host.local_addr().unwrap().port(),
                        max_packet: 1024,
                        dial: false,
                    },
                })
                .await
                .unwrap();
            let _ = next(&mut events, "game_waiting").await;
            let auth = GameAuthorization {
                peer: host.id(),
                key: MatchKey {
                    room,
                    generation: 1,
                },
                capability: [8; 32],
                max_packet: 1024,
            };
            let game = transport::connect_game(&remote, address(&host), auth)
                .await
                .unwrap();
            let virtual_host = match next(&mut events, "game_ready").await {
                Event::GameReady { virtual_port, .. } => virtual_port,
                _ => unreachable!(),
            };
            let remote_bridge = Bridge::bind(game, local_remote.local_addr().unwrap())
                .await
                .unwrap();
            let (stop, stop_rx) = watch::channel(false);
            let bridge = tokio::spawn(remote_bridge.run(stop_rx));
            let _bridge_scope = TaskScope(vec![bridge.abort_handle()]);
            commands
                .send(Request {
                    id: 4,
                    command: Command::CloseControl {
                        epoch: 1,
                        peer: remote.id(),
                    },
                })
                .await
                .unwrap();
            let _ = next(&mut events, "control_closed").await;
            control.connection.closed().await;
            local_host
                .send_to(b"fight continues", (Ipv4Addr::LOCALHOST, virtual_host))
                .await
                .unwrap();
            let mut buffer = [0; 64];
            let count = local_remote.recv(&mut buffer).await.unwrap();
            assert_eq!(&buffer[..count], b"fight continues");
            commands
                .send(Request {
                    id: 5,
                    command: Command::EndMatch {
                        epoch: 1,
                        generation: 1,
                    },
                })
                .await
                .unwrap();
            let _ = next(&mut events, "game_closed").await;
            let _ = stop.send(true);
            let _ = bridge.await.unwrap();
            // A retired spectator/game edge is independently removable and
            // does not require ending the room's other control links.
            commands
                .send(Request {
                    id: 6,
                    command: Command::PrepareGame {
                        epoch: 1,
                        peer: remote.id(),
                        room,
                        generation: 2,
                        capability: [9; 32],
                        local_port: local_host.local_addr().unwrap().port(),
                        max_packet: 1024,
                        dial: false,
                    },
                })
                .await
                .unwrap();
            let _ = next(&mut events, "game_waiting").await;
            commands
                .send(Request {
                    id: 7,
                    command: Command::EndPeer {
                        epoch: 1,
                        peer: remote.id(),
                        generation: 2,
                    },
                })
                .await
                .unwrap();
            let _ = next(&mut events, "game_closed").await;
            commands
                .send(Request {
                    id: 8,
                    command: Command::PrepareGame {
                        epoch: 1,
                        peer: remote.id(),
                        room,
                        generation: 1,
                        capability: [8; 32],
                        local_port: local_host.local_addr().unwrap().port(),
                        max_packet: 1024,
                        dial: false,
                    },
                })
                .await
                .unwrap();
            match next(&mut events, "error").await {
                Event::Error { code, .. } => assert_eq!(code, "invalid_game_registration"),
                _ => unreachable!(),
            }
            commands
                .send(Request {
                    id: 9,
                    command: Command::Leave {
                        epoch: 1,
                        abandon: false,
                    },
                })
                .await
                .unwrap();
            let _ = next(&mut events, "room_closed").await;
            // Cancellation works while Host awaits relay readiness, with no
            // network work performed by the IPC reader or the game thread.
            commands
                .send(Request {
                    id: 10,
                    command: Command::Host {
                        epoch: 2,
                        build: "test-build".into(),
                    },
                })
                .await
                .unwrap();
            commands
                .send(Request {
                    id: 11,
                    command: Command::Leave {
                        epoch: 2,
                        abandon: false,
                    },
                })
                .await
                .unwrap();
            let _ = next(&mut events, "room_closed").await;
            commands
                .send(Request {
                    id: 12,
                    command: Command::Shutdown,
                })
                .await
                .unwrap();
            service.await.unwrap().unwrap();
            host.close().await;
            remote.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn actor_admits_full_sixteen_member_room_and_fifteen_game_links() {
        timeout(Duration::from_secs(60), async {
            // The host plus fifteen helpers is the transport shape of a
            // sixteen-member room. One gameplay endpoint may represent the
            // second fighter and fourteen admitted spectators for a table.
            assert_eq!(MAX_CONTROL_PEERS, 15);
            assert_eq!(MAX_GAME_LINKS, 15);
            const _: () = assert!(MAX_TASKS >= MAX_CONTROL_PEERS + MAX_GAME_LINKS);

            let host = endpoint().await;
            let relay = iroh::defaults::prod::default_relay_map()
                .urls::<Vec<_>>()
                .remove(0);
            let invite = Invite::create(
                host.id(),
                relay,
                "full-room-build".into(),
                now().unwrap(),
                3600,
            )
            .unwrap();
            let room = invite.room();
            let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (_fault, failure) = watch::channel(false);
            let mut actor = Actor {
                endpoint: host.clone(),
                relay_only: false,
                epoch: 1,
                opening: false,
                room: Some(room),
                hosted: Some(invite.clone()),
                room_invite: Some(invite.clone()),
                host_address: None,
                controls: BTreeMap::new(),
                games: BTreeMap::new(),
                closed_generation: 0,
                tasks: JoinSet::new(),
                events: events_tx,
                recovery: None,
                admissions: BTreeMap::new(),
                admission_order: Vec::new(),
                applied_admission_members: BTreeSet::new(),
                incoming_transfer: None,
                pending_checkpoint_proposal: None,
                pending_checkpoint_retry: None,
                outgoing_transfer: None,
                committed_native_members: None,
                pending_retired_incarnations: BTreeSet::new(),
                retired_incarnations: BTreeSet::new(),
                last_exported_revision: 0,
                pending_checkpoint_ack: None,
                pending_checkpoint_committed: None,
                next_transport_message: TRANSPORT_MESSAGE_ID_BASE,
                next_coordination_operation: 1,
                pending_coordination_refresh: None,
                pending_membership_operation: None,
                pending_admission_operation: None,
                deferred_admissions: VecDeque::new(),
                pending_membership_publications: BTreeSet::new(),
                last_coordination_state: None,
                coordination_writable: false,
                last_control_rebound: None,
                unwritable_leader_since: None,
                reconnect_target: None,
                probe_reservations: BTreeMap::new(),
                pending_game_admissions: BTreeMap::new(),
                pending_probe_invalidations: BTreeMap::new(),
                probe_peers: BTreeSet::new(),
                probe_permissions: BTreeMap::new(),
                pending_probe_authorizations: BTreeMap::new(),
                retirement_started: None,
            };
            let service = tokio::spawn(async move {
                let result = actor.run(command_rx, failure).await;
                actor.clear_room();
                actor.tasks.shutdown().await;
                result
            });
            let _service_scope = TaskScope(vec![service.abort_handle()]);

            let mut remotes = Vec::with_capacity(MAX_CONTROL_PEERS);
            let mut controls = Vec::with_capacity(MAX_CONTROL_PEERS);
            for _ in 0..MAX_CONTROL_PEERS {
                let remote = endpoint().await;
                let connection = remote.connect(address(&host), CONTROL_ALPN).await.unwrap();
                let control = transport::connect_control_on(connection, &invite)
                    .await
                    .unwrap();
                remotes.push(remote);
                controls.push(control);
                match next(&mut events, "connected").await {
                    Event::Connected { .. } => (),
                    _ => unreachable!(),
                }
            }
            assert_eq!(controls.len(), 15);

            // The seventeenth endpoint (host plus sixteen remotes) is
            // rejected without disturbing any admitted control stream.
            let overflow = endpoint().await;
            let overflow_connection = overflow
                .connect(address(&host), CONTROL_ALPN)
                .await
                .unwrap();
            assert!(
                transport::connect_control_on(overflow_connection, &invite)
                    .await
                    .is_err()
            );

            // Reserve unique local UDP ports for the host-side bridges. The
            // sockets are released before each bridge is authorized.
            let mut local_ports = Vec::with_capacity(MAX_GAME_LINKS);
            for _ in 0..MAX_GAME_LINKS {
                let socket = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap();
                local_ports.push(socket.local_addr().unwrap().port());
                drop(socket);
            }
            let mut games = Vec::with_capacity(MAX_GAME_LINKS);
            for (index, remote) in remotes.iter().enumerate() {
                let peer = remote.id();
                commands
                    .send(Request {
                        id: 100 + index as u64,
                        command: Command::PrepareGame {
                            epoch: 1,
                            peer,
                            room,
                            generation: 1,
                            capability: [index as u8 + 1; 32],
                            local_port: local_ports[index],
                            max_packet: 1024,
                            dial: false,
                        },
                    })
                    .await
                    .unwrap();
                let _ = next(&mut events, "game_waiting").await;
                let auth = GameAuthorization {
                    peer: host.id(),
                    key: MatchKey {
                        room,
                        generation: 1,
                    },
                    capability: [index as u8 + 1; 32],
                    max_packet: 1024,
                };
                games.push(
                    transport::connect_game(remote, address(&host), auth)
                        .await
                        .unwrap(),
                );
                let _ = next(&mut events, "game_ready").await;
            }
            assert_eq!(games.len(), MAX_GAME_LINKS);

            // The gameplay-link budget is independent and equally strict.
            commands
                .send(Request {
                    id: 190,
                    command: Command::PrepareGame {
                        epoch: 1,
                        peer: overflow.id(),
                        room,
                        generation: 1,
                        capability: [77; 32],
                        local_port: local_ports[0],
                        max_packet: 1024,
                        dial: false,
                    },
                })
                .await
                .unwrap();
            match next(&mut events, "error").await {
                Event::Error { code, .. } => assert_eq!(code, "invalid_game_registration"),
                _ => unreachable!(),
            }

            // Retire links one at a time and keep all control channels alive;
            // this exercises the per-peer teardown path without ending the
            // room or aborting the remaining authorized links.
            for (index, remote) in remotes.iter().enumerate() {
                commands
                    .send(Request {
                        id: 200 + index as u64,
                        command: Command::EndPeer {
                            epoch: 1,
                            peer: remote.id(),
                            generation: 1,
                        },
                    })
                    .await
                    .unwrap();
                let _ = next(&mut events, "game_closed").await;
            }
            drop(games);
            assert_eq!(controls.len(), MAX_CONTROL_PEERS);

            commands
                .send(Request {
                    id: 300,
                    command: Command::Leave {
                        epoch: 1,
                        abandon: false,
                    },
                })
                .await
                .unwrap();
            let _ = next(&mut events, "room_closed").await;
            commands
                .send(Request {
                    id: 301,
                    command: Command::Shutdown,
                })
                .await
                .unwrap();
            service.await.unwrap().unwrap();
            host.close().await;
            overflow.close().await;
            for remote in remotes {
                remote.close().await;
            }
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn ipc_service_rejects_malformed_and_replayed_commands() {
        for replay in [false, true] {
            timeout(Duration::from_secs(10), async {
                let endpoint = endpoint().await;
                let (server, mut client) = tokio::io::duplex(1024);
                let service = tokio::spawn(run(server, endpoint, false));
                if replay {
                    let status = ControlFrame {
                        message_id: 2,
                        payload: br#"{"type":"status"}"#.to_vec(),
                    };
                    wire::write_ipc(&mut client, &status).await.unwrap();
                    let _ = wire::read_ipc(&mut client).await.unwrap();
                    wire::write_ipc(&mut client, &status).await.unwrap();
                } else {
                    wire::write_ipc(
                        &mut client,
                        &ControlFrame {
                            message_id: 2,
                            payload: br#"{"type":"unknown"}"#.to_vec(),
                        },
                    )
                    .await
                    .unwrap();
                }
                assert!(service.await.unwrap().is_err());
            })
            .await
            .unwrap();
        }
    }
}
