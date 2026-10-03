//! The helper's public-room requests against a real bridge on loopback, with
//! a stand-in room supervisor: listing, creating a room and taking its
//! creator's ticket, joining with a ticket, and the refusals the game acts on.
#![cfg(windows)]

use std::{
    path::PathBuf,
    sync::{Arc, Mutex},
    time::Duration,
};

use ember_bridge::{AppState, Clock, Config, Db, Keys, config, integrations::Secrets};
use ember_protocol::{
    api::SigningKeys,
    rooms::{RoomAdmission, RoomList},
};
use serde_json::{Value, json};
use sf4_net::{
    service::{Command, Event},
    tournament,
};
use tokio::{
    io::{AsyncReadExt, AsyncWriteExt},
    net::{TcpListener, TcpStream},
    sync::mpsc,
};
use zeroize::Zeroizing;

const SECRET: &str = "test-secret";
const BUILD: &str = "build-1";
const MISSING_ROOM: &str = "0123456789abcdef0123456789abcdef";

struct Dirs(PathBuf);

impl Drop for Dirs {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn dirs(label: &str) -> Dirs {
    let mut random = [0u8; 16];
    getrandom::fill(&mut random).unwrap();
    let dir = std::env::temp_dir().join(format!(
        "sf4-public-rooms-{label}-{}",
        ember_protocol::encoding::b64u(&random)
    ));
    std::fs::create_dir_all(&dir).unwrap();
    Dirs(dir)
}

/// What the stand-in supervisor holds.
#[derive(Default)]
struct Fake {
    /// `POST /rooms` bodies, as received.
    created: Vec<Value>,
    /// What `GET /rooms` lists.
    rooms: Vec<Value>,
    /// How long `POST /rooms` takes to answer.
    create_delay: Duration,
}

type Supervisor = Arc<Mutex<Fake>>;

/// Reads one HTTP/1.1 request: its first line and its body.
async fn read_request(stream: &mut TcpStream) -> Option<(String, String, Vec<u8>)> {
    let mut data = Vec::new();
    let mut chunk = [0u8; 4096];
    let head_end = loop {
        if let Some(at) = data.windows(4).position(|window| window == b"\r\n\r\n") {
            break at + 4;
        }
        let read = stream.read(&mut chunk).await.ok()?;
        if read == 0 {
            return None;
        }
        data.extend_from_slice(&chunk[..read]);
    };
    let head = String::from_utf8_lossy(&data[..head_end]).into_owned();
    let length = head
        .lines()
        .filter_map(|line| line.split_once(':'))
        .find(|(name, _)| name.eq_ignore_ascii_case("content-length"))
        .and_then(|(_, value)| value.trim().parse::<usize>().ok())
        .unwrap_or(0);
    while data.len() < head_end + length {
        let read = stream.read(&mut chunk).await.ok()?;
        if read == 0 {
            return None;
        }
        data.extend_from_slice(&chunk[..read]);
    }
    let mut parts = head.lines().next()?.split(' ');
    let method = parts.next()?.to_owned();
    let path = parts.next()?.to_owned();
    let authorized = head
        .to_ascii_lowercase()
        .contains(&format!("authorization: bearer {SECRET}"));
    authorized.then(|| (method, path, data[head_end..head_end + length].to_vec()))
}

async fn answer(stream: &mut TcpStream, status: &str, body: &str) {
    let response = format!(
        "HTTP/1.1 {status}\r\ncontent-type: application/json\r\ncontent-length: {}\r\nconnection: close\r\n\r\n{body}",
        body.len()
    );
    let _ = stream.write_all(response.as_bytes()).await;
    let _ = stream.shutdown().await;
}

async fn handle(fake: Supervisor, mut stream: TcpStream) {
    let Some((method, path, body)) = read_request(&mut stream).await else {
        return answer(&mut stream, "401 Unauthorized", "{}").await;
    };
    match (method.as_str(), path.as_str()) {
        ("POST", "/rooms") => {
            let body: Value = serde_json::from_slice(&body).unwrap();
            let room_id = body["room_id"].as_str().unwrap().to_owned();
            let delay = fake.lock().unwrap().create_delay;
            tokio::time::sleep(delay).await;
            let reply = {
                let mut fake = fake.lock().unwrap();
                fake.created.push(body.clone());
                fake.rooms.push(json!({
                    "room_id": room_id, "members": 0, "capacity": body["capacity"],
                    "tables_playing": 0, "invitation": format!("sf4e3:{room_id}"), "banned": [],
                }));
                json!({ "invitation": format!("sf4e3:{room_id}"), "region": "use1" })
            };
            answer(&mut stream, "201 Created", &reply.to_string()).await;
        }
        ("GET", "/rooms") => {
            let rooms = Value::Array(fake.lock().unwrap().rooms.clone());
            answer(&mut stream, "200 OK", &rooms.to_string()).await;
        }
        ("DELETE", path) if path.starts_with("/rooms/") => {
            let id = path.trim_start_matches("/rooms/").to_owned();
            fake.lock()
                .unwrap()
                .rooms
                .retain(|room| room["room_id"] != id.as_str());
            answer(&mut stream, "204 No Content", "").await;
        }
        _ => answer(&mut stream, "404 Not Found", "{}").await,
    }
}

async fn fake_supervisor() -> (Supervisor, String) {
    let fake = Supervisor::default();
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let url = format!("http://{}", listener.local_addr().unwrap());
    let serving = fake.clone();
    tokio::spawn(async move {
        while let Ok((stream, _)) = listener.accept().await {
            tokio::spawn(handle(serving.clone(), stream));
        }
    });
    (fake, url)
}

/// What the stand-in reports for `room_id`.
fn report(fake: &Supervisor, room_id: &str, members: u32, banned: &[&str]) {
    let mut fake = fake.lock().unwrap();
    let room = fake
        .rooms
        .iter_mut()
        .find(|room| room["room_id"] == room_id)
        .expect("the supervisor hosts the room");
    room["members"] = json!(members);
    room["banned"] = json!(banned);
}

/// A bridge on loopback, with public rooms when `supervisor` names one.
async fn bridge(
    dir: &std::path::Path,
    supervisor: Option<String>,
) -> (ember_bridge::Running, String) {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let origin = format!("http://{}", listener.local_addr().unwrap());
    let mut random = [0u8; 16];
    getrandom::fill(&mut random).unwrap();
    let rooms = supervisor.is_some();
    let config = Config {
        bridge_id: ember_protocol::encoding::prefixed_id("brg", random),
        display_name: "Loopback bridge".into(),
        origin: origin.clone(),
        listen: "127.0.0.1:0".into(),
        database: dir.join("bridge.sqlite3"),
        secrets: dir.join("secrets.json"),
        allow_loopback_http: true,
        allow_private_webhooks: true,
        mock_browser: false,
        discord: None,
        integration_secrets: rooms.then(|| "unused-in-tests.json".into()),
        rooms: supervisor.map(|supervisor_url| config::Rooms { supervisor_url }),
        tenants: vec![config::Tenant {
            id: "local".into(),
            name: "Local".into(),
            connections: vec![config::Connection {
                id: "mock-local".into(),
                kind: "mock".into(),
                environment: "local".into(),
                display_name: "Mock provider".into(),
                enabled: true,
                api_base: None,
                discord_lookup: false,
                disputes: None,
                results_url: None,
                rooms: None,
            }],
        }],
    };
    let secrets = Secrets {
        rooms_supervisor_secret: rooms.then(|| Zeroizing::new(SECRET.into())),
        ..Default::default()
    };
    let db = Db::open(&config.database).unwrap();
    let state = AppState::new(config, Keys::generate(), secrets, db, Clock::default());
    ember_bridge::sync_config(&state).await.unwrap();
    (ember_bridge::start(state, listener).unwrap(), origin)
}

struct Worker {
    handle: tournament::Handle,
    events: mpsc::Receiver<Event>,
    task: tokio::task::JoinHandle<()>,
    next: u64,
}

/// `ok`, the reason, the identity status and the data of one answer.
type Answer = (bool, Option<String>, Value, Value);

impl Worker {
    fn start(dirs: &Dirs, endpoint: &str) -> Self {
        let (sender, events) = mpsc::channel(64);
        let (handle, task) = tournament::spawn_in(
            sender,
            dirs.0.join("identity"),
            dirs.0.join("tournament"),
            false,
            endpoint.to_owned(),
        );
        Self {
            handle,
            events,
            task,
            next: 100,
        }
    }

