//! The helper's tournament worker against a real bridge on loopback: enable
//! an identity, approve the bridge, link through a signed claim, unlink, and
//! keep the same Ember ID across a worker restart.
#![cfg(windows)]

mod tournament_support;

use serde_json::{Value, json};
use sf4_net::{
    service::Command,
    tournament::{self, Request},
};
use tournament_support::*;

/// Sign-in sites are only what the bridge lists: the helper reads, cancels
/// and disconnects an account on each by its ID, and opens a sign-in page
/// only on the https origin and path the bridge declares for the site. These
/// sites declare a loopback http page, so Connect is refused before any
/// browser opens. A site the bridge does not list is not found.
#[tokio::test]
async fn sign_in_sites_through_the_helper() {
    let dirs = dirs("sites");
    let pages = "http://127.0.0.1:9";
    let sites = vec![
        site("challonge", "challonge", "Challonge", pages),
        site("startgg", "startgg", "start.gg", pages),
        site("challonge", "brackets-plus", "Brackets Plus", pages),
    ];
    let (running, origin) = bridge(&dirs.0, sites).await;
    let mut worker = Worker::start(&dirs);
    worker.next_answer(0).await;
    let (ok, reason, _, _) = worker.ask(json!({ "op": "identity_enable" })).await;
    assert!(ok, "{reason:?}");
    let (ok, _, _, inspected) = worker
        .ask(json!({ "op": "bridge_inspect", "origin": origin }))
        .await;
    assert!(ok);
    let listed = &inspected["capabilities"]["sign_in_sites"];
    assert_eq!(listed[1]["id"], "startgg");
    assert_eq!(listed[1]["display_name"], "start.gg");
    assert_eq!(listed[1]["authorize_origin"], pages);
    let bridge_id = inspected["profile"]["bridge_id"]
        .as_str()
        .unwrap()
        .to_owned();
    let (ok, reason, _, _) = worker
        .ask(json!({ "op": "bridge_approve", "origin": origin, "bridge_id": bridge_id }))
        .await;
    assert!(ok, "{reason:?}");

    for site in ["challonge", "startgg", "brackets-plus"] {
        let ask = |op: &str| json!({ "op": op, "bridge_id": bridge_id, "site": site });
        let (ok, reason, _, connection) = worker.ask(ask("site_status")).await;
        assert!(ok, "{site}: {reason:?}");
        assert_eq!(connection, json!({ "account": null }), "{site}");
        for op in ["site_cancel", "site_remove"] {
            let (ok, reason, _, _) = worker.ask(ask(op)).await;
            assert!(ok, "{site} {op}: {reason:?}");
        }
        // The bridge started a sign-in, but its page is not on an https
        // origin, so the helper ended it again; the game's own cancel after
        // that finds nothing in flight and still succeeds.
        let (ok, reason, _, _) = worker.ask(ask("site_connect")).await;
        assert!(!ok, "{site}");
        assert_eq!(reason.as_deref(), Some("bridge_invalid_response"), "{site}");
        let (ok, reason, _, _) = worker.ask(ask("site_cancel")).await;
        assert!(ok, "{site}: {reason:?}");
    }
    for op in ["site_status", "site_connect"] {
        let (ok, reason, _, _) = worker
            .ask(json!({ "op": op, "bridge_id": bridge_id, "site": "nowhere" }))
            .await;
        assert!(!ok, "{op}");
        assert_eq!(reason.as_deref(), Some("not_found"), "{op}");
    }
    // A site ID the bridge could never have is refused before any request.
    let (ok, reason, _, _) = worker
        .ask(json!({ "op": "site_status", "bridge_id": bridge_id, "site": "../discord" }))
        .await;
    assert!(!ok);
    assert_eq!(reason.as_deref(), Some("invalid_request"));
    running.abort();
}

