//! Identity and tournament-bridge operations for the native side.
//!
//! These commands never reach the room actor. The IPC reader hands them to
//! this worker, which runs key storage and KDF work on blocking threads and
//! bridge HTTP on its own bounded task set, then answers with a `tournament`
//! event. A failure here is reported and never ends the helper, and nothing
//! here waits on, or is waited on by, room or gameplay work (spec 18.3).
mod bridges;
mod client;
mod discord;
mod play;
mod spool;

use std::{
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
};

use ember_protocol::{
    EmberId, encoding::Counter, play::PublishRoom, report::Outcome as GameOutcome,
};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use tokio::sync::{Semaphore, mpsc};
use zeroize::Zeroizing;

use crate::{
    identity::{self, Identity, MAX_BACKUP_FILE, Protection},
    service::Event,
};

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
    AssignmentList {
        bridge_id: String,
    },
    MatchClaim {
        bridge_id: String,
        match_id: String,
        build: String,
    },
    RoomPublish {
        bridge_id: String,
        match_id: String,
        room: PublishRoom,
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
}

impl Request {
    fn op(&self) -> &'static str {
        match self {
            Self::IdentityStatus {} => "identity_status",
            Self::IdentityEnable { .. } => "identity_enable",
            Self::IdentityUnlock { .. } => "identity_unlock",
            Self::IdentityExport { .. } => "identity_export",
            Self::IdentityPreviewImport { .. } => "identity_preview_import",
            Self::IdentityImport { .. } => "identity_import",
            Self::IdentityReset { .. } => "identity_reset",
            Self::BridgeList {} => "bridge_list",
            Self::BridgeInspect { .. } => "bridge_inspect",
            Self::BridgeApprove { .. } => "bridge_approve",
            Self::BridgeForget { .. } => "bridge_forget",
            Self::LinkList { .. } => "link_list",
            Self::LinkClaim { .. } => "link_claim",
            Self::LinkCancel { .. } => "link_cancel",
            Self::LinkRemove { .. } => "link_remove",
            Self::DiscordStatus { .. } => "discord_status",
            Self::DiscordConnect { .. } => "discord_connect",
            Self::DiscordRemove { .. } => "discord_remove",
            Self::AssignmentList { .. } => "assignment_list",
            Self::MatchClaim { .. } => "match_claim",
            Self::RoomPublish { .. } => "room_publish",
            Self::GamePrepare { .. } => "game_prepare",
            Self::GameReport { .. } => "game_report",
            Self::MatchLeave { .. } => "match_leave",
        }
    }

    fn is_identity(&self) -> bool {
        self.op().starts_with("identity_")
    }
}

/// Why a request failed: a stable, secret-free code for the native UI.
#[derive(Debug)]
pub struct Failure(pub String);

impl From<identity::Failure> for Failure {
    fn from(failure: identity::Failure) -> Self {
        Self(
            serde_json::to_value(failure)
                .ok()
                .and_then(|value| value.as_str().map(str::to_owned))
                .unwrap_or_else(|| "io".into()),
        )
    }
}

impl Failure {
    fn new(code: &str) -> Self {
        Self(code.into())
    }
}

type Outcome = Result<Option<Value>, Failure>;

struct Job {
    request_id: u64,
    request: Request,
}

/// Where the IPC reader submits requests.
#[derive(Clone)]
pub struct Handle {
    jobs: mpsc::Sender<Job>,
    events: mpsc::Sender<Event>,
}

