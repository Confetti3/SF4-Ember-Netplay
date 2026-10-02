//! Bridge-run play (spec 14 to 16): claims, the room and its binding, game
//! permits, and scoring by two fighters' agreeing signed reports.
mod common;

use common::{Bridge, Player, code};
use ember_protocol::{
    PublicKey,
    challenge::{Action, Method},
    encoding::Counter,
    play::{Observation, SignedBinding, SignedPermit},
    report::Outcome,
};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};

const BUILD: &str = "build-1";
const ROOM: &str = "0123456789abcdef0123456789abcdef";

struct Fixture {
    bridge: Bridge,
    provider: String,
    organizer: String,
    a: Player,
    b: Player,
    links: [Json; 2],
}

async fn fixture() -> Fixture {
    let bridge = Bridge::start().await;
    let provider = bridge.provider("mock-a").await;
    let organizer = bridge.organizer("t1").await;
    let mut a = bridge.player(31);
    let mut b = bridge.player(32);
    bridge.open_session(&mut a).await;
    bridge.open_session(&mut b).await;
    let link_a = bridge.link(&provider, &a, "mock-a", "fighter-a").await;
    let link_b = bridge.link(&provider, &b, "mock-a", "fighter-b").await;
    Fixture {
        bridge,
        provider,
        organizer,
        a,
        b,
        links: [link_a, link_b],
    }
}

async fn create(f: &Fixture, external: &str, profile: &str, games_to_win: u8) -> String {
    let (status, created) = f
        .bridge
        .post_keyed(
            &f.provider,
            "/v1/matches",
            json!({
                "external_match_id": external,
                "game": "usf4",
                "participants": [
                    { "participant_id": f.links[0]["participant_id"], "ember_id": f.a.id(), "slot": 0 },
                    { "participant_id": f.links[1]["participant_id"], "ember_id": f.b.id(), "slot": 1 },
                ],
                "rules": {
                    "games_to_win": games_to_win,
                    "draw_policy": "replay_no_score",
                    "native_rules_profile": profile,
                    "edition_policy": "ultra_only",
                    "character_policy": "unrestricted_between_games",
                    "stage_policy": "p1_selects",
                    "input_delay_policy": "ember_existing_ready_policy",
                },
                "observer_policy": "authorized_only",
                "result_policy": "two_player_agreement_or_review",
                "required_build_id": "ember",
                "metadata": {},
            }),
            Some(external),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED, "{created}");
    created["match_id"].as_str().unwrap().to_owned()
}

/// A stand-in Iroh endpoint ID, distinct per player.
fn endpoint(player: &Player) -> String {
    player.id().as_str().as_bytes()[5..37]
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
}

async fn act(
    f: &Fixture,
    player: &Player,
    action: Action,
    path: &str,
    command: Json,
) -> (StatusCode, Json) {
    let body = f
        .bridge
        .prove(player, action, Method::Post, path, command)
        .await;
    f.bridge.send_proof(player, Method::Post, path, body).await
}

async fn claim(
    f: &Fixture,
    player: &Player,
    id: &str,
    endpoint: &str,
    build: &str,
) -> (StatusCode, Json) {
    act(
        f,
        player,
        Action::MatchClaim,
        &format!("/v1/matches/{id}/claims"),
        json!({ "endpoint_id": endpoint, "helper_instance_id": "ins_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a01", "build_id": build }),
    )
    .await
}

async fn publish(
    f: &Fixture,
    player: &Player,
    id: &str,
    room: &str,
    lease: Option<&Json>,
    replaces: Option<&str>,
) -> (StatusCode, Json) {
    act(
        f,
        player,
        Action::RoomPublish,
        &format!("/v1/matches/{id}/room"),
        json!({
            "room_id": room,
            "invitation": format!("sf4e3:{room}"),
            "lease_id": lease.map(|answer| answer["lease_id"].clone()),
            "fence": lease.map(|answer| answer["fence"].clone()),
            "replaces": replaces,
        }),
    )
    .await
}