/// Sign-in sites this build cannot read leave the bridge usable: the helper
/// drops those entries and keeps the rest, so inspecting and approving the
/// bridge, Discord sign-in and the sites it can read go on as before. A site
/// sign-in the bridge started but whose page is refused is ended at once;
/// Discord's refused page is left as it always was.
#[tokio::test]
async fn sites_this_build_cannot_read_leave_the_bridge_usable() {
    let dirs = dirs("mixed");
    let public = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let origin = format!("http://{}", public.local_addr().unwrap());
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let inner = format!("http://{}", listener.local_addr().unwrap());
    let pages = "http://127.0.0.1:9";
    let sites = vec![site("challonge", "challonge", "Challonge", pages)];
    let running = start_bridge(&dirs.0, sites, true, &origin, listener).await;
    let front = front(public, inner, |document| {
        let listed = document["sign_in_sites"].as_array_mut().unwrap();
        let mut later = listed[0].clone();
        later["id"] = json!("later");
        later["lookup_by"] = json!("email");
        later["weight"] = json!(0.5);
        let mut retyped = listed[0].clone();
        retyped["id"] = json!("retyped");
        retyped["sign_in"] = json!("yes");
        let mut twin = listed[0].clone();
        twin["display_name"] = json!("Second Challonge");
        let mut paused = listed[0].clone();
        paused["id"] = json!("paused");
        paused["sign_in"] = json!(false);
        listed.extend([
            later,
            paused,
            json!({ "id": "missing", "display_name": "Missing" }),
            retyped,
            twin,
            json!("startgg"),
            json!(7),
        ]);
    });
    let mut worker = Worker::start(&dirs);
    worker.next_answer(0).await;
    let (ok, reason, _, _) = worker.ask(json!({ "op": "identity_enable" })).await;
    assert!(ok, "{reason:?}");
    let (ok, reason, _, inspected) = worker
        .ask(json!({ "op": "bridge_inspect", "origin": origin }))
        .await;
    assert!(ok, "{reason:?}");
    let listed = inspected["capabilities"]["sign_in_sites"]
        .as_array()
        .unwrap();
    let ids: Vec<&str> = listed
        .iter()
        .filter_map(|site| site["id"].as_str())
        .collect();
    assert_eq!(ids, ["challonge", "later", "paused"]);
    assert_eq!(listed[0]["display_name"], "Challonge");
    let features = inspected["capabilities"]["features"].as_array().unwrap();
    assert!(features.contains(&json!("discord")), "{features:?}");
    let bridge_id = inspected["profile"]["bridge_id"]
        .as_str()
        .unwrap()
        .to_owned();
    let (ok, reason, _, _) = worker
        .ask(json!({ "op": "bridge_approve", "origin": origin, "bridge_id": bridge_id }))
        .await;
    assert!(ok, "{reason:?}");

    let discord = |op: &str| json!({ "op": op, "bridge_id": bridge_id });
    let (ok, reason, _, connection) = worker.ask(discord("discord_status")).await;
    assert!(ok, "{reason:?}");
    assert_eq!(connection, json!({ "account": null }));
    // The bridge starts Discord's sign-in; its page here is not discord.com,
    // so nothing opens.
    let (ok, reason, _, _) = worker.ask(discord("discord_connect")).await;
    assert!(!ok);
    assert_eq!(reason.as_deref(), Some("bridge_invalid_response"));
    let (ok, reason, _, _) = worker.ask(discord("discord_cancel")).await;
    assert!(ok, "{reason:?}");

    let ask = |op: &str, site: &str| json!({ "op": op, "bridge_id": bridge_id, "site": site });
    let (ok, reason, _, connection) = worker.ask(ask("site_status", "challonge")).await;
    assert!(ok, "{reason:?}");
    assert_eq!(connection, json!({ "account": null }));
    // Pressed twice: each press is its own proven start.
    for _ in 0..2 {
        let (ok, reason, _, _) = worker.ask(ask("site_connect", "challonge")).await;
        assert!(!ok);
        assert_eq!(reason.as_deref(), Some("bridge_invalid_response"));
    }
    for dropped in ["missing", "retyped"] {
        let (ok, reason, _, _) = worker.ask(ask("site_connect", dropped)).await;
        assert!(!ok, "{dropped}");
        assert_eq!(reason.as_deref(), Some("not_found"), "{dropped}");
    }
    // A site listed with its sign-in off is refused before the bridge is asked.
    let (ok, reason, _, _) = worker.ask(ask("site_connect", "paused")).await;
    assert!(!ok);
    assert_eq!(reason.as_deref(), Some("sign_in_off"));
    // A trusted bridge whose capabilities are down for the moment is
    // unavailable, not no tournament service.
    let down = |set: bool| front.down.store(set, std::sync::atomic::Ordering::SeqCst);
    down(true);
    let (ok, reason, _, _) = worker.ask(ask("site_connect", "challonge")).await;
    assert!(!ok);
    assert_eq!(reason.as_deref(), Some("service_unavailable"));
    down(false);

    // Only the test cancelled Discord's sign-in; the helper itself ended
    // Challonge's, which the test never cancelled. Every proven request
    // follows a challenge of its own: a proof is never sent twice, so a start
    // the bridge refused (which uses its proof up) is never sent again as is.
    let seen = front.seen.lock().unwrap().clone();
    let count = |request: &str| seen.iter().filter(|seen| *seen == request).count();
    let discord_start = ember_protocol::discord::START_PATH;
    assert_eq!(count(&format!("POST {discord_start}")), 1, "{seen:?}");
    assert_eq!(count(&format!("DELETE {discord_start}")), 1, "{seen:?}");
    assert_eq!(count("POST /v1/sites/challonge/start"), 2, "{seen:?}");
    assert_eq!(count("DELETE /v1/sites/challonge/start"), 2, "{seen:?}");
    for (at, request) in seen.iter().enumerate() {
        if request.ends_with("/start") {
            assert_eq!(seen[at - 1], "POST /v1/auth/challenges", "{seen:?}");
        }
    }
    for unasked in ["missing", "retyped", "paused"] {
        assert!(
            !seen
                .iter()
                .any(|seen| seen.contains(&format!("/v1/sites/{unasked}"))),
            "{seen:?}"
        );
    }
    running.abort();
}