    /// Sends `request` the way the native side does, as IPC JSON.
    async fn ask(&mut self, request: Value) -> Answer {
        self.next += 1;
        let id = self.next;
        let command: Command =
            serde_json::from_value(json!({ "type": "tournament", "request": request })).unwrap();
        let Command::Tournament { request } = command else {
            unreachable!()
        };
        self.handle.submit(id, request);
        loop {
            let event = tokio::time::timeout(Duration::from_secs(30), self.events.recv())
                .await
                .expect("an answer")
                .expect("worker running");
            if let Event::Tournament {
                request_id,
                ok,
                reason,
                identity,
                data,
                ..
            } = event
                && request_id == id
            {
                return (
                    ok,
                    reason,
                    identity.unwrap_or(Value::Null),
                    data.unwrap_or(Value::Null),
                );
            }
        }
    }

    /// Enables an identity, approves the bridge at `origin` and returns the
    /// Ember ID and the bridge's ID.
    async fn join(&mut self, origin: &str) -> (String, String) {
        let (ok, reason, identity, _) = self.ask(json!({ "op": "identity_enable" })).await;
        assert!(ok, "{reason:?}");
        let ember_id = identity["ember_id"].as_str().unwrap().to_owned();
        (ember_id, self.approve(origin).await)
    }

