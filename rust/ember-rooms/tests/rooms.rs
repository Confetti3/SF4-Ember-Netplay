//! Drives the supervisor through its HTTP routes with real child processes.
//! The children are `fake_room_host`, which behaves by room name.
#![cfg(feature = "test-fake-host")]

use std::{collections::BTreeMap, time::Duration};

use axum::{
    Router,
    body::Body,
    http::{Method, Request, StatusCode},
};
use ember_rooms::{Build, Config, Settings, Supervisor, Tuning, router};
use serde_json::{Value, json};
use tower::ServiceExt;

const SECRET: &str = "0123456789abcdef0123456789abcdef";
const FAKE: &str = env!("CARGO_BIN_EXE_fake_room_host");

struct Harness {
    supervisor: Supervisor,
    app: Router,
}

/// A harness with generous timeouts; `adjust` shrinks the ones a test needs.
fn harness(adjust: impl FnOnce(&mut Config, &mut Tuning)) -> Harness {
    let mut builds = BTreeMap::new();
    builds.insert(
        "b1".to_owned(),
        Build {
            room_host: FAKE.to_owned(),
            helper: "helper-path".to_owned(),
        },
    );
    builds.insert(
        "missing".to_owned(),
        Build {
            room_host: "definitely/not/a/binary".to_owned(),
            helper: "helper-path".to_owned(),
        },
    );
    let mut config = Config {
        bind: "127.0.0.1:47830".to_owned(),
        secret_file: "unused".to_owned(),
        max_rooms: 8,
        port_range: [45800, 45809],
        empty_close_secs: 120,
        drain_secs: 600,
        builds,
    };
    let mut tuning = Tuning::from_config(&config);
    tuning.hosted_timeout = Duration::from_secs(10);
    tuning.kill_grace = Duration::from_secs(5);
    tuning.probe_ports = false;
    adjust(&mut config, &mut tuning);
    let settings = Settings::new(config, SECRET.as_bytes()).unwrap();
    let supervisor = Supervisor::new(settings, tuning);
    Harness {
        app: router(supervisor.clone()),
        supervisor,
    }
}

fn room_id(number: u32) -> String {
    format!("{number:032x}")
}

fn body(number: u32, name: &str) -> Value {
    json!({
        "room_id": room_id(number), "name": name, "capacity": 8, "build_id": "b1",
        "creator": "emb_creator", "bridge_id": "brg_test",
        "ticket_key": "a2V5a2V5", "ticket_kid": "kid1",
    })
}

impl Harness {
    async fn send(
        &self,
        method: Method,
        path: &str,
        body: Option<Value>,
        authorization: Option<&str>,
    ) -> (StatusCode, Value) {
        let mut request = Request::builder().method(method).uri(path);
        if let Some(value) = authorization {
            request = request.header("authorization", value);
        }
        let payload = body.map_or_else(Body::empty, |value| Body::from(value.to_string()));
        let response = self
            .app
            .clone()
            .oneshot(request.body(payload).unwrap())
            .await
            .unwrap();
        let code = response.status();
        let bytes = axum::body::to_bytes(response.into_body(), usize::MAX)
            .await
            .unwrap();
        let value = serde_json::from_slice(&bytes)
            .unwrap_or_else(|_| Value::String(String::from_utf8_lossy(&bytes).into_owned()));
        (code, value)
    }

    async fn call(&self, method: Method, path: &str, body: Option<Value>) -> (StatusCode, Value) {
        let header = format!("Bearer {SECRET}");
        self.send(method, path, body, Some(&header)).await
    }

    async fn create(&self, number: u32, name: &str) -> (StatusCode, Value) {
        self.call(Method::POST, "/rooms", Some(body(number, name)))
            .await
    }

    async fn list(&self) -> Vec<Value> {
        let (code, value) = self.call(Method::GET, "/rooms", None).await;
        assert_eq!(code, StatusCode::OK);
        value.as_array().unwrap().clone()
    }

