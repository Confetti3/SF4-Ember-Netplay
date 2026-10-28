//! Helper actor: process-local IPC owns rooms and match mappings. The actor
//! routes opaque application messages; C++ SessionServer owns all lobby rules.
use std::{
    collections::{BTreeMap, BTreeSet, VecDeque},
    io,
    net::{Ipv4Addr, SocketAddr},
    sync::{Arc, atomic::Ordering},
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use iroh::{
    Endpoint, EndpointAddr, EndpointId,
    endpoint::{Connection, RecvStream, SendStream},
};
use serde::{Deserialize, Serialize};
use tokio::{
    io::{AsyncRead, AsyncWrite, AsyncWriteExt},
    sync::{Notify, mpsc, watch},
    task::{AbortHandle, JoinSet},
    time::{Instant, interval, timeout},
};

use crate::{
    bridge::{Bridge, BridgeStats},
    control::{ControlWorker, QueueError, Session},
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
/// How long the actor waits between looks at its queues while a game runs
/// or a checkpoint is in flight: gameplay links and checkpoint chunks wait
/// behind it, so it is short.
const FAST_TICK: Duration = Duration::from_millis(2);
/// The same with a room open and nothing in flight. Everything that arrives
/// wakes the loop on its own (commands, completions, control frames), so the
/// tick only bounds the timers the pumps keep.
const ROOM_TICK: Duration = Duration::from_millis(25);
/// The same with no room: nothing waits on the tick at all.
const IDLE_TICK: Duration = Duration::from_millis(50);
/// With a room open but no game, `HelperLoad` goes out every this many
/// seconds instead of every second.
const IDLE_LOAD_REPORT_SECS: u64 = 5;
/// How long a replacement control waits behind the worker it supersedes. The
/// old worker is closed at once, so its reader ends and its queue drains within
/// a few polls; the limit only bounds a worker whose session binding never
/// settles, which would otherwise hold its queue and the replacement forever.
const CONTROL_REPLACE_DRAIN_LIMIT: Duration = Duration::from_secs(5);
const LIFECYCLE_EVENT_RESERVE: usize = 8;
/// How long a copied invitation stays good: a week, the longest an
/// invitation may last.
const INVITE_LIFETIME: u64 = crate::invite::MAX_INVITE_LIFETIME_SECS;
const CHECKPOINT_TRANSFER_TIMEOUT: Duration = Duration::from_secs(15);
const COORDINATION_ADMISSION_TIMEOUT: Duration = Duration::from_secs(12);
/// One membership reconciliation. A configuration change that needs a member
/// which never answers would otherwise hold the single reconciliation slot for
/// good; the next refresh retries once it times out.
const MEMBERSHIP_OPERATION_TIMEOUT: Duration = Duration::from_secs(12);
/// A probe reservation is waited for this long on the replica that must apply
/// it before the requesting control is closed.
const PROBE_RESERVATION_TIMEOUT: Duration = Duration::from_secs(5);
/// A joiner whose control to the room's authority closes at least this many
/// times in a row, over at least `JOIN_GIVE_UP_AFTER`, without its
/// coordination member ever being admitted gives up with `join_failed`
/// instead of redialling forever. The time floor keeps a slow relay join,
/// whose first attempts can be refused in quick succession, from failing.
const MAX_JOIN_CONTROL_LOSSES: u8 = 8;
const JOIN_GIVE_UP_AFTER: Duration = Duration::from_secs(45);
/// A single authority read can time out while a relay room is admitting a
/// burst of members. Freeze native mutations immediately, but only ask one
/// deterministic surviving voter to campaign after the same committed
/// leader has remained unwritable for a full election window. This stays
/// ahead of the native replacement-room offer at fifteen seconds.
const RECOVERY_ELECTION_GRACE: Duration = Duration::from_secs(10);
// Retired process incarnations are kept as tombstones so an old Admission
// cannot be replayed on a lagging replica. The set is bounded; once it is full,
// the tombstone of an incarnation that has aged out of the replicated
// membership history makes room (`record_retirement`).
const MAX_RETIRED_INCARNATIONS: usize = crate::coordination::MAX_RETIRED_MEMBER_HISTORY;
// One monotonic wire counter covers coordination and native messages. The
// native logical message ID is carried in `NativeControlMessage` so C++ can
// preserve its IPC correlation without sharing the QUIC frame counter.
const TRANSPORT_MESSAGE_ID_BASE: u64 = 2;
mod checkpoints;
mod controls;
mod departure;
mod entry;
mod events;
mod games;
mod join;
mod latency;
mod members;
mod network;
mod probes;
mod protocol;
mod public;
mod refresh;
mod seats;
mod server_owned;
mod short_links;
mod stall;
#[cfg(test)]
mod tests;

use entry::TaskScope;
pub use entry::{decode_command, decode_event, encode_command, encode_event, run, run_on_port};
use events::EventOutbox;
use join::JoinVia;
use probes::{selected_probe_route, serve_probe};
use protocol::NativeControlMessage;
pub use protocol::{Command, Event};
use short_links::{ShortLinks, ShortPublished};

use checkpoints::{CheckpointProposalKey, OutgoingCheckpoint, PendingCheckpointCommitted};
use controls::ParkedControl;
use games::GameSlot;
#[cfg(test)]
use games::GameDiagnostics;
use members::{
    AdmissionOperationKey, AdmissionOperationResult, ControlBinding, DeferredAdmission,
    MembershipOperationKey, MembershipOperationResult, applied_member_incarnations,
};
use probes::{
    OwnProbe, PendingProbeInvalidation, ProbeAuthorization, ProbeAuthorizationKey,
    ProbeCompletion, ProbePermission, ProbeReservationKey,
};
use protocol::{CoordinationControl, native_json_depth_ok};
#[cfg(test)]
use protocol::MAX_NATIVE_JSON_DEPTH;
use refresh::{CoordinationRefresh, CoordinationRefreshKey};
use seats::primary_members;

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
    /// A join's control, with the latency endpoint bound beside it.
    GuestControl(u64, Invite, io::Result<ControlChannel>, Option<latency::LatencyEndpoint>),
    Incoming(u64, io::Result<Connection>),
    ClassifiedGame(IncomingGame, io::Result<transport::GameStream>),
    /// A hosted room's invitation, with its latency endpoint.
    Hosted(u64, io::Result<Invite>, Option<latency::LatencyEndpoint>),
    Control(u64, io::Result<ControlChannel>),
    MemberControl(u64, u64, io::Result<ControlChannel>),
    Reconnect(u64, EndpointId, io::Result<ControlChannel>),
    /// A dialed gameplay link.
    Game(u64, EndpointId, u64, io::Result<GameConnection>),
    /// A connection a listener accepted, keyed by that connection's stable
    /// id. Its failure only ends this candidate, never the slot.
    GameCandidate(u64, EndpointId, u64, usize, io::Result<GameConnection>),
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
    /// A store of this room's sealed invitation at the link service.
    ShortPublished(u64, ShortPublished),
    /// This room's short link record looked up by a new leader: epoch, code,
    /// and the keys and invitation the service holds.
    ShortAdopted(
        u64,
        String,
        Result<(crate::short_invite::Keys, String), crate::short_invite::ShortError>,
    ),
    /// A pasted short link looked up: request, epoch, build, code, and the
    /// invitation it stands for.
    ShortResolved(
        u64,
        u64,
        String,
        String,
        Result<String, crate::short_invite::ShortError>,
    ),
    ProbeReservation(ProbeReservationKey, probes::ReservationWait),
}

fn failed(code: &'static str) -> io::Error {
    io::Error::other(code)
}

/// The error event of `code` for request `request_id`, with no peer or
/// reason.
fn error_event(request_id: u64, epoch: u64, code: &str) -> Event {
    Event::Error {
        probe_failure: None,
        request_id,
        epoch,
        peer: None,
        code: code.into(),
        reason: None,
    }
}

/// A live gameplay link's counters, for `Event::Statistics`.
fn statistics_event(
    epoch: u64,
    peer: EndpointId,
    generation: u64,
    stats: &BridgeStats,
    route: String,
) -> Event {
    Event::Statistics {
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
        route,
    }
}
#[cfg(test)]
tokio::task_local! {
    /// Tests only: seconds this task's reading of the wall clock is moved by,
    /// as if this PC's clock were set differently.
    static WALL_CLOCK_OFFSET: i64;
}

fn now() -> io::Result<u64> {
    let wall = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|v| v.as_secs())
        .map_err(|_| failed("clock"))?;
    #[cfg(test)]
    let wall =
        wall.saturating_add_signed(WALL_CLOCK_OFFSET.try_with(|offset| *offset).unwrap_or(0));
    Ok(wall)
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

/// The parts of a CoordinationState that decide whether it is sent again. A
/// membership change alone moves neither term nor revision, so the counts are
/// part of it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct CoordinationMarker {
    term: u64,
    revision: u64,
    writable: bool,
    leader_local: bool,
    voter_count: usize,
    learner_count: usize,
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
    /// The replacement for each control whose worker is still draining.
    parked_controls: BTreeMap<EndpointId, ParkedControl>,
    games: BTreeMap<EndpointId, GameSlot>,
    closed_generation: u64,
    tasks: JoinSet<Completion>,
    events: EventOutbox,
    recovery: Option<crate::recovery::RecoverySession>,
    admissions: BTreeMap<u64, Admission>,
    admission_order: Vec<u64>,
    /// Admissions presented by a control whose asynchronous validation has
    /// not succeeded yet, keyed by that control's authenticated endpoint.
    /// They are installed in `admissions` only once the operation accepts
    /// them, so a rejected one never replaces an accepted binding.
    pending_admissions: BTreeMap<EndpointId, Admission>,
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
    /// Exact endpoint/incarnation roster last committed by C++. Admissions arriving
    /// after that checkpoint are not treated as departures until a later
    /// checkpoint explicitly includes them first.
    committed_native_members: Option<BTreeMap<EndpointId, u64>>,
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
    /// The controls the running admission operation settles on completion.
    pending_admission_bindings: Vec<ControlBinding>,
    deferred_admissions: VecDeque<DeferredAdmission>,
    pending_membership_publications: BTreeSet<EndpointId>,
    /// What the last CoordinationState sent said.
    last_coordination_state: Option<CoordinationMarker>,
    coordination_writable: bool,
    unwritable_leader_since: Option<(u64, u64, Instant)>,
    /// At most one committed-leader control rebind may be in flight. Without
    /// this fence a follower that has learned a new leader but has not yet
    /// connected would launch one ten-second dial per state tick.
    reconnect_target: Option<EndpointId>,
    pending_game_admissions: BTreeMap<EndpointId, (Connection, tokio::task::AbortHandle)>,
    /// Invalidations are retained until the bounded native event queue has
    /// room. Otherwise a route-changed measurement could leave native code
    /// holding a recommendation that no longer stands.
    pending_probe_invalidations: BTreeMap<EndpointId, PendingProbeInvalidation>,
    // Peers that hold a reservation to probe us. An incoming probe stream is
    // served only for a peer in this set.
    probe_peers: BTreeSet<EndpointId>,
    probe_permissions: BTreeMap<EndpointId, ProbePermission>,
    /// Checks we run, one per peer. A second check of the same peer is refused
    /// while one is in flight; a probe we are answering does not count.
    own_probes: BTreeMap<EndpointId, OwnProbe>,
    pending_probe_authorizations: BTreeMap<EndpointId, ProbeAuthorizationKey>,
    pending_probe_reservations: BTreeMap<EndpointId, ProbeReservationKey>,
    /// Set once the coordination member of a joined room has been seen in the
    /// applied membership; only a join that never got there is bounded.
    join_settled: bool,
    join_control_losses: u8,
    join_first_loss: Option<tokio::time::Instant>,
    /// A departing Raft leader keeps its authority RPC alive briefly after
    /// emitting room_closed. A simultaneous follower Leave can then obtain
    /// the committed voter-set proof before the leader's process shuts down.
    retirement_started: Option<Instant>,
    /// A graceful Leave ended without releasing the room: no successor or
    /// membership proof arrived within its bounded steps. The native client
    /// leaves locally after its own bound and sends an abandon; should it
    /// instead open a newer epoch, that epoch releases this stale room rather
    /// than being refused for it until the process restarts.
    departure_failed: bool,
    /// The room's short link, once the native side asked for one.
    short: ShortLinks,
    /// Set for a public (server-owned) room; see `public.rs`.
    public: Option<crate::public_room::PublicRoom>,
    /// Exact UDP port for this process's coordination endpoint when hosting.
    coordination_port: Option<u16>,
    /// The roster the committed checkpoint named the last time one was
    /// parsed (`observe_commit`), with the room and the commit it came from,
    /// so a read of the same commit parses nothing.
    refresh_roster: Option<seats::SeenCommit>,
    /// Who the room seats, from the last commit that said so readably.
    seats: seats::Seats,
    /// Signalled when work arrives that no other arm of the actor's loop
    /// sees: a control's reader queued a frame, or the room's Raft store
    /// committed (`install_recovery`). The loop ticks at once.
    wake: Arc<Notify>,
    /// Ping to each member; see `latency.rs`.
    latency: latency::Latency,
}