    async fn approve(&mut self, origin: &str) -> String {
        let (ok, _, _, inspected) = self
            .ask(json!({ "op": "bridge_inspect", "origin": origin }))
            .await;
        assert!(ok);
        let bridge_id = inspected["profile"]["bridge_id"]
            .as_str()
            .unwrap()
            .to_owned();
        let (ok, reason, _, _) = self
            .ask(json!({ "op": "bridge_approve", "origin": origin, "bridge_id": bridge_id }))
            .await;
        assert!(ok, "{reason:?}");
        bridge_id
    }

    async fn list(&mut self, bridge_id: &str, build: &str) -> Answer {
        self.ask(json!({ "op": "room_list", "bridge_id": bridge_id, "build": build }))
            .await
    }

    async fn create(&mut self, bridge_id: &str, name: &str, capacity: u8) -> Answer {
        self.ask(json!({ "op": "room_create", "bridge_id": bridge_id, "name": name, "capacity": capacity, "build": BUILD }))
            .await
    }

    async fn ticket(&mut self, bridge_id: &str, room_id: &str, build: &str) -> Answer {
        self.ask(json!({ "op": "room_ticket", "bridge_id": bridge_id, "room_id": room_id, "build": build }))
            .await
    }
}

fn ok(answer: Answer) -> Value {
    let (ok, reason, _, data) = answer;
    assert!(ok, "{reason:?}");
    data
}

fn refused(answer: Answer) -> String {
    let (ok, reason, _, data) = answer;
    assert!(!ok, "{data}");
    assert_eq!(data, Value::Null);
    reason.expect("a reason")
}

fn now() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_secs()
}

