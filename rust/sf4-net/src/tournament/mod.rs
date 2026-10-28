//! Identity and tournament-bridge operations for the native side.
//!
//! These commands never reach the room actor. The IPC reader hands them to
//! this worker, which runs key storage and KDF work on blocking threads and
//! bridge HTTP on its own bounded task set, then answers with a `tournament`
//! event. A failure here is reported and never ends the helper, and nothing
//! here waits on, or is waited on by, room or gameplay work (spec 18.3).
mod bridges;
mod browser;
mod builtin;
mod client;
mod desk;
mod discord;
mod play;
mod ranking;
mod replays;
mod rooms;
mod sign_in;
mod sites;
mod spool;
#[cfg(test)]
mod test_support;
mod watch;

use std::{
    path::{Path, PathBuf},
    sync::{
        Arc, Mutex,
        atomic::{AtomicU64, Ordering},
    },
};

use ember_protocol::{
    EmberId, encoding::Counter, play::PublishRoom, report::Outcome as GameOutcome,
    skill::RankingChoice,
};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use tokio::sync::{Semaphore, mpsc};
use zeroize::Zeroizing;

use crate::{
    identity::{self, Identity, MAX_BACKUP_FILE, Protection},
    service::Event,
};

pub use builtin::Builtin;

/// Bridge requests in flight at once (spec 25.1).
const HTTP_CONCURRENCY: usize = 4;
/// How often spooled reports are sent again.
const SPOOL_RETRY_SECS: u64 = 20;
const QUEUE: usize = 32;
const MAX_PATH: usize = 1024;

/// A native request. Passphrases are wiped when the request is dropped; the
/// helper never returns or logs them.
#[derive(Serialize, Deserialize)]
#[serde(tag = "op", rename_all = "snake_case", deny_unknown_fields)]
pub enum Request {
    // Empty struct variants, not unit variants: serde ignores unknown fields
    // on unit variants of an internally tagged enum.
    IdentityStatus {},
    IdentityEnable {
        #[serde(default)]
        passphrase: Option<Zeroizing<String>>,
    },
    IdentityUnlock {
        passphrase: Zeroizing<String>,
    },
    IdentityExport {
        path: String,
        passphrase: Zeroizing<String>,
    },
    IdentityPreviewImport {
        path: String,
        passphrase: Zeroizing<String>,
    },
    IdentityImport {
        path: String,
        passphrase: Zeroizing<String>,
        expected_ember_id: String,
        /// Protects the imported key under Wine/Proton or by choice.
        #[serde(default)]
        local_passphrase: Option<Zeroizing<String>>,
        #[serde(default)]
        replace: bool,
    },
    IdentityReset {
        #[serde(default)]
        confirm_ember_id: Option<String>,
    },
    BridgeList {},
    BridgeInspect {
        origin: String,
    },
    BridgeApprove {
        origin: String,
        bridge_id: String,
    },
    BridgeForget {
        bridge_id: String,
    },
    LinkList {
        bridge_id: String,
    },
    LinkClaim {
        bridge_id: String,
        connection_id: String,
        code: String,
    },
    LinkCancel {
        bridge_id: String,
        claim_id: String,
    },
    LinkRemove {
        bridge_id: String,
        link_id: String,
    },
    DiscordStatus {
        bridge_id: String,
    },
    DiscordConnect {
        bridge_id: String,
    },
    DiscordRemove {
        bridge_id: String,
    },
    DiscordCancel {
        bridge_id: String,
    },
    /// A sign-in site's requests, for the site the bridge lists under `site`.
    /// With `organizer`, the first three are for the organizer sign-in
    /// instead: its grant, a sign-in asking for the site's organizer scopes,
    /// and dropping the grant (the account stays).
    SiteStatus {
        bridge_id: String,
        site: String,
        #[serde(default)]
        organizer: bool,
    },
    SiteConnect {
        bridge_id: String,
        site: String,
        #[serde(default)]
        organizer: bool,
    },
    SiteRemove {
        bridge_id: String,
        site: String,
        #[serde(default)]
        organizer: bool,
    },
    SiteCancel {
        bridge_id: String,
        site: String,
    },
    AssignmentList {
        bridge_id: String,
    },
    /// The tournament desk: the organizer's events, or `event`'s sets.
    DeskRead {
        bridge_id: String,
        #[serde(default)]
        event: Option<String>,
    },
    /// The player's web desk sessions; with `seen`, noting the game has
    /// told the player about them, and with `end`, signing them all out.
    DeskWeb {
        bridge_id: String,
        #[serde(default)]
        seen: bool,
        #[serde(default)]
        end: bool,
    },
    /// A one-time link that opens the web desk in this PC's browser.
    DeskHandoff {
        bridge_id: String,
    },
    /// Approves the code another device's browser shows, signing that
    /// browser in to the web desk.
    DeskApprove {
        bridge_id: String,
        code: String,
        /// The number the other device shows (from `DeskCodeInfo`).
        #[serde(default)]
        number: u8,
    },
    /// What asked for another device's code and when, and the numbers to
    /// pick from before approving it.
    DeskCodeInfo {
        bridge_id: String,
        code: String,
    },
    /// Redeems an invite to Tournament Mode on a bridge that takes
    /// organizers by invitation.
    DeskRedeem {
        bridge_id: String,
        code: String,
    },
    /// An event's state on its site and the moves the site allows now.
    DeskSiteState {
        bridge_id: String,
        site: String,
        tournament: String,
    },
    /// A desk action on one of an event's sets or matches, at `path`.
    DeskAct {
        bridge_id: String,
        path: String,
        command: Value,
    },
    MatchClaim {
        bridge_id: String,
        match_id: String,
        build: String,
        /// The fighter has pressed Ready at the match's table.
        #[serde(default)]
        ready: bool,
    },
    RoomPublish {
        bridge_id: String,
        match_id: String,
        room: PublishRoom,
    },
    /// The open public rooms of `build` on a bridge.
    RoomList {
        bridge_id: String,
        build: String,
    },
    /// Opens a public room and takes its creator's ticket.
    RoomCreate {
        bridge_id: String,
        name: String,
        capacity: u8,
        build: String,
        /// Each fighter plays at their own input delay, fixed for the room's
        /// life. Sent only when true.
        #[serde(default, skip_serializing_if = "std::ops::Not::not")]
        own_delay: bool,
    },
    /// The ticket and invitation for joining an open public room.
    RoomTicket {
        bridge_id: String,
        room_id: String,
        build: String,
    },
    /// The player's own class and choice to take part in ranking.
    RankingStanding {
        bridge_id: String,
    },
    /// Takes part in ranking or not, and how the board lists the player.
    RankingChoose {
        bridge_id: String,
        choice: RankingChoice,
    },
    /// The board of classes.
    RankingBoard {
        bridge_id: String,
    },
    GamePrepare {
        bridge_id: String,
        match_id: String,
        match_generation: Counter,
    },
    GameReport {
        bridge_id: String,
        match_id: String,
        match_generation: Counter,
        result: GameOutcome,
        #[serde(default)]
        capture_frame: Option<Counter>,
        #[serde(default)]
        confirmed_input_frame: Option<Counter>,
    },
    MatchLeave {
        match_id: String,
    },
    /// An archived replay of a set's game with a confirmed result, played
    /// under room binding revision `revision`.
    ReplaySend {
        bridge_id: String,
        match_id: String,
        revision: Counter,
        path: String,
        #[serde(default)]
        sides: ember_protocol::replays::Sides,
    },
    /// Any other archived replay this player played, for the public replay
    /// archive of the first of their bridges that keeps one.
    ReplayArchive {
        path: String,
        #[serde(default)]
        sides: ember_protocol::replays::Sides,
    },
    /// The set's pack, written to `path`.
    ReplayPack {
        bridge_id: String,
        match_id: String,
        path: String,
    },
    /// Asks to watch a match this player does not play in: the room and its
    /// invitation, or why to wait (docs/design/MATCH_WATCH.md 5.3).
    MatchWatch {
        bridge_id: String,
        match_id: String,
        build: String,
    },
    MatchUnwatch {
        bridge_id: String,
        match_id: String,
    },
}

