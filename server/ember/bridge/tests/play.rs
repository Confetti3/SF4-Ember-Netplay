//! Bridge-run play (spec 14 to 16): claims, the room and its binding, game
//! permits, and scoring by two fighters' agreeing signed reports.
mod common;

use common::{
    Bridge, Player, code,
    play::{
        BUILD, ROOM, Table, act, bound, claim, create_match, descriptor, endpoint, observation,
        pack, permit, played, prepare, publish, replay, report, report_with, send_replay,
    },
};
use ember_protocol::{
    challenge::{Action, Method},
    report::Outcome,
};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};

struct Fixture {
    bridge: Bridge,
    provider: String,
    organizer: String,
    a: Player,
    b: Player,
    links: [Json; 2],
}

async fn fixture() -> Fixture {
    fixture_with(|_| {}).await
}

async fn fixture_with(adjust: impl FnOnce(&mut ember_bridge::Config)) -> Fixture {
    let bridge = Bridge::start_with(adjust, Default::default()).await;
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
    create_match(
        &f.bridge,
        &f.provider,
        &f.links,
        [&f.a, &f.b],
        external,
        profile,
        games_to_win,
    )
    .await
}

impl Table for Fixture {
    fn bridge(&self) -> &Bridge {
        &self.bridge
    }

    fn a(&self) -> &Player {
        &self.a
    }

    fn b(&self) -> &Player {
        &self.b
    }
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

/// Both fighters send each game's replay; the set keeps one of each, packs
/// them by save time, and takes nothing from anyone else.
#[tokio::test]
async fn a_sets_replays_are_kept_once_and_packed_by_save_time() {
    let f = fixture().await;
    let id = create(&f, "challonge:9001:s1:1", "ember-room-v1", 2).await;
    played(&f, &id, 3).await;
    // The set's desk event, and player b's account on its site.
    let (organizer, named) = (f.a.id().as_str().to_owned(), f.b.id().as_str().to_owned());
    f.bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO organizer_events (id, ember_id, title, status, site, tournament, name, settings, created_at)
                 VALUES ('evt_1', ?1, 'Weekly', 'ready', 'challonge', '9001', 'Weekly', '{}', 1)",
                [&organizer],
            )?;
            tx.execute(
                "INSERT INTO site_accounts (site, user_id, ember_id, username, connected_at)
                 VALUES ('challonge', '22', ?1, 'player-b', 1)",
                [&named],
            )?;
            Ok(())
        })
        .await
        .unwrap();
    // The third game was played in a second room, so under revision 2.
    for (n, revision) in [(3u8, 2), (1, 1), (2, 1)] {
        for sender in [&f.a, &f.b] {
            let status = send_replay(&f, sender, &id, &replay(n), revision).await;
            assert_eq!(
                status,
                if sender.id() == f.a.id() {
                    StatusCode::CREATED
                } else {
                    StatusCode::OK
                }
            );
        }
    }
    let mut torn = replay(4);
    torn.pop();
    assert_eq!(
        send_replay(&f, &f.a, &id, &torn, 1).await,
        StatusCode::BAD_REQUEST
    );
    let mut outsider = f.bridge.player(33);
    f.bridge.open_session(&mut outsider).await;
    assert_eq!(
        send_replay(&f, &outsider, &id, &replay(4), 1).await,
        StatusCode::NOT_FOUND
    );
    // Three games played, three replays: a fourth is no game of the set.
    assert_eq!(
        send_replay(&f, &f.a, &id, &replay(4), 2).await,
        StatusCode::BAD_REQUEST
    );
    let (header, games) = pack(&f, &f.provider, &id).await;
    assert_eq!(games, vec![replay(1), replay(2), replay(3)]);
    assert_eq!(header["rooms"], 2, "{header}");
    assert_eq!(header["score"], json!([0, 0]));
    assert_eq!(header["players"][1]["ember_id"], f.b.id().as_str());
    assert_eq!(header["tournament"], "Weekly");
    assert_eq!(header["names"], json!([null, "player-b"]));
    assert_eq!(header["games"][2]["revision"], 2);
    // A player of the set reads it too; nobody else finds it.
    assert_eq!(pack(&f, f.b.token(), &id).await.1.len(), 3);
    let response = f
        .bridge
        .client
        .get(f.bridge.url(&format!("/v1/matches/{id}/replays.pack")))
        .bearer_auth(outsider.token())
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::NOT_FOUND);
}