/// Checks `data` as the game receives it: the admission's ticket verifies
/// under the key the bridge publishes, and it admits `endpoint` as `ember_id`.
async fn assert_admits(
    origin: &str,
    bridge_id: &str,
    data: &Value,
    endpoint: &str,
    ember_id: &str,
) {
    let admission: RoomAdmission = serde_json::from_value(data.clone()).unwrap();
    admission.check().unwrap();
    let body = reqwest::get(format!("{origin}/v1/signing-keys"))
        .await
        .unwrap()
        .bytes()
        .await
        .unwrap();
    let keys: SigningKeys = serde_json::from_slice(&body).unwrap();
    let kid = &admission.ticket.kid;
    let key = keys
        .keys
        .iter()
        .find(|key| &key.kid == kid)
        .and_then(|key| key.public_key())
        .expect("the bridge publishes the ticket's key");
    let ticket = admission.ticket.verify(&key, kid).unwrap();
    assert_eq!(ticket.bridge_id, bridge_id);
    assert_eq!(ticket.endpoint_id, endpoint);
    assert_eq!(ticket.ember_id.as_str(), ember_id);
    assert_eq!(ticket.room_id, admission.room.room_id);
    assert!(ticket.admits(&admission.room.room_id, endpoint, now()));
    assert!(admission.invitation.starts_with("sf4e3:"));
}

fn listed(data: &Value) -> Vec<Value> {
    serde_json::from_value::<RoomList>(data.clone()).unwrap();
    data["rooms"].as_array().unwrap().clone()
}

#[tokio::test]
async fn create_list_and_join_a_public_room() {
    let dirs = dirs("flow");
    let (supervisor, url) = fake_supervisor().await;
    let (running, origin) = bridge(&dirs.0, Some(url)).await;
    let (kate_endpoint, joiner_endpoint) = ("a".repeat(64), "b".repeat(64));
    let (kate_dirs, joiner_dirs) = (Dirs(dirs.0.join("kate")), Dirs(dirs.0.join("joiner")));
    let mut kate = Worker::start(&kate_dirs, &kate_endpoint);
    let mut joiner = Worker::start(&joiner_dirs, &joiner_endpoint);
    let (kate_id, bridge_id) = kate.join(&origin).await;
    let (joiner_id, _) = joiner.join(&origin).await;

    // No room is open.
    assert!(listed(&ok(kate.list(&bridge_id, BUILD).await)).is_empty());

    // Creating answers the creator's admission, whose ticket verifies and
    // names this helper's endpoint and Ember ID.
    let created = ok(kate.create(&bridge_id, "Friendly matches", 2).await);
    assert_admits(&origin, &bridge_id, &created, &kate_endpoint, &kate_id).await;
    let room_id = created["room"]["room_id"].as_str().unwrap().to_owned();
    assert_eq!(created["room"]["name"], "Friendly matches");
    assert_eq!(created["room"]["capacity"], 2);
    assert_eq!(created["room"]["build_id"], BUILD);
    assert_eq!(created["room"]["region"], "use1");
    assert_eq!(supervisor.lock().unwrap().created.len(), 1);
    assert_eq!(
        supervisor.lock().unwrap().created[0]["name"],
        "Friendly matches"
    );
    assert_eq!(
        supervisor.lock().unwrap().created[0]["creator"],
        kate_id.as_str()
    );

    // A room is listed once its creator is inside, and until then nobody
    // else is given a ticket for it.
    ember_bridge::poll_rooms(&running.state).await;
    assert!(listed(&ok(kate.list(&bridge_id, BUILD).await)).is_empty());
    assert_eq!(
        refused(joiner.ticket(&bridge_id, &room_id, BUILD).await),
        "room_not_open"
    );
    report(&supervisor, &room_id, 1, &[]);
    ember_bridge::poll_rooms(&running.state).await;
    let rooms = listed(&ok(joiner.list(&bridge_id, BUILD).await));
    assert_eq!(rooms.len(), 1);
    assert_eq!(rooms[0]["room_id"], room_id.as_str());
    assert_eq!(rooms[0]["members"], 1);
    assert_eq!(rooms[0]["capacity"], 2);
    // Only the build asked for, written as a query value.
    assert!(listed(&ok(joiner.list(&bridge_id, "build 2&x=y").await)).is_empty());

    // Another player joins with a ticket for their own endpoint and ID.
    let admission = ok(joiner.ticket(&bridge_id, &room_id, BUILD).await);
    assert_admits(
        &origin,
        &bridge_id,
        &admission,
        &joiner_endpoint,
        &joiner_id,
    )
    .await;
    assert_ne!(
        admission["ticket"]["ticket"]["endpoint_id"],
        created["ticket"]["ticket"]["endpoint_id"]
    );
    assert_eq!(admission["room"]["members"], 1);

    // Refusals carry the bridge's reason.
    assert_eq!(
        refused(joiner.ticket(&bridge_id, MISSING_ROOM, BUILD).await),
        "room_not_found"
    );
    assert_eq!(
        refused(joiner.ticket(&bridge_id, &room_id, "build-2").await),
        "unsupported_build"
    );
    // One open room per Ember ID.
    assert_eq!(
        refused(kate.create(&bridge_id, "Second room", 4).await),
        "room_limit"
    );
    report(&supervisor, &room_id, 2, &[]);
    ember_bridge::poll_rooms(&running.state).await;
    assert_eq!(
        refused(joiner.ticket(&bridge_id, &room_id, BUILD).await),
        "room_full"
    );
    report(&supervisor, &room_id, 1, &[&joiner_id]);
    ember_bridge::poll_rooms(&running.state).await;
    assert_eq!(
        refused(joiner.ticket(&bridge_id, &room_id, BUILD).await),
        "banned"
    );
    // The creator is not the banned one.
    ok(kate.ticket(&bridge_id, &room_id, BUILD).await);

    // Requests the helper cannot make well formed never reach the bridge.
    assert_eq!(
        refused(joiner.ticket(&bridge_id, "../v1/links", BUILD).await),
        "invalid_request"
    );
    assert_eq!(
        refused(joiner.list(&bridge_id, "").await),
        "invalid_request"
    );
    kate.task.abort();
    joiner.task.abort();
    running.abort();
}