/// Where a request runs. Identity jobs run one at a time, in order, on a
/// blocking thread; local jobs touch only this run's stores, one at a time,
/// and never wait for an HTTP slot; bridge jobs run concurrently up to
/// `HTTP_CONCURRENCY`, starting in the order they came.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Lane {
    Identity,
    Local,
    Bridge,
}

/// Each request's op name, as serde's `rename_all` spells its tag, and its
/// lane, written once. Both matches are exhaustive, so a new variant must be
/// named and routed here.
macro_rules! requests {
    ($($variant:ident => $op:literal, $lane:ident;)*) => {
        impl Request {
            fn op(&self) -> &'static str {
                match self {
                    $(Self::$variant { .. } => $op,)*
                }
            }

            fn lane(&self) -> Lane {
                match self {
                    $(Self::$variant { .. } => Lane::$lane,)*
                }
            }
        }

        #[cfg(test)]
        const REQUESTS: &[(&str, Lane)] = &[$(($op, Lane::$lane),)*];
    };
}

requests! {
    IdentityStatus => "identity_status", Identity;
    IdentityEnable => "identity_enable", Identity;
    IdentityUnlock => "identity_unlock", Identity;
    IdentityExport => "identity_export", Identity;
    IdentityPreviewImport => "identity_preview_import", Identity;
    IdentityImport => "identity_import", Identity;
    IdentityReset => "identity_reset", Identity;
    BridgeList => "bridge_list", Local;
    BridgeInspect => "bridge_inspect", Bridge;
    BridgeApprove => "bridge_approve", Bridge;
    BridgeForget => "bridge_forget", Local;
    LinkList => "link_list", Bridge;
    LinkClaim => "link_claim", Bridge;
    LinkCancel => "link_cancel", Bridge;
    LinkRemove => "link_remove", Bridge;
    DiscordStatus => "discord_status", Bridge;
    DiscordConnect => "discord_connect", Bridge;
    DiscordRemove => "discord_remove", Bridge;
    DiscordCancel => "discord_cancel", Bridge;
    SiteStatus => "site_status", Bridge;
    SiteConnect => "site_connect", Bridge;
    SiteRemove => "site_remove", Bridge;
    SiteCancel => "site_cancel", Bridge;
    AssignmentList => "assignment_list", Bridge;
    MatchClaim => "match_claim", Bridge;
    RoomPublish => "room_publish", Bridge;
    RoomList => "room_list", Bridge;
    RoomCreate => "room_create", Bridge;
    RoomTicket => "room_ticket", Bridge;
    RankingStanding => "ranking_standing", Bridge;
    RankingChoose => "ranking_choose", Bridge;
    RankingBoard => "ranking_board", Bridge;
    GamePrepare => "game_prepare", Bridge;
    GameReport => "game_report", Bridge;
    MatchLeave => "match_leave", Local;
    MatchWatch => "match_watch", Bridge;
    DeskRead => "desk_read", Bridge;
    DeskAct => "desk_act", Bridge;
    DeskWeb => "desk_web", Bridge;
    DeskHandoff => "desk_handoff", Bridge;
    DeskApprove => "desk_approve", Bridge;
    DeskCodeInfo => "desk_code_info", Bridge;
    DeskRedeem => "desk_redeem", Bridge;
    DeskSiteState => "desk_site_state", Bridge;
    ReplaySend => "replay_send", Bridge;
    ReplayArchive => "replay_archive", Bridge;
    ReplayPack => "replay_pack", Bridge;
    MatchUnwatch => "match_unwatch", Bridge;
}

/// A code the helper itself words for a failure, as the native UI is shown
/// it. The bridge's own codes are `Failure::Bridge`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Code {
    BindingMismatch,
    BridgeChanged,
    BridgeInvalidResponse,
    BridgeMismatch,
    BridgeNotApproved,
    BridgePinned,
    BridgeResponseTooLarge,
    BridgeUnreachable,
    BrowserUnavailable,
    ChallengeMismatch,
    Corrupt,
    DeskCodeRefused,
    DeskNumberWrong,
    FileUnreadable,
    IdentityBusy,
    IdentityUnavailable,
    Internal,
    InvalidBridge,
    InvalidName,
    InvalidOrigin,
    InvalidPath,
    InvalidReplay,
    InvalidRequest,
    InviteInvalid,
    InvitesUnsupported,
    Io,
    MatchLeft,
    NotABridge,
    NotBound,
    NotFound,
    OwnDelayUnsupported,
    PermitMismatch,
    PinnedBridgeMismatch,
    RankingUnavailable,
    ReplaysUnsupported,
    ReportNotSaved,
    RoomsUnavailable,
    ServiceUnavailable,
    SignInOff,
    TooManyBridges,
    UnknownPermit,
    Unsupported,
    UnsupportedBridge,
    UntrustedSignature,
    WatchUnsupported,
}

