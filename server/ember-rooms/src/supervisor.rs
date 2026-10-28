//! Room bookkeeping and the task that drives each room host child.
//!
//! Every room has one tokio task that owns its child process. The task is
//! the only place the child is waited on, so every child is reaped however it
//! ends: it exits, closes on request, goes idle, breaks the protocol or
//! overstays its welcome. Shared state holds what the API needs to answer.
use std::{
    collections::{BTreeMap, HashSet},
    process::Stdio,
    sync::{Arc, Mutex, MutexGuard},
};

use serde::Serialize;
use serde_json::value::RawValue;
use tokio::{
    io::AsyncWriteExt,
    process::{Child, ChildStdin, Command},
    sync::{Notify, oneshot, watch},
    time::{Instant, sleep_until, timeout},
};

use crate::{
    config::{Build, Config, Settings, Tuning},
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
    /// What the room host last said about the room for its listing: a JSON
    /// object passed on as it came (see `protocol`), not read here. Left out
    /// when the host has not sent one or sent one that does not qualify.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub details: Option<Box<RawValue>>,
    /// The won games the host's latest status reported, for the skill
    /// rating, as it sent them. Replaced by every status; left out when there
    /// are none.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub games: Option<Box<RawValue>>,
    /// Changes whenever any other reported field of this room changes, and
    /// only then: a caller that saw this value for the room has seen the
    /// room as it is. Unique within this process and seeded from the clock at
    /// start, so a restarted supervisor never repeats an earlier value. The
    /// number is only ever compared for equality.
    pub rev: u64,
}

/// Raw members compare as the host wrote them.
fn same_raw(a: &Option<Box<RawValue>>, b: &Option<Box<RawValue>>) -> bool {
    a.as_deref().map(RawValue::get) == b.as_deref().map(RawValue::get)
}

/// Field by field, to tell whether a status said anything new.
impl PartialEq for RoomInfo {
    fn eq(&self, other: &Self) -> bool {
        self.room_id == other.room_id
            && self.members == other.members
            && self.capacity == other.capacity
            && self.tables_playing == other.tables_playing
            && self.invitation == other.invitation
            && self.banned == other.banned
            && self.opened == other.opened
            && same_raw(&self.details, &other.details)
            && same_raw(&self.games, &other.games)
            && self.rev == other.rev
    }
}

/// `GET /limits`: what the supervisor runs with now, so an operator can see
/// that a reload took.
#[derive(Clone, Debug, Serialize)]
pub struct LimitsView {
    pub max_rooms: usize,
    pub port_range: [u16; 2],
    /// Build ids, sorted.
    pub builds: Vec<String>,
    /// Rooms that hold a port: starting, live or closing.
    pub rooms: usize,
    /// Reloads applied since the supervisor started.
    pub reloads: u64,
    /// The settings only a restart changes, as this process read them at
    /// start, so a deployment can tell whether a reload is enough.
    pub bind: String,
    pub secret_file: String,
    pub empty_close_secs: u64,
    pub drain_secs: u64,
}

/// What a reload changed, and what it found changed that only a restart
/// applies. Both are lines for the log.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct Reloaded {
    pub applied: Vec<String>,
    pub needs_restart: Vec<String>,
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

struct Room {
    seq: u64,
    /// The primary endpoint port and the coordination port.
    ports: [u16; 2],
    close: Arc<Notify>,
    /// Set once the room is being shut down; it no longer appears in lists.
    closing: bool,
    /// The room as `GET /rooms` lists it, present once the host has reported
    /// `hosted`. Its `rev` is drawn from `Inner::next_rev` each time it changes.
    live: Option<RoomInfo>,
}

/// The part of the configuration a reload replaces while rooms run. A room
/// keeps the ports and binaries it started with, so changing these never
/// touches a running room.
struct Limits {
    max_rooms: usize,
    port_range: [u16; 2],
    builds: BTreeMap<String, Build>,
}

impl Limits {
    fn of(config: &Config) -> Self {
        Self {
            max_rooms: config.max_rooms,
            port_range: config.port_range,
            builds: config.builds.clone(),
        }
    }
}

struct Inner {
    /// Keyed by the lowercase room id.
    rooms: std::collections::HashMap<String, Room>,
    draining: bool,
    next_seq: u64,
    /// The last `RoomInfo::rev` handed out. It starts at the clock, in
    /// microseconds, which stays below 2^53 for centuries, so the numbers
    /// survive a JSON reader that holds them as doubles.
    next_rev: u64,
    limits: Limits,
    reloads: u64,
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
    capacity: u8,
    /// The configuration line, newline included.
    config_line: String,
}