async fn provider_call(origin: &str, token: &str, path: &str, body: Value) -> Value {
    let response = reqwest::Client::new()
        .post(format!("{origin}{path}"))
        .bearer_auth(token)
        .header("content-type", "application/json")
        .header(
            "idempotency-key",
            ember_protocol::encoding::b64u(&random16()),
        )
        .body(serde_json::to_vec(&body).unwrap())
        .send()
        .await
        .unwrap();
    let status = response.status();
    let text = response.text().await.unwrap();
    assert!(status.is_success(), "{path}: {status} {text}");
    serde_json::from_str(&text).unwrap()
}

#[tokio::test]
async fn link_an_identity_through_the_helper() {
    let dirs = dirs("e2e");
    let (running, origin) = bridge(&dirs.0, Vec::new()).await;
    let provider = ember_bridge::issue_credential(&running.state, Some("mock-local"), None, "e2e")
        .await
        .unwrap();

    let mut worker = Worker::start(&dirs);
    // The startup status arrives unasked, and nothing exists yet.
    let (_, _, identity, _) = worker.next_answer(0).await;
    assert_eq!(identity["state"], "disabled");
    // Bridge work needs an identity first.
    let (ok, reason, _, _) = worker
        .ask(json!({ "op": "link_list", "bridge_id": "brg_00000000-0000-4000-8000-000000000000" }))
        .await;
    assert!(!ok);
    assert_eq!(reason.as_deref(), Some("bridge_not_approved"));

    let (ok, reason, identity, _) = worker.ask(json!({ "op": "identity_enable" })).await;
    assert!(ok, "{reason:?}");
    assert_eq!(identity["state"], "ready");
    let ember_id = identity["ember_id"].as_str().unwrap().to_owned();

    // A backup goes into a folder the game names but has not made.
    let backup = dirs
        .0
        .join("settings")
        .join("identity-backups")
        .join("ember.backup");
    let backup = backup.to_str().unwrap();
    let (ok, reason, _, written) = worker
        .ask(json!({ "op": "identity_export", "path": backup, "passphrase": "backup words" }))
        .await;
    assert!(ok, "{reason:?}");
    assert_eq!(written["path"], backup);
    let (ok, _, _, preview) = worker
        .ask(json!({ "op": "identity_preview_import", "path": backup, "passphrase": "backup words" }))
        .await;
    assert!(ok);
    assert_eq!(preview["ember_id"], ember_id.as_str());
    assert_eq!(preview["same_identity"], true);

    let (ok, _, _, inspected) = worker
        .ask(json!({ "op": "bridge_inspect", "origin": origin }))
        .await;
    assert!(ok);
    let bridge_id = inspected["profile"]["bridge_id"]
        .as_str()
        .unwrap()
        .to_owned();
    assert_eq!(
        inspected["capabilities"]["connections"][0]["id"],
        "mock-local"
    );
    // Approval names the bridge ID the user saw; a different one is refused.
    let (ok, reason, _, _) = worker
        .ask(json!({ "op": "bridge_approve", "origin": origin, "bridge_id": "brg_00000000-0000-4000-8000-000000000000" }))
        .await;
    assert!(!ok);
    assert_eq!(reason.as_deref(), Some("bridge_changed"));
    let (ok, _, _, _) = worker
        .ask(json!({ "op": "bridge_approve", "origin": origin, "bridge_id": bridge_id }))
        .await;
    assert!(ok);

    let intent = provider_call(
        &origin,
        &provider,
        "/v1/link-intents",
        json!({ "subject": "kate", "display_label": "Kate" }),
    )
    .await;
    let (ok, reason, _, claim) = worker
        .ask(json!({ "op": "link_claim", "bridge_id": bridge_id, "connection_id": "mock-local", "code": intent["code"] }))
        .await;
    assert!(ok, "{reason:?}");
    assert_eq!(claim["state"], "pending");
    assert_eq!(claim["ember_id"], ember_id.as_str());
    // A wrong code comes back as the bridge's stable code.
    let (ok, reason, _, _) = worker
        .ask(json!({ "op": "link_claim", "bridge_id": bridge_id, "connection_id": "mock-local", "code": "00000-00000" }))
        .await;
    assert!(!ok);
    assert_eq!(reason.as_deref(), Some("link_expired"));

    provider_call(
        &origin,
        &provider,
        &format!(
            "/v1/link-intents/{}/approve",
            intent["intent_id"].as_str().unwrap()
        ),
        json!({ "claim_id": claim["claim_id"], "ember_id": ember_id, "subject": "kate" }),
    )
    .await;
    let (ok, _, _, links) = worker
        .ask(json!({ "op": "link_list", "bridge_id": bridge_id }))
        .await;
    assert!(ok);
    let link_id = links["links"][0]["link_id"].as_str().unwrap().to_owned();
    let (ok, reason, _, _) = worker
        .ask(json!({ "op": "link_remove", "bridge_id": bridge_id, "link_id": link_id }))
        .await;
    assert!(ok, "{reason:?}");
    let (_, _, _, links) = worker
        .ask(json!({ "op": "link_list", "bridge_id": bridge_id }))
        .await;
    assert_eq!(links["links"], json!([]));
    // Cancel in Connect Discord ends the player's sign-ins on the bridge with
    // its own proof; a bridge that offers no sign-in takes it too.
    let (ok, reason, _, _) = worker
        .ask(json!({ "op": "discord_cancel", "bridge_id": bridge_id }))
        .await;
    assert!(ok, "{reason:?}");

    // A new worker on the same stores has the same identity and bridge.
    worker.task.abort();
    let mut restarted = Worker::start(&dirs);
    let (_, _, identity, _) = restarted.next_answer(0).await;
    assert_eq!(identity["state"], "ready");
    assert_eq!(identity["ember_id"], ember_id.as_str());
    let (_, _, _, bridges) = restarted.ask(json!({ "op": "bridge_list" })).await;
    assert_eq!(bridges["bridges"][0]["bridge_id"], bridge_id.as_str());
    let (ok, _, _, _) = restarted
        .ask(json!({ "op": "link_list", "bridge_id": bridge_id }))
        .await;
    assert!(ok);

    // Importing a different identity drops the session the old one opened:
    // the next claim is signed and sent as the new identity.
    let other_dirs = Dirs(dirs.0.join("other"));
    let mut other = Worker::start(&other_dirs);
    other.next_answer(0).await;
    let (_, _, other_identity, _) = other.ask(json!({ "op": "identity_enable" })).await;
    let other_id = other_identity["ember_id"].as_str().unwrap().to_owned();
    let other_backup = other_dirs.0.join("other.backup");
    let other_backup = other_backup.to_str().unwrap();
    let (ok, _, _, _) = other
        .ask(json!({ "op": "identity_export", "path": other_backup, "passphrase": "other words" }))
        .await;
    assert!(ok);
    other.task.abort();
    let (ok, reason, identity, _) = restarted
        .ask(
            json!({ "op": "identity_import", "path": other_backup, "passphrase": "other words",
            "expected_ember_id": other_id, "replace": true }),
        )
        .await;
    assert!(ok, "{reason:?}");
    assert_eq!(identity["ember_id"], other_id.as_str());
    let intent = provider_call(
        &origin,
        &provider,
        "/v1/link-intents",
        json!({ "subject": "kate-two", "display_label": "Kate" }),
    )
    .await;
    let (ok, reason, _, claim) = restarted
        .ask(json!({ "op": "link_claim", "bridge_id": bridge_id, "connection_id": "mock-local", "code": intent["code"] }))
        .await;
    assert!(ok, "{reason:?}");
    assert_eq!(claim["ember_id"], other_id.as_str());

    // Unknown fields and passphrase-free unlocks are refused at the IPC edge.
    let bad: Result<Command, _> = serde_json::from_value(
        json!({ "type": "tournament", "request": { "op": "identity_status", "extra": 1 } }),
    );
    assert!(bad.is_err());
    running.abort();
}