impl Code {
    /// The code on the IPC wire.
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::BindingMismatch => "binding_mismatch",
            Self::BridgeChanged => "bridge_changed",
            Self::BridgeInvalidResponse => "bridge_invalid_response",
            Self::BridgeMismatch => "bridge_mismatch",
            Self::BridgeNotApproved => "bridge_not_approved",
            Self::BridgePinned => "bridge_pinned",
            Self::BridgeResponseTooLarge => "bridge_response_too_large",
            Self::BridgeUnreachable => "bridge_unreachable",
            Self::BrowserUnavailable => "browser_unavailable",
            Self::ChallengeMismatch => "challenge_mismatch",
            Self::Corrupt => "corrupt",
            Self::DeskCodeRefused => "desk_code_refused",
            Self::DeskNumberWrong => "desk_number_wrong",
            Self::FileUnreadable => "file_unreadable",
            Self::IdentityBusy => "identity_busy",
            Self::IdentityUnavailable => "identity_unavailable",
            Self::Internal => "internal",
            Self::InvalidBridge => "invalid_bridge",
            Self::InvalidName => "invalid_name",
            Self::InvalidOrigin => "invalid_origin",
            Self::InvalidPath => "invalid_path",
            Self::InvalidReplay => "invalid_replay",
            Self::InvalidRequest => "invalid_request",
            Self::InviteInvalid => "invite_invalid",
            Self::InvitesUnsupported => "invites_unsupported",
            Self::Io => "io",
            Self::MatchLeft => "match_left",
            Self::NotABridge => "not_a_bridge",
            Self::NotBound => "not_bound",
            Self::NotFound => "not_found",
            Self::OwnDelayUnsupported => "own_delay_unsupported",
            Self::PermitMismatch => "permit_mismatch",
            Self::PinnedBridgeMismatch => "pinned_bridge_mismatch",
            Self::RankingUnavailable => "ranking_unavailable",
            Self::ReplaysUnsupported => "replays_unsupported",
            Self::ReportNotSaved => "report_not_saved",
            Self::RoomsUnavailable => "rooms_unavailable",
            Self::ServiceUnavailable => "service_unavailable",
            Self::SignInOff => "sign_in_off",
            Self::TooManyBridges => "too_many_bridges",
            Self::UnknownPermit => "unknown_permit",
            Self::Unsupported => "unsupported",
            Self::UnsupportedBridge => "unsupported_bridge",
            Self::UntrustedSignature => "untrusted_signature",
            Self::WatchUnsupported => "watch_unsupported",
        }
    }
}

/// Why a request failed: a stable, secret-free code for the native UI. The
/// helper's own decisions, key storage and the bridge's refusals are kept
/// apart, so a decision on one never reads another's code by its spelling:
/// a bridge answering `internal` is not this helper failing.
#[derive(Debug)]
pub enum Failure {
    Local(Code),
    Identity(identity::Failure),
    /// A bridge's error code, or the reason it gives that a caller words
    /// apart, or `http_<status>` when its answer names neither.
    Bridge(String),
}

impl From<identity::Failure> for Failure {
    fn from(failure: identity::Failure) -> Self {
        Self::Identity(failure)
    }
}

impl Failure {
    fn new(code: Code) -> Self {
        Self::Local(code)
    }

    /// The code on the IPC wire.
    pub fn as_str(&self) -> &str {
        match self {
            Self::Local(code) => code.as_str(),
            Self::Identity(failure) => failure.as_str(),
            Self::Bridge(code) => code,
        }
    }

    fn into_reason(self) -> String {
        match self {
            Self::Bridge(code) => code,
            other => other.as_str().to_owned(),
        }
    }
}

/// A std `Mutex` locked for a request: a poisoned lock is this helper's own
/// failure.
trait Locked<T> {
    fn locked(&self) -> Result<std::sync::MutexGuard<'_, T>, Failure>;
}

impl<T> Locked<T> for Mutex<T> {
    fn locked(&self) -> Result<std::sync::MutexGuard<'_, T>, Failure> {
        self.lock().map_err(|_| Failure::new(Code::Internal))
    }
}

type Outcome = Result<Option<Value>, Failure>;

/// This helper's relay region, read when it creates a public room: a known
/// code (`ember_protocol::relay::REGIONS`), or `None` when it has none.
pub type HomeRegion = Box<dyn Fn() -> Option<&'static str> + Send + Sync>;

struct Job {
    request_id: u64,
    /// When the request was submitted, counted across every lane, so a lane
    /// can tell whether a request came before one another lane ran first
    /// (a claim queued before a `match_leave`).
    order: u64,
    request: Request,
}

/// Where the IPC reader submits requests: one queue per lane, so a request
/// never waits behind another lane's work. The native side matches answers
/// by `request_id`, so answers of different lanes may arrive in any order.
#[derive(Clone)]
pub struct Handle {
    identity: mpsc::Sender<Job>,
    local: mpsc::Sender<Job>,
    bridge: mpsc::Sender<Job>,
    events: mpsc::Sender<Event>,
    /// The next request's `Job::order`.
    order: Arc<AtomicU64>,
}

impl Handle {
    /// Queues a request on its lane, or answers `busy` at once when that
    /// lane's queue is full.
    ///
    /// Routing here does not change the one way identity and bridge work
    /// still meet: a bridge job that signs (`client::prove`) only tries the
    /// identity lock, so it fails with `identity_busy` while a long identity
    /// job (an export or an unlock) holds it, as before.
    pub fn submit(&self, request_id: u64, request: Request) {
        let op = request.op();
        let order = self.order.fetch_add(1, Ordering::SeqCst);
        let queue = match request.lane() {
            Lane::Identity => &self.identity,
            Lane::Local => &self.local,
            Lane::Bridge => &self.bridge,
        };
        if queue
            .try_send(Job {
                request_id,
                order,
                request,
            })
            .is_err()
        {
            busy(&self.events, request_id, op);
        }
    }
}