/// Brings the actor's next tick forward to now. A tick already overdue keeps
/// its deadline, so the lag it reports still counts from when it was due.
fn tick_now(tick: std::pin::Pin<&mut tokio::time::Sleep>) {
    let now = Instant::now();
    if tick.deadline() > now {
        tick.reset(now);
    }
}

impl Actor {
    fn emit(&self, event: Event) -> io::Result<()> {
        self.events.emit(event)
    }

    /// How long until the next tick, from what the pumps have to do. A
    /// public room host spends its life at `ROOM_TICK`; a helper waiting for
    /// a room at `IDLE_TICK`.
    fn tick_period(&self) -> Duration {
        let checkpoint_in_flight = self.incoming_transfer.is_some()
            || self.outgoing_transfer.is_some()
            || self.pending_checkpoint_proposal.is_some()
            || self.pending_checkpoint_retry.is_some()
            || self.pending_checkpoint_ack.is_some()
            || self.pending_checkpoint_committed.is_some();
        // Work the last tick left behind: frames past a control's poll
        // budget, or held back while the event queue to the native side is
        // full. Neither signals again, and the native side drains that queue
        // without telling the actor, so both are looked at again soon.
        let work_left = self.events.has_backlog()
            || self.controls.values().any(ControlWorker::has_queued);
        if !self.games.is_empty()
            || !self.pending_game_admissions.is_empty()
            || checkpoint_in_flight
            || work_left
            || self.opening
        {
            FAST_TICK
        } else if self.room.is_some()
            || !self.controls.is_empty()
            || !self.parked_controls.is_empty()
            || self.retirement_started.is_some()
        {
            ROOM_TICK
        } else {
            IDLE_TICK
        }
    }
    fn error(&self, request_id: u64, code: &str) -> io::Result<()> {
        self.error_at(request_id, self.epoch, code)
    }

