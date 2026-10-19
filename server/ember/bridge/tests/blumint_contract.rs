//! The places the bridge's BluMint answers follow BluMint's partner docs
//! rather than the bridge's own habits: a close is answered `200` whatever
//! its reason, and a creation naming an id the bridge cannot match is
//! `player_unlinked`.
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
