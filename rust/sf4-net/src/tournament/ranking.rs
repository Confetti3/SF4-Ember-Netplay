//! Rankings for the native side (docs/design/SKILL_BADGE.md, "Rankings"): the
//! player's own standing, their choice to take part, and the board of
//! classes. Like the room routes these use the player session alone, and
//! what the bridge answers is checked before the game sees it. A bridge
//! without the routes is told apart as `ranking_unavailable`.
use std::sync::Arc;

use super::{
    Code, Failure, Outcome, Shared,
    client::{self, Body, BridgeCall, Refusals, Verb},
    play::{self, Features, Need},
    rooms::check_game,
};
use ember_protocol::{
    rooms::is_default_game,
    skill::{
        BOARD_PATH, Board, CHOICE_TOO_SOON, NO_ACCOUNT, RankingChoice, STANDING_PATH, Standing,
    },
};

/// Refusals of the ranking routes that the game words apart.
const REASONS: [&str; 2] = [NO_ACCOUNT, CHOICE_TOO_SOON];
/// A 404 naming neither is a bridge without the routes.
const REFUSALS: Refusals<'static> = Refusals {
    reasons: &REASONS,
    missing: Some(Code::RankingUnavailable),
};

/// `path` for `game`'s ranking: as it was for SF4 Ember's, so its requests
/// are what they always were, and with `?game=` for another game's. A game
/// the protocol does not know is an invalid request.
fn game_path(path: &str, game: &str) -> Result<String, Failure> {
    check_game(game)?;
    Ok(if is_default_game(game) {
        path.to_owned()
    } else {
        format!("{path}?game={game}")
    })
}

/// Whether a bridge offering `offered` may be asked for `game`'s ranking:
/// SF4's always, as before; another game's only by a bridge that keeps
/// rankings per game, since an older one ignores `?game=` and would answer,
/// or change, the player's SF4 standing and board. Refused as a bridge
/// without rankings, which the game words as such.
fn games_offered(game: &str, offered: Features) -> Result<(), Failure> {
    if is_default_game(game) || offered.ranking_games {
        Ok(())
    } else {
        Err(Failure::new(Code::RankingUnavailable))
    }
}

/// `game_path`, once the bridge is known to keep `game`'s ranking apart: its
/// capabilities, read once a run, and again while it lacks the feature.
async fn path_for(
    shared: &Arc<Shared>,
    bridge_id: &str,
    path: &str,
    game: &str,
) -> Result<String, Failure> {
    let path = game_path(path, game)?;
    if !is_default_game(game) {
        let bridge = client::approved(shared, bridge_id)?;
        games_offered(game, play::features(shared, &bridge, Need::RankingGames).await?)?;
    }
    Ok(path)
}

/// The player's standing in `game` on `bridge_id`.
pub async fn standing(shared: &Arc<Shared>, bridge_id: &str, game: &str) -> Outcome {
    let path = path_for(shared, bridge_id, STANDING_PATH, game).await?;
    let call = BridgeCall::new(Verb::Get, &path, &[200]).refusals(REFUSALS);
    let standing: Standing = client::request_as(shared, bridge_id, &call).await?;
    standing.check().map_err(|_| invalid_response())?;
    to_json(&standing)
}

/// Stores the player's choice in `game` and answers their standing after it.
pub async fn choose(
    shared: &Arc<Shared>,
    bridge_id: &str,
    choice: RankingChoice,
    game: &str,
) -> Outcome {
    choice
        .check()
        .map_err(|_| Failure::new(Code::InvalidRequest))?;
    let path = path_for(shared, bridge_id, STANDING_PATH, game).await?;
    let body = serde_json::to_vec(&choice).map_err(|_| Failure::new(Code::Internal))?;
    let call = BridgeCall::new(Verb::Put, &path, &[200])
        .body(Body::Json(&body))
        .refusals(REFUSALS);
    let standing: Standing = client::request_as(shared, bridge_id, &call).await?;
    standing.check().map_err(|_| invalid_response())?;
    to_json(&standing)
}

/// `game`'s board of classes on `bridge_id`.
pub async fn board(shared: &Arc<Shared>, bridge_id: &str, game: &str) -> Outcome {
    let path = path_for(shared, bridge_id, BOARD_PATH, game).await?;
    let call = BridgeCall::new(Verb::Get, &path, &[200]).refusals(REFUSALS);
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
            json!({ "op": "ranking_board", "bridge_id": "brg_x", "game": 1 }),
        ] {
            assert!(
                serde_json::from_value::<Request>(bad.clone()).is_err(),
                "{bad}"
            );
        }
    }

    #[test]
    fn a_game_other_than_sf4s_is_named_and_sf4s_never_is() {
        // SFV Ember names its game; SF4 Ember's requests read back without one.
        for value in [
            json!({ "op": "ranking_standing", "bridge_id": "brg_x", "game": "sfv" }),
            json!({ "op": "ranking_board", "bridge_id": "brg_x", "game": "sfv" }),
            json!({ "op": "ranking_choose", "bridge_id": "brg_x",
                "choice": { "ranked": false }, "game": "sfv" }),
        ] {
            let parsed: Request = serde_json::from_value(value.clone()).expect("a request");
            assert_eq!(serde_json::to_value(&parsed).unwrap(), value);
        }
        let named = json!({ "op": "ranking_board", "bridge_id": "brg_x", "game": "usf4" });
        let parsed: Request = serde_json::from_value(named).expect("a request");
        assert_eq!(
            serde_json::to_value(&parsed).unwrap(),
            json!({ "op": "ranking_board", "bridge_id": "brg_x" })
        );
        // The paths: SF4's exactly as before, another game's with its key,
        // and a game the protocol does not know is refused before any call.
        assert_eq!(game_path(STANDING_PATH, "usf4").unwrap(), "/v1/ranking/me");
        assert_eq!(game_path(BOARD_PATH, "usf4").unwrap(), "/v1/ranking/board");
        assert_eq!(
            game_path(STANDING_PATH, "sfv").unwrap(),
            "/v1/ranking/me?game=sfv"
        );
        assert_eq!(
            game_path(BOARD_PATH, "sfv").unwrap(),
            "/v1/ranking/board?game=sfv"
        );
        // Another game asks only a bridge that keeps rankings per game; SF4's
        // asks any bridge with rankings, as before.
        let old = Features {
            ranking: true,
            ..Features::default()
        };
        let per_game = Features {
            ranking_games: true,
            ..old
        };
        assert!(games_offered("usf4", old).is_ok());
        assert_eq!(
            games_offered("sfv", old).unwrap_err().as_str(),
            "ranking_unavailable"
        );
        assert!(games_offered("sfv", per_game).is_ok());
        for unknown in ["sf6", "", "SFV", "sfv&x=1"] {
            assert_eq!(
                game_path(BOARD_PATH, unknown).unwrap_err().as_str(),
                "invalid_request",
                "{unknown}"
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