/// The answer to a request no queue had room for.
fn busy(events: &mpsc::Sender<Event>, request_id: u64, op: &str) {
    let _ = events.try_send(Event::Tournament {
        request_id,
        op: op.into(),
        ok: false,
        reason: Some("busy".into()),
        identity: None,
        data: None,
    });
}

pub(crate) struct Shared {
    identity: Mutex<Identity>,
    /// The identity status the last answer that carried one held (`respond`).
    status_sent: Mutex<Option<identity::Status>>,
    bridges: Mutex<bridges::Store>,
    client: client::Client,
    http: Arc<Semaphore>,
    play: Mutex<play::State>,
    spool: spool::Spool,
    /// This helper run's Iroh endpoint, which tournament claims name.
    endpoint_id: String,
    /// Names this helper run in claims and reports.
    instance_id: String,
    home_region: HomeRegion,
}

/// Starts the worker on the per-user stores. The identity store is opened on
/// a blocking thread; nothing is created until the user enables an identity.
/// `endpoint_id` is this helper run's Iroh endpoint, and `home_region` reads
/// its relay region.
pub fn spawn(
    events: mpsc::Sender<Event>,
    endpoint_id: String,
    home_region: HomeRegion,
) -> (Handle, tokio::task::JoinHandle<()>) {
    spawn_with(events, endpoint_id, home_region, || {
        let dir = tournament_directory();
        (
            Identity::open_default(),
            bridges::Store::open(dir.clone(), &builtin::builtins()),
            spool::Spool::open(spool_directory(&dir)),
        )
    })
}

/// Reports wait in their own folder beside the bridge list.
fn spool_directory(dir: &Path) -> PathBuf {
    if dir.as_os_str().is_empty() {
        PathBuf::new()
    } else {
        dir.join("reports")
    }
}

/// Starts the worker on stores in the given directories, trusting the
/// `builtins` given in place of this build's, for tests.
pub fn spawn_in(
    events: mpsc::Sender<Event>,
    identity_dir: PathBuf,
    tournament_dir: PathBuf,
    wine: bool,
    endpoint_id: String,
    home_region: HomeRegion,
    builtins: Vec<Builtin>,
) -> (Handle, tokio::task::JoinHandle<()>) {
    spawn_with(events, endpoint_id, home_region, move || {
        (
            Identity::open(identity_dir, wine, identity::Cost::DEFAULT),
            bridges::Store::open(tournament_dir.clone(), &builtins),
            spool::Spool::open(spool_directory(&tournament_dir)),
        )
    })
}

fn spawn_with(
    events: mpsc::Sender<Event>,
    endpoint_id: String,
    home_region: HomeRegion,
    open: impl FnOnce() -> (Identity, bridges::Store, spool::Spool) + Send + 'static,
) -> (Handle, tokio::task::JoinHandle<()>) {
    let (identity_jobs, mut identity_queue) = mpsc::channel::<Job>(QUEUE);
    let (local_jobs, mut local_queue) = mpsc::channel::<Job>(QUEUE);
    let (bridge_jobs, mut bridge_queue) = mpsc::channel::<Job>(QUEUE);
    let handle = Handle {
        identity: identity_jobs,
        local: local_jobs,
        bridge: bridge_jobs,
        events: events.clone(),
        order: Arc::new(AtomicU64::new(1)),
    };
    let task = tokio::spawn(async move {
        let opened = tokio::task::spawn_blocking(open).await;
        let Ok((identity, bridges, spool)) = opened else {
            return;
        };
        let mut random = [0u8; 16];
        if getrandom::fill(&mut random).is_err() {
            return;
        }
        let shared = Arc::new(Shared {
            identity: Mutex::new(identity),
            status_sent: Mutex::new(None),
            bridges: Mutex::new(bridges),
            client: client::Client::new(),
            http: Arc::new(Semaphore::new(HTTP_CONCURRENCY)),
            play: Mutex::new(play::State::default()),
            spool,
            endpoint_id,
            instance_id: ember_protocol::encoding::prefixed_id("ins", random),
            home_region,
        });
        // An initial status lets the native side render without asking.
        respond(&events, &shared, 0, "identity_status", Ok(None), None).await;
        // Identity operations run one at a time, in order.
        let serial = {
            let shared = shared.clone();
            let events = events.clone();
            tokio::spawn(async move {
                while let Some(job) = identity_queue.recv().await {
                    let op = job.request.op();
                    let worker = shared.clone();
                    let (outcome, status) =
                        tokio::task::spawn_blocking(move || identity_request(&worker, job.request))
                            .await
                            .unwrap_or_else(|_| (Err(Failure::new(Code::Internal)), None));
                    respond(&events, &shared, job.request_id, op, outcome, status).await;
                }
            })
        };
        // Local operations, which reach no bridge, run one at a time, in
        // order, and take no HTTP slot.
        let local = {
            let shared = shared.clone();
            let events = events.clone();
            tokio::spawn(async move {
                while let Some(job) = local_queue.recv().await {
                    let op = job.request.op();
                    let outcome = local_request(&shared, job.order, job.request).await;
                    respond(&events, &shared, job.request_id, op, outcome, None).await;
                }
            })
        };
        // Spooled reports go out in the background, oldest first, also after
        // a restart; one pass at a time so they keep their order. A helper
        // with no spool (the headless room host) has nothing to resend, and
        // each pass would only start a blocking thread to find that out.
        let resend = shared.spool.usable().then(|| {
            let shared = shared.clone();
            tokio::spawn(async move {
                loop {
                    tokio::time::sleep(std::time::Duration::from_secs(SPOOL_RETRY_SECS)).await;
                    if let Ok(_permit) = shared.http.acquire().await {
                        play::deliver_pending(&shared).await;
                    }
                }
            })
        });
        // Bridge jobs start only when an HTTP slot is free, so their bounded
        // channel is the only place they wait, and in order, since the
        // semaphore is fair; the set owns every running one.
        let mut running = tokio::task::JoinSet::new();
        while let Some(job) = bridge_queue.recv().await {
            let Ok(permit) = shared.http.clone().acquire_owned().await else {
                break;
            };
            while running.try_join_next().is_some() {}
            let shared = shared.clone();
            let events = events.clone();
            running.spawn(async move {
                let _permit = permit;
                let op = job.request.op();
                let outcome = bridge_request(&shared, job.order, job.request).await;
                respond(&events, &shared, job.request_id, op, outcome, None).await;
            });
        }
        serial.abort();
        local.abort();
        if let Some(resend) = resend {
            resend.abort();
        }
        running.abort_all();
    });
    (handle, task)
}

