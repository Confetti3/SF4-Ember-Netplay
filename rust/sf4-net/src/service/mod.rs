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

mod checkpoints;
mod controls;
mod entry;
mod games;
mod members;
mod probes;
mod protocol;
mod refresh;
mod stall;
#[cfg(test)]
mod tests;

use entry::TaskScope;
pub use entry::run;
use probes::{selected_probe_route, serve_probe};
use protocol::NativeControlMessage;
pub use protocol::{Command, Event};

struct Request {
    id: u64,
    command: Command,
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

/// The helper's answer to `Command::Status`, from the running actor or while
/// a departure is draining.
fn status(request_id: u64, endpoint: &Endpoint, epoch: u64, peers: usize, games: usize) -> Event {
    Event::Status {
        request_id,
        endpoint: endpoint.id(),
        ip_transports: endpoint.bound_sockets().len(),
        fixed_port: transport::fixed_port::bound(endpoint),
        epoch,
        peers,
        games,
    }
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
            Command::Status => self.emit(status(
                id,
                &self.endpoint,
                self.epoch,
                self.controls.len(),
                self.games.len(),
            ))?,
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
                self.completed_checkpoint_proposal(key, transfer, _waiter_result)
                    .await
            }
            Completion::CoordinationRefresh(key, refresh) => {
                self.completed_coordination_refresh(key, refresh).await
            }
            Completion::MembershipReconciliation(key, result) => {
                self.completed_membership_reconciliation(key, result).await
            }
            Completion::Admission(key, result) => self.completed_admission(key, result).await,
            Completion::ProbeAuthorization(key, result) => {
                self.completed_probe_authorization(key, result).await
            }
            Completion::Hosted(epoch, result) => self.completed_hosted(epoch, result).await,
            Completion::Incoming(epoch, result) => self.completed_incoming(epoch, result).await,
            Completion::ClassifiedGame(incoming, result) => {
                self.completed_classified_game(incoming, result).await
            }
            Completion::Control(epoch, result) => {
                self.completed_control(epoch, result, joined_invite).await
            }
            Completion::Game(epoch, peer, generation, result) => {
                self.completed_game(epoch, peer, generation, result).await
            }
            Completion::BridgeEnded(epoch, peer, generation, failure) => {
                self.completed_bridge_ended(epoch, peer, generation, failure)
                    .await
            }
            Completion::Probe(epoch, result) => self.completed_probe(epoch, result).await,
        }
    }

    async fn completed_hosted(&mut self, epoch: u64, result: io::Result<Invite>) -> io::Result<()> {
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
        // Load over the current statistics second; see Event::HelperLoad.
        let (mut tick_lag_max, mut tick_body_max) = (Duration::ZERO, Duration::ZERO);
        let mut event_free_min = usize::MAX;
        let busy = stall::Busy::new(self.events.clone());
        let _watchdog = TaskScope(vec![busy.watch()]);
        loop {
            tokio::select! {
                _ = failed_ipc.changed() => return Err(failed("IPC disconnected")),
                request = commands.recv() => match request {
                    Some(request) => {
                        let _step = busy.enter(stall::command_stage(&request.command));
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
                    let _step = busy.enter("accept");
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
                    let _step = busy.enter(match &result { Some(Ok(completion)) => stall::completion_stage(completion), _ => "task" });
                    match result { Some(Ok(completion)) => self.completed(completion).await?,
                        Some(Err(error)) if error.is_cancelled() => (), _ => return Err(failed("helper worker failed")) }
                }
                scheduled = tick.tick() => {
                    let _step = busy.enter("tick");
                    let started = Instant::now();
                    tick_lag_max = tick_lag_max.max(started.saturating_duration_since(scheduled));
                    event_free_min = event_free_min.min(self.events.capacity());
                    self.expire_departure_grace();
                    self.start_next_admission_operation();
                    self.pump_membership_publications();
                    busy.stage("tick:poll_controls");
                    self.poll_controls().await?;
                    busy.stage("tick");
                    self.pump_pending_checkpoint_ack();
                    self.pump_pending_checkpoint_committed();
                    self.expire_checkpoint_transfers();
                    self.expire_probe_permissions();
                    self.invalidate_changed_probe_routes();
                    self.pump_outgoing_checkpoint();
                    busy.stage("tick:pump_committed_checkpoint");
                    self.pump_committed_checkpoint().await?;
                    tick_body_max = tick_body_max.max(started.elapsed());
                },
                _ = statistics.tick() => {
                    let _step = busy.enter("statistics");
                    self.emit_coordination_state().await?;
                    let micros = |value: Duration| u64::try_from(value.as_micros()).unwrap_or(u64::MAX);
                    let (lag, body) = (micros(tick_lag_max), micros(tick_body_max));
                    let free = if event_free_min == usize::MAX { self.events.capacity() } else { event_free_min } as u64;
                    (tick_lag_max, tick_body_max, event_free_min) = (Duration::ZERO, Duration::ZERO, usize::MAX);
                    // While a room is open, not only during a match: a report that
                    // stops at match end must mean the actor stopped (F-008).
                    if (self.room.is_some() || !self.games.is_empty()) && self.events.capacity() > LIFECYCLE_EVENT_RESERVE {
                        self.emit(Event::HelperLoad { epoch: self.epoch, actor_tick_lag_max_us: lag,
                            actor_tick_body_max_us: body, event_queue_free_min: free })?;
                    }
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
