//! The places the bridge's BluMint answers follow BluMint's partner docs
//! rather than the bridge's own habits: a close is answered `200` whatever
//! its reason, the create URL is registered last, and a creation naming an
//! id the bridge cannot match is `player_unlinked`.
mod common;

use common::blumint::*;
use reqwest::StatusCode;
use serde_json::{Value, json};

/// The reasons of the audit entries the closes left, in order.
async fn close_reasons(f: &Fixture) -> Vec<String> {
    f.bridge
        .state()
        .db
        .read(|tx| {
            Ok(tx
                .prepare(
                    "SELECT reason FROM audit_entries WHERE action = 'blumint.close' ORDER BY id",
                )?
                .query_map([], |row| row.get::<_, String>(0))?
                .collect::<rusqlite::Result<Vec<_>>>()?)
        })
        .await
        .unwrap()
}

// "Close the lobby the same way, and answer 200, whatever the reason": the
// reason is for the partner's logs, so one that is empty, long, not text or
// absent still closes the match, and the bridge keeps a bounded text of it.
#[tokio::test]
async fn a_close_answers_200_whatever_its_reason() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let long = "r".repeat(65);
    let bodies = [
        json!({ "reason": "" }),
        json!({ "reason": long }),
        json!({ "reason": "a\u{7}b\nc" }),
        json!({ "reason": 7 }),
        json!({ "reason": null }),
        json!({ "reason": { "later": "shape" } }),
        json!({}),
    ];
    for (n, mut body) in bodies.into_iter().enumerate() {
        let key = format!("set-{n}");
        let (_, created) = create(&f, &key, 2).await;
        let id = created["matchId"].as_str().unwrap().to_owned();
        body["idempotencyKey"] = Value::from(key);
        let (status, closed) = close(&f, body.clone()).await;
        assert_eq!(status, StatusCode::OK, "{body}: {closed}");
        assert_eq!(closed, json!({ "closed": true }));
        assert_eq!(match_state(&f, &id).await, "cancelled", "{body}");
    }
    let prefix = "closed by BluMint: ";
    assert_eq!(
        close_reasons(&f).await,
        [
            format!("{prefix}unspecified"),
            format!("{prefix}{}", "r".repeat(64)),
            format!("{prefix}abc"),
            format!("{prefix}unspecified"),
            format!("{prefix}unspecified"),
            format!("{prefix}unspecified"),
            format!("{prefix}unspecified"),
        ]
    );
}

// BluMint's keys are 1 to 128 letters, digits and hyphens. A key outside that
// names nothing the bridge made, and "a key you hold nothing under" is the
// 200 row: BluMint reads any other answer as a failed close and gives the
// match to its host. Nothing is recorded under it.
#[tokio::test]
async fn a_close_naming_a_key_the_bridge_cannot_hold_answers_200() {
    let f = fixture().await;
    for key in ["".to_owned(), "k".repeat(129), "has a space".to_owned()] {
        let (status, closed) =
            close(&f, json!({ "idempotencyKey": key, "reason": "decided" })).await;
        assert_eq!(status, StatusCode::OK, "{key}: {closed}");
        assert_eq!(closed, json!({ "closed": true }));
    }
    let recorded: i64 = f
        .bridge
        .state()
        .db
        .read(|tx| {
            Ok(
                tx.query_row("SELECT COUNT(*) FROM idempotency_records", [], |row| {
                    row.get(0)
                })?,
            )
        })
        .await
        .unwrap();
    assert_eq!(recorded, 0);
    // A body that names no key is not a close, and BluMint never sends one.
    let (status, refused) = close(&f, json!({ "reason": "decided" })).await;
    assert_eq!(status, StatusCode::BAD_REQUEST, "{refused}");
    assert_eq!(refused, json!({ "code": "invalid_request" }));
}