/// `%LOCALAPPDATA%\Ember\Tournament\v1`, beside the identity store.
fn tournament_directory() -> PathBuf {
    identity::default_directory()
        .ok()
        .and_then(|dir| {
            dir.parent()
                .and_then(Path::parent)
                .map(|root| root.join(r"Tournament\v1"))
        })
        .unwrap_or_default()
}

/// Answers a request. An identity answer always carries the identity status,
/// `status` when the identity job read it under its own lock; any other answer
/// carries it only when it differs from the one last sent, since the native
/// side keeps the last view it was given when an answer has none. Nothing here
/// waits on the identity lock: one held by a key operation omits the status
/// rather than blocking a runtime thread, and an omitted one is not counted as
/// sent.
async fn respond(
    events: &mpsc::Sender<Event>,
    shared: &Shared,
    request_id: u64,
    op: &str,
    outcome: Outcome,
    status: Option<identity::Status>,
) {
    let status = status.or_else(|| {
        shared
            .identity
            .try_lock()
            .ok()
            .map(|identity| identity.status())
    });
    let status = status.filter(|status| {
        let Ok(mut sent) = shared.status_sent.lock() else {
            return true;
        };
        let carried = op.starts_with("identity_") || sent.as_ref() != Some(status);
        if carried {
            *sent = Some(status.clone());
        }
        carried
    });
    let status = status.and_then(|status| serde_json::to_value(status).ok());
    let (ok, reason, data) = match outcome {
        Ok(data) => (true, None, data),
        Err(failure) => (false, Some(failure.into_reason()), None),
    };
    // Waits for queue room instead of dropping: answers are rare and the
    // native side must see each one.
    let _ = events
        .send(Event::Tournament {
            request_id,
            op: op.into(),
            ok,
            reason,
            identity: status,
            data,
        })
        .await;
}

fn check_path(path: &str) -> Result<&Path, Failure> {
    let candidate = Path::new(path);
    if path.is_empty() || path.len() > MAX_PATH || !candidate.is_absolute() || path.contains('\0') {
        return Err(Failure::new(Code::InvalidPath));
    }
    Ok(candidate)
}

fn read_backup(path: &str) -> Result<Zeroizing<Vec<u8>>, Failure> {
    read_file(path, MAX_BACKUP_FILE)
}

/// The file at `path`, refused above `max` bytes.
fn read_file(path: &str, max: usize) -> Result<Zeroizing<Vec<u8>>, Failure> {
    use std::io::Read;
    let path = check_path(path)?;
    let file = std::fs::File::open(path).map_err(|_| Failure::new(Code::FileUnreadable))?;
    if file
        .metadata()
        .map_err(|_| Failure::new(Code::FileUnreadable))?
        .len()
        > max as u64
    {
        return Err(Failure::new(Code::Corrupt));
    }
    let mut bytes = Zeroizing::new(Vec::new());
    file.take(max as u64 + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| Failure::new(Code::FileUnreadable))?;
    Ok(bytes)
}

fn protection(passphrase: Option<&Zeroizing<String>>) -> Protection<'_> {
    passphrase.map_or(Protection::Default, |passphrase| {
        Protection::Passphrase(passphrase)
    })
}

/// An identity request, and the identity status as it stands after it, read
/// under the same lock.
fn identity_request(shared: &Shared, request: Request) -> (Outcome, Option<identity::Status>) {
    let mut identity = match shared.identity.locked() {
        Ok(identity) => identity,
        Err(failure) => return (Err(failure), None),
    };
    let outcome = run_identity(shared, &mut identity, request);
    let status = identity.status();
    (outcome, Some(status))
}

fn run_identity(shared: &Shared, identity: &mut Identity, request: Request) -> Outcome {
    match request {
        Request::IdentityStatus {} => Ok(None),
        Request::IdentityEnable { passphrase } => {
            identity.enable(protection(passphrase.as_ref()))?;
            Ok(None)
        }
        Request::IdentityUnlock { passphrase } => {
            identity.unlock(&passphrase)?;
            Ok(None)
        }
        Request::IdentityExport { path, passphrase } => {
            let target = check_path(&path)?;
            // The game names a file in its own backups folder, which may not exist yet.
            if let Some(parent) = target.parent() {
                std::fs::create_dir_all(parent).map_err(|_| Failure::new(Code::Io))?;
            }
            identity.export(target, &passphrase)?;
            Ok(Some(json!({ "path": path })))
        }
        Request::IdentityPreviewImport { path, passphrase } => {
            let bytes = read_backup(&path)?;
            let preview = identity.preview_import(&bytes, &passphrase)?;
            Ok(serde_json::to_value(preview).ok())
        }
        Request::IdentityImport {
            path,
            passphrase,
            expected_ember_id,
            local_passphrase,
            replace,
        } => {
            let expected = EmberId::parse(&expected_ember_id)
                .map_err(|_| Failure::new(Code::InvalidRequest))?;
            let bytes = read_backup(&path)?;
            identity.import(
                &bytes,
                &passphrase,
                &expected,
                protection(local_passphrase.as_ref()),
                replace,
            )?;
            // Sessions may belong to the identity the import replaced.
            shared.client.forget_sessions();
            Ok(None)
        }
        Request::IdentityReset { confirm_ember_id } => {
            let confirm = confirm_ember_id
                .as_deref()
                .map(EmberId::parse)
                .transpose()
                .map_err(|_| Failure::new(Code::InvalidRequest))?;
            identity.reset(confirm.as_ref())?;
            // Sessions belong to the retired key.
            shared.client.forget_sessions();
            Ok(None)
        }
        _ => Err(Failure::new(Code::InvalidRequest)),
    }
}

