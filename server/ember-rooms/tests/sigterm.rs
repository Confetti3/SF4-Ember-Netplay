//! Runs the real `ember-rooms` binary and sends it SIGTERM, as systemd does
//! on a restart (`KillMode=mixed`: the signal goes to the supervisor only).
//! The supervisor must drain: refuse new rooms, leave an occupied room's host
//! running, and exit 0 once the room ends or `drain_secs` has passed. SIGHUP,
//! as `systemctl reload` sends it, must re-read the configuration and leave
//! the supervisor and its rooms running. Unix only; on other platforms this
//! file compiles to nothing.
#![cfg(all(unix, feature = "test-fake-host"))]

use std::{
    io::{Read, Write},
    net::{TcpListener, TcpStream},
    path::PathBuf,
    process::{Child, Command, ExitStatus, Stdio},
    time::{Duration, Instant},
};

use serde_json::{Value, json};

const SECRET: &str = "0123456789abcdef0123456789abcdef";
const FAKE: &str = env!("CARGO_BIN_EXE_fake_room_host");
const SUPERVISOR: &str = env!("CARGO_BIN_EXE_ember-rooms");

/// A running supervisor with its files. Dropping it kills what is left.
struct Running {
    child: Child,
    dir: PathBuf,
    port: u16,
}

impl Drop for Running {
    fn drop(&mut self) {
        // A killed supervisor closes its children's stdin, which ends them.
        let _ = self.child.kill();
        let _ = self.child.wait();
        let _ = std::fs::remove_dir_all(&self.dir);
    }
}

fn free_port() -> u16 {
    TcpListener::bind("127.0.0.1:0")
        .unwrap()
        .local_addr()
        .unwrap()
        .port()
}