async fn bridge_key(f: &Fixture) -> (PublicKey, String) {
    let response = f
        .bridge
        .client
        .get(f.bridge.url("/v1/signing-keys"))
        .send()
        .await
        .unwrap();
    let (_, keys) = common::read(response).await;
    let key = &keys["keys"][0];
    (
        PublicKey::from_b64u(key["x"].as_str().unwrap()).unwrap(),
        key["kid"].as_str().unwrap().to_owned(),
    )
}

fn descriptor(binding: &Json, player: &Player, generation: u64) -> Json {
    json!({
        "assignment_generation": binding["assignment_generation"],
        "binding_revision": binding["binding_revision"],
        "room_id": binding["room_id"],
        "table_id": 0,
        "match_generation": generation.to_string(),
        "rules_digest": binding["rules_digest"],
        "roster_digest": binding["roster_digest"],
        "build_id": binding["build_id"],
        "endpoint_id": endpoint(player),
    })
}

async fn prepare(
    f: &Fixture,
    player: &Player,
    id: &str,
    binding: &Json,
    generation: u64,
) -> (StatusCode, Json) {
    act(
        f,
        player,
        Action::AttemptPrepare,
        &format!("/v1/matches/{id}/attempts/prepare"),
        descriptor(binding, player, generation),
    )
    .await
}

/// Both fighters prepare the same game; returns the permit both receive.
async fn permit(f: &Fixture, id: &str, binding: &Json, generation: u64) -> SignedPermit {
    let (status, first) = prepare(f, &f.a, id, binding, generation).await;
    assert_eq!(
        (status, first["state"].as_str()),
        (StatusCode::OK, Some("pending")),
        "{first}"
    );
    let (status, second) = prepare(f, &f.b, id, binding, generation).await;
    assert_eq!(
        (status, second["state"].as_str()),
        (StatusCode::OK, Some("permitted")),
        "{second}"
    );
    // The first fighter asking again gets the same permit.
    let (_, again) = prepare(f, &f.a, id, binding, generation).await;
    assert_eq!(again["permit"], second["permit"]);
    let signed: SignedPermit = serde_json::from_value(second["permit"].clone()).unwrap();
    let (key, kid) = bridge_key(f).await;
    signed.verify(&key, &kid).unwrap();
    signed
}

fn observation(n: u8, result: Outcome) -> Observation {
    let native = result.is_native_result();
    Observation {
        observation_id: format!("obs_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a{n:02}"),
        result,
        capture_frame: native.then_some(Counter(900)),
        confirmed_input_frame: native.then_some(Counter(899)),
        observed_at: 1,
        helper_instance_id: "ins_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a01".into(),
    }
}

async fn report(
    f: &Fixture,
    sender: &Player,
    reporter: &Player,
    id: &str,
    permit: &SignedPermit,
    n: u8,
    result: Outcome,
) -> (StatusCode, Json) {
    let signed = permit
        .permit
        .report(reporter.id(), observation(n, result))
        .unwrap()
        .sign(&reporter.identity)
        .unwrap();
    let response = f
        .bridge
        .client
        .post(f.bridge.url(&format!("/v1/matches/{id}/reports")))
        .bearer_auth(sender.token())
        .header("content-type", "application/json")
        .body(serde_json::to_vec(&signed).unwrap())
        .send()
        .await
        .unwrap();
    common::read(response).await
}