/// A request that reaches no bridge: this run's bridge list and what it
/// verified for a match. `order` is the request's `Job::order`.
async fn local_request(shared: &Arc<Shared>, order: u64, request: Request) -> Outcome {
    match request {
        Request::BridgeList {} => {
            let (bridges, removed) = {
                let store = shared.bridges.locked()?;
                (store.list(), store.removed())
            };
            Ok(Some(json!({ "bridges": bridges, "removed": removed })))
        }
        Request::BridgeForget { bridge_id } => {
            let store = shared.clone();
            let id = bridge_id.clone();
            tokio::task::spawn_blocking(move || store.bridges.locked()?.forget(&id))
                .await
                .map_err(|_| Failure::new(Code::Internal))??;
            shared.client.forget_session(&bridge_id);
            Ok(None)
        }
        Request::MatchLeave { match_id } => play::forget(shared, &match_id, order),
        _ => Err(Failure::new(Code::InvalidRequest)),
    }
}

/// A request to a bridge. `order` is the request's `Job::order`.
async fn bridge_request(shared: &Arc<Shared>, order: u64, request: Request) -> Outcome {
    match request {
        Request::BridgeInspect { origin } => {
            let (profile, offered) = shared.client.inspect(&origin).await?;
            Ok(Some(
                json!({ "profile": profile, "capabilities": offered.to_json() }),
            ))
        }
        Request::BridgeApprove { origin, bridge_id } => {
            let (profile, _) = shared.client.inspect(&origin).await?;
            if profile.bridge_id != bridge_id {
                return Err(Failure::new(Code::BridgeChanged));
            }
            // The keys that sign this bridge's bindings and permits, from
            // the origin being approved.
            let keys = shared.client.signing_keys(&origin).await?;
            let store = shared.clone();
            let displaced = tokio::task::spawn_blocking(move || {
                store.bridges.locked()?.approve(&profile, keys)
            })
            .await
            .map_err(|_| Failure::new(Code::Internal))??;
            // A replaced profile's session is never offered to another origin.
            for id in displaced.iter().chain([&bridge_id]) {
                shared.client.forget_session(id);
            }
            Ok(None)
        }
        Request::LinkList { bridge_id } => client::links(shared, &bridge_id).await,
        Request::LinkClaim {
            bridge_id,
            connection_id,
            code,
        } => client::claim(shared, &bridge_id, &connection_id, &code).await,
        Request::LinkCancel {
            bridge_id,
            claim_id,
        } => client::cancel_claim(shared, &bridge_id, &claim_id).await,
        Request::LinkRemove { bridge_id, link_id } => {
            client::remove_link(shared, &bridge_id, &link_id).await
        }
        Request::DiscordStatus { bridge_id } => {
            sign_in::status(shared, &bridge_id, &discord::routes()).await
        }
        Request::DiscordConnect { bridge_id } => {
            sign_in::connect(shared, &bridge_id, &discord::routes()).await
        }
        Request::DiscordRemove { bridge_id } => {
            sign_in::remove(shared, &bridge_id, &discord::routes()).await
        }
        Request::DiscordCancel { bridge_id } => {
            sign_in::cancel(shared, &bridge_id, &discord::routes()).await
        }
        Request::SiteStatus {
            bridge_id,
            site,
            organizer,
        } => sites::status(shared, &bridge_id, &site, organizer).await,
        Request::SiteConnect {
            bridge_id,
            site,
            organizer,
        } => sites::connect(shared, &bridge_id, &site, organizer).await,
        Request::SiteRemove {
            bridge_id,
            site,
            organizer,
        } => sites::remove(shared, &bridge_id, &site, organizer).await,
        Request::SiteCancel { bridge_id, site } => sites::cancel(shared, &bridge_id, &site).await,
        Request::AssignmentList { bridge_id } => play::assignments(shared, &bridge_id).await,
        Request::DeskRead { bridge_id, event } => {
            desk::read(shared, &bridge_id, event.as_deref()).await
        }
        Request::DeskAct {
            bridge_id,
            path,
            command,
        } => desk::act(shared, &bridge_id, &path, command).await,
        Request::DeskWeb {
            bridge_id,
            seen,
            end,
        } => desk::web(shared, &bridge_id, seen, end).await,
        Request::DeskHandoff { bridge_id } => desk::handoff(shared, &bridge_id).await,
        Request::DeskApprove {
            bridge_id,
            code,
            number,
        } => desk::approve(shared, &bridge_id, &code, number).await,
        Request::DeskCodeInfo { bridge_id, code } => {
            desk::code_info(shared, &bridge_id, &code).await
        }
        Request::DeskRedeem { bridge_id, code } => desk::redeem(shared, &bridge_id, &code).await,
        Request::DeskSiteState {
            bridge_id,
            site,
            tournament,
        } => desk::site_state(shared, &bridge_id, &site, &tournament).await,
        Request::MatchClaim {
            bridge_id,
            match_id,
            build,
            ready,
        } => play::claim_match(shared, &bridge_id, &match_id, &build, ready, order).await,
        Request::RoomPublish {
            bridge_id,
            match_id,
            room,
        } => play::publish_room(shared, &bridge_id, &match_id, room).await,
        Request::RoomList { bridge_id, build } => rooms::list(shared, &bridge_id, &build).await,
        Request::RoomCreate {
            bridge_id,
            name,
            capacity,
            build,
            own_delay,
        } => rooms::create(shared, &bridge_id, &name, capacity, &build, own_delay).await,
        Request::RoomTicket {
            bridge_id,
            room_id,
            build,
        } => rooms::ticket(shared, &bridge_id, &room_id, &build).await,
        Request::RankingStanding { bridge_id } => ranking::standing(shared, &bridge_id).await,
        Request::RankingChoose { bridge_id, choice } => {
            ranking::choose(shared, &bridge_id, choice).await
        }
        Request::RankingBoard { bridge_id } => ranking::board(shared, &bridge_id).await,
        Request::GamePrepare {
            bridge_id,
            match_id,
            match_generation,
        } => play::prepare_game(shared, &bridge_id, &match_id, match_generation).await,
        Request::GameReport {
            bridge_id,
            match_id,
            match_generation,
            result,
            capture_frame,
            confirmed_input_frame,
        } => {
            play::report_game(
                shared,
                &bridge_id,
                &match_id,
                match_generation,
                result,
                capture_frame,
                confirmed_input_frame,
            )
            .await
        }
        Request::ReplaySend {
            bridge_id,
            match_id,
            revision,
            path,
            sides,
        } => replays::send(shared, &bridge_id, &match_id, revision.0, &path, &sides).await,
        Request::ReplayArchive { path, sides } => replays::archive(shared, &path, &sides).await,
        Request::ReplayPack {
            bridge_id,
            match_id,
            path,
        } => replays::pack(shared, &bridge_id, &match_id, &path).await,
        Request::MatchWatch {
            bridge_id,
            match_id,
            build,
        } => watch::watch_match(shared, &bridge_id, &match_id, &build).await,
        Request::MatchUnwatch {
            bridge_id,
            match_id,
        } => watch::unwatch_match(shared, &bridge_id, &match_id).await,
        _ => Err(Failure::new(Code::InvalidRequest)),
    }
}