/// Starts the supervisor with a temporary configuration. `ports` is the first
/// UDP port of the range handed to room hosts.
fn start(name: &str, drain_secs: u64, ports: u16) -> Running {
    let dir = std::env::temp_dir().join(format!("ember-rooms-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    std::fs::write(dir.join("secret"), SECRET).unwrap();
    let port = free_port();
    let config = json!({
        "bind": format!("127.0.0.1:{port}"),
        "secret_file": dir.join("secret"),
        "max_rooms": 4,
        "port_range": [ports, ports + 9],
        "empty_close_secs": 600,
        "drain_secs": drain_secs,
        "builds": { "b1": { "room_host": FAKE, "helper": "helper-path" } },
    });
    std::fs::write(dir.join("config.json"), config.to_string()).unwrap();
    let child = Command::new(SUPERVISOR)
        .arg(dir.join("config.json"))
        .env("FAKE_ROOM_HOST_PIDFILE", dir.join("host.pid"))
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .spawn()
        .unwrap();
    let running = Running { child, dir, port };
    let deadline = Instant::now() + Duration::from_secs(15);
    while http(&running, "GET", "/health", None).map(|reply| reply.0) != Some(200) {
        assert!(Instant::now() < deadline, "the supervisor never came up");
        std::thread::sleep(Duration::from_millis(50));
    }
    running
}

/// One HTTP/1.1 request; the status and the body as JSON (a string when it
/// is not JSON). `None` when the connection fails.
fn http(running: &Running, method: &str, path: &str, body: Option<&Value>) -> Option<(u16, Value)> {
    let mut stream = TcpStream::connect(("127.0.0.1", running.port)).ok()?;
    stream
        .set_read_timeout(Some(Duration::from_secs(10)))
        .unwrap();
    let payload = body.map(Value::to_string).unwrap_or_default();
    let request = format!(
        "{method} {path} HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {SECRET}\r\n\
         Content-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{payload}",
        payload.len()
    );
    stream.write_all(request.as_bytes()).ok()?;
    let mut reply = Vec::new();
    stream.read_to_end(&mut reply).ok()?;
    let text = String::from_utf8_lossy(&reply);
    let (head, body) = text.split_once("\r\n\r\n")?;
    let status = head.split_whitespace().nth(1)?.parse().ok()?;
    let value = serde_json::from_str(body).unwrap_or_else(|_| Value::String(body.to_owned()));
    Some((status, value))
}

fn room_body(number: u32, name: &str, build: &str) -> Value {
    json!({
        "room_id": format!("{number:032x}"), "name": name, "capacity": 8, "build_id": build,
        "creator": "emb_creator", "bridge_id": "brg_test",
        "ticket_key": "a2V5a2V5", "ticket_kid": "kid1",
    })
}

fn signal(name: &str, pid: u32) {
    let status = Command::new("sh")
        .arg("-c")
        .arg(format!("kill -{name} {pid}"))
        .status()
        .unwrap();
    assert!(status.success(), "kill -{name} {pid}");
}

fn alive(pid: u32) -> bool {
    Command::new("sh")
        .arg("-c")
        .arg(format!("kill -0 {pid} 2>/dev/null"))
        .status()
        .unwrap()
        .success()
}

fn wait_for(what: &str, seconds: u64, mut done: impl FnMut() -> bool) {
    let deadline = Instant::now() + Duration::from_secs(seconds);
    while !done() {
        assert!(Instant::now() < deadline, "timed out waiting for {what}");
        std::thread::sleep(Duration::from_millis(25));
    }
}

fn wait_for_exit(running: &mut Running, seconds: u64) -> ExitStatus {
    let deadline = Instant::now() + Duration::from_secs(seconds);
    loop {
        if let Some(status) = running.child.try_wait().unwrap() {
            return status;
        }
        assert!(
            Instant::now() < deadline,
            "the supervisor did not exit in {seconds} s"
        );
        std::thread::sleep(Duration::from_millis(25));
    }
}

/// Creates an occupied room and returns the id of its host process.
fn occupied_room(running: &Running) -> u32 {
    let (status, _) = http(
        running,
        "POST",
        "/rooms",
        Some(&room_body(1, "members:2", "b1")),
    )
    .unwrap();
    assert_eq!(status, 201);
    wait_for("the room's member count", 10, || {
        http(running, "GET", "/rooms", None)
            .is_some_and(|(_, rooms)| rooms.get(0).is_some_and(|room| room["members"] == 2))
    });
    std::fs::read_to_string(running.dir.join("host.pid"))
        .unwrap()
        .trim()
        .parse()
        .unwrap()
}

/// Sends SIGTERM to the supervisor alone and waits until it is draining. An
/// unknown build is refused as `unsupported_build` until the drain starts and
/// as `room_limit` after, and never creates a room either way.
fn terminate(running: &Running) {
    signal("TERM", running.child.id());
    wait_for("the drain to begin", 10, || {
        http(
            running,
            "POST",
            "/rooms",
            Some(&room_body(9, "normal", "nope")),
        )
        .is_some_and(|(status, reply)| status == 409 && reply["reason"] == "room_limit")
    });
}

#[test]
fn sigterm_drains_and_the_supervisor_exits_once_the_room_ends() {
    let mut running = start("sigterm-room", 60, 47200);
    let host = occupied_room(&running);
    assert!(alive(host));

    terminate(&running);
    // A room of a known build is refused too, and the room is still served.
    let (status, reply) = http(
        &running,
        "POST",
        "/rooms",
        Some(&room_body(2, "normal", "b1")),
    )
    .unwrap();
    assert_eq!((status, &reply["reason"]), (409, &json!("room_limit")));
    let (status, rooms) = http(&running, "GET", "/rooms", None).unwrap();
    assert_eq!((status, rooms.as_array().map(Vec::len)), (200, Some(1)));

    // The host got no signal: it is still running a moment later, and so is
    // the supervisor, which is waiting for the room.
    std::thread::sleep(Duration::from_millis(700));
    assert!(alive(host), "the room host died with the SIGTERM");
    assert!(running.child.try_wait().unwrap().is_none());

    let (status, _) = http(&running, "DELETE", &format!("/rooms/{:032x}", 1), None).unwrap();
    assert_eq!(status, 204);
    let status = wait_for_exit(&mut running, 20);
    assert_eq!(status.code(), Some(0));
    wait_for("the host to be gone", 5, || !alive(host));
}

#[test]
fn sigterm_closes_a_room_that_outlasts_the_drain_time() {
    let mut running = start("sigterm-timeout", 2, 47210);
    let host = occupied_room(&running);

    let begun = Instant::now();
    terminate(&running);
    std::thread::sleep(Duration::from_millis(500));
    assert!(alive(host), "the room host died with the SIGTERM");

    let status = wait_for_exit(&mut running, 20);
    assert_eq!(status.code(), Some(0));
    assert!(
        begun.elapsed() >= Duration::from_millis(1500),
        "the supervisor did not wait out drain_secs: {:?}",
        begun.elapsed()
    );
    wait_for("the host to be gone", 5, || !alive(host));
}

/// Rewrites the running supervisor's configuration file through `change`.
fn rewrite_config(running: &Running, change: impl FnOnce(&mut Value)) {
    let path = running.dir.join("config.json");
    let mut config: Value = serde_json::from_str(&std::fs::read_to_string(&path).unwrap()).unwrap();
    change(&mut config);
    std::fs::write(&path, config.to_string()).unwrap();
}

fn limits(running: &Running) -> Value {
    let (status, limits) = http(running, "GET", "/limits", None).unwrap();
    assert_eq!(status, 200);
    limits
}

#[test]
fn sighup_reloads_the_limits_and_keeps_the_rooms() {
    let mut running = start("sighup", 60, 47220);
    let host = occupied_room(&running);
    assert_eq!(limits(&running)["max_rooms"], 4);

    rewrite_config(&running, |config| {
        config["max_rooms"] = json!(5);
        config["builds"]["b2"] = config["builds"]["b1"].clone();
    });
    signal("HUP", running.child.id());
    wait_for("the reload", 10, || limits(&running)["reloads"] == 1);
    let now = limits(&running);
    assert_eq!(
        (&now["max_rooms"], &now["builds"]),
        (&json!(5), &json!(["b1", "b2"]))
    );
    assert!(
        running.child.try_wait().unwrap().is_none(),
        "SIGHUP ended the supervisor"
    );
    assert!(alive(host), "SIGHUP ended the room host");
    let (status, rooms) = http(&running, "GET", "/rooms", None).unwrap();
    assert_eq!((status, rooms.as_array().map(Vec::len)), (200, Some(1)));
    let (status, _) = http(
        &running,
        "POST",
        "/rooms",
        Some(&room_body(2, "normal", "b2")),
    )
    .unwrap();
    assert_eq!(status, 201);

    // A file that does not parse is refused and changes nothing.
    std::fs::write(running.dir.join("config.json"), "{ not json").unwrap();
    signal("HUP", running.child.id());
    std::thread::sleep(Duration::from_millis(500));
    assert!(
        running.child.try_wait().unwrap().is_none(),
        "a bad reload ended the supervisor"
    );
    let after = limits(&running);
    assert_eq!(
        (&after["max_rooms"], &after["reloads"]),
        (&json!(5), &json!(1))
    );
    assert!(alive(host));
}