/// Claims and publishes the room; returns the binding JSON both fighters hold.
async fn bound(f: &Fixture, id: &str) -> Json {
    let (status, host) = claim(f, &f.a, id, &endpoint(&f.a), BUILD).await;
    assert_eq!(
        (status, host["role"].as_str()),
        (StatusCode::OK, Some("host")),
        "{host}"
    );
    let (status, wait) = claim(f, &f.b, id, &endpoint(&f.b), BUILD).await;
    assert_eq!(
        (status, wait["role"].as_str()),
        (StatusCode::OK, Some("wait")),
        "{wait}"
    );
    // Only the lease holder publishes the first room.
    let (status, refused) = publish(f, &f.b, id, ROOM, Some(&host), None).await;
    assert_eq!(status, StatusCode::CONFLICT, "{refused}");
    let (status, room) = publish(f, &f.a, id, ROOM, Some(&host), None).await;
    assert_eq!(
        (status, room["role"].as_str()),
        (StatusCode::OK, Some("room")),
        "{room}"
    );
    let (_, joined) = claim(f, &f.b, id, &endpoint(&f.b), BUILD).await;
    assert_eq!(joined["invitation"], format!("sf4e3:{ROOM}"));
    let signed: SignedBinding = serde_json::from_value(joined["binding"].clone()).unwrap();
    let (key, kid) = bridge_key(f).await;
    let binding = signed.verify(&key, &kid).unwrap();
    assert_eq!(binding.fighters[0].endpoint_id, endpoint(&f.a));
    assert_eq!(binding.slot_of(&endpoint(&f.b)), Some(1));
    assert_eq!(binding.games_to_win, 2);
    serde_json::to_value(binding).unwrap()
}

async fn state(f: &Fixture, id: &str) -> Json {
    let (status, found) = f
        .bridge
        .get(&f.provider, &format!("/v1/matches/{id}"))
        .await;
    assert_eq!(status, StatusCode::OK, "{found}");
    found
}

#[tokio::test]
async fn agreeing_reports_score_a_set() {
    let f = fixture().await;
    let id = create(&f, "play-1", "ember-room-v1", 2).await;
    let binding = bound(&f, &id).await;
    assert_eq!(state(&f, &id).await["state"], "ready");

    let mut generation = 10;
    for (n, result) in [
        (1u8, Outcome::P1Win),
        (3, Outcome::P2Win),
        (5, Outcome::P1Win),
    ] {
        generation += 1;
        let permit = permit(&f, &id, &binding, generation).await;
        assert_eq!(state(&f, &id).await["state"], "running");
        let (status, first) = report(&f, &f.a, &f.a, &id, &permit, n, result).await;
        assert_eq!(status, StatusCode::CREATED, "{first}");
        assert_eq!(first["match_state"], "awaiting_reports");
        // The host may forward the other fighter's report unchanged.
        let (status, second) = report(&f, &f.a, &f.b, &id, &permit, n + 1, result).await;
        assert_eq!(status, StatusCode::CREATED, "{second}");
        assert_eq!(second["attempt_state"], "accepted");
    }
    let finished = state(&f, &id).await;
    assert_eq!(finished["state"], "completed");
    assert_eq!(finished["scores"][0]["wins"], 2);
    assert_eq!(finished["scores"][1]["wins"], 1);
    let events = f.bridge.events(&f.provider, "0").await;
    let completed = events
        .iter()
        .find(|e| e["type"] == "io.ember.tournament.match.completed.v1")
        .unwrap();
    assert_eq!(completed["data"]["resolution"], "player_agreement");
    assert_eq!(
        completed["data"]["evidence_report_ids"]
            .as_array()
            .unwrap()
            .len(),
        2
    );
    // A finished match permits nothing more.
    let (status, _) = prepare(&f, &f.a, &id, &binding, generation + 1).await;
    assert_eq!(status, StatusCode::CONFLICT);
    // The player's own list shows the set as Ember plays it.
    let (_, listed) = f.bridge.get(f.a.token(), "/v1/assignments").await;
    let row = &listed["assignments"][0];
    assert_eq!(row["match_id"], id.as_str());
    assert_eq!(row["native_rules_profile"], "ember-room-v1");
    assert_eq!(row["games_to_win"], 2);
    assert_eq!(row["wins"], json!([2, 1]));
    assert_eq!(row["opponent"]["fingerprint"], f.b.id().fingerprint());
}

