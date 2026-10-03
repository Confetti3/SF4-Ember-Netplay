//! Room bookkeeping and the task that drives each room host child.
//!
//! Every room has one tokio task that owns its child process. The task is
//! the only place the child is waited on, so every child is reaped however it
//! ends: it exits, closes on request, goes idle, breaks the protocol or
//! overstays its welcome. Shared state holds what the API needs to answer.
use std::{
    collections::HashSet,
    process::Stdio,
    sync::{Arc, Mutex, MutexGuard},
};

use serde::Serialize;
use tokio::{
    io::AsyncWriteExt,
    process::{Child, ChildStdin, Command},
    sync::{Notify, oneshot, watch},
    time::{Instant, sleep_until, timeout},
};

use crate::{
    config::{Settings, Tuning},
    protocol::{
        ChildConfig, ChildMessage, CreateRoom, LineReader, MAX_ROOM_BANS, ReadError, parse_line,
    },
};

/// What the host reported when it came up.
#[derive(Clone, Debug, Serialize)]
pub struct Hosted {
    pub invitation: String,
    pub region: String,
}

/// One entry of `GET /rooms`.
#[derive(Clone, Debug, Serialize)]
pub struct RoomInfo {
    pub room_id: String,
    pub members: u32,
    pub capacity: u8,
    pub tables_playing: u32,
    pub invitation: String,
    pub banned: Vec<String>,
    /// Whether the room has ever reported a member. It is latched here, so a
    /// member who came and went between two polls still counts.
    pub opened: bool,
}

#[derive(Debug, PartialEq, Eq)]
pub enum CreateError {
    /// The named request field is malformed.
    Invalid(&'static str),
    /// At the room limit, out of ports, or draining.
    RoomLimit,
    UnsupportedBuild,
    Exists,
    /// The host exited, broke the protocol or never reported `hosted`.
    HostFailed,
}

struct Live {
    members: u32,
    tables_playing: u32,
    invitation: String,
    banned: Vec<String>,
    /// Set by the first status with a member and kept for the room's life.
    opened: bool,
}

struct Room {
    seq: u64,
    /// The primary endpoint port and the coordination port.
    ports: [u16; 2],
    capacity: u8,
    close: Arc<Notify>,
    /// Set once the room is being shut down; it no longer appears in lists.
    closing: bool,
    /// Present once the host has reported `hosted`.
    live: Option<Live>,
}

#[derive(Default)]
struct Inner {
    /// Keyed by the lowercase room id.
    rooms: std::collections::HashMap<String, Room>,
    draining: bool,
    next_seq: u64,
}

struct Shared {
    settings: Settings,
    tuning: Tuning,
    inner: Mutex<Inner>,
    /// The number of rooms, including ones still starting or closing.
    count: watch::Sender<usize>,
}

#[derive(Clone)]
pub struct Supervisor {
    shared: Arc<Shared>,
}

/// What `run_room` needs to start a child.
struct Spec {
    room_host: String,
    build_id: String,
    ports: [u16; 2],
    /// The configuration line, newline included.
    config_line: String,
}

impl Supervisor {
    pub fn new(settings: Settings, tuning: Tuning) -> Self {
        Self {
            shared: Arc::new(Shared {
                settings,
                tuning,
                inner: Mutex::default(),
                count: watch::channel(0).0,
            }),
        }
    }

    pub fn settings(&self) -> &Settings {
        &self.shared.settings
    }

    fn lock(&self) -> MutexGuard<'_, Inner> {
        self.shared
            .inner
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    fn publish_count(&self, inner: &Inner) {
        self.shared.count.send_replace(inner.rooms.len());
    }

    /// Rooms that hold a port: starting, live or closing.
    pub fn room_count(&self) -> usize {
        self.lock().rooms.len()
    }

    pub fn is_draining(&self) -> bool {
        self.lock().draining
    }

    /// The two lowest ports in the range that no room holds and nothing
    /// else on this machine has bound. They need not be adjacent.
    fn free_ports(&self, inner: &Inner) -> Option<[u16; 2]> {
        let [low, high] = self.shared.settings.config.port_range;
        let used: HashSet<u16> = inner.rooms.values().flat_map(|room| room.ports).collect();
        let mut free = (low..=high)
            .filter(|port| !used.contains(port))
            .filter(|port| !self.shared.tuning.probe_ports || port_is_free(*port));
        Some([free.next()?, free.next()?])
    }

