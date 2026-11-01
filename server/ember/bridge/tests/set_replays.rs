//! A tournament set's replays (docs/design/TOURNAMENT_DESK.md, 4b): each
//! confirmed game's replay, kept once and packed by save time, kept or
//! pruned as replay retention says, and served from a public event's page.
mod common;

use common::play::{
    Fixture, bound, create, fixture, fixture_with, pack, permit, played, replay_of, report,
    send_replay, state,
};
use ember_protocol::report::Outcome;
use reqwest::StatusCode;
use serde_json::{Value as Json, json};

const DAY: u64 = 24 * 60 * 60;

/// Both fighters send each game's replay; the set keeps one of each, packs
/// them by save time, and takes nothing from anyone else.
#[tokio::test]
async fn a_sets_replays_are_kept_once_and_packed_by_save_time() {
    let f = fixture().await;
    let id = create(&f, "challonge:9001:s1:1", "ember-room-v1", 2).await;
    played(&f, &id, 3).await;
    // The set's desk event, and player b's account on its site.
    let (organizer, named) = (f.a.id().as_str().to_owned(), f.b.id().as_str().to_owned());
    let match_id = id.clone();
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
            // A second room, as if the first one was replaced.
            tx.execute(
                "UPDATE match_rooms SET binding_revision = binding_revision + 1 WHERE match_id = ?1",
                [&match_id],
            )?;
            Ok(())
        })
        .await
        .unwrap();
    // The third game was played in a second room, so under revision 2.
    for (n, revision) in [(3u8, 2), (1, 1), (2, 1)] {
        for sender in [&f.a, &f.b] {
            let status = send_replay(&f, sender, &id, &replay_of(&f.bridge, n), revision).await;
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
    let mut torn = replay_of(&f.bridge, 4);
    torn.pop();
    assert_eq!(
        send_replay(&f, &f.a, &id, &torn, 1).await,
        StatusCode::BAD_REQUEST
    );
    let mut outsider = f.bridge.player(33);
    f.bridge.open_session(&mut outsider).await;
    assert_eq!(
        send_replay(&f, &outsider, &id, &replay_of(&f.bridge, 4), 1).await,
        StatusCode::NOT_FOUND
    );
    // Three games played, three replays: a fourth is no game of the set.
    assert_eq!(
        send_replay(&f, &f.a, &id, &replay_of(&f.bridge, 4), 2).await,
        StatusCode::BAD_REQUEST
    );
    let (header, games) = pack(&f, &f.provider, &id).await;
    assert_eq!(
        games,
        vec![
            replay_of(&f.bridge, 1),
            replay_of(&f.bridge, 2),
            replay_of(&f.bridge, 3)
        ]
    );
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
        send_replay(&f, &f.a, &id, &replay_of(&f.bridge, n), 1).await;
    }
    (f, id)
}