    /// A refused Host or Join names the epoch it was asked for. The native
    /// client filters events by its current epoch, so an error tagged with the
    /// helper's previous epoch would leave that attempt joining forever.
    fn error_at(&self, request_id: u64, epoch: u64, code: &str) -> io::Result<()> {
        self.emit(error_event(request_id, epoch, code))
    }

    /// An error whose `reason` says at which stage it failed (`relay_unreachable`,
    /// `host_unreachable`, `timeout`, `refused`, `control_lost`). The code is
    /// unchanged, so a native client that ignores reasons behaves as before.
    fn error_because(&self, request_id: u64, code: &str, reason: Option<&str>) -> io::Result<()> {
        self.emit(Event::Error {
            probe_failure: None,
            request_id,
            epoch: self.epoch,
            peer: None,
            code: code.into(),
            reason: reason.map(Into::into),
        })
    }

    // Additive, allowlisted diagnostics: keep the existing error code so older
    // native clients still treat a rejected probe as a nonfatal operation.
    fn probe_error(&self, failure: probes::ProbeFailure) -> io::Result<()> {
        self.emit(Event::Error {
            request_id: 0,
            epoch: self.epoch,
            peer: None,
            code: "probe_unavailable".into(),
            probe_failure: Some(failure as u8),
            reason: None,
        })
    }

