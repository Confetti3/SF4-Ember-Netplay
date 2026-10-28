//! What the tournament worker tests share: a real bridge on loopback, with
//! sign-in sites and Discord when a test wants them; a front that passes
//! requests through to it and can change what it answers; an origin that
//! serves another bridge's profile; and a helper's tournament worker driven
//! the way the native side drives it.

use std::{
    path::Path,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
    },
    time::Duration,
};

use ember_bridge::{
    Clock, config,
    integrations::Secrets,
    testing::{loopback_config, serve},
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

/// A test's own folder, removed when the test ends.
pub use ember_bridge::testing::{TempDir as Dirs, random16};

/// A new folder under the system's temporary folder, named for `kind`.
pub fn dirs(kind: &str) -> Dirs {
    Dirs::new(&format!("sf4-tournament-{kind}"))
}

/// A loopback bridge with these sign-in sites, each with a client secret,
/// and its origin.
pub async fn bridge(dir: &Path, sites: Vec<config::SignInSite>) -> (ember_bridge::Running, String) {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let origin = format!("http://{}", listener.local_addr().unwrap());
    let running = start_bridge(dir, sites, false, &origin, listener).await;
    (running, origin)
}

/// A bridge on `listener` that names `origin` as its own, with these
/// sign-in sites and, with `discord`, Discord sign-in whose pages are on
/// loopback rather than discord.com.
pub async fn start_bridge(
    dir: &Path,
    sites: Vec<config::SignInSite>,
    discord: bool,
    origin: &str,
    listener: TcpListener,
) -> ember_bridge::Running {
    let secrets = Secrets {
        site_client_secrets: sites
            .iter()
            .map(|site| (site.id.clone(), Zeroizing::new("site-secret".to_owned())))
            .collect(),
        discord_client_secret: discord.then(|| Zeroizing::new("discord-secret".to_owned())),
        ..Default::default()
    };
    let mut config = loopback_config(dir, origin);
    config.integration_secrets =
        (!sites.is_empty() || discord).then(|| dir.join("integrations.json"));
    config.discord = discord.then(|| config::Discord {
        client_id: "1546980049692135514".into(),
        api_base: "http://127.0.0.1:9/api".into(),
    });
    config.sign_in_sites = sites;
    serve(config, secrets, Clock::default(), listener).await
}

/// The site `deploy/sites/<file>.json` defines, listed as `id` and named
/// `name`, with every page it would use at `origin`.
pub fn site(file: &str, id: &str, name: &str, origin: &str) -> config::SignInSite {
    let path = format!(
        "{}/../../server/ember/bridge/deploy/sites/{file}.json",
        env!("CARGO_MANIFEST_DIR")
    );
    let mut site: Value = serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap();
    site["id"] = json!(id);
    site["display_name"] = json!(name);
    localize(&mut site, origin);
    site["hosts"] = json!(["127.0.0.1"]);
    serde_json::from_value(site).unwrap()
}

/// Every URL in `value` (sign-in, user, organizer) moved to `origin`, keeping
/// its path. `link_prefix` stays: it is shown, not fetched, and must be https.
fn localize(value: &mut Value, origin: &str) {
    match value {
        Value::String(text) if text.starts_with("https://") => {
            // Not through Url: a "{tournament}" placeholder in the path must
            // survive as written.
            let rest = &text["https://".len()..];
            let path = rest.find('/').map_or("", |at| &rest[at..]);
            *text = format!("{origin}{path}");
        }
        Value::Object(fields) => fields
            .iter_mut()
            .filter(|(key, _)| *key != "link_prefix")
            .for_each(|(_, field)| localize(field, origin)),
        Value::Array(items) => items.iter_mut().for_each(|item| localize(item, origin)),
        _ => {}
    }
}

/// One HTTP/1.1 request as it was sent: its method, path, headers (names in
/// lowercase) and body.
pub struct HttpRequest {
    pub method: String,
    pub path: String,
    pub headers: Vec<(String, String)>,
    pub body: Vec<u8>,
}

/// The request `socket` sends, or none when it closes first.
pub async fn read_request(socket: &mut TcpStream) -> Option<HttpRequest> {
    let mut bytes = Vec::new();
    let mut buffer = [0; 4096];
    let head_end = loop {
        if let Some(at) = bytes.windows(4).position(|end| end == b"\r\n\r\n") {
            break at + 4;
        }
        match socket.read(&mut buffer).await {
            Ok(0) | Err(_) => return None,
            Ok(read) => bytes.extend_from_slice(&buffer[..read]),
        }
    };
    let head = String::from_utf8_lossy(&bytes[..head_end]).into_owned();
    let mut lines = head.lines();
    let mut first = lines.next().unwrap_or("").split(' ');
    let (method, path) = (
        first.next().unwrap_or("").to_owned(),
        first.next().unwrap_or("").to_owned(),
    );
    let headers: Vec<(String, String)> = lines
        .filter_map(|line| line.split_once(':'))
        .map(|(name, value)| (name.trim().to_ascii_lowercase(), value.trim().to_owned()))
        .collect();
    let length = headers
        .iter()
        .find(|(name, _)| name == "content-length")
        .and_then(|(_, value)| value.parse::<usize>().ok())
        .unwrap_or(0);
    while bytes.len() < head_end + length {
        match socket.read(&mut buffer).await {
            Ok(0) | Err(_) => break,
            Ok(read) => bytes.extend_from_slice(&buffer[..read]),
        }
    }
    Some(HttpRequest {
        method,
        path,
        headers,
        body: bytes.split_off(head_end),
    })
}

/// Answers on `socket` with `status` (such as `200 OK`) and closes it.
pub async fn write_response(socket: &mut TcpStream, status: &str, kind: &str, body: &[u8]) {
    let head = format!(
        "HTTP/1.1 {status}\r\ncontent-type: {kind}\r\ncontent-length: {}\r\nconnection: close\r\n\r\n",
        body.len()
    );
    let _ = socket.write_all(head.as_bytes()).await;
    let _ = socket.write_all(body).await;
}

/// What a front saw, and a switch that makes its capabilities unavailable.
pub struct Front {
    pub seen: Arc<Mutex<Vec<String>>>,
    pub down: Arc<AtomicBool>,
}

/// A front at `listener` for the bridge at `inner`: each request passes
/// through as it was sent, except that the bridge's capabilities document
/// is changed by `change` on its way back, or answered 503 while `down` is
/// set. Notes the method and path of every request, in order.
pub fn front(listener: TcpListener, inner: String, change: fn(&mut Value)) -> Front {
    let seen = Arc::new(Mutex::new(Vec::new()));
    let down = Arc::new(AtomicBool::new(false));
    let (log, unavailable) = (seen.clone(), down.clone());
    tokio::spawn(async move {
        let client = reqwest::Client::new();
        while let Ok((mut socket, _)) = listener.accept().await {
            let Some(request) = read_request(&mut socket).await else {
                continue;
            };
            let HttpRequest {
                method,
                path,
                headers,
                body,
            } = request;
            log.lock().unwrap().push(format!("{method} {path}"));
            let capabilities = path == "/v1/capabilities";
            if capabilities && unavailable.load(Ordering::SeqCst) {
                write_response(
                    &mut socket,
                    "503 Service Unavailable",
                    "application/json",
                    &[],
                )
                .await;
                continue;
            }
            let mut forward = client.request(
                reqwest::Method::from_bytes(method.as_bytes()).unwrap(),
                format!("{inner}{path}"),
            );
            for (name, value) in &headers {
                if !matches!(name.as_str(), "host" | "content-length" | "connection") {
                    forward = forward.header(name, value);
                }
            }
            let response = forward.body(body).send().await.unwrap();
            let status = response.status();
            let kind = response
                .headers()
                .get("content-type")
                .map(|value| value.to_str().unwrap().to_owned())
                .unwrap_or_else(|| "application/json".into());
            let mut answer = response.bytes().await.unwrap().to_vec();
            if capabilities && status == 200 {
                let mut document: Value = serde_json::from_slice(&answer).unwrap();
                change(&mut document);
                answer = serde_json::to_vec(&document).unwrap();
            }
            write_response(&mut socket, &status.to_string(), &kind, &answer).await;
        }
    });
    Front { seen, down }
}

/// An origin whose well-known document names another bridge, and the paths
/// of every request it was sent.
pub async fn foreign_origin() -> (String, Arc<Mutex<Vec<String>>>) {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let origin = format!("http://{}", listener.local_addr().unwrap());
    let paths = Arc::new(Mutex::new(Vec::new()));
    let (served, profile) = (
        paths.clone(),
        json!({
            "bridge_id": ember_protocol::encoding::prefixed_id("brg", random16()),
            "origin": origin, "display_name": "Another bridge", "api_versions": ["v1"],
            "capabilities_url": format!("{origin}/v1/capabilities"),
            "signing_keys_url": format!("{origin}/v1/signing-keys"),
        }),
    );
    tokio::spawn(async move {
        while let Ok((mut socket, _)) = listener.accept().await {
            let path = read_request(&mut socket)
                .await
                .map(|request| request.path)
                .unwrap_or_default();
            served.lock().unwrap().push(path.clone());
            if path == "/.well-known/ember-bridge.json" {
                let body = serde_json::to_vec(&profile).unwrap();
                write_response(&mut socket, "200 OK", "application/json", &body).await;
            } else {
                write_response(&mut socket, "404 Not Found", "application/json", &[]).await;
            }
        }
    });
    (origin, paths)
}

/// A helper's tournament worker, asked the way the native side asks it.
pub struct Worker {
    pub handle: tournament::Handle,
    pub events: mpsc::Receiver<Event>,
    pub task: tokio::task::JoinHandle<()>,
    next: u64,
}

impl Worker {
    pub fn start(dirs: &Dirs) -> Self {
        Self::start_as(dirs, "a".repeat(64))
    }

    /// A worker whose helper run has the Iroh endpoint `endpoint`.
    pub fn start_as(dirs: &Dirs, endpoint: String) -> Self {
        Self::start_pinning(dirs, endpoint, Vec::new())
    }

    /// A worker that trusts `builtins` as a build pinning them does.
    pub fn start_pinning(
        dirs: &Dirs,
        endpoint: String,
        builtins: Vec<tournament::Builtin>,
    ) -> Self {
        let (sender, events) = mpsc::channel(64);
        let (handle, task) = tournament::spawn_in(
            sender,
            dirs.0.join("identity"),
            dirs.0.join("tournament"),
            false,
            endpoint,
            Box::new(|| None),
            builtins,
        );
        Self {
            handle,
            events,
            task,
            next: 100,
        }
    }

    pub async fn next_answer(&mut self, request_id: u64) -> (bool, Option<String>, Value, Value) {
        loop {
            let event = tokio::time::timeout(Duration::from_secs(30), self.events.recv())
                .await
                .expect("an answer")
                .expect("worker running");
            if let Event::Tournament {
                request_id: id,
                ok,
                reason,
                identity,
                data,
                ..
            } = event
                && id == request_id
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

    /// Sends `request` the way the native side does, as IPC JSON.
    pub async fn ask(&mut self, request: Value) -> (bool, Option<String>, Value, Value) {
        self.next += 1;
        let id = self.next;
        let command: Command =
            serde_json::from_value(json!({ "type": "tournament", "request": request })).unwrap();
        let Command::Tournament { request } = command else {
            unreachable!()
        };
        self.handle.submit(id, request);
        self.next_answer(id).await
    }
}