/// The bridge waits up to 30 s for a room host to start and the host may take
/// 25 s to say it is hosted, longer than the helper's ordinary 15 s request
/// timeout; a creation that finishes in between still reaches the creator.
#[tokio::test]
async fn a_room_that_takes_longer_than_an_ordinary_request_is_still_created() {
    let dirs = dirs("slow");
    let (supervisor, url) = fake_supervisor().await;
    supervisor.lock().unwrap().create_delay = Duration::from_secs(17);
    let (running, origin) = bridge(&dirs.0, Some(url)).await;
    let endpoint = "a".repeat(64);
    let mut worker = Worker::start(&dirs, &endpoint);
    let (ember_id, bridge_id) = worker.join(&origin).await;

    let created = ok(worker.create(&bridge_id, "Slow to start", 4).await);
    assert_admits(&origin, &bridge_id, &created, &endpoint, &ember_id).await;
    assert_eq!(supervisor.lock().unwrap().created.len(), 1);
    worker.task.abort();
    running.abort();
}

#[tokio::test]
async fn an_invalid_name_is_refused_before_any_bridge_is_asked() {
    let dirs = dirs("name");
    let (supervisor, url) = fake_supervisor().await;
    let (running, origin) = bridge(&dirs.0, Some(url)).await;
    let mut worker = Worker::start(&dirs, &"a".repeat(64));
    let (_, bridge_id) = worker.join(&origin).await;

    let too_long = "x".repeat(65);
    for name in ["", " padded", "two\nlines", too_long.as_str()] {
        // A bridge the helper never approved would answer `bridge_not_approved`
        // if the request got as far as looking it up.
        assert_eq!(
            refused(
                worker
                    .create("brg_00000000-0000-4000-8000-000000000000", name, 4)
                    .await
            ),
            "invalid_name",
            "{name:?}"
        );
        assert_eq!(
            refused(worker.create(&bridge_id, name, 4).await),
            "invalid_name"
        );
    }
    assert_eq!(
        refused(worker.create(&bridge_id, "Room", 1).await),
        "invalid_request"
    );
    assert_eq!(
        refused(worker.create(&bridge_id, "Room", 17).await),
        "invalid_request"
    );
    assert!(supervisor.lock().unwrap().created.is_empty());
    // Nothing was opened, so a valid request is the creator's first.
    ok(worker.create(&bridge_id, "Room", 4).await);
    assert_eq!(supervisor.lock().unwrap().created.len(), 1);
    worker.task.abort();
    running.abort();
}