    /// A native send to `peer` was not queued. The native side treats it as
    /// advisory; `reason` says which of the send's checks refused it.
    fn control_send_failed(
        &self,
        request_id: u64,
        peer: EndpointId,
        reason: &str,
    ) -> io::Result<()> {
        self.emit(Event::Error {
            probe_failure: None,
            request_id,
            epoch: self.epoch,
            peer: Some(peer),
            code: "control_send_failed".into(),
            reason: Some(reason.into()),
        })
    }

    fn emit_bulk(&self, event: Event) -> bool {
        self.events.emit_bulk(event)
    }

    fn clear_room(&mut self) {
        if let Some(recovery) = self.recovery.take() {
            tokio::spawn(async move {
                recovery.stop().await;
            });
        }
        self.hosted = None;
        self.room_invite = None;
        self.short.clear();
        self.host_address = None;
        self.room = None;
        // The cached roster holds the room's whole checkpoint.
        self.refresh_roster = None;
        self.seats = seats::Seats::Open;
        self.opening = false;
        self.controls.clear();
        self.clear_parked_controls();
        self.games.clear();
        self.tasks.abort_all();
        self.closed_generation = 0;
        self.admissions.clear();
        self.admission_order.clear();
        self.pending_admissions.clear();
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
        self.pending_admission_bindings.clear();
        self.deferred_admissions.clear();
        self.pending_membership_publications.clear();
        self.last_coordination_state = None;
        self.unwritable_leader_since = None;
        self.reconnect_target = None;
        self.pending_game_admissions.clear();
        self.pending_probe_invalidations.clear();
        self.probe_peers.clear();
        self.probe_permissions.clear();
        self.own_probes.clear();
        self.pending_probe_authorizations.clear();
        self.pending_probe_reservations.clear();
        self.join_settled = false;
        self.join_control_losses = 0;
        self.join_first_loss = None;
        self.retirement_started = None;
        self.departure_failed = false;
        self.public = None;
        self.latency_clear();
    }
    fn begin(&mut self, epoch: u64, build: &str) -> bool {
        if epoch <= self.epoch || epoch > i64::MAX as u64 || build.is_empty() || build.len() > 128 {
            return false;
        }
        // A room whose departure the room never confirmed is released by the
        // next valid epoch rather than refusing it until the process restarts,
        // the same way an explicit abandon releases it: its coordination vote
        // lingers for the departure grace so the leader can still commit the
        // removal. A room already in that grace hands its route over likewise.
        if self.departure_failed {
            self.abandon_room();
        }
        if self.retirement_started.is_some() {
            self.detach_departure_grace();
            self.clear_room();
        }
        if self.room.is_some() || self.opening || !self.games.is_empty() {
            return false;
        }
        self.epoch = epoch;
        self.opening = true;
        true
    }
    fn matches(&self, epoch: u64) -> bool {
        epoch == self.epoch && epoch != 0
    }

