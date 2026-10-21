//! The helper's ranking requests against a real bridge on loopback: a
//! player's standing, the board, the refusals the game words apart, and a
//! bridge that has no ranking.
#![cfg(windows)]

mod public_rooms_support;

use public_rooms_support::*;
use serde_json::json;

#[tokio::test]
async fn a_player_reads_their_standing_and_the_board() {
    let dirs = dirs("ranking");
    let (_supervisor, url) = fake_supervisor().await;
    let (running, origin) = bridge(&dirs.0, Some(url)).await;
    let mut worker = Worker::start(&dirs, &"a".repeat(64));
    let (_, bridge_id) = worker.join(&origin).await;

    let standing = json!({ "op": "ranking_standing", "bridge_id": bridge_id });
    assert_eq!(
        ok(worker.ask(standing).await),
        json!({ "ranking": "no_account" })
    );
    let opt_out =
        json!({ "op": "ranking_choose", "bridge_id": bridge_id, "choice": { "ranked": false } });
    assert_eq!(refused(worker.ask(opt_out).await), "no_discord_account");
    // This bridge keeps classes hidden, so its board is empty.
    let board = json!({ "op": "ranking_board", "bridge_id": bridge_id });
    assert_eq!(ok(worker.ask(board).await), json!({ "players": [] }));
    // A listing the bridge would refuse is refused before any bridge is asked.
    let invalid = json!({ "op": "ranking_choose", "bridge_id": "brg_never_approved",
        "choice": { "ranked": true, "listing": { "name": " Player 1" } } });
    assert_eq!(refused(worker.ask(invalid).await), "invalid_request");
    worker.task.abort();
    running.abort();
}

#[tokio::test]
async fn a_bridge_without_ranking_is_told_apart() {
    let dirs = dirs("no-ranking");
    let (running, origin) = bridge(&dirs.0, None).await;
    let mut worker = Worker::start(&dirs, &"a".repeat(64));
    let (_, bridge_id) = worker.join(&origin).await;
    for op in ["ranking_standing", "ranking_board"] {
        let request = json!({ "op": op, "bridge_id": bridge_id });
        assert_eq!(refused(worker.ask(request).await), "ranking_unavailable");
    }
    worker.task.abort();
    running.abort();
}
