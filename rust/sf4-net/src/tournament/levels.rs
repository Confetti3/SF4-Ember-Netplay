//! Fighter levels for the native side (docs/design/FIGHTER_LEVELS.md, "Wire
//! contracts"): the player's own levels and the look-up of other players'
//! cards. Like the ranking routes these use the player session alone, and
//! what the bridge answers is checked before the game sees it. A bridge that
//! does not list `levels` is never asked, and is told apart as
//! `levels_unavailable`, as one that answers 404 is.
use std::sync::Arc;

use super::{
    Code, Failure, Outcome, Shared,
    client::{self, Body, BridgeCall, Refusals, Verb},
    play::{self, Need},
};
use ember_protocol::levels::{
    CardAnswers, CardLookup, CardSelector, LEVELS_PATH, LOOKUP_PATH, OwnLevels,
};

/// A 404 is a bridge without the routes; no reason is worded apart.
const REFUSALS: Refusals<'static> = Refusals {
    reasons: &[],
    missing: Some(Code::LevelsUnavailable),
};

/// The bridge's capabilities list `levels`, or this is `levels_unavailable`
/// and nothing is asked of it. One that lacks the feature is read again on
/// the next request, so a bridge upgraded meanwhile is heard.
async fn offered(shared: &Arc<Shared>, bridge_id: &str) -> Result<(), Failure> {
    let bridge = client::approved(shared, bridge_id)?;
    if play::features(shared, &bridge, Need::Levels).await?.levels {
        Ok(())
    } else {
        Err(Failure::new(Code::LevelsUnavailable))
    }
}

/// The player's own levels on `bridge_id`.
pub async fn own(shared: &Arc<Shared>, bridge_id: &str) -> Outcome {
    offered(shared, bridge_id).await?;
    let call = BridgeCall::new(Verb::Get, LEVELS_PATH, &[200]).refusals(REFUSALS);
    let levels: OwnLevels = client::request_as(shared, bridge_id, &call).await?;
    levels.check().map_err(|_| invalid_response())?;
    to_json(&levels)
}