    /// Starts a room host and waits for it to report `hosted`.
    pub async fn create(&self, request: CreateRoom) -> Result<Hosted, CreateError> {
        request.validate().map_err(CreateError::Invalid)?;
        let key = request.room_id.to_ascii_lowercase();
        let close = Arc::new(Notify::new());
        let (hosted_tx, hosted_rx) = oneshot::channel();
        let spec = {
            let mut inner = self.lock();
            if inner.draining {
                return Err(CreateError::RoomLimit);
            }
            let build = self
                .shared
                .settings
                .config
                .builds
                .get(&request.build_id)
                .ok_or(CreateError::UnsupportedBuild)?;
            if inner.rooms.contains_key(&key) {
                return Err(CreateError::Exists);
            }
            if inner.rooms.len() >= self.shared.settings.config.max_rooms {
                return Err(CreateError::RoomLimit);
            }
            let ports = self.free_ports(&inner).ok_or(CreateError::RoomLimit)?;
            let line = serde_json::to_string(&ChildConfig {
                room: &request,
                helper: &build.helper,
                port: ports[0],
                coordination_port: ports[1],
            })
            .map_err(|_| CreateError::HostFailed)?;
            inner.next_seq += 1;
            let seq = inner.next_seq;
            inner.rooms.insert(
                key.clone(),
                Room {
                    seq,
                    ports,
                    capacity: request.capacity,
                    close: close.clone(),
                    closing: false,
                    live: None,
                },
            );
            self.publish_count(&inner);
            Spec {
                room_host: build.room_host.clone(),
                build_id: request.build_id.clone(),
                ports,
                config_line: line + "\n",
            }
        };
        tokio::spawn(run_room(self.clone(), key, spec, close, Some(hosted_tx)));
        // The task drops the sender after it has reaped the child and freed
        // the port, so an error here means the host failed.
        hosted_rx.await.map_err(|_| CreateError::HostFailed)
    }

    /// Live rooms, oldest first.
    pub fn list(&self) -> Vec<RoomInfo> {
        let inner = self.lock();
        let mut rooms: Vec<(u64, RoomInfo)> = inner
            .rooms
            .iter()
            .filter(|(_, room)| !room.closing)
            .filter_map(|(key, room)| {
                let live = room.live.as_ref()?;
                Some((
                    room.seq,
                    RoomInfo {
                        room_id: key.clone(),
                        members: live.members,
                        capacity: room.capacity,
                        tables_playing: live.tables_playing,
                        invitation: live.invitation.clone(),
                        banned: live.banned.clone(),
                        opened: live.opened,
                    },
                ))
            })
            .collect();
        rooms.sort_by_key(|(seq, _)| *seq);
        rooms.into_iter().map(|(_, info)| info).collect()
    }

    /// Asks a room's host to close. The room stays counted until the child
    /// has exited. False when there is no such room.
    pub fn close_room(&self, room_id: &str) -> bool {
        let mut inner = self.lock();
        match inner.rooms.get_mut(&room_id.to_ascii_lowercase()) {
            Some(room) => {
                // Out of the list now, not when the task gets to it.
                room.closing = true;
                room.close.notify_one();
                true
            }
            None => false,
        }
    }

    /// Refuses new rooms from now on.
    pub fn begin_drain(&self) {
        self.lock().draining = true;
    }

    /// Refuses new rooms, waits up to the drain time for the rest to end,
    /// then closes whatever is left and waits for those children too.
    pub async fn drain(&self) {
        self.begin_drain();
        let mut count = self.shared.count.subscribe();
        // `wait_for` hands back a guard that blocks senders; drop it at once.
        let timed_out = timeout(
            self.shared.tuning.drain,
            count.wait_for(|rooms| *rooms == 0),
        )
        .await
        .is_err();
        if timed_out {
            let inner = self.lock();
            for room in inner.rooms.values() {
                room.close.notify_one();
            }
            drop(inner);
        }
        let _ = count.wait_for(|rooms| *rooms == 0).await;
    }

    fn mark_closing(&self, key: &str) {
        if let Some(room) = self.lock().rooms.get_mut(key) {
            room.closing = true;
        }
    }