/// The replays the bridge holds for set `id`.
async fn held(f: &Fixture, id: &str) -> i64 {
    let id = id.to_owned();
    f.bridge
        .state()
        .db
        .read(move |tx| {
            Ok(tx.query_row(
                "SELECT COUNT(*) FROM match_replays WHERE match_id = ?1",
                [id],
                |row| row.get(0),
            )?)
        })
        .await
        .unwrap()
}

/// A bridge that keeps replays `days` days, backing packs up to `backup`.
async fn retained(days: u64, backup: &std::path::Path) -> (Fixture, String) {
    let backup = backup.to_owned();
    let f = fixture_with(move |config| {
        config.replay_retention = Some(ember_bridge::config::ReplayRetention {
            days,
            backup: Some(backup),
        })
    })
    .await;
    let id = create(&f, "play-retained", "ember-room-v1", 2).await;
    played(&f, &id, 2).await;
    for n in [1, 2] {
        send_replay(&f, &f.a, &id, &replay(n), 1).await;
    }
    (f, id)
}

#[tokio::test]
async fn replays_are_kept_forever_by_default() {
    let f = fixture().await;
    let id = create(&f, "play-forever", "ember-room-v1", 2).await;
    played(&f, &id, 1).await;
    send_replay(&f, &f.a, &id, &replay(1), 1).await;
    for _ in 0..3 {
        f.bridge.clock.advance(3650 * DAY as i64);
        ember_bridge::maintain(f.bridge.state()).await;
    }
    assert_eq!(held(&f, &id).await, 1);
}

/// With a retention, a set untouched that long is written to the backup
/// folder as its pack, then its replays go.
#[tokio::test]
async fn old_sets_are_backed_up_then_pruned() {
    let backup = std::env::temp_dir().join(format!("ember-replay-backup-{}", std::process::id()));
    let (f, id) = retained(30, &backup).await;
    let served = f
        .bridge
        .client
        .get(f.bridge.url(&format!("/v1/matches/{id}/replays.pack")))
        .bearer_auth(&f.provider)
        .send()
        .await
        .unwrap()
        .bytes()
        .await
        .unwrap();
    // Unplayed, the set expires after a day, which is its last change.
    f.bridge.clock.advance(DAY as i64 + 1);
    ember_bridge::maintain(f.bridge.state()).await;
    f.bridge.clock.advance(29 * DAY as i64);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(held(&f, &id).await, 2, "kept within the retention");
    f.bridge.clock.advance(DAY as i64 + 1);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(held(&f, &id).await, 0);
    let saved = std::fs::read(backup.join(format!("{id}.emberpack"))).unwrap();
    assert_eq!(saved, served);
    std::fs::remove_dir_all(&backup).unwrap();
}