#[cfg(test)]
mod tests {
    use std::{collections::HashSet, time::Duration};

    use super::*;

    // Every op the list names is the tag serde reads for its variant, each
    // appears once, and only identity ops run on the identity lane, as the
    // `identity_` prefix routed them before.
    #[test]
    fn each_request_is_named_and_routed_once() {
        let mut seen = HashSet::new();
        for &(op, lane) in REQUESTS {
            assert!(seen.insert(op), "{op} listed twice");
            assert_eq!(lane == Lane::Identity, op.starts_with("identity_"), "{op}");
            match serde_json::from_value::<Request>(json!({ "op": op })) {
                Ok(request) => {
                    assert_eq!(request.op(), op);
                    assert_eq!(request.lane(), lane);
                }
                Err(error) => assert!(
                    !error.to_string().contains("unknown variant"),
                    "{op}: {error}"
                ),
            }
        }
        assert_eq!(seen.len(), 48);
        let local: Vec<_> = REQUESTS
            .iter()
            .filter(|(_, lane)| *lane == Lane::Local)
            .map(|(op, _)| *op)
            .collect();
        assert_eq!(local, ["bridge_list", "bridge_forget", "match_leave"]);
        let forget: Request =
            serde_json::from_value(json!({ "op": "bridge_forget", "bridge_id": "brg_x" })).unwrap();
        assert_eq!((forget.op(), forget.lane()), ("bridge_forget", Lane::Local));
    }

    /// An origin that takes every connection and never answers, counting them.
    async fn silent_origin() -> (
        String,
        Arc<std::sync::atomic::AtomicUsize>,
        tokio::task::JoinHandle<()>,
    ) {
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let origin = format!("http://{}", listener.local_addr().unwrap());
        let taken = Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let counted = taken.clone();
        let task = tokio::spawn(async move {
            let mut held = Vec::new();
            while let Ok((socket, _)) = listener.accept().await {
                counted.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
                held.push(socket);
            }
        });
        (origin, taken, task)
    }

    async fn answer(events: &mut mpsc::Receiver<Event>) -> (u64, String, bool, Option<String>) {
        let event = tokio::time::timeout(Duration::from_secs(10), events.recv())
            .await
            .expect("an answer in time")
            .expect("worker running");
        let Event::Tournament {
            request_id,
            op,
            ok,
            reason,
            ..
        } = event
        else {
            panic!("not a tournament answer");
        };
        (request_id, op, ok, reason)
    }