/// A GET of `path` on the bridge, as JSON.
async fn bridge_document(origin: &str, path: &str) -> Value {
    let text = reqwest::get(format!("{origin}{path}"))
        .await
        .unwrap()
        .text()
        .await
        .unwrap();
    serde_json::from_str(&text).unwrap()
}

// A bridge the build pins is trusted with no approval while its origin
// serves the pinned ID. Forgotten, it is not trusted until the player
// approves it again by its address. Pinned at an origin that serves another
// bridge, it is not trusted at all, and is sent nothing past the check.
#[tokio::test]
async fn a_pinned_bridge_is_trusted_without_an_approval() {
    let dirs = dirs("pinned");
    let (running, origin) = bridge(&dirs.0, Vec::new()).await;
    let profile = bridge_document(&origin, "/.well-known/ember-bridge.json").await;
    let bridge_id = profile["bridge_id"].as_str().unwrap().to_owned();
    let keys = bridge_document(&origin, "/v1/signing-keys").await;
    let (kid, x) = (
        keys["keys"][0]["kid"].as_str().unwrap(),
        keys["keys"][0]["x"].as_str().unwrap(),
    );
    let pin = |bridge_id: &str, origin: &str| tournament::Builtin {
        bridge_id: bridge_id.into(),
        origin: origin.into(),
        display_name: "Pinned bridge".into(),
        keys: vec![(kid.into(), x.into())],
    };
    let mut worker = Worker::start_pinning(&dirs, "a".repeat(64), vec![pin(&bridge_id, &origin)]);
    worker.next_answer(0).await;
    let (ok, _, _, _) = worker.ask(json!({ "op": "identity_enable" })).await;
    assert!(ok);
    let (_, _, _, listed) = worker.ask(json!({ "op": "bridge_list" })).await;
    assert_eq!(listed["bridges"][0]["bridge_id"], bridge_id.as_str());
    assert_eq!(listed["bridges"][0]["builtin"], true);
    assert!(listed["bridges"][0].get("active").is_none());
    let links = json!({ "op": "link_list", "bridge_id": bridge_id });
    let (ok, reason, _, _) = worker.ask(links.clone()).await;
    assert!(ok, "{reason:?}");
    // Its first session makes it one of the player's services.
    let (_, _, _, listed) = worker.ask(json!({ "op": "bridge_list" })).await;
    assert_eq!(listed["bridges"][0]["active"], true);

    let (ok, _, _, _) = worker
        .ask(json!({ "op": "bridge_forget", "bridge_id": bridge_id }))
        .await;
    assert!(ok);
    let (ok, reason, _, _) = worker.ask(links.clone()).await;
    assert!(!ok);
    assert_eq!(reason.as_deref(), Some("bridge_not_approved"));
    let (ok, reason, _, _) = worker
        .ask(json!({ "op": "bridge_approve", "origin": origin, "bridge_id": bridge_id }))
        .await;
    assert!(ok, "{reason:?}");
    let (_, _, _, listed) = worker.ask(json!({ "op": "bridge_list" })).await;
    assert_eq!(listed["bridges"][0]["builtin"], true);
    assert_eq!(listed["removed"], json!([]));
    let (ok, reason, _, _) = worker.ask(links.clone()).await;
    assert!(ok, "{reason:?}");
    worker.task.abort();

    // Pinned at an origin that now serves another bridge: asked twice, it
    // is checked twice and sent no challenge or session request.
    let other = Dirs(dirs.0.join("other"));
    let (foreign, paths) = foreign_origin().await;
    let mut worker = Worker::start_pinning(&other, "a".repeat(64), vec![pin(&bridge_id, &foreign)]);
    worker.next_answer(0).await;
    let (ok, _, _, _) = worker.ask(json!({ "op": "identity_enable" })).await;
    assert!(ok);
    for _ in 0..2 {
        let (ok, reason, _, _) = worker.ask(links.clone()).await;
        assert!(!ok);
        assert_eq!(reason.as_deref(), Some("pinned_bridge_mismatch"));
    }
    assert_eq!(
        *paths.lock().unwrap(),
        vec!["/.well-known/ember-bridge.json"; 2]
    );
    worker.task.abort();
    running.abort();
}