#[tokio::test]
async fn reports_are_idempotent_and_never_rewritten() {
    let f = fixture().await;
    let id = create(&f, "play-2", "ember-room-v1", 2).await;
    let binding = bound(&f, &id).await;
    let permit = permit(&f, &id, &binding, 21).await;
    let (_, first) = report(&f, &f.a, &f.a, &id, &permit, 1, Outcome::P1Win).await;
    let (status, retry) = report(&f, &f.a, &f.a, &id, &permit, 1, Outcome::P1Win).await;
    assert_eq!(
        (status, &retry["report_id"]),
        (StatusCode::OK, &first["report_id"])
    );
    // The same observation with a different result is refused.
    let (status, conflict) = report(&f, &f.a, &f.a, &id, &permit, 1, Outcome::P2Win).await;
    assert_eq!(
        (status, code(&conflict)),
        (StatusCode::CONFLICT, "idempotency_conflict")
    );
    // A later contradicting observation is kept as evidence, but the first counts.
    let (status, _) = report(&f, &f.a, &f.a, &id, &permit, 2, Outcome::P2Win).await;
    assert_eq!(status, StatusCode::CREATED);
    let (_, decided) = report(&f, &f.b, &f.b, &id, &permit, 3, Outcome::P1Win).await;
    assert_eq!(decided["attempt_state"], "accepted");
    assert_eq!(state(&f, &id).await["scores"][0]["wins"], 1);
    // A report for a game the match never permitted is refused.
    let mut forged = permit.clone();
    forged.permit.permit_id = "per_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9aff".into();
    let (status, _) = report(&f, &f.a, &f.a, &id, &forged, 9, Outcome::P1Win).await;
    assert_eq!(status, StatusCode::CONFLICT);
}

#[tokio::test]
async fn disagreement_holds_the_match_for_an_organizer() {
    let f = fixture().await;
    let id = create(&f, "play-3", "ember-room-v1", 2).await;
    let binding = bound(&f, &id).await;
    let permit = permit(&f, &id, &binding, 31).await;
    report(&f, &f.a, &f.a, &id, &permit, 1, Outcome::P1Win).await;
    let (_, held) = report(&f, &f.b, &f.b, &id, &permit, 2, Outcome::P2Win).await;
    assert_eq!(held["attempt_state"], "review");
    assert_eq!(held["match_state"], "needs_review");
    // The next game waits for the organizer.
    let (status, blocked) = prepare(&f, &f.a, &id, &binding, 32).await;
    assert_eq!(status, StatusCode::CONFLICT, "{blocked}");
    let snapshot = state(&f, &id).await;
    let (status, _) = f
        .bridge
        .post_keyed(
            &f.organizer,
            &format!("/v1/matches/{id}/adjudications"),
            json!({ "kind": "game_result", "winner_slot": 1, "reason": "Stream VOD", "expected_revision": snapshot["revision"] }),
            Some("result-without-attempt"),
        )
        .await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let (status, decided) = f
        .bridge
        .post_keyed(
            &f.organizer,
            &format!("/v1/matches/{id}/adjudications"),
            json!({
                "kind": "game_result", "winner_slot": 1, "reason": "Stream VOD",
                "attempt_id": permit.permit.attempt_id, "expected_revision": snapshot["revision"],
            }),
            Some("decide"),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED, "{decided}");
    assert_eq!(decided["state"], "between_games");
    assert_eq!(state(&f, &id).await["scores"][1]["wins"], 1);
    let (status, next) = prepare(&f, &f.a, &id, &binding, 32).await;
    assert_eq!(
        (status, next["state"].as_str()),
        (StatusCode::OK, Some("pending"))
    );
}

#[tokio::test]
async fn a_lone_report_or_a_silent_game_goes_to_review() {
    let f = fixture().await;
    let id = create(&f, "play-4", "ember-room-v1", 2).await;
    let binding = bound(&f, &id).await;
    let permit = permit(&f, &id, &binding, 41).await;
    report(&f, &f.a, &f.a, &id, &permit, 1, Outcome::P1Win).await;
    f.bridge.clock.advance(59);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(state(&f, &id).await["state"], "awaiting_reports");
    f.bridge.clock.advance(2);
    ember_bridge::maintain(f.bridge.state()).await;
    let held = state(&f, &id).await;
    assert_eq!(held["state"], "needs_review");
    // The partner's report, arriving late, is stored but changes nothing.
    let (status, late) = report(&f, &f.b, &f.b, &id, &permit, 2, Outcome::P1Win).await;
    assert_eq!(
        (status, late["attempt_state"].as_str()),
        (StatusCode::CREATED, Some("review"))
    );

    // An organizer voids the game; the fighters play it again.
    let (status, voided) = f
        .bridge
        .post_keyed(
            &f.organizer,
            &format!("/v1/matches/{id}/adjudications"),
            json!({ "kind": "void_game", "attempt_id": permit.permit.attempt_id, "reason": "Replay", "expected_revision": held["revision"] }),
            Some("void"),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED, "{voided}");
    assert_eq!(voided["state"], "between_games");
}