#[tokio::test]
async fn replays_are_kept_forever_by_default() {
    let f = fixture().await;
    let id = create(&f, "play-forever", "ember-room-v1", 2).await;
    played(&f, &id, 1).await;
    send_replay(&f, &f.a, &id, &replay_of(&f.bridge, 1), 1).await;
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

/// A game nobody reported goes to review, and its replay waits; decided in
/// place by the organizer, it takes the replay, its time ending at the
/// decision.
#[tokio::test]
async fn a_game_decided_without_reports_takes_its_replay() {
    let mut f = fixture().await;
    let id = create(&f, "play-unreported", "ember-room-v1", 2).await;
    let binding = bound(&f, &id).await;
    let permit = permit(&f, &id, &binding, 31).await;
    let game = ember_protocol::replays::wrap(&[7; 64], f.bridge.clock.now() as u32 + 60);
    f.bridge.clock.advance(2 * 3600);
    ember_bridge::maintain(f.bridge.state()).await;
    f.bridge.open_session(&mut f.a).await;
    f.bridge.open_session(&mut f.b).await;
    assert_eq!(
        send_replay(&f, &f.a, &id, &game, 1).await,
        StatusCode::SERVICE_UNAVAILABLE
    );
    let snapshot = state(&f, &id).await;
    let (status, decided) = f
        .bridge
        .post_keyed(
            &f.organizer,
            &format!("/v1/matches/{id}/adjudications"),
            json!({
                "kind": "game_result", "winner_slot": 0, "reason": "Stream VOD",
                "attempt_id": permit.permit.attempt_id, "expected_revision": snapshot["revision"],
            }),
            Some("decide-unreported"),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED, "{decided}");
    assert_eq!(
        send_replay(&f, &f.a, &id, &game, 1).await,
        StatusCode::CREATED
    );
}

#[tokio::test]
async fn a_public_events_finished_set_is_downloaded_by_anyone() {
    let f = fixture().await;
    let id = create(&f, "http:weekly:r1-1:1", "ember-room-v1", 2).await;
    let binding = bound(&f, &id).await;
    for (n, generation) in [(1u8, 11), (3, 12)] {
        let permit = permit(&f, &id, &binding, generation).await;
        report(&f, &f.a, &f.a, &id, &permit, n, Outcome::P1Win).await;
        report(&f, &f.a, &f.b, &id, &permit, n + 1, Outcome::P1Win).await;
        assert_eq!(
            send_replay(&f, &f.a, &id, &replay_of(&f.bridge, n), 1).await,
            StatusCode::CREATED
        );
    }
    assert_eq!(state(&f, &id).await["state"], "completed");
    let anyone = |path: String| {
        let request = f.bridge.client.get(f.bridge.url(&path));
        async move { request.send().await.unwrap() }
    };
    let crc = ember_protocol::replays::crc_name(&replay_of(&f.bridge, 1)).unwrap();
    let pack_path = format!("/v1/public/matches/{id}/replays.pack");
    let game_path = format!("/v1/public/matches/{id}/replays/{crc}");
    // Not public: as if there were no such set.
    assert_eq!(
        anyone(pack_path.clone()).await.status(),
        StatusCode::NOT_FOUND
    );
    // The desk event whose sets the connector sent name the match.
    let (organizer, sets) = (
        f.a.id().as_str().to_owned(),
        json!([{ "id": "r1-1", "match_id": id }]).to_string(),
    );
    f.bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO organizer_events (id, ember_id, title, status, site, tournament, name, settings, created_at, public, sets)
                 VALUES ('evt_1', ?1, 'Weekly', 'ready', 'ember-bracket-site', 'weekly', 'Weekly', '{}', 1, 1, ?2)",
                [&organizer, &sets],
            )?;
            Ok(())
        })
        .await
        .unwrap();
    let downloaded = anyone(pack_path).await;
    assert_eq!(downloaded.status(), StatusCode::OK);
    let header: Json = serde_json::from_slice(
        &anyone(format!("/v1/public/matches/{id}/replays"))
            .await
            .bytes()
            .await
            .unwrap(),
    )
    .unwrap();
    assert_eq!(header["games"][0]["crc"], crc);
    // Its header names the event, found by its sets.
    assert_eq!(pack(&f, &f.provider, &id).await.0["tournament"], "Weekly");
    assert!(
        downloaded.headers()["content-disposition"]
            .to_str()
            .unwrap()
            .ends_with(".emberpack\"")
    );
    let game = anyone(game_path).await;
    assert_eq!(game.status(), StatusCode::OK);
    assert_eq!(
        game.bytes().await.unwrap().to_vec(),
        replay_of(&f.bridge, 1)
    );
    assert_eq!(
        anyone(format!("/v1/public/matches/{id}/replays/00000000"))
            .await
            .status(),
        StatusCode::NOT_FOUND
    );
}

// A claim that changes nothing gets the binding it got before, byte for
// byte and signature included, however much later it comes: a binding's
// `issued_at` is when its room was published. Both fighters hold the same
// binding, and a change it names moves it.