#[test]
fn requests_carry_no_debug_output_of_secrets() {
    // `Request` has no Debug derive; this only needs to compile without one.
    fn takes(_: &Request) {}
    let request: Request =
        serde_json::from_value(json!({ "op": "identity_unlock", "passphrase": "secret words" }))
            .unwrap();
    takes(&request);
}

/// One worker with an enabled identity, the bridge approved, and its
/// identity linked to `subject` on the mock connection.
async fn linked_worker(
    dirs: &Dirs,
    endpoint: &str,
    origin: &str,
    provider: &str,
    subject: &str,
) -> (Worker, String, String, Value) {
    let mut worker = Worker::start_as(dirs, endpoint.to_owned());
    worker.next_answer(0).await;
    let (_, _, identity, _) = worker.ask(json!({ "op": "identity_enable" })).await;
    let ember_id = identity["ember_id"].as_str().unwrap().to_owned();
    let (_, _, _, inspected) = worker
        .ask(json!({ "op": "bridge_inspect", "origin": origin }))
        .await;
    let bridge_id = inspected["profile"]["bridge_id"]
        .as_str()
        .unwrap()
        .to_owned();
    let (ok, reason, _, _) = worker
        .ask(json!({ "op": "bridge_approve", "origin": origin, "bridge_id": bridge_id }))
        .await;
    assert!(ok, "{reason:?}");
    let intent = provider_call(
        origin,
        provider,
        "/v1/link-intents",
        json!({ "subject": subject }),
    )
    .await;
    let (ok, reason, _, claim) = worker
        .ask(json!({ "op": "link_claim", "bridge_id": bridge_id, "connection_id": "mock-local", "code": intent["code"] }))
        .await;
    assert!(ok, "{reason:?}");
    let link = provider_call(
        origin,
        provider,
        &format!(
            "/v1/link-intents/{}/approve",
            intent["intent_id"].as_str().unwrap()
        ),
        json!({ "claim_id": claim["claim_id"], "ember_id": ember_id, "subject": subject }),
    )
    .await;
    (worker, ember_id, bridge_id, link)
}