    // Four bridge requests hang on an origin that never answers and a fifth
    // waits for their HTTP slots; identity and local requests sent after
    // them answer at once. Once the origin goes away, every bridge request
    // answers, and no more than four were ever in flight.
    #[tokio::test]
    async fn identity_status_answers_while_four_bridge_requests_hang() {
        let mut seed = [0; 16];
        getrandom::fill(&mut seed).unwrap();
        let dir =
            std::env::temp_dir().join(ember_protocol::encoding::prefixed_id("sf4-net-lanes", seed));
        let (sender, mut events) = mpsc::channel(64);
        let (handle, task) = spawn_in(
            sender,
            dir.join("identity"),
            dir.join("tournament"),
            false,
            "a".repeat(64),
            Box::new(|| None),
            Vec::new(),
        );
        // The status every worker sends once its stores are open.
        assert_eq!(answer(&mut events).await.0, 0);
        let (origin, taken, silent) = silent_origin().await;
        for id in 1..=5 {
            let inspect = json!({ "op": "bridge_inspect", "origin": origin });
            handle.submit(id, serde_json::from_value(inspect).unwrap());
        }
        let started = tokio::time::Instant::now();
        while taken.load(std::sync::atomic::Ordering::SeqCst) < HTTP_CONCURRENCY {
            assert!(
                started.elapsed() < Duration::from_secs(10),
                "bridge requests not sent"
            );
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
        for (id, request) in [
            (10, json!({ "op": "identity_status" })),
            (11, json!({ "op": "bridge_list" })),
            (12, json!({ "op": "match_leave", "match_id": "emt_x" })),
        ] {
            handle.submit(id, serde_json::from_value(request).unwrap());
        }
        let mut answered = Vec::new();
        for _ in 0..3 {
            let (id, op, ok, _) = answer(&mut events).await;
            assert!(ok, "{op}");
            answered.push(id);
        }
        answered.sort_unstable();
        assert_eq!(answered, [10, 11, 12]);
        // Well inside the 15 s a hanging bridge request may take.
        assert!(started.elapsed() < Duration::from_secs(5));
        assert_eq!(
            taken.load(std::sync::atomic::Ordering::SeqCst),
            HTTP_CONCURRENCY
        );

        silent.abort();
        let mut failed = Vec::new();
        for _ in 0..5 {
            let (id, op, ok, reason) = answer(&mut events).await;
            assert_eq!((op.as_str(), ok), ("bridge_inspect", false));
            assert_eq!(reason.as_deref(), Some("bridge_unreachable"));
            failed.push(id);
        }
        failed.sort_unstable();
        assert_eq!(failed, [1, 2, 3, 4, 5]);
        drop(handle);
        let _ = tokio::time::timeout(Duration::from_secs(10), task).await;
        let _ = std::fs::remove_dir_all(dir);
    }

    // The codes the game reads are spelled as they always were, and a bridge's
    // own code or a key-storage failure is worded as it was.
    #[test]
    fn failure_codes_are_the_words_the_game_reads() {
        for (code, word) in [
            (Code::BindingMismatch, "binding_mismatch"),
            (Code::BridgeChanged, "bridge_changed"),
            (Code::BridgeInvalidResponse, "bridge_invalid_response"),
            (Code::BridgeMismatch, "bridge_mismatch"),
            (Code::BridgeNotApproved, "bridge_not_approved"),
            (Code::BridgePinned, "bridge_pinned"),
            (Code::BridgeResponseTooLarge, "bridge_response_too_large"),
            (Code::BridgeUnreachable, "bridge_unreachable"),
            (Code::BrowserUnavailable, "browser_unavailable"),
            (Code::ChallengeMismatch, "challenge_mismatch"),
            (Code::Corrupt, "corrupt"),
            (Code::FileUnreadable, "file_unreadable"),
            (Code::IdentityBusy, "identity_busy"),
            (Code::IdentityUnavailable, "identity_unavailable"),
            (Code::Internal, "internal"),
            (Code::InvalidBridge, "invalid_bridge"),
            (Code::InvalidName, "invalid_name"),
            (Code::InvalidOrigin, "invalid_origin"),
            (Code::InvalidPath, "invalid_path"),
            (Code::InvalidRequest, "invalid_request"),
            (Code::Io, "io"),
            (Code::MatchLeft, "match_left"),
            (Code::NotABridge, "not_a_bridge"),
            (Code::NotBound, "not_bound"),
            (Code::NotFound, "not_found"),
            (Code::OwnDelayUnsupported, "own_delay_unsupported"),
            (Code::PermitMismatch, "permit_mismatch"),
            (Code::PinnedBridgeMismatch, "pinned_bridge_mismatch"),
            (Code::RankingUnavailable, "ranking_unavailable"),
            (Code::ReportNotSaved, "report_not_saved"),
            (Code::RoomsUnavailable, "rooms_unavailable"),
            (Code::ServiceUnavailable, "service_unavailable"),
            (Code::SignInOff, "sign_in_off"),
            (Code::TooManyBridges, "too_many_bridges"),
            (Code::UnknownPermit, "unknown_permit"),
            (Code::UnsupportedBridge, "unsupported_bridge"),
            (Code::UntrustedSignature, "untrusted_signature"),
            (Code::WatchUnsupported, "watch_unsupported"),
        ] {
            assert_eq!(Failure::new(code).as_str(), word);
            assert_eq!(Failure::new(code).into_reason(), word);
        }
        let from_identity: Failure = identity::Failure::WrongPassphrase.into();
        assert_eq!(from_identity.as_str(), "wrong_passphrase");
        let bridge = Failure::Bridge("http_502".into());
        assert_eq!(bridge.as_str(), "http_502");
        assert_eq!(bridge.into_reason(), "http_502");
    }

    /// The identity member of the next answer, whether it has one.
    async fn carried(events: &mut mpsc::Receiver<Event>) -> Option<Value> {
        let Some(Event::Tournament { identity, .. }) = events.recv().await else {
            panic!("not a tournament answer");
        };
        identity
    }

    // An identity answer always carries the status. Any other answer carries
    // it when it differs from the last one sent, and a status that could not
    // be read (the lock held by a key operation) is neither sent nor counted
    // as sent.
    #[tokio::test]
    async fn the_identity_status_travels_with_identity_answers_and_when_it_changes() {
        let dir = std::env::temp_dir().join(ember_protocol::encoding::prefixed_id(
            "sf4-net-status",
            [5; 16],
        ));
        let shared = test_support::shared_pinning(&dir, &[]);
        let (sender, mut events) = mpsc::channel(8);
        let answer = |op: &'static str| {
            let (sender, shared) = (&sender, &shared);
            async move { respond(sender, shared, 1, op, Ok(None), None).await }
        };

        answer("bridge_list").await;
        let first = carried(&mut events)
            .await
            .expect("the first status is sent");
        assert_eq!(first["state"], "disabled");
        answer("bridge_list").await;
        assert!(carried(&mut events).await.is_none(), "unchanged");
        answer("identity_status").await;
        assert_eq!(carried(&mut events).await, Some(first));

        // The lock held: the status is left out and not counted as sent.
        shared
            .identity
            .lock()
            .unwrap()
            .enable(identity::Protection::Passphrase("a test passphrase"))
            .unwrap();
        let held = shared.identity.lock().unwrap();
        answer("bridge_list").await;
        assert!(carried(&mut events).await.is_none(), "locked");
        drop(held);
        answer("bridge_list").await;
        let changed = carried(&mut events).await.expect("the new status is sent");
        assert_eq!(changed["state"], "ready");
        answer("bridge_list").await;
        assert!(carried(&mut events).await.is_none(), "unchanged again");

        // An identity job's own reading is the one sent.
        let status = shared.identity.lock().unwrap().status();
        respond(
            &sender,
            &shared,
            2,
            "identity_unlock",
            Ok(None),
            Some(status),
        )
        .await;
        assert_eq!(carried(&mut events).await, Some(changed));
        let _ = std::fs::remove_dir_all(dir);
    }

    // A full lane answers `busy` at once, and only that lane is full.
    #[tokio::test]
    async fn a_full_lane_answers_busy() {
        let (events, mut answers) = mpsc::channel(4 * QUEUE);
        let (identity, _identity_queue) = mpsc::channel(1);
        let (local, _local_queue) = mpsc::channel(1);
        let (bridge, _bridge_queue) = mpsc::channel(1);
        let handle = Handle {
            identity,
            local,
            bridge,
            events,
            order: Arc::new(AtomicU64::new(1)),
        };
        let submit = |id: u64, request: Value| {
            handle.submit(id, serde_json::from_value(request).unwrap());
        };
        submit(
            1,
            json!({ "op": "bridge_inspect", "origin": "http://127.0.0.1:9" }),
        );
        submit(
            2,
            json!({ "op": "bridge_inspect", "origin": "http://127.0.0.1:9" }),
        );
        submit(3, json!({ "op": "identity_status" }));
        submit(4, json!({ "op": "bridge_list" }));
        let (id, op, ok, reason) = answer(&mut answers).await;
        assert_eq!((id, op.as_str(), ok), (2, "bridge_inspect", false));
        assert_eq!(reason.as_deref(), Some("busy"));
        assert!(answers.try_recv().is_err());
    }
}