/// A backup that cannot be written keeps the set's replays.
#[tokio::test]
async fn a_failed_backup_keeps_the_replays() {
    // A file where the folder should be.
    let backup = std::env::temp_dir().join(format!("ember-replay-blocked-{}", std::process::id()));
    std::fs::write(&backup, b"").unwrap();
    let (f, id) = retained(30, &backup).await;
    for _ in 0..2 {
        f.bridge.clock.advance(31 * DAY as i64);
        ember_bridge::maintain(f.bridge.state()).await;
    }
    assert_eq!(held(&f, &id).await, 2);
    std::fs::remove_file(&backup).unwrap();
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

const DAY: u64 = 24 * 60 * 60;

/// The match's `expires_at`, which must lie a whole lifetime after some moment
/// in `[from, to]` (the clock is the wall clock, so a request is checked
/// against the window around it rather than one reading).
async fn assert_deadline(f: &Fixture, id: &str, from: u64, to: u64) {
    let expires = state(f, id).await["expires_at"].as_u64().unwrap();
    assert!(
        (from + DAY..=to + DAY).contains(&expires),
        "{expires} outside {}..={}",
        from + DAY,
        to + DAY
    );
}

// A game the fighters could not settle waits for the organizer, who may
// decide it long after the match's first deadline. The decision is game
// activity: the match gets a whole lifetime from it and is not expired by the
// next maintenance pass.
#[tokio::test]
async fn an_old_review_game_decided_late_leaves_a_full_lifetime() {
    let f = fixture().await;
    let id = create(&f, "play-expiry-1", "ember-room-v1", 2).await;
    let binding = bound(&f, &id).await;
    let permit = permit(&f, &id, &binding, 51).await;
    report(&f, &f.a, &f.a, &id, &permit, 1, Outcome::P1Win).await;
    let (_, held) = report(&f, &f.b, &f.b, &id, &permit, 2, Outcome::P2Win).await;
    assert_eq!(held["match_state"], "needs_review");

    // The original deadline passes while the organizer has not looked.
    f.bridge.clock.advance(DAY as i64 + 3600);
    ember_bridge::maintain(f.bridge.state()).await;
    let waiting = state(&f, &id).await;
    assert_eq!(waiting["state"], "needs_review");

    let before = f.bridge.clock.now();
    let (status, decided) = f
        .bridge
        .post_keyed(
            &f.organizer,
            &format!("/v1/matches/{id}/adjudications"),
            json!({
                "kind": "game_result", "winner_slot": 0, "reason": "Stream VOD",
                "attempt_id": permit.permit.attempt_id, "expected_revision": waiting["revision"],
            }),
            Some("decide-late"),
        )
        .await;
    let after = f.bridge.clock.now();
    assert_eq!(status, StatusCode::CREATED, "{decided}");
    assert_eq!(decided["state"], "between_games");

    // The next pass finds a match that was just decided, not an old one.
    ember_bridge::maintain(f.bridge.state()).await;
    let found = state(&f, &id).await;
    assert_eq!(found["state"], "between_games");
    assert_eq!(found["scores"][0]["wins"], 1);
    assert_deadline(&f, &id, before, after).await;
}

// Starting a game and settling it by the fighters' agreeing reports each move
// the deadline to a whole lifetime from then.
#[tokio::test]
async fn a_permit_and_agreeing_reports_each_extend_the_deadline() {
    let f = fixture().await;
    let id = create(&f, "play-expiry-2", "ember-room-v1", 2).await;
    let binding = bound(&f, &id).await;
    let created = state(&f, &id).await["created_at"].as_u64().unwrap();
    assert_eq!(state(&f, &id).await["expires_at"], created + DAY);

    f.bridge.clock.advance(120);
    let before = f.bridge.clock.now();
    let permit = permit(&f, &id, &binding, 61).await;
    let after = f.bridge.clock.now();
    assert_deadline(&f, &id, before, after).await;

    f.bridge.clock.advance(120);
    let before = f.bridge.clock.now();
    report(&f, &f.a, &f.a, &id, &permit, 1, Outcome::P1Win).await;
    let (_, second) = report(&f, &f.b, &f.b, &id, &permit, 2, Outcome::P1Win).await;
    let after = f.bridge.clock.now();
    assert_eq!(second["attempt_state"], "accepted");
    assert_deadline(&f, &id, before, after).await;
}

#[tokio::test]
async fn a_lone_report_or_a_silent_game_goes_to_review() {
    let f = fixture().await;
    let id = create(&f, "play-4", "ember-room-v1", 2).await;
    let binding = bound(&f, &id).await;
    let permit = permit(&f, &id, &binding, 41).await;
    // A second short of the partner's minute and a second past it: real time
    // must not count, or a busy machine pushes the first check over.
    f.bridge.clock.freeze();
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
    // The test clock follows the wall clock, so stay a few seconds clear of
    // the deadline: a second that passes during the test must not end it early.
    f.bridge.clock.advance(120 + 30 * 60 - 10);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(state(&f, &id).await["state"], "running");
    f.bridge.clock.advance(11);
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
    // It still waits for the other fighter's report at 21 s, the wait where
    // disputes restart.
    report(&f, &f.b, &f.b, &id, &permit_b, 2, Outcome::Abort).await;
    f.bridge.clock.advance(21);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(state(&f, &id).await["state"], "awaiting_reports");
    f.bridge.clock.advance(40);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(state(&f, &id).await["state"], "needs_review");
}

/// Every match has one play link, the same for both players, that a site
/// can show in its messages: the page opens the match in Ember.
#[tokio::test]
async fn a_match_has_one_play_link_for_both_players() {
    let f = fixture().await;
    let id = create(&f, "play-12", "ember-room-v1", 2).await;
    let link = format!("https://embernetplay.link/m#{}/{id}", f.bridge.bridge_id);
    assert_eq!(state(&f, &id).await["play_url"], link.as_str());
}

async fn send_report(f: &Fixture, sender: &Player, id: &str, body: &Json) -> StatusCode {
    let response = f
        .bridge
        .client
        .post(f.bridge.url(&format!("/v1/matches/{id}/reports")))
        .bearer_auth(sender.token())
        .header("content-type", "application/json")
        .body(serde_json::to_vec(body).unwrap())
        .send()
        .await
        .unwrap();
    common::read(response).await.0
}

/// Reports a player could forge or misdirect: signed by someone who is not
/// the reporter, changed after signing, sent to another match, or for the
/// other fighter's slot. None of them count, and the game still scores from
/// the two honest reports.
#[tokio::test]
async fn forged_and_misdirected_reports_are_refused() {
    let f = fixture().await;
    let id = create(&f, "play-14", "ember-room-v1", 2).await;
    let binding = bound(&f, &id).await;
    let permit = permit(&f, &id, &binding, 31).await;
    let honest = permit
        .permit
        .report(f.a.id(), observation(1, Outcome::P1Win))
        .unwrap();
    // Carrying another player's key: it does not match the reporter. (The
    // library refuses to sign that way, so the forgery is made on the wire.)
    let stranger = f.bridge.player(41);
    for key in [f.b.identity.public_key(), stranger.identity.public_key()] {
        let mut forged = serde_json::to_value(honest.sign(&f.a.identity).unwrap()).unwrap();
        forged["public_key"] = serde_json::to_value(key).unwrap();
        assert_ne!(
            send_report(&f, &f.a, &id, &forged).await,
            StatusCode::CREATED
        );
    }
    // Changed after signing.
    let mut tampered = serde_json::to_value(honest.sign(&f.a.identity).unwrap()).unwrap();
    tampered["report"]["result"] = json!("p2_win");
    assert_ne!(
        send_report(&f, &f.a, &id, &tampered).await,
        StatusCode::CREATED
    );
    // Sent to a match it is not for.
    let signed = serde_json::to_value(honest.sign(&f.a.identity).unwrap()).unwrap();
    let elsewhere = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a77";
    assert_ne!(
        send_report(&f, &f.a, elsewhere, &signed).await,
        StatusCode::CREATED
    );
    // Nothing counted yet; the honest pair still scores the game.
    assert_eq!(state(&f, &id).await["scores"][0]["wins"], 0);
    assert_eq!(
        send_report(&f, &f.a, &id, &signed).await,
        StatusCode::CREATED
    );
    let (_, decided) = report(&f, &f.b, &f.b, &id, &permit, 2, Outcome::P1Win).await;
    assert_eq!(decided["attempt_state"], "accepted");
    assert_eq!(state(&f, &id).await["scores"][0]["wins"], 1);
}

#[tokio::test]
async fn a_games_facts_show_once_both_fighters_say_the_same() {
    let f = fixture().await;
    let (status, capabilities) = f.bridge.get(&f.provider, "/v1/capabilities").await;
    assert_eq!(status, StatusCode::OK);
    assert!(
        capabilities["features"]
            .as_array()
            .unwrap()
            .contains(&json!("reports.facts"))
    );
    let id = create(&f, "play-facts", "ember-room-v1", 2).await;
    let binding = bound(&f, &id).await;
    let facts = |perfect: bool| {
        serde_json::from_value::<ember_protocol::facts::GameFacts>(json!({
            "fighters": [{ "character": "RYU", "costume": 0, "color": 0 }, { "character": "BLR", "costume": 1, "color": 3 }],
            "rounds": [
                { "winner": 0, "finish": "ultra", "perfect": perfect, "from_frame": "10", "to_frame": "900" },
                { "winner": 0, "finish": "ko", "perfect": false, "from_frame": "1000", "to_frame": "2000" },
            ],
        }))
        .unwrap()
    };
    // Game 1: both say the same; game 2: they differ; game 3: one older
    // game sends none. Each result counts all the same.
    let mut generation = 10;
    for (n, mine, theirs) in [
        (1u8, Some(facts(true)), Some(facts(true))),
        (3, Some(facts(true)), Some(facts(false))),
    ] {
        generation += 1;
        let permit = permit(&f, &id, &binding, generation).await;
        let (status, first) =
            report_with(&f, &f.a, &f.a, &id, &permit, n, Outcome::P1Win, mine).await;
        assert_eq!(status, StatusCode::CREATED, "{first}");
        let (status, second) =
            report_with(&f, &f.a, &f.b, &id, &permit, n + 1, Outcome::P1Win, theirs).await;
        assert_eq!(
            (status, second["attempt_state"].as_str()),
            (StatusCode::CREATED, Some("accepted")),
            "{second}"
        );
    }
    let finished = state(&f, &id).await;
    assert_eq!(finished["state"], "completed");
    let attempts = finished["attempts"].as_array().unwrap();
    assert_eq!(
        attempts[0]["facts"],
        serde_json::to_value(facts(true)).unwrap()
    );
    assert!(
        attempts[1].get("facts").is_none(),
        "facts that differ are not kept"
    );
}
