//! The helper's public-room requests against a real bridge on loopback, with
//! a stand-in room supervisor: listing, creating a room and taking its
//! creator's ticket, joining with a ticket, and the refusals the game acts on.
#![cfg(windows)]

mod public_rooms_support;

use std::time::Duration;

use public_rooms_support::*;
use serde_json::{Value, json};
use tokio::net::TcpListener;

#[tokio::test]
async fn create_list_and_join_a_public_room() {
    let dirs = dirs("flow");
    let (supervisor, url) = fake_supervisor().await;
    let (running, origin) = bridge(&dirs.0, Some(url)).await;
    let (kate_endpoint, joiner_endpoint) = ("a".repeat(64), "b".repeat(64));
    let (kate_dirs, joiner_dirs) = (Dirs(dirs.0.join("kate")), Dirs(dirs.0.join("joiner")));
    let kate_region = Region::default();
    let mut kate = Worker::start_in(&kate_dirs, &kate_endpoint, kate_region.clone());
    let mut joiner = Worker::start(&joiner_dirs, &joiner_endpoint);
    let (kate_id, bridge_id) = kate.join(&origin).await;
    let (joiner_id, _) = joiner.join(&origin).await;

    // No room is open.
    assert!(listed(&ok(kate.list(&bridge_id, BUILD).await)).is_empty());

    // Creating answers the creator's admission, whose ticket verifies and
    // names this helper's endpoint and Ember ID. The room is in the creator's
    // region, not the room host's, and the helper reads that region when it
    // creates: it was unknown when the helper started.
    *kate_region.lock().unwrap() = Some("euc1");
    let created = ok(kate.create(&bridge_id, "Friendly matches", 2).await);
    assert_admits(&origin, &bridge_id, &created, &kate_endpoint, &kate_id).await;
    let room_id = created["room"]["room_id"].as_str().unwrap().to_owned();
    assert_eq!(created["room"]["name"], "Friendly matches");
    assert_eq!(created["room"]["capacity"], 2);
    assert_eq!(created["room"]["build_id"], BUILD);
    assert_eq!(created["room"]["region"], "euc1");
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
    assert_eq!(rooms[0]["region"], "euc1");
    // Only the build asked for, written as a query value.
    assert!(listed(&ok(joiner.list(&bridge_id, "build 2&x=y").await)).is_empty());

    // The helper asks for details: what the room host reported comes back
    // checked and whole, with the time the bridge listed the rooms.
    set_details(
        &supervisor,
        &room_id,
        json!({ "name": "Friendly matches", "capacity": 2, "locked": true, "host_name": "Kate",
            "fighters": [3], "set_format": 3, "rotation": 1 }),
    );
    ember_bridge::poll_rooms(&running.state).await;
    let data = ok(joiner.list(&bridge_id, BUILD).await);
    assert!(data["listed_at"].as_u64().is_some(), "{data}");
    let room = &listed(&data)[0];
    assert_eq!(room["host_name"], "Kate");
    assert_eq!(room["fighters"], json!([3]));
    assert_eq!(room["locked"], true);
    assert_eq!(
        (&room["set_format"], &room["rotation"]),
        (&json!(3), &json!(1))
    );
    assert_eq!(
        refused(joiner.ticket(&bridge_id, &room_id, BUILD).await),
        "room_locked"
    );
    set_details(
        &supervisor,
        &room_id,
        json!({ "name": "Friendly matches", "capacity": 2, "locked": false, "host_name": "Kate",
            "fighters": [3], "set_format": 3, "rotation": 1 }),
    );
    ember_bridge::poll_rooms(&running.state).await;

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
    // One open room per Ember ID, said as the player's own room.
    assert_eq!(
        refused(kate.create(&bridge_id, "Second room", 4).await),
        "room_limit_own_room"
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
    // A creator of no known region leaves the room in its host's.
    assert_eq!(created["room"]["region"], "use1");
    assert_eq!(supervisor.lock().unwrap().created.len(), 1);
    worker.task.abort();
    running.abort();
}

/// A room for each player's own input delay, from the helper's request to the
/// supervisor's body: the field goes on only for that room, comes back in its
/// admission and in the listing, and a room with the shared delay never has it.
#[tokio::test]
async fn a_rooms_own_delay_goes_from_the_helper_through_the_bridge_to_the_supervisor() {
    let dirs = dirs("own-delay");
    let (supervisor, url) = fake_supervisor().await;
    let (running, origin) = bridge(&dirs.0, Some(url)).await;
    let (kate_endpoint, sam_endpoint) = ("a".repeat(64), "b".repeat(64));
    let (kate_dirs, sam_dirs) = (Dirs(dirs.0.join("kate")), Dirs(dirs.0.join("sam")));
    let mut kate = Worker::start(&kate_dirs, &kate_endpoint);
    let mut sam = Worker::start(&sam_dirs, &sam_endpoint);
    let (kate_id, bridge_id) = kate.join(&origin).await;
    let (sam_id, _) = sam.join(&origin).await;

    let own = ok(kate.create_own(&bridge_id, "Own delay", 4).await);
    assert_admits(&origin, &bridge_id, &own, &kate_endpoint, &kate_id).await;
    assert_eq!(own["room"]["own_delay"], true);
    let shared = ok(sam.create(&bridge_id, "Shared delay", 4).await);
    assert_admits(&origin, &bridge_id, &shared, &sam_endpoint, &sam_id).await;
    assert!(shared["room"].get("own_delay").is_none(), "{shared}");

    let sent = supervisor.lock().unwrap().created.clone();
    assert_eq!(sent.len(), 2);
    assert_eq!(sent[0]["own_delay"], true);
    assert!(sent[1].get("own_delay").is_none(), "{}", sent[1]);

    let (own_id, shared_id) = (
        own["room"]["room_id"].as_str().unwrap().to_owned(),
        shared["room"]["room_id"].as_str().unwrap().to_owned(),
    );
    report(&supervisor, &own_id, 1, &[]);
    report(&supervisor, &shared_id, 1, &[]);
    ember_bridge::poll_rooms(&running.state).await;
    let rooms = listed(&ok(sam.list(&bridge_id, BUILD).await));
    let find = |id: &str| rooms.iter().find(|room| room["room_id"] == id).unwrap();
    assert_eq!(find(&own_id)["own_delay"], true);
    assert!(find(&shared_id).get("own_delay").is_none());
    // Another player's ticket for the room says so too.
    let admitted = ok(sam.ticket(&bridge_id, &own_id, BUILD).await);
    assert_eq!(admitted["room"]["own_delay"], true);
    kate.task.abort();
    sam.task.abort();
    running.abort();
}

/// Kills the supervisor process the chain test starts, whatever the test does.
struct SupervisorProcess(std::process::Child);

impl Drop for SupervisorProcess {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

/// The whole create path with the real parts: the helper's request, the real
/// bridge, the real `ember-rooms` supervisor process, and the real
/// `sf4e-room-host` it starts. A room for each player's own delay is hosted by
/// a room host whose own snapshot says so: its status line reports `own_delay`,
/// the supervisor passes it on and the bridge takes it as the room's word. A
/// room with the shared delay says nothing anywhere. Needs the public Iroh
/// relays, so it is ignored unless asked for, with the binaries named by
/// `EMBER_ROOMS_EXE` (`cargo build -p ember-rooms` in server/ember-rooms),
/// `SF4E_ROOM_HOST_EXE` and `SF4E_HELPER_EXE`: `cargo test --test
/// public_rooms_bridge -- --ignored a_rooms_own_delay_reaches`
#[tokio::test]
#[ignore = "needs the ember-rooms, sf4e-room-host and sf4-net binaries and the public Iroh relays"]
async fn a_rooms_own_delay_reaches_the_real_room_host_through_the_real_supervisor() {
    let path = |name: &str| std::env::var(name).unwrap_or_else(|_| panic!("{name} is not set"));
    let (rooms_exe, host_exe, helper_exe) = (
        path("EMBER_ROOMS_EXE"),
        path("SF4E_ROOM_HOST_EXE"),
        path("SF4E_HELPER_EXE"),
    );
    let dirs = dirs("chain");
    // The supervisor wants at least 32 bytes of secret; the bridge is given the same.
    let secret = "chain-test-secret-0123456789abcdef";
    std::fs::write(dirs.0.join("supervisor.secret"), secret).unwrap();
    let port = {
        let probe = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        probe.local_addr().unwrap().port()
    };
    let config = json!({
        "bind": format!("127.0.0.1:{port}"),
        "secret_file": dirs.0.join("supervisor.secret"),
        "max_rooms": 4, "port_range": [45830, 45839], "empty_close_secs": 120, "drain_secs": 5,
        "builds": { BUILD: { "room_host": host_exe, "helper": helper_exe } },
    });
    std::fs::write(dirs.0.join("rooms.json"), config.to_string()).unwrap();
    let _supervisor = SupervisorProcess(
        std::process::Command::new(&rooms_exe)
            .arg(dirs.0.join("rooms.json"))
            .spawn()
            .expect("the supervisor starts"),
    );
    // The bridge installs this when it starts; this client is built before it.
    let _ = rustls::crypto::ring::default_provider().install_default();
    let http = reqwest::Client::new();
    let base = format!("http://127.0.0.1:{port}");
    let mut up = false;
    for _ in 0..100 {
        if http
            .get(format!("{base}/health"))
            .send()
            .await
            .is_ok_and(|reply| reply.status().is_success())
        {
            up = true;
            break;
        }
        tokio::time::sleep(Duration::from_millis(100)).await;
    }
    assert!(up, "the supervisor answers");

    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let origin = format!("http://{}", listener.local_addr().unwrap());
    let running = bridge_at(
        &dirs.0,
        Some(base.clone()),
        listener,
        origin.clone(),
        secret,
    )
    .await;
    let (kate_endpoint, sam_endpoint) = ("a".repeat(64), "b".repeat(64));
    let (kate_dirs, sam_dirs) = (Dirs(dirs.0.join("kate")), Dirs(dirs.0.join("sam")));
    let mut kate = Worker::start(&kate_dirs, &kate_endpoint);
    let mut sam = Worker::start(&sam_dirs, &sam_endpoint);
    let (_, bridge_id) = kate.join(&origin).await;
    sam.join(&origin).await;

    // The public create: the helper asks the bridge, which asks the supervisor,
    // which starts a room host and waits for it to be hosted.
    let own = ok(kate.create_own(&bridge_id, "Own delay", 4).await);
    assert_eq!(own["room"]["own_delay"], true);
    let shared = ok(sam.create(&bridge_id, "Shared delay", 4).await);
    assert!(shared["room"].get("own_delay").is_none(), "{shared}");
    let (own_id, shared_id) = (
        own["room"]["room_id"].as_str().unwrap().to_owned(),
        shared["room"]["room_id"].as_str().unwrap().to_owned(),
    );

    // What each real room host says about its room, as the supervisor lists it.
    let mut reported = Value::Null;
    for _ in 0..100 {
        let body = http
            .get(format!("{base}/rooms"))
            .bearer_auth(secret)
            .send()
            .await
            .unwrap()
            .bytes()
            .await
            .unwrap();
        let list: Value = serde_json::from_slice(&body).unwrap();
        let has = |id: &str| {
            list.as_array()
                .unwrap()
                .iter()
                .any(|room| room["room_id"] == id && room["details"].is_object())
        };
        if has(&own_id) && has(&shared_id) {
            reported = list;
            break;
        }
        tokio::time::sleep(Duration::from_millis(200)).await;
    }
    assert!(!reported.is_null(), "both room hosts reported their rooms");
    let details = |id: &str| {
        reported
            .as_array()
            .unwrap()
            .iter()
            .find(|room| room["room_id"] == id)
            .unwrap()["details"]
            .clone()
    };
    assert_eq!(details(&own_id)["own_delay"], true, "{reported}");
    assert!(details(&shared_id).get("own_delay").is_none(), "{reported}");

    // The bridge takes the host's report as the room's word: the row says what
    // the host's own snapshot said.
    ember_bridge::poll_rooms(&running.state).await;
    let stored = |id: String| {
        let state = running.state.clone();
        async move {
            state
                .db
                .read(move |tx| {
                    Ok(tx.query_row(
                        "SELECT own_delay FROM rooms WHERE room_id = ?1",
                        [id],
                        |row| row.get::<_, i64>(0),
                    )?)
                })
                .await
                .unwrap()
        }
    };
    assert_eq!(stored(own_id.clone()).await, 1);
    assert_eq!(stored(shared_id.clone()).await, 0);
    for id in [&own_id, &shared_id] {
        let _ = http
            .delete(format!("{base}/rooms/{id}"))
            .bearer_auth(secret)
            .send()
            .await;
    }
    kate.task.abort();
    sam.task.abort();
    running.abort();
}

/// A bridge that predates the option refuses the create. The player is told so,
/// the room is not made with the shared delay instead, and the same bridge
/// still makes the room with the shared delay.
#[tokio::test]
async fn a_create_for_each_players_own_delay_fails_visibly_on_an_older_bridge() {
    let dirs = dirs("older-bridge");
    let (supervisor, url) = fake_supervisor().await;
    let front = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let origin = format!("http://{}", front.local_addr().unwrap());
    let behind = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let upstream = behind.local_addr().unwrap().to_string();
    let running = bridge_at(&dirs.0, Some(url), behind, origin.clone(), SECRET).await;
    tokio::spawn(older_bridge_in_front(front, upstream));
    let mut worker = Worker::start(&dirs, &"a".repeat(64));
    let (_, bridge_id) = worker.join(&origin).await;

    assert_eq!(
        refused(worker.create_own(&bridge_id, "Own delay", 4).await),
        "own_delay_unsupported"
    );
    assert!(supervisor.lock().unwrap().created.is_empty());
    // Not retried as a room with the shared delay, and the shared delay still works.
    let created = ok(worker.create(&bridge_id, "Shared delay", 4).await);
    assert!(created["room"].get("own_delay").is_none());
    let sent = supervisor.lock().unwrap().created.clone();
    assert_eq!(sent.len(), 1);
    assert!(sent[0].get("own_delay").is_none());
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