#[tokio::test]
async fn claims_and_descriptors_are_checked() {
    let f = fixture().await;
    // Organizer-reported matches are not played through Ember.
    let organized = create(&f, "play-6", "organizer-reported-v1", 2).await;
    let (status, refused) = claim(&f, &f.a, &organized, &endpoint(&f.a), BUILD).await;
    assert_eq!(
        (status, code(&refused)),
        (StatusCode::UNPROCESSABLE_ENTITY, "unsupported_rules")
    );
    let (status, _) = f
        .bridge
        .post_keyed(
            &f.provider,
            &format!("/v1/matches/{organized}/cancel"),
            json!({ "reason": "test", "expected_revision": "1" }),
            Some("cancel-organized"),
        )
        .await;
    assert_eq!(status, StatusCode::OK);

    let id = create(&f, "play-7", "ember-room-v1", 2).await;
    // Someone else's match does not exist for a stranger.
    let mut stranger = f.bridge.player(39);
    f.bridge.open_session(&mut stranger).await;
    let (status, _) = claim(&f, &stranger, &id, &endpoint(&stranger), BUILD).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    // Different builds cannot share a room.
    claim(&f, &f.a, &id, &endpoint(&f.a), BUILD).await;
    let (status, mismatch) = claim(&f, &f.b, &id, &endpoint(&f.b), "build-2").await;
    assert_eq!(
        (status, code(&mismatch)),
        (StatusCode::UNPROCESSABLE_ENTITY, "incompatible_build")
    );

    let binding = bound(&f, &id).await;
    // A descriptor for another room, or an old binding, is refused.
    let mut wrong = descriptor(&binding, &f.a, 50);
    wrong["room_id"] = json!("f".repeat(32));
    let (status, _) = act(
        &f,
        &f.a,
        Action::AttemptPrepare,
        &format!("/v1/matches/{id}/attempts/prepare"),
        wrong,
    )
    .await;
    assert_eq!(status, StatusCode::CONFLICT);
    // A fighter's new endpoint moves the binding revision.
    let (_, moved) = claim(&f, &f.b, &id, &"e".repeat(64), BUILD).await;
    assert_eq!(moved["binding"]["binding"]["binding_revision"], "2");
    let (status, stale) = prepare(&f, &f.a, &id, &binding, 50).await;
    assert_eq!(status, StatusCode::CONFLICT, "{stale}");
    claim(&f, &f.b, &id, &endpoint(&f.b), BUILD).await;
    let (_, current) = claim(&f, &f.a, &id, &endpoint(&f.a), BUILD).await;
    let binding = current["binding"]["binding"].clone();
    assert_eq!(binding["binding_revision"], "3");
    let permit = permit(&f, &id, &binding, 50).await;
    report(&f, &f.a, &f.a, &id, &permit, 1, Outcome::Draw).await;
    let (_, drawn) = report(&f, &f.b, &f.b, &id, &permit, 2, Outcome::Draw).await;
    assert_eq!(drawn["match_state"], "between_games");
    // A native game is used once.
    let (status, reused) = prepare(&f, &f.a, &id, &binding, 50).await;
    assert_eq!(status, StatusCode::CONFLICT, "{reused}");
    // Replacing the room needs the current room's ID and moves the generation.
    let (status, _) = publish(&f, &f.b, &id, &"1".repeat(32), None, Some(&"2".repeat(32))).await;
    assert_eq!(status, StatusCode::CONFLICT);
    let (status, replaced) = publish(&f, &f.b, &id, &"1".repeat(32), None, Some(ROOM)).await;
    assert_eq!(status, StatusCode::OK, "{replaced}");
    assert_eq!(replaced["binding"]["binding"]["assignment_generation"], "2");
    assert_eq!(state(&f, &id).await["scores"][0]["wins"], 0);
}