impl Supervisor {
    pub fn new(settings: Settings, tuning: Tuning) -> Self {
        let inner = Inner {
            rooms: Default::default(),
            draining: false,
            next_seq: 0,
            next_rev: rev_seed(),
            limits: Limits::of(&settings.config),
            reloads: 0,
        };
        Self {
            shared: Arc::new(Shared {
                settings,
                tuning,
                inner: Mutex::new(inner),
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
        let [low, high] = inner.limits.port_range;
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
            let build = inner
                .limits
                .builds
                .get(&request.build_id)
                .cloned()
                .ok_or(CreateError::UnsupportedBuild)?;
            if inner.rooms.contains_key(&key) {
                return Err(CreateError::Exists);
            }
            if inner.rooms.len() >= inner.limits.max_rooms {
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
                capacity: request.capacity,
                config_line: line + "\n",
            }
        };
        tokio::spawn(run_room(self.clone(), key, spec, close, Some(hosted_tx)));
        // The task drops the sender after it has reaped the child and freed
        // the port, so an error here means the host failed.
        hosted_rx.await.map_err(|_| CreateError::HostFailed)
    }

    /// The limits in force now.
    pub fn limits(&self) -> LimitsView {
        let inner = self.lock();
        let started = &self.shared.settings.config;
        LimitsView {
            max_rooms: inner.limits.max_rooms,
            port_range: inner.limits.port_range,
            builds: inner.limits.builds.keys().cloned().collect(),
            rooms: inner.rooms.len(),
            reloads: inner.reloads,
            bind: started.bind.clone(),
            secret_file: started.secret_file.clone(),
            empty_close_secs: started.empty_close_secs,
            drain_secs: started.drain_secs,
        }
    }

    /// Takes `max_rooms`, `port_range` and `builds` from a freshly read
    /// configuration while rooms keep running. A room that already runs keeps
    /// its ports and binaries, and a lower `max_rooms` closes no room: it only
    /// refuses new ones until enough have ended. The settings only a restart
    /// applies (the address, the secret file and the timings) are reported,
    /// not taken. A configuration that does not validate changes nothing.
    pub fn reload(&self, config: &Config) -> Result<Reloaded, String> {
        config.validate()?;
        let running = &self.shared.settings.config;
        let mut reloaded = Reloaded::default();
        for (name, differs) in [
            ("bind", config.bind != running.bind),
            ("secret_file", config.secret_file != running.secret_file),
            (
                "empty_close_secs",
                config.empty_close_secs != running.empty_close_secs,
            ),
            ("drain_secs", config.drain_secs != running.drain_secs),
        ] {
            if differs {
                reloaded
                    .needs_restart
                    .push(format!("{name} changed; it takes effect after a restart"));
            }
        }
        let mut inner = self.lock();
        let old = &inner.limits;
        if config.max_rooms != old.max_rooms {
            let mut line = format!("max_rooms {} -> {}", old.max_rooms, config.max_rooms);
            if inner.rooms.len() > config.max_rooms {
                line += &format!(
                    " ({} rooms run; none is closed, new ones wait until fewer run)",
                    inner.rooms.len()
                );
            }
            reloaded.applied.push(line);
        }
        if config.port_range != old.port_range {
            reloaded.applied.push(format!(
                "port_range {}-{} -> {}-{} (running rooms keep their ports)",
                old.port_range[0], old.port_range[1], config.port_range[0], config.port_range[1]
            ));
        }
        for (id, build) in &config.builds {
            match old.builds.get(id) {
                None => reloaded.applied.push(format!("build {id} added")),
                Some(previous) if previous != build => {
                    reloaded.applied.push(format!("build {id} updated"))
                }
                Some(_) => {}
            }
        }
        for id in old
            .builds
            .keys()
            .filter(|id| !config.builds.contains_key(*id))
        {
            reloaded
                .applied
                .push(format!("build {id} removed (its running rooms continue)"));
        }
        inner.limits = Limits::of(config);
        inner.reloads += 1;
        Ok(reloaded)
    }

    /// Live rooms, oldest first.
    pub fn list(&self) -> Vec<RoomInfo> {
        let inner = self.lock();
        let mut rooms: Vec<(u64, RoomInfo)> = inner
            .rooms
            .values()
            .filter(|room| !room.closing)
            .filter_map(|room| Some((room.seq, room.live.clone()?)))
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

    /// Records what the host reported. `opened` is latched here, and the
    /// room's `rev` moves only when some reported field differs from the last
    /// report, so a host that repeats itself leaves it where it was.
    fn set_live(&self, key: &str, mut live: RoomInfo) {
        let mut guard = self.lock();
        let inner = &mut *guard;
        if let Some(room) = inner.rooms.get_mut(key) {
            if let Some(previous) = &room.live {
                live.opened |= previous.opened;
                live.rev = previous.rev;
            }
            if room.live.as_ref() != Some(&live) {
                inner.next_rev += 1;
                live.rev = inner.next_rev;
                room.live = Some(live);
            }
        }
    }

    fn remove(&self, key: &str) {
        let mut inner = self.lock();
        inner.rooms.remove(key);
        self.publish_count(&inner);
    }
}

/// Where this process's `RoomInfo::rev` numbers start: the clock in
/// microseconds, so they differ from any earlier process's.
fn rev_seed() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |since| u64::try_from(since.as_micros()).unwrap_or(0))
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
    hosted_tx: Option<oneshot::Sender<Hosted>>,
) {
    let mut task = RoomTask {
        supervisor: &supervisor,
        key: &key,
        capacity: spec.capacity,
        shutdown: Shutdown::default(),
        hosted: false,
        hosted_tx,
        empty_since: None,
    };
    let (exit, reason) = task.drive(&spec, &close).await;
    supervisor.remove(&key);
    eprintln!(
        "ember-rooms: room stopped room_id={key} ports={},{} build={} exit={exit} reason={reason}",
        spec.ports[0], spec.ports[1], spec.build_id
    );
    // Only now does a pending `POST /rooms` hear that the host failed.
    drop(task);
}

/// Why and how a room is being shut down, plus its pending deadlines.
#[derive(Default)]
struct Shutdown {
    stdin: Option<ChildStdin>,
    /// Set by the first close or kill. Not the same as `stdin` being gone:
    /// a child whose stdin was never taken has not been asked to close.
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
}

/// One room's task: the child's shutdown and what the host has said so far.
/// The child and its output reader stay with `drive`, which waits on them.
struct RoomTask<'a> {
    supervisor: &'a Supervisor,
    key: &'a str,
    capacity: u8,
    shutdown: Shutdown,
    /// The host has reported `hosted`.
    hosted: bool,
    /// Answers the `POST /rooms` that started the room; `run_room` drops it
    /// after the child is reaped when the host never reported `hosted`.
    hosted_tx: Option<oneshot::Sender<Hosted>>,
    /// Since when the room has had nobody in it, from `hosted` on.
    empty_since: Option<Instant>,
}

