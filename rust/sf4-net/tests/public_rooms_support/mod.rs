//! What the public room tests share: a real bridge on loopback, a stand-in room
//! supervisor, and a helper's tournament worker driven the way the native side
//! drives it.
#![allow(dead_code)]

use std::{
    path::Path,
    sync::{Arc, Mutex},
    time::Duration,
};

use ember_bridge::{
    Clock,
    integrations::Secrets,
    testing::{loopback_config, serve, supervisor as stand_in},
};
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

/// The stand-in room supervisor is the bridge tests' own.
pub use ember_bridge::testing::{
    TempDir as Dirs,
    supervisor::{BUILD, SECRET, Supervisor, fake_supervisor},
};

pub const MISSING_ROOM: &str = "0123456789abcdef0123456789abcdef";

pub fn dirs(label: &str) -> Dirs {
    Dirs::new(&format!("sf4-public-rooms-{label}"))
}

/// A helper's home relay region, which the test sets while the helper runs.
pub type Region = Arc<Mutex<Option<&'static str>>>;

/// Reads one HTTP/1.1 request: its head (the request line and the headers, up
/// to the blank line) and its body.
pub async fn read_raw(stream: &mut TcpStream) -> Option<(String, Vec<u8>)> {
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
    Some((head, data[head_end..head_end + length].to_vec()))
}

pub async fn answer(stream: &mut TcpStream, status: &str, body: &str) {
    let response = format!(
        "HTTP/1.1 {status}\r\ncontent-type: application/json\r\ncontent-length: {}\r\nconnection: close\r\n\r\n{body}",
        body.len()
    );
    let _ = stream.write_all(response.as_bytes()).await;
    let _ = stream.shutdown().await;
}

/// What the stand-in reports for `room_id`: `members` and `banned`, and no
/// table playing.
pub fn report(fake: &Supervisor, room_id: &str, members: u32, banned: &[&str]) {
    stand_in::report(fake, room_id, members, 0, banned);
}

/// What the stand-in reports as the room host's `details` for `room_id`.
pub fn set_details(fake: &Supervisor, room_id: &str, details: Value) {
    stand_in::report_details(fake, room_id, details);
}

/// A bridge on loopback, with public rooms when `supervisor` names one.
pub async fn bridge(dir: &Path, supervisor: Option<String>) -> (ember_bridge::Running, String) {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let origin = format!("http://{}", listener.local_addr().unwrap());
    (
        bridge_at(dir, supervisor, listener, origin.clone(), SECRET).await,
        origin,
    )
}

/// A bridge that listens on `listener` and describes itself as served from
/// `origin`, which is where a stand-in in front of it listens.
pub async fn bridge_at(
    dir: &Path,
    supervisor: Option<String>,
    listener: TcpListener,
    origin: String,
    supervisor_secret: &str,
) -> ember_bridge::Running {
    let mut config = loopback_config(dir, &origin);
    let rooms = supervisor.is_some();
    if let Some(url) = supervisor {
        stand_in::enable(&mut config, url);
    }
    let secrets = Secrets {
        rooms_supervisor_secret: rooms.then(|| Zeroizing::new(supervisor_secret.into())),
        ..Default::default()
    };
    serve(config, secrets, Clock::default(), listener).await
}

/// What an older bridge answers to a create with a field it does not know: its
/// own refusal of the body, as an invalid request.
pub const OLD_BRIDGE_REFUSAL: &str = r#"{"error":{"code":"invalid_request","message":"The request body is not valid.","retryable":false,"request_id":"req_old","details":{}}}"#;

/// Stands in front of the bridge as an older one would be: it answers a
/// create that carries `own_delay` with the refusal an older bridge gives for
/// a field it does not know, and passes every other request to the bridge.
pub async fn older_bridge_in_front(listener: TcpListener, bridge: String) {
    while let Ok((client, _)) = listener.accept().await {
        tokio::spawn(relay(client, bridge.clone()));
    }
}