#[tokio::test]
async fn a_cancelled_match_stays_cancelled() {
    let f = fixture().await;
    let id = create(&f, "play-8", "ember-room-v1", 2).await;
    let binding = bound(&f, &id).await;
    let permit = permit(&f, &id, &binding, 61).await;
    let (status, _) = f
        .bridge
        .post_keyed(
            &f.provider,
            &format!("/v1/matches/{id}/cancel"),
            json!({ "reason": "Bracket reset", "expected_revision": state(&f, &id).await["revision"] }),
            Some("cancel-play"),
        )
        .await;
    assert_eq!(status, StatusCode::OK);
    report(&f, &f.a, &f.a, &id, &permit, 1, Outcome::P1Win).await;
    let (status, late) = report(&f, &f.b, &f.b, &id, &permit, 2, Outcome::P1Win).await;
    assert_eq!(status, StatusCode::CREATED, "{late}");
    assert_eq!(late["attempt_state"], "aborted");
    assert_eq!(late["match_state"], "cancelled");
    assert_eq!(state(&f, &id).await["scores"][0]["wins"], 0);
}

#[tokio::test]
async fn an_unlink_mid_game_waits_for_the_organizer() {
    let f = fixture().await;
    let id = create(&f, "play-9", "ember-room-v1", 2).await;
    let binding = bound(&f, &id).await;
    let permit = permit(&f, &id, &binding, 71).await;
    let (_, links) = f.bridge.get(f.b.token(), "/v1/links").await;
    let link = links["links"][0]["link_id"].as_str().unwrap().to_owned();
    let path = format!("/v1/links/{link}");
    let body = f
        .bridge
        .prove(
            &f.b,
            Action::LinkRemove,
            Method::Delete,
            &path,
            json!({ "link_id": link }),
        )
        .await;
    let (status, _) = f.bridge.send_proof(&f.b, Method::Delete, &path, body).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(state(&f, &id).await["state"], "needs_review");
    // Agreeing reports do not undo the review.
    report(&f, &f.a, &f.a, &id, &permit, 1, Outcome::P1Win).await;
    let (_, receipt) = report(&f, &f.b, &f.b, &id, &permit, 2, Outcome::P1Win).await;
    assert_eq!(receipt["attempt_state"], "review");
    assert_eq!(receipt["match_state"], "needs_review");
}

#[tokio::test]
async fn a_publish_retry_and_a_silent_game() {
    let f = fixture().await;
    let id = create(&f, "play-10", "ember-room-v1", 2).await;
    let (_, host) = claim(&f, &f.a, &id, &endpoint(&f.a), BUILD).await;
    claim(&f, &f.b, &id, &endpoint(&f.b), BUILD).await;
    let (status, first) = publish(&f, &f.a, &id, ROOM, Some(&host), None).await;
    assert_eq!(status, StatusCode::OK, "{first}");
    // The same publish again, as after a lost answer, returns the room.
    let (status, again) = publish(&f, &f.a, &id, ROOM, Some(&host), None).await;
    assert_eq!(
        (status, again["role"].as_str()),
        (StatusCode::OK, Some("room")),
        "{again}"
    );
    let binding = again["binding"]["binding"].clone();
    // A permitted game nobody reports goes to review half an hour after its start window.
    permit(&f, &id, &binding, 81).await;
    f.bridge.clock.advance(120 + 30 * 60 - 1);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(state(&f, &id).await["state"], "running");
    f.bridge.clock.advance(2);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(state(&f, &id).await["state"], "needs_review");
}