    fn set_live(&self, key: &str, mut live: Live) {
        if let Some(room) = self.lock().rooms.get_mut(key) {
            live.opened |= room.live.as_ref().is_some_and(|previous| previous.opened);
            room.live = Some(live);
        }
    }

    fn remove(&self, key: &str) {
        let mut inner = self.lock();
        inner.rooms.remove(key);
        self.publish_count(&inner);
    }
}

/// Whether nothing on this machine holds the UDP port. The room host binds
/// it a moment later, so this only skips ports another service owns.
fn port_is_free(port: u16) -> bool {
    std::net::UdpSocket::bind(("0.0.0.0", port)).is_ok()
}

/// Log text from a child is untrusted: quote it and cut it short.
fn quoted(text: &str) -> String {
    let short: String = text.chars().take(80).collect();
    format!("{short:?}")
}

async fn run_room(
    supervisor: Supervisor,
    key: String,
    spec: Spec,
    close: Arc<Notify>,
    mut hosted_tx: Option<oneshot::Sender<Hosted>>,
) {
    let (exit, reason) = drive(&supervisor, &key, &spec, &close, &mut hosted_tx).await;
    supervisor.remove(&key);
    eprintln!(
        "ember-rooms: room stopped room_id={key} ports={},{} build={} exit={exit} reason={reason}",
        spec.ports[0], spec.ports[1], spec.build_id
    );
    // Only now does a pending `POST /rooms` hear that the host failed.
    drop(hosted_tx);
}

/// Why and how a room is being shut down, plus its pending deadlines.
struct Shutdown {
    stdin: Option<ChildStdin>,
    started: bool,
    /// When a child that was asked to close is killed instead.
    kill_at: Option<Instant>,
    reason: String,
}

impl Shutdown {
    fn begin(&mut self, why: String) {
        if self.reason.is_empty() {
            self.reason = why;
        }
    }

    /// Closes the child's stdin, which asks it to close the room and exit.
    fn close(&mut self, supervisor: &Supervisor, key: &str, why: String) {
        if self.started {
            return;
        }
        self.started = true;
        self.begin(why);
        self.stdin = None;
        self.kill_at = Some(Instant::now() + supervisor.shared.tuning.kill_grace);
        supervisor.mark_closing(key);
    }

