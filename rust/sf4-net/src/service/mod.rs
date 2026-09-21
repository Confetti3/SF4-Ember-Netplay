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
mod members;
mod probes;
mod protocol;
mod refresh;
#[cfg(test)]
mod tests;

#[cfg(test)]
use entry::TaskScope;
pub use entry::run;
use probes::{run_probe, selected_probe_route, serve_probe};
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
                    if let Some(previous) = self.probe_reservations.remove(&probe.peer)
                        && previous.reported
                        && !probe.report
                    {
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
                    if probe.report {
                        let route = probe
                            .connection
                            .as_ref()
                            .map(selected_probe_route)
                            .unwrap_or_else(|| "unavailable".into());
                        if probe.route_changed {
                            if let Some(connection) = probe.connection
                                && connection.close_reason().is_none()
                            {
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
                scheduled = tick.tick() => {
                    let started = Instant::now();
                    tick_lag_max = tick_lag_max.max(started.saturating_duration_since(scheduled));
                    event_free_min = event_free_min.min(self.events.capacity());
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
                    tick_body_max = tick_body_max.max(started.elapsed());
                },
                _ = statistics.tick() => {
                    self.emit_coordination_state().await?;
                    let micros = |value: Duration| u64::try_from(value.as_micros()).unwrap_or(u64::MAX);
                    let (lag, body) = (micros(tick_lag_max), micros(tick_body_max));
                    let free = if event_free_min == usize::MAX { self.events.capacity() } else { event_free_min } as u64;
                    (tick_lag_max, tick_body_max, event_free_min) = (Duration::ZERO, Duration::ZERO, usize::MAX);
                    if !self.games.is_empty() && self.events.capacity() > LIFECYCLE_EVENT_RESERVE {
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