#[tokio::test]
async fn play_a_set_through_two_helpers() {
    let dirs = dirs("play");
    let (running, origin) = bridge(&dirs.0, Vec::new()).await;
    let provider = ember_bridge::issue_credential(&running.state, Some("mock-local"), None, "play")
        .await
        .unwrap();
    let (host_endpoint, guest_endpoint) = ("a".repeat(64), "b".repeat(64));
    let host_dirs = Dirs(dirs.0.join("host"));
    let guest_dirs = Dirs(dirs.0.join("guest"));
    let (mut host, host_id, bridge_id, host_link) =
        linked_worker(&host_dirs, &host_endpoint, &origin, &provider, "host").await;
    let (mut guest, guest_id, _, guest_link) =
        linked_worker(&guest_dirs, &guest_endpoint, &origin, &provider, "guest").await;
    let created = provider_call(
        &origin,
        &provider,
        "/v1/matches",
        json!({
            "external_match_id": "helper-set",
            "game": "usf4",
            "participants": [
                { "participant_id": host_link["participant_id"], "ember_id": host_id, "slot": 0 },
                { "participant_id": guest_link["participant_id"], "ember_id": guest_id, "slot": 1 },
            ],
            "rules": {
                "games_to_win": 2, "draw_policy": "replay_no_score", "native_rules_profile": "ember-room-v1",
                "edition_policy": "ultra_only", "character_policy": "unrestricted_between_games",
                "stage_policy": "p1_selects", "input_delay_policy": "ember_existing_ready_policy",
            },
            "observer_policy": "authorized_only",
            "result_policy": "two_player_agreement_or_review",
            "required_build_id": "ember",
            "metadata": {},
        }),
    )
    .await;
    let match_id = created["match_id"].as_str().unwrap().to_owned();

    let (_, _, _, assignments) = host
        .ask(json!({ "op": "assignment_list", "bridge_id": bridge_id }))
        .await;
    assert_eq!(assignments["assignments"][0]["match_id"], match_id.as_str());
    // The site's Play button opens this page, which opens the match in Ember.
    assert_eq!(
        created["play_url"],
        format!("https://embernetplay.link/m#{bridge_id}/{match_id}").as_str()
    );
    // The first claim hosts; the second waits for the room.
    let claim = json!({ "op": "match_claim", "bridge_id": bridge_id, "match_id": match_id, "build": "build-1" });
    let (ok, reason, _, lease) = host.ask(claim.clone()).await;
    assert!(ok, "{reason:?}");
    assert_eq!(lease["role"], "host");
    let (_, _, _, wait) = guest.ask(claim.clone()).await;
    assert_eq!(wait["role"], "wait");
    let room_id = "0123456789abcdef0123456789abcdef";
    let (ok, reason, _, published) = host
        .ask(json!({ "op": "room_publish", "bridge_id": bridge_id, "match_id": match_id, "room": {
            "room_id": room_id, "invitation": "sf4e3:test", "lease_id": lease["lease_id"], "fence": lease["fence"], "replaces": null,
        }}))
        .await;
    assert!(ok, "{reason:?}");
    // The helper hands the game the checked binding, with its own slot.
    assert_eq!(published["binding"]["local_slot"], 0);
    let (_, _, _, joined) = guest.ask(claim.clone()).await;
    assert_eq!(joined["invitation"], "sf4e3:test");
    assert_eq!(joined["binding"]["local_slot"], 1);
    assert_eq!(
        joined["binding"]["fighters"][0]["endpoint_id"],
        host_endpoint.as_str()
    );

    for (generation, result) in [(3u64, "p1_win"), (4, "p1_win")] {
        let prepare = json!({ "op": "game_prepare", "bridge_id": bridge_id, "match_id": match_id, "match_generation": generation.to_string() });
        let (_, _, _, pending) = host.ask(prepare.clone()).await;
        assert_eq!(pending["state"], "pending");
        let (ok, reason, _, permitted) = guest.ask(prepare.clone()).await;
        assert!(ok, "{reason:?}");
        assert_eq!(permitted["state"], "permitted");
        let (_, _, _, again) = host.ask(prepare).await;
        assert_eq!(again["permit_id"], permitted["permit_id"]);
        let report = json!({
            "op": "game_report", "bridge_id": bridge_id, "match_id": match_id, "match_generation": generation.to_string(),
            "result": result, "capture_frame": "900", "confirmed_input_frame": "899",
        });
        let (ok, reason, _, first) = host.ask(report.clone()).await;
        assert!(ok, "{reason:?}");
        assert_eq!(
            (first["saved"].as_bool(), first["delivered"].as_bool()),
            (Some(true), Some(true))
        );
        let (_, _, _, second) = guest.ask(report).await;
        assert_eq!(second["attempt_state"], "accepted");
    }
    let (ok, _, _, last) = guest
        .ask(json!({ "op": "assignment_list", "bridge_id": bridge_id }))
        .await;
    assert!(ok);
    assert_eq!(last["assignments"][0]["state"], "completed");
    // Delivered reports leave nothing in the spool.
    let spooled = std::fs::read_dir(host_dirs.0.join("tournament").join("reports"))
        .map(|entries| entries.count())
        .unwrap_or(0);
    assert_eq!(spooled, 0);
    // A game this helper never got a permit for cannot be reported.
    let (ok, reason, _, _) = host
        .ask(json!({ "op": "game_report", "bridge_id": bridge_id, "match_id": match_id, "match_generation": "99", "result": "p1_win", "capture_frame": "1", "confirmed_input_frame": "1" }))
        .await;
    assert!(!ok);
    assert_eq!(reason.as_deref(), Some("unknown_permit"));

    // Both fighters' games send each game's replay; the set keeps one of
    // each, and either fighter saves the set's pack.
    for n in [1u8, 2] {
        let path = dirs.0.join(format!("game-{n}.emberreplay"));
        std::fs::write(
            &path,
            ember_protocol::replays::wrap(&[n; 64], 1_800_000_000 + u32::from(n)),
        )
        .unwrap();
        for (worker, kept) in [(&mut host, true), (&mut guest, false)] {
            let (ok, reason, _, sent) = worker
                .ask(json!({ "op": "replay_send", "bridge_id": bridge_id, "match_id": match_id, "revision": "1", "path": path }))
                .await;
            assert!(ok, "{reason:?}");
            assert_eq!(sent["kept"], kept);
        }
    }
    let torn = dirs.0.join("torn.emberreplay");
    std::fs::write(&torn, b"EMBRPLY1").unwrap();
    let (ok, reason, _, _) = host
        .ask(json!({ "op": "replay_send", "bridge_id": bridge_id, "match_id": match_id, "revision": "1", "path": torn }))
        .await;
    assert_eq!((ok, reason.as_deref()), (false, Some("invalid_replay")));
    let pack = dirs.0.join("set.emberpack");
    let (ok, reason, _, _) = guest
        .ask(json!({ "op": "replay_pack", "bridge_id": bridge_id, "match_id": match_id, "path": pack }))
        .await;
    assert!(ok, "{reason:?}");
    let bytes = std::fs::read(&pack).unwrap();
    assert_eq!(&bytes[..8], ember_protocol::replays::PACK_MAGIC);
    // A bridge without a replay archive takes no other game, for good.
    let (ok, reason, _, _) = host
        .ask(json!({ "op": "replay_archive", "path": dirs.0.join("game-1.emberreplay"), "sides": { "a": "Ann", "b": "Bob Ü", "side": 0, "country": "BE" } }))
        .await;
    assert_eq!((ok, reason.as_deref()), (false, Some("replays_unsupported")));
    running.abort();
}

