//! Identity and tournament-bridge operations for the native side.
//!
//! These commands never reach the room actor. The IPC reader hands them to
//! this worker, which runs key storage and KDF work on blocking threads and
//! bridge HTTP on its own bounded task set, then answers with a `tournament`
//! event. A failure here is reported and never ends the helper, and nothing
//! here waits on, or is waited on by, room or gameplay work (spec 18.3).
mod bridges;
mod client;

use std::{
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
};

use ember_protocol::EmberId;
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
            let _ = self.events.try_send(Event::Tournament {
                request_id,
                op: op.into(),
                ok: false,
                reason: Some("busy".into()),
                identity: None,
                data: None,
            });
        }
    }
}

pub(crate) struct Shared {
    identity: Mutex<Identity>,
    bridges: Mutex<bridges::Store>,
    client: client::Client,
    http: Semaphore,
}

/// Starts the worker on the per-user stores. The identity store is opened on
/// a blocking thread; nothing is created until the user enables an identity.
pub fn spawn(events: mpsc::Sender<Event>) -> (Handle, tokio::task::JoinHandle<()>) {
    spawn_with(events, || {
        (
            Identity::open_default(),
            bridges::Store::open(tournament_directory()),
        )
    })
}

/// Starts the worker on stores in the given directories, for tests.
pub fn spawn_in(
    events: mpsc::Sender<Event>,
    identity_dir: PathBuf,
    tournament_dir: PathBuf,
    wine: bool,
) -> (Handle, tokio::task::JoinHandle<()>) {
    spawn_with(events, move || {
        (
            Identity::open(identity_dir, wine, identity::Cost::DEFAULT),
            bridges::Store::open(tournament_dir),
        )
    })
}

fn spawn_with(
    events: mpsc::Sender<Event>,
    open: impl FnOnce() -> (Identity, bridges::Store) + Send + 'static,
) -> (Handle, tokio::task::JoinHandle<()>) {
    let (jobs, mut receiver) = mpsc::channel::<Job>(QUEUE);
    let handle = Handle {
        jobs,
        events: events.clone(),
    };
    let task = tokio::spawn(async move {
        let opened = tokio::task::spawn_blocking(open).await;
        let Ok((identity, bridges)) = opened else {
            return;
        };
        let shared = Arc::new(Shared {
            identity: Mutex::new(identity),
            bridges: Mutex::new(bridges),
            client: client::Client::new(),
            http: Semaphore::new(HTTP_CONCURRENCY),
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
        while let Some(job) = receiver.recv().await {
            if job.request.is_identity() {
                if identity_jobs.try_send(job).is_err() {
                    break;
                }
                continue;
            }
            let shared = shared.clone();
            let events = events.clone();
            tokio::spawn(async move {
                let op = job.request.op();
                let outcome = match shared.http.acquire().await {
                    Ok(_permit) => bridge_request(&shared, job.request).await,
                    Err(_) => Err(Failure::new("internal")),
                };
                respond(&events, &shared, job.request_id, op, outcome).await;
            });
        }
        serial.abort();
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
            let store = shared.clone();
            tokio::task::spawn_blocking(move || {
                store
                    .bridges
                    .lock()
                    .map_err(|_| Failure::new("internal"))?
                    .approve(&profile)
            })
            .await
            .map_err(|_| Failure::new("internal"))??;
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
        _ => Err(Failure::new("invalid_request")),
    }
}