#[tokio::test]
async fn a_lone_cancel_closes_the_game_and_a_lone_abort_waits_for_review() {
    let f = fixture().await;
    let id = create(&f, "play-11", "ember-room-v1", 2).await;
    let binding = bound(&f, &id).await;
    // A start called off before the game began: only one fighter's helper had the permit.
    let permit_a = permit(&f, &id, &binding, 91).await;
    report(&f, &f.a, &f.a, &id, &permit_a, 1, Outcome::Cancel).await;
    f.bridge.clock.advance(61);
    ember_bridge::maintain(f.bridge.state()).await;
    let after = state(&f, &id).await;
    assert_eq!(after["state"], "between_games", "{after}");
    assert_eq!(after["attempts"][0]["state"], "aborted");
    // The next game can be prepared at once.
    let permit_b = permit(&f, &id, &binding, 92).await;
    // A game that started and broke, reported by one fighter, is for an organizer.
    report(&f, &f.b, &f.b, &id, &permit_b, 2, Outcome::Abort).await;
    f.bridge.clock.advance(61);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(state(&f, &id).await["state"], "needs_review");
}

async fn handoff(f: &Fixture, id: &str, player: &Player) -> (StatusCode, Json) {
    f.bridge
        .post(
            &f.provider,
            "/v1/handoffs",
            json!({ "match_id": id, "ember_id": player.id() }),
        )
        .await
}

async fn redeem(f: &Fixture, player: &Player, code: &Json) -> (StatusCode, Json) {
    act(
        f,
        player,
        Action::HandoffRedeem,
        "/v1/handoffs/redeem",
        json!({ "handoff": code }),
    )
    .await
}

#[tokio::test]
async fn a_handoff_names_the_match_once_for_its_own_player() {
    let f = fixture().await;
    let id = create(&f, "play-12", "ember-room-v1", 2).await;
    let (status, made) = handoff(&f, &id, &f.a).await;
    assert_eq!(status, StatusCode::CREATED, "{made}");
    let code = made["handoff"].clone();
    assert_eq!(code.as_str().unwrap().len(), 43);
    assert!(
        made["uri"]
            .as_str()
            .unwrap()
            .starts_with("ember://tournament/open?bridge=brg_")
    );
    // The other fighter cannot use it, and a proof made for another route
    // does not redeem it.
    let (status, _) = redeem(&f, &f.b, &code).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let response = f
        .bridge
        .client
        .post(f.bridge.url("/v1/auth/challenges"))
        .bearer_auth(f.a.session.as_deref().unwrap())
        .header("content-type", "application/json")
        .body(
            serde_json::to_vec(&json!({
                "public_key": f.a.identity.public_key(),
                "action": Action::MatchClaim,
                "method": Method::Post,
                "path": "/v1/handoffs/redeem",
                "request_digest": "0".repeat(43),
            }))
            .unwrap(),
        )
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::BAD_REQUEST);
    let (status, redeemed) = redeem(&f, &f.a, &code).await;
    assert_eq!(
        (status, redeemed["match_id"].as_str()),
        (StatusCode::OK, Some(id.as_str())),
        "{redeemed}"
    );
    // Once only.
    let (status, _) = redeem(&f, &f.a, &code).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    // A code lapses after a minute.
    let (_, late) = handoff(&f, &id, &f.a).await;
    f.bridge.clock.advance(61);
    let (status, _) = redeem(&f, &f.a, &late["handoff"]).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    // Only an assigned player of a match played through Ember gets one.
    let (status, _) = handoff(&f, &id, &f.bridge.player(40)).await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    // Nor a match that is finished.
    let (status, _) = f
        .bridge
        .post_keyed(
            &f.provider,
            &format!("/v1/matches/{id}/cancel"),
            json!({ "reason": "Bracket reset", "expected_revision": state(&f, &id).await["revision"] }),
            Some("cancel-handoff"),
        )
        .await;
    assert_eq!(status, StatusCode::OK);
    let (status, _) = handoff(&f, &id, &f.a).await;
    assert_eq!(status, StatusCode::CONFLICT);
    let organized = create(&f, "play-13", "organizer-reported-v1", 2).await;
    let (status, _) = handoff(&f, &organized, &f.a).await;
    assert_eq!(status, StatusCode::UNPROCESSABLE_ENTITY);
}