/// An origin that takes every connection and never answers, counting them.
async fn silent_origin() -> (
    String,
    std::sync::Arc<std::sync::atomic::AtomicUsize>,
    tokio::task::JoinHandle<()>,
) {
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let origin = format!("http://{}", listener.local_addr().unwrap());
    let taken = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));
    let counted = taken.clone();
    let task = tokio::spawn(async move {
        let mut held = Vec::new();
        while let Ok((socket, _)) = listener.accept().await {
            counted.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            held.push(socket);
        }
    });
    (origin, taken, task)
}

/// A request as the native side sends it, under its own request id.
fn submit(worker: &Worker, request_id: u64, request: Value) {
    let command: Command =
        serde_json::from_value(json!({ "type": "tournament", "request": request })).unwrap();
    let Command::Tournament { request } = command else {
        unreachable!()
    };
    worker.handle.submit(request_id, request);
}

// A leave runs at once while bridge requests wait for an HTTP slot, so a
// claim sent before it can reach the bridge after it. That claim is dropped:
// the binding of the match the game left is not restored. A claim sent after
// the leave holds the match again.
#[tokio::test]
async fn a_claim_sent_before_leaving_does_not_hold_the_match_again() {
    let dirs = dirs("leave");
    let (running, origin) = bridge(&dirs.0, Vec::new()).await;
    let provider = ember_bridge::issue_credential(&running.state, Some("mock-local"), None, "leave")
        .await
        .unwrap();
    let (host_endpoint, guest_endpoint) = ("a".repeat(64), "b".repeat(64));
    let host_dirs = Dirs(dirs.0.join("host"));
    let guest_dirs = Dirs(dirs.0.join("guest"));
    let (mut host, host_id, bridge_id, host_link) =
        linked_worker(&host_dirs, &host_endpoint, &origin, &provider, "host").await;
    let (mut guest, guest_id, _, guest_link) =
        linked_worker(&guest_dirs, &guest_endpoint, &origin, &provider, "guest").await;
    let created = provider_call(
        &origin,
        &provider,
        "/v1/matches",
        json!({
            "external_match_id": "helper-leave",
            "game": "usf4",
            "participants": [
                { "participant_id": host_link["participant_id"], "ember_id": host_id, "slot": 0 },
                { "participant_id": guest_link["participant_id"], "ember_id": guest_id, "slot": 1 },
            ],
            "rules": {
                "games_to_win": 2, "draw_policy": "replay_no_score", "native_rules_profile": "ember-room-v1",
                "edition_policy": "ultra_only", "character_policy": "unrestricted_between_games",
                "stage_policy": "p1_selects", "input_delay_policy": "ember_existing_ready_policy",
            },
            "observer_policy": "authorized_only",
            "result_policy": "two_player_agreement_or_review",
            "required_build_id": "ember",
            "metadata": {},
        }),
    )
    .await;
    let match_id = created["match_id"].as_str().unwrap().to_owned();
    let claim = json!({ "op": "match_claim", "bridge_id": bridge_id, "match_id": match_id, "build": "build-1" });
    let (ok, reason, _, lease) = host.ask(claim.clone()).await;
    assert!(ok, "{reason:?}");
    let (_, _, _, wait) = guest.ask(claim.clone()).await;
    assert_eq!(wait["role"], "wait");
    let (ok, reason, _, _) = host
        .ask(json!({ "op": "room_publish", "bridge_id": bridge_id, "match_id": match_id, "room": {
            "room_id": "0123456789abcdef0123456789abcdef", "invitation": "sf4e3:test",
            "lease_id": lease["lease_id"], "fence": lease["fence"], "replaces": null,
        }}))
        .await;
    assert!(ok, "{reason:?}");
    let (_, _, _, joined) = guest.ask(claim.clone()).await;
    assert_eq!(joined["binding"]["local_slot"], 1);

    // Four bridge requests hang on an origin that never answers, holding
    // every HTTP slot, so the guest's next claim waits behind them.
    let (silent, taken, task) = silent_origin().await;
    for id in 1..=4 {
        submit(&guest, id, json!({ "op": "bridge_inspect", "origin": silent }));
    }
    let started = tokio::time::Instant::now();
    while taken.load(std::sync::atomic::Ordering::SeqCst) < 4 {
        assert!(
            started.elapsed() < std::time::Duration::from_secs(10),
            "bridge requests not sent"
        );
        tokio::time::sleep(std::time::Duration::from_millis(10)).await;
    }
    submit(&guest, 10, claim.clone());
    submit(&guest, 11, json!({ "op": "match_leave", "match_id": match_id }));
    let (ok, reason, _, _) = guest.next_answer(11).await;
    assert!(ok, "{reason:?}");
    // The origin goes away, the slots free up, and the claim runs after the leave.
    task.abort();
    let (ok, reason, _, _) = guest.next_answer(10).await;
    assert_eq!((ok, reason.as_deref()), (false, Some("match_left")));
    let prepare = json!({ "op": "game_prepare", "bridge_id": bridge_id, "match_id": match_id, "match_generation": "3" });
    let (ok, reason, _, _) = guest.ask(prepare.clone()).await;
    assert_eq!((ok, reason.as_deref()), (false, Some("not_bound")));

    // A claim sent after the leave holds it again, as before.
    let (ok, reason, _, again) = guest.ask(claim).await;
    assert!(ok, "{reason:?}");
    assert_eq!(again["binding"]["local_slot"], 1);
    let (ok, reason, _, pending) = guest.ask(prepare).await;
    assert!(ok, "{reason:?}");
    assert_eq!(pending["state"], "pending");
    running.abort();
}