    /// Tests only: `command` with IPC channels that never deliver or fail.
    #[cfg(test)]
    async fn command_now(&mut self, request: Request) -> io::Result<bool> {
        let (_commands, mut commands) = mpsc::channel(1);
        let (_failed, mut failed_ipc) = watch::channel(false);
        self.command(request, &mut commands, &mut failed_ipc).await
    }

    /// Runs one native command; false when the helper is to stop. A Leave
    /// keeps answering `commands` while the departure runs, and ends early
    /// when `failed_ipc` reports the pipe lost.
    async fn command(
        &mut self,
        request: Request,
        commands: &mut mpsc::Receiver<Request>,
        failed_ipc: &mut watch::Receiver<bool>,
    ) -> io::Result<bool> {
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
                return self.leave_command(epoch, abandon, commands, failed_ipc).await;
            }
            Command::Status => self.emit(status(
                id,
                &self.endpoint,
                self.epoch,
                self.controls.len(),
                self.games.len(),
            ))?,
            Command::Shutdown => return Ok(false),
            // The IPC reader routes these to the tournament worker.
            Command::Tournament { .. } => self.error(id, "invalid_request")?,
            Command::Host { epoch, build } => {
                if !self.begin(epoch, &build) {
                    self.error_at(id, epoch, "invalid_room_state")?;
                    return Ok(true);
                }
                self.spawn_host(epoch, build, None);
            }
            Command::HostPublic {
                epoch,
                build,
                room,
                ticket_key,
                ticket_kid,
                bridge_id,
                creator,
            } => self.host_public(
                id,
                epoch,
                build,
                room,
                (&ticket_key, &ticket_kid, &bridge_id, &creator),
            )?,
            Command::JoinPublic {
                epoch,
                invitation,
                ticket,
                build,
            } => self.join_public(id, epoch, &invitation, build, ticket)?,
            Command::BanAccount { epoch, account } => self.ban_account(id, epoch, &account)?,
            Command::Join {
                epoch,
                invitation,
                build,
                observer,
            } => self.join_command(id, epoch, &invitation, build, observer)?,
            Command::ShortInvite { epoch } => self.short_invite_command(epoch)?,
            Command::LatencyWatch { epoch, room, view, on } => self.latency_watch(epoch, room, view, on),
            Command::Send {
                epoch,
                peer,
                message_id,
                payload,
                control,
            } => self.send_native(id, epoch, peer, message_id, payload, control)?,
            Command::CloseControl { epoch, peer } => self.close_control_command(id, epoch, peer)?,
            Command::PrepareGame {
                epoch,
                peer,
                room,
                generation,
                capability,
                local_port,
                max_packet,
                dial,
            } => self.prepare_game(
                id, epoch, peer, room, generation, capability, local_port, max_packet, dial,
            )?,
            Command::EndMatch { epoch, generation } => self.end_match(id, epoch, generation)?,
            Command::EndPeer {
                epoch,
                peer,
                generation,
            } => self.end_peer(id, epoch, peer, generation)?,
        }
        Ok(true)
    }

    async fn completed(&mut self, completion: Completion) -> io::Result<()> {
        match completion {
            Completion::GuestControl(epoch, invite, result, latency) => {
                if result.is_ok() {
                    self.latency_adopt(epoch, latency);
                }
                self.completed_control(epoch, result, Some(invite)).await
            }
            Completion::MemberControl(epoch, incarnation, result) => {
                if let Ok(channel) = &result {
                    let peer = channel.connection.remote_id();
                    let current = epoch == self.epoch
                        && self.control_rejoin_member(peer) == Some(incarnation);
                    let active = if let Some(recovery) = &self.recovery {
                        recovery.coordinator.store().applied_members().await.contains(&incarnation)
                    } else {
                        false
                    };
                    if !current || !active {
                        channel.connection.close(1u32.into(), b"member retired");
                        return Ok(());
                    }
                }
                self.completed_control(epoch, result, None).await
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
                self.completed_control(epoch, result, retrying_join).await
            }
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
            Completion::Hosted(epoch, result, latency) => {
                self.completed_hosted(epoch, result, latency).await
            }
            Completion::Incoming(epoch, result) => self.completed_incoming(epoch, result).await,
            Completion::ClassifiedGame(incoming, result) => {
                self.completed_classified_game(incoming, result).await
            }
            Completion::Control(epoch, result) => self.completed_control(epoch, result, None).await,
            Completion::Game(epoch, peer, generation, result) => {
                self.completed_game(epoch, peer, generation, result).await
            }
            Completion::GameCandidate(epoch, peer, generation, candidate, result) => {
                self.completed_game_candidate(epoch, peer, generation, candidate, result)
                    .await
            }
            Completion::BridgeEnded(epoch, peer, generation, failure) => {
                self.completed_bridge_ended(epoch, peer, generation, failure)
                    .await
            }
            Completion::Probe(epoch, result) => self.completed_probe(epoch, result).await,
            Completion::ProbeReservation(key, applied) => {
                self.completed_probe_reservation(key, applied).await
            }
            Completion::ShortPublished(epoch, published) => {
                self.completed_short_publish(epoch, published)
            }
            Completion::ShortResolved(id, epoch, build, code, result) => {
                self.completed_short_resolve(id, epoch, build, code, result)
            }
            Completion::ShortAdopted(epoch, code, found) => {
                self.completed_short_adopt(epoch, code, found)
            }
        }
    }

    async fn run(
        &mut self,
        mut commands: mpsc::Receiver<Request>,
        mut failed_ipc: watch::Receiver<bool>,
    ) -> io::Result<()> {
        // The next tick is set after every tick (`tick_period`), and an
        // arrival on any other arm brings it forward (`tick_now`), so the loop
        // is woken by work and the timer only bounds the pumps' timers.
        let tick = tokio::time::sleep_until(Instant::now());
        tokio::pin!(tick);
        let wake = self.wake.clone();
        let mut statistics = interval(Duration::from_secs(1));
        let mut statistics_ticks: u64 = 0;
        // Load since the last HelperLoad report; see Event::HelperLoad.
        let mut load = stall::LoadWindow::default();
        // The last network summary the native side was sent; see network.rs.
        let mut reported_network = None;
        let busy = stall::Busy::new(self.events.sender());
        let _watchdog = TaskScope(vec![busy.watch()]);
        loop {
            tokio::select! {
                _ = failed_ipc.changed() => return Err(failed("IPC disconnected")),
                request = commands.recv() => match request {
                    Some(request) => {
                        tick_now(tick.as_mut());
                        let _step = busy.enter(stall::command_stage(&request.command));
                        if !self.command(request, &mut commands, &mut failed_ipc).await? {
                            return Ok(());
                        }
                    },
                    None => return Err(failed("IPC disconnected")),
                },
                incoming = self.endpoint.accept() => {
                    tick_now(tick.as_mut());
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
                incoming = latency::accept(self.latency.endpoint.as_ref()), if self.latency.endpoint.is_some() => {
                    tick_now(tick.as_mut());
                    let _step = busy.enter("latency:accept");
                    self.latency_incoming(incoming);
                }
                Some(result) = self.latency.openings_in.join_next_with_id(), if !self.latency.openings_in.is_empty() => {
                    tick_now(tick.as_mut());
                    let _step = busy.enter("latency:inbound");
                    self.latency_inbound_done(result).await;
                }
                Some(result) = self.latency.openings_out.join_next_with_id(), if !self.latency.openings_out.is_empty() => {
                    tick_now(tick.as_mut());
                    let _step = busy.enter("latency:outbound");
                    self.latency_outbound_done(result).await;
                }
                result = self.tasks.join_next(), if !self.tasks.is_empty() => {
                    tick_now(tick.as_mut());
                    let _step = busy.enter(match &result { Some(Ok(completion)) => stall::completion_stage(completion), _ => "task" });
                    match result { Some(Ok(completion)) => self.completed(completion).await?,
                        Some(Err(error)) if error.is_cancelled() => (), _ => return Err(failed("helper worker failed")) }
                }
                // A control queued a frame or the room committed: tick now.
                _ = wake.notified() => tick_now(tick.as_mut()),
                () = &mut tick => {
                    let _step = busy.enter("tick");
                    let started = Instant::now();
                    load.tick(started.saturating_duration_since(tick.deadline()), self.events.capacity());
                    // Before poll_controls, which drops departed controls.
                    load.control_queue(self.controls.values().map(ControlWorker::take_queue_peak).max().unwrap_or(0));
                    self.events.flush()?;
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
                    self.pump_probe_invalidations();
                    self.pump_outgoing_checkpoint();
                    busy.stage("tick:latency");
                    self.latency_tick().await;
                    busy.stage("tick:pump_committed_checkpoint");
                    self.pump_committed_checkpoint().await?;
                    load.body(started.elapsed());
                    tick.as_mut().reset(Instant::now() + self.tick_period());
                },
                _ = statistics.tick() => {
                    let _step = busy.enter("statistics");
                    statistics_ticks = statistics_ticks.wrapping_add(1);
                    self.second(statistics_ticks, &busy, &mut load, &mut reported_network).await?;
                }
            }
        }
    }
}
