//! Player records: sets and games won and lost, computed from finished matches
//! when they are read (an extension beyond EMBER-TB-001).
mod common;

use common::league::fixture;
use reqwest::StatusCode;
use serde_json::{Value, json};

async fn record(f: &common::league::Fixture, token: &str, index: usize) -> (StatusCode, Value) {
    f.bridge
        .get(token, &format!("/v1/players/{}/record", f.id(index)))
        .await
}

#[tokio::test]
async fn records_follow_finished_matches() {
    let f = fixture(3).await;
    // 0 beats 1 two games to one; 2 beats 0; 1 and 2 never play theirs.
    let long = f.ordinary("record-long", 0, 1, 2).await;
    let long_id = long["match_id"].as_str().unwrap().to_owned();
    for winner in [0, 1, 0] {
        let (status, body) = f.game(&long_id, winner).await;
        assert_eq!(status, StatusCode::CREATED, "{body}");
    }
    let short = f.bracket("record-short", 0, 2).await;
    f.game(short["match_id"].as_str().unwrap(), 1).await;
    let skipped = f.bracket("record-skipped", 1, 2).await;
    let (status, body) = f
        .bridge
        .post_keyed(
            &f.provider,
            &format!(
                "/v1/matches/{}/cancel",
                skipped["match_id"].as_str().unwrap()
            ),
            json!({ "reason": "No show", "expected_revision": skipped["revision"] }),
            Some("record-skip"),
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{body}");

    let (status, mine) = record(&f, &f.provider, 0).await;
    assert_eq!(status, StatusCode::OK, "{mine}");
    assert_eq!(mine["sets"], json!({ "played": 2, "won": 1, "lost": 1 }));
    assert_eq!(mine["games"], json!({ "won": 2, "lost": 2, "drawn": 0 }));
    assert_eq!(mine["cancelled"], 0);
    let recent = mine["recent"].as_array().unwrap();
    assert_eq!(recent.len(), 2);
    assert_eq!(recent[0]["result"], "lost");
    assert_eq!(recent[0]["opponent"]["ember_id"], f.id(2));
    assert!(recent[0]["opponent"]["participant_id"].is_string());
    assert_eq!(
        (
            &recent[1]["result"],
            &recent[1]["wins"],
            &recent[1]["opponent_wins"]
        ),
        (&json!("won"), &json!(2), &json!(1))
    );

    let (_, theirs) = record(&f, &f.organizer, 1).await;
    assert_eq!(theirs["sets"], json!({ "played": 1, "won": 0, "lost": 1 }));
    assert_eq!(theirs["cancelled"], 1);
    assert_eq!(theirs["recent"][0]["result"], "cancelled");

    // A player reads their own record, without other players' account IDs,
    // and nobody else's.
    let (status, own) = record(&f, f.players[0].token(), 0).await;
    assert_eq!(status, StatusCode::OK, "{own}");
    assert_eq!(own["sets"], mine["sets"]);
    assert!(own["recent"][0]["opponent"].get("participant_id").is_none());
    let (status, _) = record(&f, f.players[0].token(), 1).await;
    assert_eq!(status, StatusCode::NOT_FOUND);

    // Another connection's provider sees none of these matches.
    let other = f.bridge.provider("mock-b").await;
    let (_, elsewhere) = record(&f, &other, 0).await;
    assert_eq!(
        elsewhere["sets"],
        json!({ "played": 0, "won": 0, "lost": 0 })
    );

    // Voiding the game player 1 won keeps the set finished and is reflected
    // at once.
    let (_, snapshot) = f
        .bridge
        .get(&f.organizer, &format!("/v1/matches/{long_id}"))
        .await;
    let attempt = snapshot["attempts"][1]["attempt_id"]
        .as_str()
        .unwrap()
        .to_owned();
    let (status, body) = f.void(&long_id, &attempt).await;
    assert_eq!(status, StatusCode::CREATED, "{body}");
    let (_, corrected) = record(&f, &f.provider, 0).await;
    assert_eq!(corrected["sets"], mine["sets"]);
    assert_eq!(
        corrected["games"],
        json!({ "won": 2, "lost": 1, "drawn": 0 })
    );
}