/// One card, or none, for each of `cards`, in order.
pub async fn lookup(shared: &Arc<Shared>, bridge_id: &str, cards: Vec<CardSelector>) -> Outcome {
    let lookup = CardLookup { cards };
    lookup
        .check()
        .map_err(|_| Failure::new(Code::InvalidRequest))?;
    offered(shared, bridge_id).await?;
    let body = serde_json::to_vec(&lookup).map_err(|_| Failure::new(Code::Internal))?;
    let call = BridgeCall::new(Verb::Post, LOOKUP_PATH, &[200])
        .body(Body::Json(&body))
        .refusals(REFUSALS);
    let answers: CardAnswers = client::request_as(shared, bridge_id, &call).await?;
    answers
        .check_for(&lookup)
        .map_err(|_| invalid_response())?;
    to_json(&answers)
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
    use crate::tournament::{Lane, Request, test_support::shared_pinning};

    const TOKEN: &str = "c1.AbCdEf_-0123";
    const FRIEND: &str = "fr_11111111-1111-4111-8111-111111111111";

    fn reply(status: u16, body: &[u8]) -> Result<Value, Failure> {
        client::answer(status, body, &[200], &REFUSALS)
    }

    fn read<T: serde::de::DeserializeOwned>(body: &[u8]) -> Result<T, Failure> {
        client::read(200, body, &[200], &REFUSALS)
    }

    #[test]
    fn requests_have_the_shapes_the_game_sends() {
        let own = json!({ "op": "levels_me", "bridge_id": "brg_x" });
        let lookup = json!({ "op": "cards_lookup", "bridge_id": "brg_x",
            "cards": [{ "card": TOKEN }, { "friend": FRIEND }] });
        for (value, op) in [(own, "levels_me"), (lookup, "cards_lookup")] {
            let parsed: Request = serde_json::from_value(value.clone()).expect("a request");
            assert_eq!(parsed.op(), op);
            assert_eq!(parsed.lane(), Lane::Bridge);
            assert_eq!(serde_json::to_value(&parsed).unwrap(), value);
        }
    }

    #[test]
    fn requests_with_unknown_members_or_selectors_are_refused() {
        for bad in [
            json!({ "op": "levels_me" }),
            json!({ "op": "levels_me", "bridge_id": "brg_x", "extra": 1 }),
            json!({ "op": "cards_lookup", "bridge_id": "brg_x" }),
            json!({ "op": "cards_lookup", "bridge_id": "brg_x", "cards": [], "extra": 1 }),
            json!({ "op": "cards_lookup", "bridge_id": "brg_x", "cards": [{ "code": TOKEN }] }),
            json!({ "op": "cards_lookup", "bridge_id": "brg_x",
                "cards": [{ "card": TOKEN, "friend": FRIEND }] }),
            json!({ "op": "cards_lookup", "bridge_id": "brg_x", "cards": [{ "card": 1 }] }),
        ] {
            assert!(
                serde_json::from_value::<Request>(bad.clone()).is_err(),
                "{bad}"
            );
        }
    }

    // A look-up the bridge would refuse is refused before any bridge is asked.
    #[tokio::test]
    async fn a_malformed_look_up_is_refused_before_it_is_asked() {
        let dir = std::env::temp_dir().join(format!("sf4-net-levels-{}", std::process::id()));
        let shared = shared_pinning(&dir, &[]);
        let refused = |cards: Vec<CardSelector>| {
            let shared = shared.clone();
            async move {
                lookup(&shared, "brg_never_approved", cards)
                    .await
                    .unwrap_err()
                    .as_str()
                    .to_owned()
            }
        };
        assert_eq!(refused(vec![]).await, "invalid_request");
        assert_eq!(
            refused(vec![CardSelector::Card("fr_x".into())]).await,
            "invalid_request"
        );
        assert_eq!(
            refused(vec![CardSelector::Card(TOKEN.into()); 17]).await,
            "invalid_request"
        );
        // Well formed, it reaches the bridge it names, which was never approved.
        assert_eq!(
            refused(vec![CardSelector::Card(TOKEN.into())]).await,
            "bridge_not_approved"
        );
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn a_bridge_without_levels_is_told_apart() {
        assert_eq!(reply(404, b"").unwrap_err().as_str(), "levels_unavailable");
        let not_found = serde_json::to_vec(&json!({ "error": {
            "code": "not_found", "message": "m", "retryable": false,
            "request_id": "req_1", "details": {},
        }}))
        .unwrap();
        assert_eq!(
            reply(404, &not_found).unwrap_err().as_str(),
            "levels_unavailable"
        );
        let limited = serde_json::to_vec(&json!({ "error": {
            "code": "rate_limited", "message": "m", "retryable": false,
            "request_id": "req_1", "details": {},
        }}))
        .unwrap();
        assert_eq!(reply(429, &limited).unwrap_err().as_str(), "rate_limited");
        assert_eq!(reply(502, b"").unwrap_err().as_str(), "http_502");
    }

    #[test]
    fn what_the_bridge_answers_is_read_strictly() {
        let counted = br#"{"levels":"counted","shown":true,"fighters":[
            {"fighter":3,"level":42,"games":1000,"wins":900,"floor":1845,"next":1932,"since":1}]}"#;
        assert!(read::<OwnLevels>(counted).unwrap().check().is_ok());
        for bad in [
            &br#"{"levels":"counted","fighters":[],"extra":1}"#[..],
            br#"{"levels":"top"}"#,
            br#"{"levels":"counted","fighters":[{"fighter":3,"level":42,"games":1,"wins":0,"floor":0,"next":2,"since":1,"extra":1}]}"#,
        ] {
            assert_eq!(
                read::<OwnLevels>(bad).unwrap_err().as_str(),
                "bridge_invalid_response"
            );
        }
        // A fighter whose numbers disagree parses but does not check.
        let wrong = read::<OwnLevels>(
            br#"{"levels":"counted","fighters":[{"fighter":3,"level":42,"games":5,"wins":9,"floor":0,"next":99,"since":1}]}"#,
        )
        .unwrap();
        assert!(wrong.check().is_err());

        // Another player's card holds no wins.
        let card = br#"{"cards":[{"fighters":[{"fighter":3,"level":31,"games":612,"wins":1}],"relation":"none"}]}"#;
        assert!(read::<CardAnswers>(card).is_err());
        // An answer with another count of entries than the request is refused.
        let answers = read::<CardAnswers>(br#"{"cards":[{"relation":"friend"},null]}"#).unwrap();
        let one = CardLookup {
            cards: vec![CardSelector::Card(TOKEN.into())],
        };
        assert!(answers.check_for(&one).is_err());
    }
}