    /// Waits until the supervisor holds `count` rooms.
    async fn rooms_settle_at(&self, count: usize) {
        for _ in 0..500 {
            if self.supervisor.room_count() == count {
                return;
            }
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
        panic!(
            "expected {count} rooms, have {}",
            self.supervisor.room_count()
        );
    }
}

#[tokio::test]
async fn health_is_open_and_everything_else_needs_the_secret() {
    let harness = harness(|_, _| {});
    let (code, value) = harness.send(Method::GET, "/health", None, None).await;
    assert_eq!((code, value), (StatusCode::OK, json!("ok")));

    for authorization in [
        None,
        Some("Bearer wrong"),
        Some("Bearer 0123456789abcdef0123456789abcdee"),
        Some("Bearer "),
        Some(SECRET),
    ] {
        for (method, path) in [
            (Method::GET, "/rooms"),
            (Method::POST, "/rooms"),
            (Method::DELETE, "/rooms/abc"),
        ] {
            let (code, _) = harness.send(method, path, None, authorization).await;
            assert_eq!(code, StatusCode::UNAUTHORIZED, "{authorization:?}");
        }
    }
    assert!(harness.list().await.is_empty());
}

#[tokio::test]
async fn a_room_is_created_and_listed() {
    let harness = harness(|_, _| {});
    let (code, created) = harness.create(1, "normal").await;
    assert_eq!(code, StatusCode::CREATED);
    assert_eq!(created["region"], "test");
    // The fake echoes its configuration: id, the two lowest ports, build, creator,
    // bridge, ticket key and kid, helper path and capacity.
    let expected = format!(
        "inv:{}:45800:45801:b1:emb_creator:brg_test:a2V5a2V5:kid1:helper-path:8",
        room_id(1)
    );
    assert_eq!(created["invitation"], expected);

    let rooms = harness.list().await;
    assert_eq!(rooms.len(), 1);
    assert_eq!(rooms[0]["room_id"], room_id(1));
    assert_eq!(rooms[0]["members"], 0);
    assert_eq!(rooms[0]["capacity"], 8);
    assert_eq!(rooms[0]["tables_playing"], 0);
    assert_eq!(rooms[0]["invitation"], expected);
    assert_eq!(rooms[0]["banned"], json!([]));
    assert_eq!(rooms[0]["opened"], false);

    // A second room takes the next two ports and lists after the first.
    let (_, second) = harness.create(2, "normal").await;
    assert!(
        second["invitation"]
            .as_str()
            .unwrap()
            .contains(":45802:45803:")
    );
    let ids: Vec<Value> = harness
        .list()
        .await
        .iter()
        .map(|room| room["room_id"].clone())
        .collect();
    assert_eq!(ids, [json!(room_id(1)), json!(room_id(2))]);
}

#[tokio::test]
async fn the_list_carries_the_hosts_latest_status() {
    let harness = harness(|_, _| {});
    harness.create(1, "members:4").await;
    // The status line follows `hosted` a moment later.
    for _ in 0..200 {
        if harness.list().await[0]["members"] == 4 {
            break;
        }
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    let rooms = harness.list().await;
    assert_eq!(rooms[0]["members"], 4);
    assert_eq!(rooms[0]["tables_playing"], 2);
    assert_eq!(rooms[0]["banned"], json!(["banned-4"]));
}

#[tokio::test]
async fn the_list_carries_the_hosts_details_whole() {
    let harness = harness(|_, _| {});
    harness.create(1, "details").await;
    for _ in 0..200 {
        if harness.list().await[0].get("details").is_some() {
            break;
        }
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    let rooms = harness.list().await;
    assert_eq!(rooms[0]["members"], 1);
    // The supervisor's own capacity is the creation one; the details are the host's.
    assert_eq!(rooms[0]["capacity"], 8);
    assert_eq!(
        rooms[0]["details"],
        json!({ "name": "Renamed", "capacity": 6, "locked": true, "host_name": "Kate",
            "fighters": [3, 255], "set_format": 3, "rotation": 1, "future": { "x": 1 } })
    );
}

/// Details that are not an object, or too big, are left out: the room stays up
/// and the list answers.
#[tokio::test]
async fn malformed_details_leave_the_room_up_and_the_list_answering() {
    let harness = harness(|_, _| {});
    let (code, _) = harness.create(1, "bad-details").await;
    assert_eq!(code, StatusCode::CREATED);
    for _ in 0..200 {
        if harness.list().await[0]["members"] == 1 {
            break;
        }
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    tokio::time::sleep(Duration::from_millis(300)).await;
    let rooms = harness.list().await;
    assert_eq!(rooms.len(), 1);
    assert_eq!(rooms[0]["members"], 1);
    assert!(rooms[0].get("details").is_none(), "{}", rooms[0]);
    assert_eq!(harness.supervisor.room_count(), 1);
}

/// Details nested past the JSON parser's recursion limit are as droppable as
/// any other bad details: the status around them still counts, and the room
/// is not killed for them.
#[tokio::test]
async fn deeply_nested_details_leave_the_room_up_and_the_list_answering() {
    let harness = harness(|_, _| {});
    let (code, _) = harness.create(1, "deep-details").await;
    assert_eq!(code, StatusCode::CREATED);
    for _ in 0..200 {
        if harness.list().await[0]["members"] == 1 {
            break;
        }
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    tokio::time::sleep(Duration::from_millis(300)).await;
    let rooms = harness.list().await;
    assert_eq!(rooms.len(), 1);
    assert_eq!(rooms[0]["members"], 1);
    assert_eq!(rooms[0]["opened"], true);
    assert!(rooms[0].get("details").is_none(), "{}", rooms[0]);
    assert_eq!(harness.supervisor.room_count(), 1);
}

/// A member who came and went before the bridge polled still opened the room:
/// the list shows the room empty and opened.
#[tokio::test]
async fn a_room_that_filled_and_emptied_between_polls_is_listed_as_opened() {
    let harness = harness(|_, _| {});
    let (code, _) = harness.create(1, "occupied-then-empty").await;
    assert_eq!(code, StatusCode::CREATED);
    // Both statuses follow `hosted` at once; give them time to land before the
    // first list, which is then the only look at the room.
    tokio::time::sleep(Duration::from_millis(300)).await;
    for _ in 0..200 {
        let rooms = harness.list().await;
        if rooms[0]["opened"] == true {
            break;
        }
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    tokio::time::sleep(Duration::from_millis(200)).await;
    let rooms = harness.list().await;
    assert_eq!(rooms[0]["opened"], true);
    assert_eq!(rooms[0]["members"], 0);
}

#[tokio::test]
async fn a_maximum_size_status_is_accepted_and_listed_whole() {
    let harness = harness(|_, _| {});
    let (code, _) = harness.create(1, "max-status").await;
    assert_eq!(code, StatusCode::CREATED);
    // The status follows `hosted` a moment later.
    for _ in 0..200 {
        if harness.list().await[0]["members"] == 1 {
            break;
        }
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    let rooms = harness.list().await;
    assert_eq!(rooms.len(), 1);
    assert_eq!(rooms[0]["members"], 1);
    assert_eq!(rooms[0]["banned"].as_array().unwrap().len(), 512);
    assert_eq!(rooms[0]["invitation"].as_str().unwrap().len(), 4096);
    // The line was not taken for an overflow: the host is still running.
    tokio::time::sleep(Duration::from_millis(300)).await;
    assert_eq!(harness.supervisor.room_count(), 1);
    assert_eq!(harness.list().await.len(), 1);
}

#[tokio::test]
async fn a_status_naming_too_many_bans_kills_the_host() {
    let harness = harness(|_, _| {});
    let (code, _) = harness.create(1, "too-many-bans").await;
    assert_eq!(code, StatusCode::CREATED);
    harness.rooms_settle_at(0).await;
    assert!(harness.list().await.is_empty());
}

#[tokio::test]
async fn malformed_requests_are_refused() {
    let harness = harness(|_, _| {});
    for (field, change) in [
        ("room_id", json!({ "room_id": "short" })),
        ("capacity", json!({ "capacity": 1 })),
        ("capacity", json!({ "capacity": 17 })),
        ("name", json!({ "name": "" })),
        ("name", json!({ "name": "two\nlines" })),
        ("creator", json!({ "creator": "" })),
    ] {
        let mut request = body(1, "normal");
        request.as_object_mut().unwrap().extend(
            change
                .as_object()
                .unwrap()
                .iter()
                .map(|(key, value)| (key.clone(), value.clone())),
        );
        let (code, reply) = harness.call(Method::POST, "/rooms", Some(request)).await;
        assert_eq!(code, StatusCode::BAD_REQUEST, "{field}");
        assert_eq!(reply["field"], field);
    }
    // Missing fields, a wrong type and a body that is not JSON.
    for payload in [json!({ "room_id": room_id(1) }), json!(7)] {
        let (code, reply) = harness.call(Method::POST, "/rooms", Some(payload)).await;
        assert_eq!(code, StatusCode::BAD_REQUEST);
        assert_eq!(reply["reason"], "invalid_request");
    }
    let header = format!("Bearer {SECRET}");
    let (code, _) = harness
        .send(Method::POST, "/rooms", None, Some(&header))
        .await;
    assert_eq!(code, StatusCode::BAD_REQUEST);
    assert_eq!(harness.supervisor.room_count(), 0);
}

#[tokio::test]
async fn a_duplicate_room_id_is_refused_whatever_its_case() {
    let harness = harness(|_, _| {});
    assert_eq!(harness.create(0xabc, "normal").await.0, StatusCode::CREATED);
    let (code, reply) = harness.create(0xabc, "normal").await;
    assert_eq!(
        (code, reply),
        (StatusCode::CONFLICT, json!({ "reason": "exists" }))
    );
    let mut shouting = body(0xabc, "normal");
    shouting["room_id"] = json!(room_id(0xabc).to_uppercase());
    let (code, reply) = harness.call(Method::POST, "/rooms", Some(shouting)).await;
    assert_eq!(
        (code, reply["reason"].clone()),
        (StatusCode::CONFLICT, json!("exists"))
    );
    assert_eq!(harness.list().await.len(), 1);
}

#[tokio::test]
async fn an_unknown_build_is_refused() {
    let harness = harness(|_, _| {});
    let mut request = body(1, "normal");
    request["build_id"] = json!("nope");
    let (code, reply) = harness.call(Method::POST, "/rooms", Some(request)).await;
    assert_eq!(
        (code, reply),
        (
            StatusCode::CONFLICT,
            json!({ "reason": "unsupported_build" })
        )
    );
    assert_eq!(harness.supervisor.room_count(), 0);
}

#[tokio::test]
async fn the_room_limit_is_enforced() {
    let harness = harness(|config, _| config.max_rooms = 2);
    assert_eq!(harness.create(1, "normal").await.0, StatusCode::CREATED);
    assert_eq!(harness.create(2, "normal").await.0, StatusCode::CREATED);
    let (code, reply) = harness.create(3, "normal").await;
    assert_eq!(
        (code, reply),
        (StatusCode::CONFLICT, json!({ "reason": "room_limit" }))
    );
    assert_eq!(harness.list().await.len(), 2);
}

#[tokio::test]
async fn both_ports_are_reused_after_their_room_closes() {
    let harness = harness(|config, _| config.port_range = [45800, 45803]);
    let (_, first) = harness.create(1, "normal").await;
    assert!(
        first["invitation"]
            .as_str()
            .unwrap()
            .contains(":45800:45801:")
    );
    let (_, second) = harness.create(2, "normal").await;
    assert!(
        second["invitation"]
            .as_str()
            .unwrap()
            .contains(":45802:45803:")
    );
    // All four ports are held.
    let (code, reply) = harness.create(3, "normal").await;
    assert_eq!(
        (code, reply),
        (StatusCode::CONFLICT, json!({ "reason": "room_limit" }))
    );
    let (code, _) = harness
        .call(Method::DELETE, &format!("/rooms/{}", room_id(1)), None)
        .await;
    assert_eq!(code, StatusCode::NO_CONTENT);
    harness.rooms_settle_at(1).await;
    let (code, again) = harness.create(3, "normal").await;
    assert_eq!(code, StatusCode::CREATED);
    assert!(
        again["invitation"]
            .as_str()
            .unwrap()
            .contains(":45800:45801:")
    );
}

#[tokio::test]
async fn one_free_port_is_not_enough_for_a_room() {
    let harness = harness(|config, _| config.port_range = [45800, 45802]);
    assert_eq!(harness.create(1, "normal").await.0, StatusCode::CREATED);
    let (code, reply) = harness.create(2, "normal").await;
    assert_eq!(
        (code, reply),
        (StatusCode::CONFLICT, json!({ "reason": "room_limit" }))
    );
    assert_eq!(harness.supervisor.room_count(), 1);
}

#[tokio::test]
async fn ports_another_process_holds_are_skipped() {
    // 46801 is taken, so the pair is not adjacent.
    let held = std::net::UdpSocket::bind(("0.0.0.0", 46801)).unwrap();
    let harness = harness(|config, tuning| {
        config.port_range = [46800, 46803];
        tuning.probe_ports = true;
    });
    let (code, created) = harness.create(1, "normal").await;
    assert_eq!(code, StatusCode::CREATED);
    assert!(
        created["invitation"]
            .as_str()
            .unwrap()
            .contains(":46800:46802:")
    );
    drop(held);
}

#[tokio::test]
async fn a_host_that_exits_early_fails_the_request_and_frees_its_ports() {
    let harness = harness(|config, _| config.port_range = [45800, 45801]);
    let (code, reply) = harness.create(1, "exit-early").await;
    assert_eq!(
        (code, reply),
        (StatusCode::BAD_GATEWAY, json!({ "reason": "host_failed" }))
    );
    // The room is gone and its ports are free again by the time we hear.
    assert_eq!(harness.supervisor.room_count(), 0);
    let (code, created) = harness.create(2, "normal").await;
    assert_eq!(code, StatusCode::CREATED);
    assert!(
        created["invitation"]
            .as_str()
            .unwrap()
            .contains(":45800:45801:")
    );
}

#[tokio::test]
async fn a_host_binary_that_cannot_start_fails_the_request() {
    let harness = harness(|_, _| {});
    let mut request = body(1, "normal");
    request["build_id"] = json!("missing");
    let (code, reply) = harness.call(Method::POST, "/rooms", Some(request)).await;
    assert_eq!(
        (code, reply),
        (StatusCode::BAD_GATEWAY, json!({ "reason": "host_failed" }))
    );
    assert_eq!(harness.supervisor.room_count(), 0);
}

#[tokio::test]
async fn a_host_that_never_reports_hosted_is_killed() {
    let harness = harness(|_, tuning| tuning.hosted_timeout = Duration::from_millis(400));
    let started = std::time::Instant::now();
    let (code, reply) = harness.create(1, "no-hosted").await;
    assert_eq!(
        (code, reply),
        (StatusCode::BAD_GATEWAY, json!({ "reason": "host_failed" }))
    );
    assert!(started.elapsed() >= Duration::from_millis(400));
    assert_eq!(harness.supervisor.room_count(), 0);
}

#[tokio::test]
async fn protocol_errors_before_hosted_fail_the_request() {
    let harness = harness(|_, _| {});
    for name in ["garbage", "status-first"] {
        let (code, reply) = harness.create(1, name).await;
        assert_eq!(
            (code, reply),
            (StatusCode::BAD_GATEWAY, json!({ "reason": "host_failed" })),
            "{name}"
        );
        assert_eq!(harness.supervisor.room_count(), 0, "{name}");
    }
}

#[tokio::test]
async fn a_protocol_error_after_hosted_kills_the_host() {
    // These children ignore nothing: they would wait on stdin forever, so the
    // room only ends if the supervisor kills them.
    let harness = harness(|_, _| {});
    for name in ["garbage-late", "oversize"] {
        let (code, _) = harness.create(1, name).await;
        assert_eq!(code, StatusCode::CREATED, "{name}");
        harness.rooms_settle_at(0).await;
        assert!(harness.list().await.is_empty());
    }
}

#[tokio::test]
async fn unknown_message_types_are_ignored() {
    let harness = harness(|_, _| {});
    let (code, _) = harness.create(1, "unknown-types").await;
    assert_eq!(code, StatusCode::CREATED);
    assert_eq!(harness.list().await.len(), 1);
}

#[tokio::test]
async fn delete_closes_a_room_and_unknown_rooms_are_404() {
    let harness = harness(|_, _| {});
    harness.create(1, "normal").await;
    let (code, _) = harness
        .call(Method::DELETE, &format!("/rooms/{}", room_id(2)), None)
        .await;
    assert_eq!(code, StatusCode::NOT_FOUND);
    let (code, _) = harness
        .call(Method::DELETE, &format!("/rooms/{}", room_id(1)), None)
        .await;
    assert_eq!(code, StatusCode::NO_CONTENT);
    harness.rooms_settle_at(0).await;
    assert!(harness.list().await.is_empty());
}

#[tokio::test]
async fn a_host_that_ignores_the_close_request_is_killed() {
    let harness = harness(|_, tuning| tuning.kill_grace = Duration::from_millis(300));
    harness.create(1, "ignore-stdin").await;
    let (code, _) = harness
        .call(Method::DELETE, &format!("/rooms/{}", room_id(1)), None)
        .await;
    assert_eq!(code, StatusCode::NO_CONTENT);
    // Gone from the list at once, from the books once the child is reaped.
    assert!(harness.list().await.is_empty());
    harness.rooms_settle_at(0).await;
}

#[tokio::test]
async fn an_empty_room_closes_after_the_grace_period() {
    let harness = harness(|_, tuning| tuning.empty_close = Duration::from_millis(300));
    harness.create(1, "normal").await;
    assert_eq!(harness.list().await.len(), 1);
    harness.rooms_settle_at(0).await;
}

#[tokio::test]
async fn a_room_with_members_is_left_open() {
    let harness = harness(|_, tuning| tuning.empty_close = Duration::from_millis(300));
    harness.create(1, "members:2").await;
    tokio::time::sleep(Duration::from_millis(1200)).await;
    assert_eq!(harness.supervisor.room_count(), 1);
    assert_eq!(harness.list().await.len(), 1);
}

/// The room host says `closed` when its last member leaves on purpose: the
/// room leaves the list at once, long before the empty grace, while a room
/// whose last member dropped stays listed for that grace.
#[tokio::test]
async fn a_room_its_host_closes_leaves_the_list_at_once() {
    let harness = harness(|_, _| {});
    let (code, _) = harness.create(1, "occupied-then-empty").await;
    assert_eq!(code, StatusCode::CREATED);
    let (code, _) = harness.create(2, "last-member-left").await;
    assert_eq!(code, StatusCode::CREATED);
    let mut listed = Vec::new();
    for _ in 0..100 {
        listed = harness.list().await;
        if listed.len() == 1 {
            break;
        }
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    // Out of the list while its host is still closing, then off the books.
    assert_eq!(harness.supervisor.room_count(), 2);
    assert_eq!(listed.len(), 1, "{listed:?}");
    assert_eq!(listed[0]["room_id"], room_id(1));
    harness.rooms_settle_at(1).await;
    tokio::time::sleep(Duration::from_millis(300)).await;
    let listed = harness.list().await;
    assert_eq!((listed.len(), &listed[0]["members"]), (1, &json!(0)));
}

#[tokio::test]
async fn draining_refuses_new_rooms_and_finishes_when_the_rooms_end() {
    let harness = harness(|_, _| {});
    harness.create(1, "normal").await;
    let drain = tokio::spawn({
        let supervisor = harness.supervisor.clone();
        async move { supervisor.drain().await }
    });
    for _ in 0..200 {
        if harness.supervisor.is_draining() {
            break;
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    let (code, reply) = harness.create(2, "normal").await;
    assert_eq!(
        (code, reply),
        (StatusCode::CONFLICT, json!({ "reason": "room_limit" }))
    );
    // The room is still served while it runs.
    assert_eq!(harness.list().await.len(), 1);
    tokio::time::sleep(Duration::from_millis(150)).await;
    assert!(!drain.is_finished(), "the room has not ended yet");
    harness
        .call(Method::DELETE, &format!("/rooms/{}", room_id(1)), None)
        .await;
    tokio::time::timeout(Duration::from_secs(10), drain)
        .await
        .expect("drain should finish once the room ends")
        .unwrap();
    assert_eq!(harness.supervisor.room_count(), 0);
}

#[tokio::test]
async fn a_drain_that_runs_out_of_time_closes_the_rooms() {
    let harness = harness(|_, tuning| {
        tuning.drain = Duration::from_millis(300);
        tuning.kill_grace = Duration::from_millis(300);
    });
    harness.create(1, "normal").await;
    harness.create(2, "ignore-stdin").await;
    harness.create(3, "members:3").await;
    tokio::time::timeout(Duration::from_secs(15), harness.supervisor.drain())
        .await
        .expect("drain should close what is left");
    assert_eq!(harness.supervisor.room_count(), 0);
}

#[tokio::test]
async fn a_drain_with_no_rooms_returns_at_once() {
    let harness = harness(|_, _| {});
    tokio::time::timeout(Duration::from_secs(2), harness.supervisor.drain())
        .await
        .expect("nothing to wait for");
    assert!(harness.supervisor.is_draining());
}
