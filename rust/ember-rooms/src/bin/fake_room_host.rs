//! A stand-in for `sf4e-room-host`, used only by the supervisor's tests.
//!
//! It speaks the child protocol (see `ember_rooms::protocol`) and behaves
//! according to the room name in its configuration line:
//!
//! - `exit-early`: exits with status 3 at once.
//! - `no-hosted`: reads stdin and never reports `hosted`.
//! - `garbage`: prints a line that is not JSON instead of `hosted`.
//! - `garbage-late`: reports `hosted`, then prints a line that is not JSON.
//! - `oversize`: reports `hosted`, then prints a line over 128 KiB.
//! - `status-first`: prints a `status` before `hosted`.
//! - `ignore-stdin`: reports `hosted`, then ignores the close request.
//! - `unknown-types`: prints messages of unknown type around `hosted`.
//! - `members:<n>`: reports `hosted` and `n` members, `n / 2` playing tables
//!   and one banned id, `banned-<n>`.
//! - `occupied-then-empty`: reports `hosted`, one member, then none.
//! - `last-member-left`: like `occupied-then-empty`, then says `closed` on its
//!   own and exits 0 half a second later, whatever its stdin does.
//! - `details`: reports `hosted`, then one member with listing details, an
//!   unknown key among them.
//! - `bad-details`: reports `hosted`, then one member with details that are
//!   not an object, then a larger one than allowed, then none.
//! - `deep-details`: reports `hosted`, then one member whose details nest 200
//!   arrays deep, past the JSON parser's own recursion limit.
//! - `max-status`: reports `hosted`, then the largest valid status: 512 banned
//!   Ember IDs and an invitation of 4096 bytes.
//! - `too-many-bans`: like `max-status` with 513 banned IDs, which is a
//!   protocol error.
//! - anything else: reports `hosted` and zero members.
//!
//! When `FAKE_ROOM_HOST_PIDFILE` is set it writes its process id to that file
//! at start (tests that run the real supervisor binary use it).
//!
//! The ones that report `hosted` print `closed` and exit 0 when stdin closes.
//! The invitation echoes the configuration it was given (ports included), so tests can check
//! what the supervisor sent.
use std::{
    io::{BufRead, Write},
    process::ExitCode,
    time::Duration,
};

use serde_json::{Value, json};

fn send(message: &Value) {
    let mut out = std::io::stdout().lock();
    let _ = writeln!(out, "{message}");
    let _ = out.flush();
}

fn text<'a>(config: &'a Value, field: &str) -> &'a str {
    config[field].as_str().unwrap_or("")
}

/// A well-formed Ember ID (57 bytes) that differs for each `index` below 1024.
fn ember_id(index: usize) -> String {
    const SYMBOLS: &[u8] = b"abcdefghijklmnopqrstuvwxyz234567";
    let mut body = [b'a'; 51];
    body[0] = SYMBOLS[(index / 32) % 32];
    body[1] = SYMBOLS[index % 32];
    format!("emb1_{}a", String::from_utf8_lossy(&body))
}

fn wait_for_stdin_close() {
    let mut sink = String::new();
    let mut stdin = std::io::stdin().lock();
    while stdin.read_line(&mut sink).is_ok_and(|read| read > 0) {
        sink.clear();
    }
}