// Registering the create URL turns on automatic lobbies for the game's
// tournaments, so BluMint is told where to look players up and close lobbies
// first, and a registration that stops short of the create URL leaves them
// off.
#[tokio::test]
async fn a_registration_that_fails_before_the_create_url_leaves_lobbies_off() {
    let f = fixture().await;
    f.blumint
        .lock()
        .unwrap()
        .answers
        .extend([(200, None), (404, None)]);
    assert!(
        ember_bridge::register_blumint(f.bridge.state(), CONNECTION)
            .await
            .is_err()
    );
    let paths: Vec<String> = f
        .blumint
        .lock()
        .unwrap()
        .calls
        .iter()
        .map(|(path, ..)| path.clone())
        .collect();
    assert_eq!(
        paths,
        [
            "/api/tournaments/setLookupPlayer",
            "/api/tournaments/match/setClose"
        ]
    );
}

/// BluMint's request for Kate against Sam, each seat named by `first` and
/// `second` as the identities it carries.
fn naming(f: &Fixture, first: Value, second: Value) -> Value {
    let mut body = request(f, 2);
    body["teams"][0]["players"][0]["identities"] = first;
    body["teams"][1]["players"][0]["identities"] = second;
    body
}

// "You can't match the id of a player the create names" is player_unlinked,
// naming "every id from the request you don't recognise", which BluMint
// retries under the same key; invalid_request is final, and sends the host to
// run the match by hand. An id that is not an Ember ID is one the bridge
// cannot match, whatever form BluMint names the player in.
#[tokio::test]
async fn a_creation_naming_an_id_the_bridge_cannot_match_is_player_unlinked() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (kate, sam) = (
        json!({ "inGameId": f.kate.id() }),
        json!({ "inGameId": f.sam.id() }),
    );
    let unlinked = |players: Vec<&str>| {
        (
            StatusCode::BAD_REQUEST,
            json!({ "code": "player_unlinked", "players": players }),
        )
    };

    // An id that is not an Ember ID, and a player named by an account of
    // BluMint's rather than an Ember ID.
    let junk = naming(
        &f,
        json!([kate.clone()]),
        json!([{ "inGameId": "not-an-ember-id" }]),
    );
    assert_eq!(
        create_with(&f, "set-1", junk).await,
        unlinked(vec!["not-an-ember-id"])
    );
    let account = naming(
        &f,
        json!([kate.clone()]),
        json!([{ "provider": "discord", "inGameId": SAM }]),
    );
    assert_eq!(create_with(&f, "set-1", account).await, unlinked(vec![SAM]));
    // Every id the bridge does not recognise is named, from both players,
    // and a player carrying several names all of theirs.
    let both = naming(
        &f,
        json!([{ "provider": "discord", "inGameId": KATE }, { "inGameId": "other-id" }]),
        json!([{ "provider": "discord", "inGameId": SAM }]),
    );
    assert_eq!(
        create_with(&f, "set-1", both).await,
        unlinked(vec![KATE, "other-id", SAM])
    );
    // An Ember ID with no link is named with them, in the order of the players.
    let stranger = f.bridge.player(9);
    let mixed = naming(
        &f,
        json!([{ "inGameId": stranger.id() }]),
        json!([{ "inGameId": "not-an-ember-id" }]),
    );
    assert_eq!(
        create_with(&f, "set-1", mixed).await,
        unlinked(vec![stranger.id().as_str(), "not-an-ember-id"])
    );
    // Nothing was made or stored against the key, so BluMint's retry under
    // it, with ids the bridge recognises, makes the match.
    assert_eq!(matches(&f).await, 0);
    let (status, created) = create(&f, "set-1", 2).await;
    assert_eq!(status, StatusCode::OK, "{created}");

    // What breaks the rules stays final, and is checked first: a player with
    // no identity at all or with two Ember IDs, the same player on both
    // teams, a set longer than Ember plays, a missing length.
    let junk = || json!([{ "inGameId": "not-an-ember-id" }]);
    let mut long = naming(&f, junk(), junk());
    long["series"]["gamesToWin"] = json!(11);
    let mut lengthless = naming(&f, junk(), junk());
    lengthless["series"] = json!(null);
    for (body, expected) in [
        (naming(&f, json!([]), junk()), "invalid_request"),
        (
            naming(&f, json!([kate.clone(), sam.clone()]), junk()),
            "invalid_request",
        ),
        (naming(&f, junk(), junk()), "player_unlinked"),
        (long, "match_unsupported"),
        (lengthless, "invalid_request"),
    ] {
        let (status, refused) = create_with(&f, "set-2", body.clone()).await;
        assert_eq!(status, StatusCode::BAD_REQUEST, "{body}: {refused}");
        assert_eq!(refusal(&refused), expected, "{body}");
    }
    let same = naming(&f, json!([kate.clone()]), json!([kate]));
    let (_, refused) = create_with(&f, "set-2", same).await;
    assert_eq!(refusal(&refused), "invalid_request");
}