impl Handle {
    /// Queues a request, or answers `busy` at once when the queue is full.
    pub fn submit(&self, request_id: u64, request: Request) {
        let op = request.op();
        if self
            .jobs
            .try_send(Job {
                request_id,
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
    bridges: Mutex<bridges::Store>,
    client: client::Client,
    http: Arc<Semaphore>,
    play: Mutex<play::State>,
    spool: spool::Spool,
    /// This helper run's Iroh endpoint, which tournament claims name.
    endpoint_id: String,
    /// Names this helper run in claims and reports.
    instance_id: String,
}

/// Starts the worker on the per-user stores. The identity store is opened on
/// a blocking thread; nothing is created until the user enables an identity.
/// `endpoint_id` is this helper run's Iroh endpoint.
pub fn spawn(
    events: mpsc::Sender<Event>,
    endpoint_id: String,
) -> (Handle, tokio::task::JoinHandle<()>) {
    spawn_with(events, endpoint_id, || {
        let dir = tournament_directory();
        (
            Identity::open_default(),
            bridges::Store::open(dir.clone()),
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

/// Starts the worker on stores in the given directories, for tests.
pub fn spawn_in(
    events: mpsc::Sender<Event>,
    identity_dir: PathBuf,
    tournament_dir: PathBuf,
    wine: bool,
    endpoint_id: String,
) -> (Handle, tokio::task::JoinHandle<()>) {
    spawn_with(events, endpoint_id, move || {
        (
            Identity::open(identity_dir, wine, identity::Cost::DEFAULT),
            bridges::Store::open(tournament_dir.clone()),
            spool::Spool::open(spool_directory(&tournament_dir)),
        )
    })
}

fn spawn_with(
    events: mpsc::Sender<Event>,
    endpoint_id: String,
    open: impl FnOnce() -> (Identity, bridges::Store, spool::Spool) + Send + 'static,
) -> (Handle, tokio::task::JoinHandle<()>) {
    let (jobs, mut receiver) = mpsc::channel::<Job>(QUEUE);
    let handle = Handle {
        jobs,
        events: events.clone(),
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
            bridges: Mutex::new(bridges),
            client: client::Client::new(),
            http: Arc::new(Semaphore::new(HTTP_CONCURRENCY)),
            play: Mutex::new(play::State::default()),
            spool,
            endpoint_id,
            instance_id: ember_protocol::encoding::prefixed_id("ins", random),
        });
        // An initial status lets the native side render without asking.
        respond(&events, &shared, 0, "identity_status", Ok(None)).await;
        // Identity operations run one at a time, in order; bridge requests
        // run concurrently up to the HTTP bound.
        let (identity_jobs, mut identity_queue) = mpsc::channel::<Job>(QUEUE);
        let serial = {
            let shared = shared.clone();
            let events = events.clone();
            tokio::spawn(async move {
                while let Some(job) = identity_queue.recv().await {
                    let op = job.request.op();
                    let worker = shared.clone();
                    let outcome =
                        tokio::task::spawn_blocking(move || identity_request(&worker, job.request))
                            .await
                            .unwrap_or_else(|_| Err(Failure::new("internal")));
                    respond(&events, &shared, job.request_id, op, outcome).await;
                }
            })
        };
        // Spooled reports go out in the background, oldest first, also after
        // a restart; one pass at a time so they keep their order.
        let resend = {
            let shared = shared.clone();
            tokio::spawn(async move {
                loop {
                    tokio::time::sleep(std::time::Duration::from_secs(SPOOL_RETRY_SECS)).await;
                    if let Ok(_permit) = shared.http.acquire().await {
                        play::deliver_pending(&shared).await;
                    }
                }
            })
        };
        // Bridge jobs start only when an HTTP slot is free, so the bounded
        // channel is the only place they wait; the set owns every running one.
        let mut running = tokio::task::JoinSet::new();
        while let Some(job) = receiver.recv().await {
            if job.request.is_identity() {
                if let Err(full) = identity_jobs.try_send(job) {
                    let job = full.into_inner();
                    busy(&events, job.request_id, job.request.op());
                }
                continue;
            }
            let Ok(permit) = shared.http.clone().acquire_owned().await else {
                break;
            };
            while running.try_join_next().is_some() {}
            let shared = shared.clone();
            let events = events.clone();
            running.spawn(async move {
                let _permit = permit;
                let op = job.request.op();
                let outcome = bridge_request(&shared, job.request).await;
                respond(&events, &shared, job.request_id, op, outcome).await;
            });
        }
        serial.abort();
        resend.abort();
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

async fn respond(
    events: &mpsc::Sender<Event>,
    shared: &Shared,
    request_id: u64,
    op: &str,
    outcome: Outcome,
) {
    // Only tried: an identity operation holding the lock omits the status
    // rather than blocking a runtime thread.
    let status = shared
        .identity
        .try_lock()
        .ok()
        .and_then(|identity| serde_json::to_value(identity.status()).ok());
    let (ok, reason, data) = match outcome {
        Ok(data) => (true, None, data),
        Err(Failure(code)) => (false, Some(code), None),
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
        return Err(Failure::new("invalid_path"));
    }
    Ok(candidate)
}

fn read_backup(path: &str) -> Result<Zeroizing<Vec<u8>>, Failure> {
    use std::io::Read;
    let path = check_path(path)?;
    let file = std::fs::File::open(path).map_err(|_| Failure::new("file_unreadable"))?;
    if file
        .metadata()
        .map_err(|_| Failure::new("file_unreadable"))?
        .len()
        > MAX_BACKUP_FILE as u64
    {
        return Err(Failure::new("corrupt"));
    }
    let mut bytes = Zeroizing::new(Vec::new());
    file.take(MAX_BACKUP_FILE as u64 + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| Failure::new("file_unreadable"))?;
    Ok(bytes)
}

fn protection(passphrase: Option<&Zeroizing<String>>) -> Protection<'_> {
    passphrase.map_or(Protection::Default, |passphrase| {
        Protection::Passphrase(passphrase)
    })
}

fn identity_request(shared: &Shared, request: Request) -> Outcome {
    let mut identity = shared
        .identity
        .lock()
        .map_err(|_| Failure::new("internal"))?;
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
                std::fs::create_dir_all(parent).map_err(|_| Failure::new("io"))?;
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
            let expected =
                EmberId::parse(&expected_ember_id).map_err(|_| Failure::new("invalid_request"))?;
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
                .map_err(|_| Failure::new("invalid_request"))?;
            identity.reset(confirm.as_ref())?;
            // Sessions belong to the retired key.
            shared.client.forget_sessions();
            Ok(None)
        }
        _ => Err(Failure::new("invalid_request")),
    }
}

async fn bridge_request(shared: &Arc<Shared>, request: Request) -> Outcome {
    match request {
        Request::BridgeList {} => {
            let bridges = shared
                .bridges
                .lock()
                .map_err(|_| Failure::new("internal"))?
                .list();
            Ok(Some(json!({ "bridges": bridges })))
        }
        Request::BridgeInspect { origin } => {
            let (profile, capabilities) = shared.client.inspect(&origin).await?;
            Ok(Some(
                json!({ "profile": profile, "capabilities": capabilities }),
            ))
        }
        Request::BridgeApprove { origin, bridge_id } => {
            let (profile, _) = shared.client.inspect(&origin).await?;
            if profile.bridge_id != bridge_id {
                return Err(Failure::new("bridge_changed"));
            }
            // The keys that sign this bridge's bindings and permits, from
            // the origin being approved.
            let keys = shared.client.signing_keys(&origin).await?;
            let store = shared.clone();
            let displaced = tokio::task::spawn_blocking(move || {
                store
                    .bridges
                    .lock()
                    .map_err(|_| Failure::new("internal"))?
                    .approve(&profile, keys)
            })
            .await
            .map_err(|_| Failure::new("internal"))??;
            // A replaced profile's session is never offered to another origin.
            for id in displaced.iter().chain([&bridge_id]) {
                shared.client.forget_session(id);
            }
            Ok(None)
        }
        Request::BridgeForget { bridge_id } => {
            let store = shared.clone();
            let id = bridge_id.clone();
            tokio::task::spawn_blocking(move || {
                store
                    .bridges
                    .lock()
                    .map_err(|_| Failure::new("internal"))?
                    .forget(&id)
            })
            .await
            .map_err(|_| Failure::new("internal"))??;
            shared.client.forget_session(&bridge_id);
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
        Request::DiscordStatus { bridge_id } => discord::status(shared, &bridge_id).await,
        Request::DiscordConnect { bridge_id } => discord::connect(shared, &bridge_id).await,
        Request::DiscordRemove { bridge_id } => discord::remove(shared, &bridge_id).await,
        Request::AssignmentList { bridge_id } => play::assignments(shared, &bridge_id).await,
        Request::MatchClaim {
            bridge_id,
            match_id,
            build,
        } => play::claim_match(shared, &bridge_id, &match_id, &build).await,
        Request::RoomPublish {
            bridge_id,
            match_id,
            room,
        } => play::publish_room(shared, &bridge_id, &match_id, room).await,
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
        Request::MatchLeave { match_id } => play::forget(shared, &match_id),
        _ => Err(Failure::new("invalid_request")),
    }
}