impl RoomTask<'_> {
    /// Closes the child's stdin, which asks it to close the room and exit.
    fn close(&mut self, why: String) {
        if self.shutdown.started {
            return;
        }
        self.shutdown.started = true;
        self.shutdown.begin(why);
        self.shutdown.stdin = None;
        self.shutdown.kill_at = Some(Instant::now() + self.supervisor.shared.tuning.kill_grace);
        self.supervisor.mark_closing(self.key);
    }

    fn kill(&mut self, child: &mut Child, why: String) {
        self.shutdown.started = true;
        self.shutdown.begin(why);
        self.shutdown.stdin = None;
        self.shutdown.kill_at = None;
        let _ = child.start_kill();
        self.supervisor.mark_closing(self.key);
    }

    /// Runs one child to its end. Returns its exit status and why it ended.
    async fn drive(&mut self, spec: &Spec, close: &Notify) -> (String, String) {
        let supervisor = self.supervisor;
        let tuning = &supervisor.shared.tuning;
        let key = self.key;
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
        self.shutdown.stdin = child.stdin.take();
        let mut reader = child.stdout.take().map(LineReader::new);
        if let Some(stdin) = self.shutdown.stdin.as_mut() {
            let written = async {
                stdin.write_all(spec.config_line.as_bytes()).await?;
                stdin.flush().await
            };
            if let Err(error) = written.await {
                self.kill(
                    &mut child,
                    format!("cannot write the configuration: {error}"),
                );
            }
        }
        if reader.is_none() {
            self.kill(&mut child, "no stdout".to_owned());
        }
        let hosted_deadline = Instant::now() + tuning.hosted_timeout;
        let status = loop {
            let empty_deadline = self
                .empty_since
                .map_or(hosted_deadline, |since| since + tuning.empty_close);
            let kill_deadline = self.shutdown.kill_at.unwrap_or(hosted_deadline);
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
                            self.close("host closed its output".to_owned());
                            continue;
                        }
                        Err(ReadError::TooLong) => Err("status line too long".to_owned()),
                        Err(ReadError::Io(error)) => Err(format!("read failed: {error}")),
                    };
                    let failure = match message {
                        Ok(message) => self.apply(message),
                        Err(error) => Some(error),
                    };
                    if let Some(error) = failure {
                        reader = None;
                        self.kill(&mut child, format!("protocol error: {error}"));
                    }
                }
                () = close.notified(), if !self.shutdown.started => {
                    self.close("close requested".to_owned());
                }
                () = sleep_until(hosted_deadline), if !self.hosted && !self.shutdown.started => {
                    self.kill(&mut child, "host did not report hosted in time".to_owned());
                }
                () = sleep_until(empty_deadline), if self.empty_since.is_some() && !self.shutdown.started => {
                    self.close("empty".to_owned());
                }
                () = sleep_until(kill_deadline), if self.shutdown.kill_at.is_some() => {
                    self.kill(&mut child, "host ignored the close request".to_owned());
                }
            }
        };
        let exit = match status {
            Ok(status) => status.to_string(),
            Err(error) => format!("unknown ({error})"),
        };
        let reason = if self.shutdown.reason.is_empty() {
            "host exited".to_owned()
        } else {
            std::mem::take(&mut self.shutdown.reason)
        };
        (exit, reason)
    }

    /// The room as `GET /rooms` lists it after this report; `set_live` latches
    /// `opened` and assigns the `rev`.
    fn listing(
        &self,
        members: u32,
        tables_playing: u32,
        invitation: String,
        banned: Vec<String>,
        details: Option<Box<RawValue>>,
        games: Option<Box<RawValue>>,
    ) -> RoomInfo {
        RoomInfo {
            room_id: self.key.to_owned(),
            members,
            capacity: self.capacity,
            tables_playing,
            invitation,
            banned,
            opened: members >= 1,
            details,
            games,
            rev: 0,
        }
    }

    /// Applies one message. Returns a description of the protocol error, if any.
    fn apply(&mut self, message: ChildMessage) -> Option<String> {
        match message {
            ChildMessage::Unknown => {}
            ChildMessage::Hosted { invitation, region } => {
                if invitation.is_empty() {
                    return Some("empty invitation".to_owned());
                }
                // A second `hosted` is ignored; so is one that arrives after the
                // room has started closing.
                if !self.hosted && !self.shutdown.started {
                    self.hosted = true;
                    // The grace period for a room nobody has entered runs from
                    // here.
                    self.empty_since = Some(Instant::now());
                    let live = self.listing(0, 0, invitation.clone(), Vec::new(), None, None);
                    self.supervisor.set_live(self.key, live);
                    if let Some(sender) = self.hosted_tx.take() {
                        let _ = sender.send(Hosted { invitation, region });
                    }
                }
            }
            ChildMessage::Status {
                members,
                tables_playing,
                invitation,
                banned,
                details,
                games,
            } => {
                if !self.hosted {
                    return Some("status before hosted".to_owned());
                }
                if invitation.is_empty() {
                    return Some("empty invitation".to_owned());
                }
                if banned.len() > MAX_ROOM_BANS {
                    return Some("too many banned accounts".to_owned());
                }
                if !self.shutdown.started {
                    if members == 0 {
                        self.empty_since.get_or_insert_with(Instant::now);
                    } else {
                        self.empty_since = None;
                    }
                    // `details`: not an object or too big (`parse_line` left
                    // it out), this status says nothing about the room's
                    // details and the room stays up. `games`: only this
                    // status's; the host repeats its recent ones, and the
                    // bridge counts each once.
                    let live =
                        self.listing(members, tables_playing, invitation, banned, details, games);
                    self.supervisor.set_live(self.key, live);
                }
            }
            ChildMessage::Closed { reason } => {
                self.close(format!("host closed: {}", quoted(&reason)));
            }
        }
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// `GET /rooms` writes each room's members in this order, with `details`
    /// and `games` as the host sent them and left out when absent.
    #[test]
    fn a_rooms_listing_keeps_its_member_order() {
        let raw = |text: &str| Some(RawValue::from_string(text.to_owned()).unwrap());
        let mut room = RoomInfo {
            room_id: "0123456789abcdef0123456789abcdef".to_owned(),
            members: 2,
            capacity: 8,
            tables_playing: 1,
            invitation: "sf4e3:x".to_owned(),
            banned: vec!["emb1_a".to_owned()],
            opened: true,
            details: raw(r#"{"name":"Room","fighters":[3,255]}"#),
            games: raw(r#"[{"table":0,"generation":7,"winner":"a","loser":"b"}]"#),
            rev: 1_700_000_000_000_001,
        };
        assert_eq!(
            serde_json::to_string(&room).unwrap(),
            r#"{"room_id":"0123456789abcdef0123456789abcdef","members":2,"capacity":8,"tables_playing":1,"invitation":"sf4e3:x","banned":["emb1_a"],"opened":true,"details":{"name":"Room","fighters":[3,255]},"games":[{"table":0,"generation":7,"winner":"a","loser":"b"}],"rev":1700000000000001}"#
        );
        room.details = None;
        room.games = None;
        assert_eq!(
            serde_json::to_string(&room).unwrap(),
            r#"{"room_id":"0123456789abcdef0123456789abcdef","members":2,"capacity":8,"tables_playing":1,"invitation":"sf4e3:x","banned":["emb1_a"],"opened":true,"rev":1700000000000001}"#
        );
    }
}