/// The digest a creation was stored with, which a repeat of it is compared by.
async fn create_digest(f: &Fixture, key: &str) -> String {
    let external_id = format!("blumint:{key}");
    f.bridge
        .state()
        .db
        .read(move |tx| {
            Ok(tx.query_row(
                "SELECT create_digest FROM matches WHERE external_match_id = ?1",
                [external_id],
                |row| row.get(0),
            )?)
        })
        .await
        .unwrap()
}

// A key's repeats are compared with the digest its first request stored, and
// matches made by a bridge already running keep theirs through an upgrade. So
// the digest of a known creation is pinned: if these bytes ever change, every
// live match's repeat under its key would answer 409 idempotency_key_reused.
// Kate and Sam are the fixture's players 1 and 2, and a whole set of two games
// starts from 0-0.
#[tokio::test]
async fn a_creations_digest_is_the_one_stored_with_live_matches() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (status, created) = create(&f, "set-1", 2).await;
    assert_eq!(status, StatusCode::OK, "{created}");
    assert_eq!(
        create_digest(&f, "set-1").await,
        "e_X3PLqjNU72v1lsjMrPsFxYZkvgukS8F_aUCsKBYdY"
    );
    // A set that starts from a score has it in the digest too, once the first
    // is closed and its players are free.
    close(
        &f,
        json!({ "idempotencyKey": "set-1", "reason": "decided" }),
    )
    .await;
    let mut resumed = request(&f, 3);
    resumed["teams"][0]["startingWins"] = json!(2);
    resumed["teams"][1]["startingWins"] = json!(1);
    let (status, created) = create_with(&f, "set-2", resumed).await;
    assert_eq!(status, StatusCode::OK, "{created}");
    assert_eq!(
        create_digest(&f, "set-2").await,
        "g_MhxCNtSwDfbzUV8w5tYIcmXv3h3S-iD_-Fl72pWJQ"
    );
}

// The key is answered before the players are looked at. A key a close
// refused, or one that already made a different set, is a 409 whatever the
// creation names; "a lobby made from it would have no players and might never
// be closed", and BluMint must not be told to look the players up again.
#[tokio::test]
async fn a_key_is_answered_before_the_players_are_matched() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let reused = (
        StatusCode::CONFLICT,
        json!({ "code": "idempotency_key_reused" }),
    );
    let stranger = || {
        naming(
            &f,
            json!([{ "inGameId": f.kate.id() }]),
            json!([{ "inGameId": "not-an-ember-id" }]),
        )
    };

    // A key that made a match, asked again for a set it cannot seat.
    let (status, created) = create(&f, "set-1", 2).await;
    assert_eq!(status, StatusCode::OK, "{created}");
    assert_eq!(create(&f, "set-1", 2).await, (status, created));
    assert_eq!(create_with(&f, "set-1", stranger()).await, reused);

    // A key a close refused, with a set it can seat and one it cannot.
    let (closed, _) = close(
        &f,
        json!({ "idempotencyKey": "set-1", "reason": "decided" }),
    )
    .await;
    assert_eq!(closed, StatusCode::OK);
    assert_eq!(create_with(&f, "set-1", stranger()).await, reused);
    assert_eq!(create(&f, "set-1", 2).await, reused);

    // A key the close met first, before any creation.
    close(
        &f,
        json!({ "idempotencyKey": "set-2", "reason": "decided" }),
    )
    .await;
    assert_eq!(create_with(&f, "set-2", stranger()).await, reused);
    assert_eq!(matches(&f).await, 1);
}
