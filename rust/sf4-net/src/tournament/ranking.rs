//! Rankings for the native side (docs/design/SKILL_BADGE.md, "Rankings"): the
//! player's own standing, their choice to take part, and the board of
//! classes. Like the room routes these use the player session alone, and
//! what the bridge answers is checked before the game sees it. A bridge
//! without the routes is told apart as `ranking_unavailable`.
use std::sync::Arc;

use super::{
    Code, Failure, Outcome, Shared,
    client::{self, Body, BridgeCall, Refusals, Verb},
};
use ember_protocol::skill::{
    BOARD_PATH, Board, CHOICE_TOO_SOON, NO_ACCOUNT, RankingChoice, STANDING_PATH, Standing,
};

/// Refusals of the ranking routes that the game words apart.
const REASONS: [&str; 2] = [NO_ACCOUNT, CHOICE_TOO_SOON];
/// A 404 naming neither is a bridge without the routes.
const REFUSALS: Refusals<'static> = Refusals {
    reasons: &REASONS,
    missing: Some(Code::RankingUnavailable),
};

/// The player's standing on `bridge_id`.
pub async fn standing(shared: &Arc<Shared>, bridge_id: &str) -> Outcome {
    let call = BridgeCall::new(Verb::Get, STANDING_PATH, &[200]).refusals(REFUSALS);
    let standing: Standing = client::request_as(shared, bridge_id, &call).await?;
    standing.check().map_err(|_| invalid_response())?;
    to_json(&standing)
}

/// Stores the player's choice and answers their standing after it.
pub async fn choose(shared: &Arc<Shared>, bridge_id: &str, choice: RankingChoice) -> Outcome {
    choice
        .check()
        .map_err(|_| Failure::new(Code::InvalidRequest))?;
    let body = serde_json::to_vec(&choice).map_err(|_| Failure::new(Code::Internal))?;
    let call = BridgeCall::new(Verb::Put, STANDING_PATH, &[200])
        .body(Body::Json(&body))
        .refusals(REFUSALS);
    let standing: Standing = client::request_as(shared, bridge_id, &call).await?;
    standing.check().map_err(|_| invalid_response())?;
    to_json(&standing)
}

/// The board of classes on `bridge_id`.
pub async fn board(shared: &Arc<Shared>, bridge_id: &str) -> Outcome {
    let call = BridgeCall::new(Verb::Get, BOARD_PATH, &[200]).refusals(REFUSALS);
    let board: Board = client::request_as(shared, bridge_id, &call).await?;
    board.check().map_err(|_| invalid_response())?;
    to_json(&board)
}

fn invalid_response() -> Failure {
    Failure::new(Code::BridgeInvalidResponse)
}

fn to_json<T: serde::Serialize>(value: &T) -> Outcome {
    serde_json::to_value(value)
        .map(Some)
        .map_err(|_| Failure::new(Code::Internal))
}

#[cfg(test)]
mod tests {
    use serde_json::{Value, json};

    use super::*;
    use crate::tournament::Request;

    fn reply(status: u16, body: &[u8]) -> Result<Value, Failure> {
        client::answer(status, body, &[200], &REFUSALS)
    }

    #[test]
    fn requests_have_the_shapes_the_game_sends() {
        let standing = json!({ "op": "ranking_standing", "bridge_id": "brg_x" });
        let choose = json!({ "op": "ranking_choose", "bridge_id": "brg_x",
            "choice": { "ranked": true, "listing": { "name": "Player 1", "country": "GB" } } });
        let opt_out =
            json!({ "op": "ranking_choose", "bridge_id": "brg_x", "choice": { "ranked": false } });
        let board = json!({ "op": "ranking_board", "bridge_id": "brg_x" });
        for (value, op) in [
            (standing, "ranking_standing"),
            (choose, "ranking_choose"),
            (opt_out, "ranking_choose"),
            (board, "ranking_board"),
        ] {
            let parsed: Request = serde_json::from_value(value.clone()).expect("a request");
            assert_eq!(parsed.op(), op);
            assert_eq!(parsed.lane(), crate::tournament::Lane::Bridge);
            assert_eq!(serde_json::to_value(&parsed).unwrap(), value);
        }
        for bad in [
            json!({ "op": "ranking_standing" }),
            json!({ "op": "ranking_board", "bridge_id": "brg_x", "extra": 1 }),
            json!({ "op": "ranking_choose", "bridge_id": "brg_x", "choice": { "ranked": "yes" } }),
        ] {
            assert!(
                serde_json::from_value::<Request>(bad.clone()).is_err(),
                "{bad}"
            );
        }
    }

    #[test]
    fn a_bridge_without_ranking_is_told_apart() {
        let refusal = |code: &str, reason: Option<&str>| {
            let details = reason.map_or(json!({}), |reason| json!({ "reason": reason }));
            serde_json::to_vec(&json!({ "error": {
                "code": code, "message": "m", "retryable": false, "request_id": "req_1", "details": details,
            }}))
            .unwrap()
        };
        assert_eq!(reply(404, b"").unwrap_err().as_str(), "ranking_unavailable");
        let body = refusal("not_found", None);
        assert_eq!(
            reply(404, &body).unwrap_err().as_str(),
            "ranking_unavailable"
        );
        for reason in REASONS {
            let body = refusal("forbidden", Some(reason));
            assert_eq!(reply(409, &body).unwrap_err().as_str(), reason);
        }
        let body = refusal("rate_limited", None);
        assert_eq!(reply(429, &body).unwrap_err().as_str(), "rate_limited");
        assert_eq!(
            reply(200, b"{\"players\":[]}").unwrap(),
            json!({ "players": [] })
        );
    }
}