fn main() -> ExitCode {
    // A stray fake must not outlive a failed test run.
    std::thread::spawn(|| {
        std::thread::sleep(Duration::from_secs(120));
        std::process::exit(99);
    });
    let mut line = String::new();
    if std::io::stdin().lock().read_line(&mut line).is_err() {
        return ExitCode::FAILURE;
    }
    let Ok(config) = serde_json::from_str::<Value>(&line) else {
        return ExitCode::FAILURE;
    };
    // Lets a test find this process: it writes its id where it is told.
    if let Some(path) = std::env::var_os("FAKE_ROOM_HOST_PIDFILE") {
        let _ = std::fs::write(path, std::process::id().to_string());
    }
    let name = text(&config, "name").to_owned();
    let invitation = format!(
        "inv:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}",
        text(&config, "room_id"),
        config["port"],
        config["coordination_port"],
        text(&config, "build_id"),
        text(&config, "creator"),
        text(&config, "bridge_id"),
        text(&config, "ticket_key"),
        text(&config, "ticket_kid"),
        text(&config, "helper"),
        config["capacity"],
    );
    let hosted = json!({ "type": "hosted", "invitation": invitation, "region": "test" });
    let status = |members: u64, tables: u64, banned: Vec<String>| {
        json!({ "type": "status", "members": members, "tables_playing": tables,
                "invitation": invitation, "banned": banned })
    };
    match name.as_str() {
        "exit-early" => return ExitCode::from(3),
        "no-hosted" => {
            wait_for_stdin_close();
            return ExitCode::SUCCESS;
        }
        "garbage" => {
            println!("this is not json");
            wait_for_stdin_close();
            return ExitCode::SUCCESS;
        }
        "status-first" => {
            send(&status(0, 0, Vec::new()));
            wait_for_stdin_close();
            return ExitCode::SUCCESS;
        }
        _ => {}
    }
    if name == "unknown-types" {
        send(&json!({ "type": "future-thing", "payload": [1, 2, 3] }));
    }
    send(&hosted);
    match name.as_str() {
        "garbage-late" => println!("{{ nope"),
        "oversize" => {
            let long = "x".repeat(140_000);
            send(&json!({ "type": "status", "members": 0, "invitation": long }));
        }
        "ignore-stdin" => {
            send(&status(0, 0, Vec::new()));
            // Sleep until the self-destruct thread ends the process.
            loop {
                std::thread::sleep(Duration::from_secs(1));
            }
        }
        "unknown-types" => {
            send(&json!({ "type": "future-thing" }));
            send(&status(0, 0, Vec::new()));
        }
        "occupied-then-empty" => {
            send(&status(1, 0, Vec::new()));
            send(&status(0, 0, Vec::new()));
        }
        "last-member-left" => {
            send(&status(1, 0, Vec::new()));
            send(&status(0, 0, Vec::new()));
            send(&json!({ "type": "closed", "reason": "last_member_left" }));
            // The real host takes a moment to close the room before it exits.
            std::thread::sleep(Duration::from_millis(500));
            return ExitCode::SUCCESS;
        }
        "details" => send(&json!({
            "type": "status", "members": 1, "tables_playing": 0, "invitation": invitation,
            "details": { "name": "Renamed", "capacity": 6, "locked": true,
                "host_name": "Kate", "fighters": [3, 255], "set_format": 3, "rotation": 1,
                "future": { "x": 1 } },
        })),
        "bad-details" => {
            for details in [json!("nope"), json!({ "name": "n".repeat(3000) })] {
                send(
                    &json!({ "type": "status", "members": 1, "invitation": invitation,
                    "details": details }),
                );
            }
            send(&status(1, 0, Vec::new()));
        }
        "deep-details" => {
            let (open, close) = ("[".repeat(200), "]".repeat(200));
            let line = format!(
                r#"{{"type":"status","members":1,"invitation":{invitation:?},"details":{{"fighters":{open}{close}}}}}"#
            );
            let mut out = std::io::stdout().lock();
            let _ = writeln!(out, "{line}");
            let _ = out.flush();
        }
        "max-status" | "too-many-bans" => {
            let count = if name == "max-status" { 512 } else { 513 };
            let banned: Vec<String> = (0..count).map(ember_id).collect();
            let full = format!("sf4e3:{}", "x".repeat(4090));
            send(
                &json!({ "type": "status", "members": 1, "tables_playing": 0,
                          "invitation": full, "banned": banned }),
            );
        }
        other => {
            let members = other
                .strip_prefix("members:")
                .and_then(|count| count.parse::<u64>().ok());
            match members {
                Some(count) => send(&status(count, count / 2, vec![format!("banned-{count}")])),
                None => send(&status(0, 0, Vec::new())),
            }
        }
    }
    wait_for_stdin_close();
    send(&json!({ "type": "closed", "reason": "stdin closed" }));
    ExitCode::SUCCESS
}