#[tokio::test]
async fn a_bridge_without_rooms_offers_none() {
    let dirs = dirs("off");
    let (running, origin) = bridge(&dirs.0, None).await;
    let mut worker = Worker::start(&dirs, &"a".repeat(64));
    let (_, bridge_id) = worker.join(&origin).await;
    assert_eq!(
        refused(worker.list(&bridge_id, BUILD).await),
        "rooms_unavailable"
    );
    assert_eq!(
        refused(worker.create(&bridge_id, "Room", 4).await),
        "rooms_unavailable"
    );
    assert_eq!(
        refused(worker.ticket(&bridge_id, MISSING_ROOM, BUILD).await),
        "rooms_unavailable"
    );
    worker.task.abort();
    running.abort();
}

/// The three requests answer as `assignment_list` does when the identity or
/// the bridge is not there to use.
async fn same_as_assignments(worker: &mut Worker, bridge_id: &str) -> String {
    let expected = refused(
        worker
            .ask(json!({ "op": "assignment_list", "bridge_id": bridge_id }))
            .await,
    );
    assert_eq!(refused(worker.list(bridge_id, BUILD).await), expected);
    assert_eq!(refused(worker.create(bridge_id, "Room", 4).await), expected);
    assert_eq!(
        refused(worker.ticket(bridge_id, MISSING_ROOM, BUILD).await),
        expected
    );
    expected
}

#[tokio::test]
async fn a_missing_or_locked_identity_and_an_unknown_bridge_behave_as_for_assignments() {
    let dirs = dirs("identity");
    let (_, url) = fake_supervisor().await;
    let (running, origin) = bridge(&dirs.0, Some(url)).await;
    let endpoint = "a".repeat(64);
    let mut worker = Worker::start(&dirs, &endpoint);

    // The bridge is not approved yet.
    let unknown = "brg_00000000-0000-4000-8000-000000000000";
    assert_eq!(
        same_as_assignments(&mut worker, unknown).await,
        "bridge_not_approved"
    );
    // Approved, but there is no identity.
    let bridge_id = worker.approve(&origin).await;
    let missing = same_as_assignments(&mut worker, &bridge_id).await;
    assert_eq!(missing, "identity_unavailable");

    // A protected identity is locked after a restart.
    let (ok_enabled, reason, _, _) = worker
        .ask(json!({ "op": "identity_enable", "passphrase": "room words" }))
        .await;
    assert!(ok_enabled, "{reason:?}");
    worker.task.abort();
    let mut restarted = Worker::start(&dirs, &endpoint);
    let locked = same_as_assignments(&mut restarted, &bridge_id).await;
    assert_eq!(locked, "identity_unavailable");

    // Unlocked, the same worker serves rooms.
    let (unlocked, reason, _, _) = restarted
        .ask(json!({ "op": "identity_unlock", "passphrase": "room words" }))
        .await;
    assert!(unlocked, "{reason:?}");
    assert!(listed(&ok(restarted.list(&bridge_id, BUILD).await)).is_empty());
    restarted.task.abort();
    running.abort();
}