    fn kill(&mut self, supervisor: &Supervisor, key: &str, child: &mut Child, why: String) {
        self.started = true;
        self.begin(why);
        self.stdin = None;
        self.kill_at = None;
        let _ = child.start_kill();
        supervisor.mark_closing(key);
    }
}

/// Runs one child to its end. Returns its exit status and why it ended.
async fn drive(
    supervisor: &Supervisor,
    key: &str,
    spec: &Spec,
    close: &Notify,
    hosted_tx: &mut Option<oneshot::Sender<Hosted>>,
) -> (String, String) {
    let tuning = &supervisor.shared.tuning;
    let mut command = Command::new(&spec.room_host);
    command
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::inherit())
        .kill_on_drop(true);
    let mut child = match command.spawn() {
        Ok(child) => child,
        Err(error) => return ("not started".to_owned(), format!("spawn failed: {error}")),
    };
    eprintln!(
        "ember-rooms: room started room_id={key} ports={},{} build={}",
        spec.ports[0], spec.ports[1], spec.build_id
    );
    let mut shutdown = Shutdown {
        stdin: child.stdin.take(),
        started: false,
        kill_at: None,
        reason: String::new(),
    };
    let mut reader = child.stdout.take().map(LineReader::new);
    if let Some(stdin) = shutdown.stdin.as_mut() {
        let written = async {
            stdin.write_all(spec.config_line.as_bytes()).await?;
            stdin.flush().await
        };
        if let Err(error) = written.await {
            shutdown.kill(
                supervisor,
                key,
                &mut child,
                format!("cannot write the configuration: {error}"),
            );
        }
    }
    if reader.is_none() {
        shutdown.kill(supervisor, key, &mut child, "no stdout".to_owned());
    }
    let hosted_deadline = Instant::now() + tuning.hosted_timeout;
    let mut hosted = false;
    let mut empty_since: Option<Instant> = None;
    let status = loop {
        let empty_deadline =
            empty_since.map_or(hosted_deadline, |since| since + tuning.empty_close);
        let kill_deadline = shutdown.kill_at.unwrap_or(hosted_deadline);
        let reading = reader.is_some();
        tokio::select! {
            status = child.wait() => break status,
            line = async {
                match reader.as_mut() {
                    Some(reader) => reader.next().await,
                    None => std::future::pending().await,
                }
            }, if reading => {
                let message = match line {
                    Ok(Some(line)) => parse_line(&line).map_err(|error| format!("malformed line: {}", quoted(&error))),
                    Ok(None) => {
                        reader = None;
                        shutdown.close(supervisor, key, "host closed its output".to_owned());
                        continue;
                    }
                    Err(ReadError::TooLong) => Err("status line too long".to_owned()),
                    Err(ReadError::Io(error)) => Err(format!("read failed: {error}")),
                };
                let failure = match message {
                    Ok(message) => apply(
                        supervisor,
                        key,
                        message,
                        &mut shutdown,
                        &mut hosted,
                        &mut empty_since,
                        hosted_tx,
                    ),
                    Err(error) => Some(error),
                };
                if let Some(error) = failure {
                    reader = None;
                    shutdown.kill(supervisor, key, &mut child, format!("protocol error: {error}"));
                }
            }
            () = close.notified(), if !shutdown.started => {
                shutdown.close(supervisor, key, "close requested".to_owned());
            }
            () = sleep_until(hosted_deadline), if !hosted && !shutdown.started => {
                shutdown.kill(supervisor, key, &mut child, "host did not report hosted in time".to_owned());
            }
            () = sleep_until(empty_deadline), if empty_since.is_some() && !shutdown.started => {
                shutdown.close(supervisor, key, "empty".to_owned());
            }
            () = sleep_until(kill_deadline), if shutdown.kill_at.is_some() => {
                shutdown.kill(supervisor, key, &mut child, "host ignored the close request".to_owned());
            }
        }
    };
    let exit = match status {
        Ok(status) => status.to_string(),
        Err(error) => format!("unknown ({error})"),
    };
    let reason = if shutdown.reason.is_empty() {
        "host exited".to_owned()
    } else {
        shutdown.reason
    };
    (exit, reason)
}

/// Applies one message. Returns a description of the protocol error, if any.
fn apply(
    supervisor: &Supervisor,
    key: &str,
    message: ChildMessage,
    shutdown: &mut Shutdown,
    hosted: &mut bool,
    empty_since: &mut Option<Instant>,
    hosted_tx: &mut Option<oneshot::Sender<Hosted>>,
) -> Option<String> {
    match message {
        ChildMessage::Unknown => {}
        ChildMessage::Hosted { invitation, region } => {
            if invitation.is_empty() {
                return Some("empty invitation".to_owned());
            }
            // A second `hosted` is ignored; so is one that arrives after the
            // room has started closing.
            if !*hosted && !shutdown.started {
                *hosted = true;
                // The grace period for a room nobody has entered runs from
                // here.
                *empty_since = Some(Instant::now());
                supervisor.set_live(
                    key,
                    Live {
                        members: 0,
                        tables_playing: 0,
                        invitation: invitation.clone(),
                        banned: Vec::new(),
                        opened: false,
                    },
                );
                if let Some(sender) = hosted_tx.take() {
                    let _ = sender.send(Hosted { invitation, region });
                }
            }
        }
        ChildMessage::Status {
            members,
            tables_playing,
            invitation,
            banned,
        } => {
            if !*hosted {
                return Some("status before hosted".to_owned());
            }
            if invitation.is_empty() {
                return Some("empty invitation".to_owned());
            }
            if banned.len() > MAX_ROOM_BANS {
                return Some("too many banned accounts".to_owned());
            }
            if !shutdown.started {
                if members == 0 {
                    empty_since.get_or_insert_with(Instant::now);
                } else {
                    *empty_since = None;
                }
                supervisor.set_live(
                    key,
                    Live {
                        members,
                        tables_playing,
                        invitation,
                        banned,
                        opened: members >= 1,
                    },
                );
            }
        }
        ChildMessage::Closed { reason } => {
            shutdown.close(supervisor, key, format!("host closed: {}", quoted(&reason)));
        }
    }
    None
}