pub async fn relay(mut client: TcpStream, bridge: String) {
    let Some((head, body)) = read_raw(&mut client).await else {
        return;
    };
    let unknown_field = head.starts_with("POST /v1/rooms ")
        && serde_json::from_slice::<Value>(&body).is_ok_and(|body| body.get("own_delay").is_some());
    if unknown_field {
        return answer(&mut client, "400 Bad Request", OLD_BRIDGE_REFUSAL).await;
    }
    // One request per connection: the bridge is asked to close after its answer.
    let mut forwarded = String::new();
    for line in head
        .split("\r\n")
        .filter(|line| !line.is_empty() && !line.to_ascii_lowercase().starts_with("connection:"))
    {
        forwarded.push_str(line);
        forwarded.push_str("\r\n");
    }
    forwarded.push_str("connection: close\r\n\r\n");
    let Ok(mut server) = TcpStream::connect(&bridge).await else {
        return;
    };
    let _ = server.write_all(forwarded.as_bytes()).await;
    let _ = server.write_all(&body).await;
    let mut response = Vec::new();
    let _ = server.read_to_end(&mut response).await;
    let _ = client.write_all(&response).await;
    let _ = client.shutdown().await;
}

pub struct Worker {
    pub handle: tournament::Handle,
    pub events: mpsc::Receiver<Event>,
    pub task: tokio::task::JoinHandle<()>,
    pub next: u64,
}

/// `ok`, the reason, the identity status and the data of one answer.
pub type Answer = (bool, Option<String>, Value, Value);

impl Worker {
    /// A helper whose home relay has no known region.
    pub fn start(dirs: &Dirs, endpoint: &str) -> Self {
        Self::start_in(dirs, endpoint, Region::default())
    }

    /// A helper whose home relay is in whatever region `region` holds when it
    /// creates a room.
    pub fn start_in(dirs: &Dirs, endpoint: &str, region: Region) -> Self {
        let (sender, events) = mpsc::channel(64);
        let (handle, task) = tournament::spawn_in(
            sender,
            dirs.0.join("identity"),
            dirs.0.join("tournament"),
            false,
            endpoint.to_owned(),
            Box::new(move || *region.lock().unwrap()),
            Vec::new(),
        );
        Self {
            handle,
            events,
            task,
            next: 100,
        }
    }

    /// Sends `request` the way the native side does, as IPC JSON.
    pub async fn ask(&mut self, request: Value) -> Answer {
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
    pub async fn join(&mut self, origin: &str) -> (String, String) {
        let (ok, reason, identity, _) = self.ask(json!({ "op": "identity_enable" })).await;
        assert!(ok, "{reason:?}");
        let ember_id = identity["ember_id"].as_str().unwrap().to_owned();
        (ember_id, self.approve(origin).await)
    }

    pub async fn approve(&mut self, origin: &str) -> String {
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

    pub async fn list(&mut self, bridge_id: &str, build: &str) -> Answer {
        self.ask(json!({ "op": "room_list", "bridge_id": bridge_id, "build": build }))
            .await
    }

    pub async fn create(&mut self, bridge_id: &str, name: &str, capacity: u8) -> Answer {
        self.ask(json!({ "op": "room_create", "bridge_id": bridge_id, "name": name, "capacity": capacity, "build": BUILD }))
            .await
    }

    /// A create that asks for each player's own input delay.
    pub async fn create_own(&mut self, bridge_id: &str, name: &str, capacity: u8) -> Answer {
        self.ask(json!({ "op": "room_create", "bridge_id": bridge_id, "name": name, "capacity": capacity, "build": BUILD, "own_delay": true }))
            .await
    }

    pub async fn ticket(&mut self, bridge_id: &str, room_id: &str, build: &str) -> Answer {
        self.ask(json!({ "op": "room_ticket", "bridge_id": bridge_id, "room_id": room_id, "build": build }))
            .await
    }
}

pub fn ok(answer: Answer) -> Value {
    let (ok, reason, _, data) = answer;
    assert!(ok, "{reason:?}");
    data
}

pub fn refused(answer: Answer) -> String {
    let (ok, reason, _, data) = answer;
    assert!(!ok, "{data}");
    assert_eq!(data, Value::Null);
    reason.expect("a reason")
}

pub fn now() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_secs()
}

/// Checks `data` as the game receives it: the admission's ticket verifies
/// under the key the bridge publishes, and it admits `endpoint` as `ember_id`.
pub async fn assert_admits(
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

pub fn listed(data: &Value) -> Vec<Value> {
    serde_json::from_value::<RoomList>(data.clone()).unwrap();
    data["rooms"].as_array().unwrap().clone()
}
