//! Points for the native side (docs/design/FIGHTER_LEVELS.md, "Wire
//! contracts"): the player's own account and fighter points and the look-up of
//! other players' cards. Like the ranking routes these use the player session alone, and
//! what the bridge answers is checked before the game sees it. A bridge that
//! does not list `points` is never asked, and is told apart as
//! `points_unavailable`, as one that answers 404 is.
//!
//! The profile adds what the player's card shows, saved to the bridge, a
//! search for other players, and two more look-up selectors (a received
//! request and a friend code; docs/design/PROFILE.md, "Helper ops"). These
//! are sent only to a bridge that lists `profiles`, and one that does not is
//! `profiles_unavailable`.
use std::sync::Arc;

use super::{
    Code, Failure, Outcome, Shared,
    client::{self, Body, BridgeCall, Refusals, Verb},
    play::{self, Need},
};
use ember_protocol::{
    levels::{
        CARDS_ME_PATH, CLASS_BONUS_POINTS_PATH, CardAnswers, CardLookup, CardSave, CardSearch,
        CardSelector, LOOKUP_PATH, OwnLevels, POINTS_PATH, SEARCH_PATH, SearchAnswer,
    },
    skill::NO_ACCOUNT,
};

/// A 404 is a bridge without the routes; no reason is worded apart.
const REFUSALS: Refusals<'static> = Refusals {
    reasons: &[],
    missing: Some(Code::LevelsUnavailable),
};

/// Refusals of the profile routes that the game words apart: a player with
/// no Discord account has no card. A 404 naming it is no bridge without the
/// routes; any other is. A bridge that is asked too often answers
/// `rate_limited`, which needs no table.
const PROFILE_REASONS: [&str; 1] = [NO_ACCOUNT];
const PROFILE_REFUSALS: Refusals<'static> = Refusals {
    reasons: &PROFILE_REASONS,
    missing: Some(Code::ProfilesUnavailable),
};

/// The statuses a bridge saves a card with.
const SAVED: &[u16] = &[204, 200];

/// The bridge's capabilities list `points`, or this is `points_unavailable`
/// and nothing is asked of it. One that lacks the feature is read again on
/// the next request, so a bridge upgraded meanwhile is heard. Answers what
/// the bridge offers.
async fn offered(shared: &Arc<Shared>, bridge_id: &str) -> Result<play::Features, Failure> {
    let bridge = client::approved(shared, bridge_id)?;
    let features = play::features(shared, &bridge, Need::Levels).await?;
    if features.levels {
        Ok(features)
    } else {
        Err(Failure::new(Code::LevelsUnavailable))
    }
}

/// Where the player's own points are asked. A bridge that lists the class
/// bonus answers the points as they are earned only when asked with its
/// query: asked plainly it answers games plus wins, for the builds that
/// refuse anything else. A bridge from before the bonus is asked plainly.
fn points_path(offered: &play::Features) -> &'static str {
    if offered.class_bonus {
        CLASS_BONUS_POINTS_PATH
    } else {
        POINTS_PATH
    }
}

/// The bridge's capabilities list `profiles`, or this is
/// `profiles_unavailable` and nothing is asked of it. One that lacks the
/// feature is read again on the next request, so a bridge upgraded meanwhile
/// is heard.
async fn profiles_offered(shared: &Arc<Shared>, bridge_id: &str) -> Result<(), Failure> {
    let bridge = client::approved(shared, bridge_id)?;
    if play::features(shared, &bridge, Need::Profiles).await?.profiles {
        Ok(())
    } else {
        Err(Failure::new(Code::ProfilesUnavailable))
    }
}

/// The player's own points on `bridge_id`.
pub async fn own(shared: &Arc<Shared>, bridge_id: &str) -> Outcome {
    let path = points_path(&offered(shared, bridge_id).await?);
    let call = BridgeCall::new(Verb::Get, path, &[200]).refusals(REFUSALS);
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
    let profile_selector = |selector: &CardSelector| {
        matches!(selector, CardSelector::Request(_) | CardSelector::Code(_))
    };
    if lookup.cards.iter().any(profile_selector) {
        profiles_offered(shared, bridge_id).await?;
    }
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

/// Saves what the player's card shows, replacing the whole of it. The bridge
/// answers 204; the game is answered `{}`.
pub async fn save(shared: &Arc<Shared>, bridge_id: &str, card: CardSave) -> Outcome {
    card.check()
        .map_err(|_| Failure::new(Code::InvalidRequest))?;
    profiles_offered(shared, bridge_id).await?;
    let body = serde_json::to_vec(&card).map_err(|_| Failure::new(Code::Internal))?;
    let call = BridgeCall::new(Verb::Put, CARDS_ME_PATH, SAVED)
        .body(Body::Json(&body))
        .refusals(PROFILE_REFUSALS);
    put(shared, bridge_id, &call).await?;
    Ok(Some(serde_json::json!({})))
}

/// Puts `call` to the bridge `bridge_id`, which answers no body.
async fn put(shared: &Arc<Shared>, bridge_id: &str, call: &BridgeCall<'_>) -> Result<(), Failure> {
    client::with_session(shared, bridge_id, |bridge, token| async move {
        let (status, body) = client::send(shared, &bridge, Some(&token), call).await?;
        saved(status, &body)
    })
    .await
}

/// Whether the bridge's answer to a save is one that saved it, else why not.
fn saved(status: u16, body: &[u8]) -> Result<(), Failure> {
    if SAVED.contains(&status) {
        Ok(())
    } else {
        Err(client::refused(status, body, &PROFILE_REFUSALS))
    }
}

/// The players whose name is, or begins with, `name`, or whose friend code is
/// `code`: exactly one is given.
pub async fn search(
    shared: &Arc<Shared>,
    bridge_id: &str,
    name: Option<String>,
    code: Option<String>,
) -> Outcome {
    let search = CardSearch { name, code };
    search
        .check()
        .map_err(|_| Failure::new(Code::InvalidRequest))?;
    profiles_offered(shared, bridge_id).await?;
    let body = serde_json::to_vec(&search).map_err(|_| Failure::new(Code::Internal))?;
    let call = BridgeCall::new(Verb::Post, SEARCH_PATH, &[200])
        .body(Body::Json(&body))
        .refusals(PROFILE_REFUSALS);
    let answer: SearchAnswer = client::request_as(shared, bridge_id, &call).await?;
    answer.check().map_err(|_| invalid_response())?;
    to_json(&answer)
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
    const REQUEST: &str = "rq_11111111-1111-4111-8111-111111111111";

    fn reply(status: u16, body: &[u8]) -> Result<Value, Failure> {
        client::answer(status, body, &[200], &REFUSALS)
    }

    fn read<T: serde::de::DeserializeOwned>(body: &[u8]) -> Result<T, Failure> {
        client::read(200, body, &[200], &REFUSALS)
    }

    #[test]
    fn requests_have_the_shapes_the_game_sends() {
        let own = json!({ "op": "points_me", "bridge_id": "brg_x" });
        let lookup = json!({ "op": "cards_lookup", "bridge_id": "brg_x",
            "cards": [{ "card": TOKEN }, { "friend": FRIEND }] });
        for (value, op) in [(own, "points_me"), (lookup, "cards_lookup")] {
            let parsed: Request = serde_json::from_value(value.clone()).expect("a request");
            assert_eq!(parsed.op(), op);
            assert_eq!(parsed.lane(), Lane::Bridge);
            assert_eq!(serde_json::to_value(&parsed).unwrap(), value);
        }
    }

    #[test]
    fn requests_with_unknown_members_or_selectors_are_refused() {
        for bad in [
            json!({ "op": "points_me" }),
            json!({ "op": "points_me", "bridge_id": "brg_x", "extra": 1 }),
            json!({ "op": "cards_lookup", "bridge_id": "brg_x" }),
            json!({ "op": "cards_lookup", "bridge_id": "brg_x", "cards": [], "extra": 1 }),
            json!({ "op": "cards_lookup", "bridge_id": "brg_x", "cards": [{ "name": TOKEN }] }),
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
                    .into_owned()
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
    fn class_bonus_points_are_asked_only_of_a_bridge_that_lists_them() {
        let plain = play::Features {
            levels: true,
            ..play::Features::default()
        };
        assert_eq!(points_path(&plain), POINTS_PATH);
        let bonus = play::Features {
            class_bonus: true,
            ..plain
        };
        assert_eq!(points_path(&bonus), CLASS_BONUS_POINTS_PATH);
    }

    #[test]
    fn a_bridge_without_levels_is_told_apart() {
        assert_eq!(reply(404, b"").unwrap_err().as_str(), "points_unavailable");
        let not_found = serde_json::to_vec(&json!({ "error": {
            "code": "not_found", "message": "m", "retryable": false,
            "request_id": "req_1", "details": {},
        }}))
        .unwrap();
        assert_eq!(
            reply(404, &not_found).unwrap_err().as_str(),
            "points_unavailable"
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
    fn what_the_bridge_answers_is_read_leniently_about_members_and_strictly_about_values() {
        let counted = br#"{"points":"counted",
            "account":{"points":1900,"games":1000,"wins":900,"tier":1,"floor":500,"next":2000,"since":1},
            "fighters":[
            {"fighter":3,"points":1900,"games":1000,"wins":900,"tier":3,"floor":1500,"next":2500,"since":1}]}"#;
        assert!(read::<OwnLevels>(counted).unwrap().check().is_ok());
        // An unknown state is refused.
        assert_eq!(
            read::<OwnLevels>(br#"{"points":"top"}"#)
                .unwrap_err()
                .as_str(),
            "bridge_invalid_response"
        );
        // A member a later bridge adds is ignored.
        let extra = br#"{"points":"counted","extra":1,"account":{"points":1,"games":1,"wins":0,"tier":0,"floor":0,"next":500,"since":1,"extra":1},"fighters":[{"fighter":3,"points":1,"games":1,"wins":0,"tier":0,"floor":0,"next":200,"since":1,"extra":1}]}"#;
        assert!(read::<OwnLevels>(extra).unwrap().check().is_ok());
        // An answer without the account is refused: it is always sent.
        assert_eq!(
            read::<OwnLevels>(br#"{"points":"counted","fighters":[]}"#)
                .unwrap_err()
                .as_str(),
            "bridge_invalid_response"
        );
        // Points between the games and the games plus the wins check: a win
        // earns its second point only with the class bonus.
        let bonus = br#"{"points":"counted",
            "account":{"points":1400,"games":1000,"wins":900,"tier":1,"floor":500,"next":2000,"since":1},
            "fighters":[
            {"fighter":3,"points":1000,"games":1000,"wins":900,"tier":2,"floor":700,"next":1500,"since":1}]}"#;
        assert!(read::<OwnLevels>(bonus).unwrap().check().is_ok());
        // Fewer points than games do not.
        let short = br#"{"points":"counted","account":{"points":999,"games":1000,"wins":900,"tier":1,"floor":500,"next":2000,"since":1},"fighters":[]}"#;
        assert!(read::<OwnLevels>(short).unwrap().check().is_err());
        // A fighter whose numbers disagree parses but does not check.
        let wrong = read::<OwnLevels>(
            br#"{"points":"counted","account":{"points":0,"games":0,"wins":0,"tier":0,"floor":0,"next":500},"fighters":[{"fighter":3,"points":14,"games":5,"wins":9,"tier":0,"floor":0,"next":200,"since":1}]}"#,
        )
        .unwrap();
        assert!(wrong.check().is_err());
        // So does an account whose numbers disagree.
        let wrong_account = read::<OwnLevels>(
            br#"{"points":"counted","account":{"points":14,"games":5,"wins":9,"tier":0,"floor":0,"next":500,"since":1},"fighters":[]}"#,
        )
        .unwrap();
        assert!(wrong_account.check().is_err());

        // Another player's card is read without what it does not know, and a
        // relation it does not know is none.
        let card = br#"{"cards":[{"account":{"points":2100,"tier":2},"fighters":[{"fighter":3,"points":1986,"tier":3,"wins":1}],"relation":"none"}]}"#;
        assert!(read::<CardAnswers>(card).is_ok());
        let later = br#"{"cards":[{"account":{"points":0,"tier":0},"relation":"blocked_by"}]}"#;
        assert!(read::<CardAnswers>(later).is_ok());
        // A card without its account is refused.
        assert!(read::<CardAnswers>(br#"{"cards":[{"relation":"none"}]}"#).is_err());
        // An answer with another count of entries than the request is refused.
        let answers = read::<CardAnswers>(
            br#"{"cards":[{"account":{"points":0,"tier":0},"relation":"friend"},null]}"#,
        )
        .unwrap();
        let one = CardLookup {
            cards: vec![CardSelector::Card(TOKEN.into())],
        };
        assert!(answers.check_for(&one).is_err());
    }
    // The award counters ride through to the game as the bridge sent them, and
    // an answer without them is no worse for it.
    #[test]
    fn award_counters_pass_through_and_are_optional() {
        let with = br#"{"points":"counted",
            "account":{"points":13,"games":9,"wins":4,"tier":0,"floor":0,"next":500,"since":1,"best_streak":3,"peak_class":6},
            "fighters":[]}"#;
        let levels = read::<OwnLevels>(with).unwrap();
        assert!(levels.check().is_ok());
        let value = to_json(&levels).unwrap().unwrap();
        assert_eq!(value["account"]["best_streak"], 3);
        assert_eq!(value["account"]["peak_class"], 6);
        let without = br#"{"points":"counted",
            "account":{"points":13,"games":9,"wins":4,"tier":0,"floor":0,"next":500,"since":1},
            "fighters":[]}"#;
        let value = to_json(&read::<OwnLevels>(without).unwrap()).unwrap().unwrap();
        assert!(value["account"].get("best_streak").is_none());

        let card = br#"{"cards":[{"account":{"points":2,"tier":0},"fighters":[],"relation":"none",
            "awards":{"games":100,"wins":50,"best_streak":5,"peak_class":6}}]}"#;
        let answers = read::<CardAnswers>(card).unwrap();
        let value = to_json(&answers).unwrap().unwrap();
        assert_eq!(value["cards"][0]["awards"]["games"], 100);
        assert_eq!(value["cards"][0]["awards"]["peak_class"], 6);
        // A class past the top fails the answer's check.
        let high = br#"{"cards":[{"account":{"points":2,"tier":0},"relation":"none","awards":{"peak_class":11}}]}"#;
        assert!(read::<CardAnswers>(high).unwrap().check().is_err());
    }

    // The most played opponent rides through to the game typed, as the bridge
    // sent it, and an answer without one is no worse for it.
    #[test]
    fn the_most_played_opponent_passes_through_and_is_optional() {
        let with = br#"{"points":"counted",
            "account":{"points":13,"games":9,"wins":4,"tier":0,"floor":0,"next":500,"since":1,"rival":74},
            "fighters":[],
            "rival":{"card":"c1.AbCdEf_-0123","name":"Player 7","country":"US","badge":{"band":8,"settled":true},"won":41,"lost":33,"extra":1}}"#;
        let levels = read::<OwnLevels>(with).unwrap();
        assert!(levels.check().is_ok());
        let value = to_json(&levels).unwrap().unwrap();
        // The count of games against that player is the account's own member.
        assert_eq!(value["account"]["rival"], 74);
        assert_eq!(
            value["rival"],
            json!({"card": "c1.AbCdEf_-0123", "name": "Player 7", "country": "US",
                   "badge": {"band": 8, "settled": true}, "won": 41, "lost": 33})
        );
        // One without a flag or a class carries neither.
        let bare = br#"{"points":"counted",
            "account":{"points":2,"games":1,"wins":0,"tier":0,"floor":0,"next":500,"since":1},
            "fighters":[],
            "rival":{"card":"c1.AbCdEf_-0123","name":"Player 7","won":0,"lost":1}}"#;
        let value = to_json(&read::<OwnLevels>(bare).unwrap()).unwrap().unwrap();
        assert_eq!(
            value["rival"],
            json!({"card": "c1.AbCdEf_-0123", "name": "Player 7", "won": 0, "lost": 1})
        );
        // A bridge before it sends none, and none is written.
        let without = br#"{"points":"counted",
            "account":{"points":13,"games":9,"wins":4,"tier":0,"floor":0,"next":500,"since":1},
            "fighters":[]}"#;
        let value = to_json(&read::<OwnLevels>(without).unwrap()).unwrap().unwrap();
        assert!(value.get("rival").is_none());
        // An opponent that is not a card token, or with no games, fails the answer.
        for wrong in [
            br#"{"card":"x","name":"Player 7","won":1,"lost":1}"#.as_slice(),
            br#"{"card":"c1.AbCdEf_-0123","name":"Player 7","won":0,"lost":0}"#.as_slice(),
        ] {
            let body = format!(
                r#"{{"points":"counted","account":{{"points":13,"games":9,"wins":4,"tier":0,"floor":0,"next":500,"since":1}},"fighters":[],"rival":{}}}"#,
                std::str::from_utf8(wrong).unwrap()
            );
            assert!(read::<OwnLevels>(body.as_bytes()).unwrap().check().is_err(), "{body}");
        }
    }

    fn profile_reply(status: u16, body: &[u8]) -> Result<Value, Failure> {
        client::answer(status, body, &[200], &PROFILE_REFUSALS)
    }

    fn refusal_body(code: &str, reason: Option<&str>) -> Vec<u8> {
        let details = reason.map_or(json!({}), |reason| json!({ "reason": reason }));
        serde_json::to_vec(&json!({ "error": {
            "code": code, "message": "m", "retryable": false,
            "request_id": "req_1", "details": details,
        }}))
        .unwrap()
    }

    #[test]
    fn the_profile_requests_have_the_shapes_the_game_sends() {
        let save = json!({ "op": "card_save", "bridge_id": "brg_x", "card": {
            "listing": { "name": "Player 1", "country": "DE" }, "main": 3,
            "worn": { "title": "matches", "icon": "streak" }, "pins": ["rank", "wins"],
            "style": { "device": 2, "link": 1, "region": "euc1", "looking": 34 } } });
        let bare_save = json!({ "op": "card_save", "bridge_id": "brg_x",
            "card": { "listing": { "name": "Player 1" } } });
        let by_name = json!({ "op": "cards_search", "bridge_id": "brg_x", "name": "Play" });
        let by_code = json!({ "op": "cards_search", "bridge_id": "brg_x", "code": "4F2A-91CX" });
        let lookup = json!({ "op": "cards_lookup", "bridge_id": "brg_x",
            "cards": [{ "request": REQUEST }, { "code": "4F2A-91CX" }] });
        for (value, op) in [
            (save, "card_save"),
            (bare_save, "card_save"),
            (by_name, "cards_search"),
            (by_code, "cards_search"),
            (lookup, "cards_lookup"),
        ] {
            let parsed: Request = serde_json::from_value(value.clone()).expect("a request");
            assert_eq!(parsed.op(), op);
            assert_eq!(parsed.lane(), Lane::Bridge);
            assert_eq!(serde_json::to_value(&parsed).unwrap(), value);
        }
    }

    #[test]
    fn profile_requests_with_unknown_members_are_refused() {
        for bad in [
            json!({ "op": "card_save", "bridge_id": "brg_x" }),
            json!({ "op": "card_save", "bridge_id": "brg_x", "card": {} }),
            json!({ "op": "card_save", "bridge_id": "brg_x",
                "card": { "listing": { "name": "Player 1" }, "extra": 1 } }),
            json!({ "op": "card_save", "bridge_id": "brg_x",
                "card": { "listing": { "name": "Player 1", "extra": 1 } } }),
            json!({ "op": "card_save", "bridge_id": "brg_x", "extra": 1,
                "card": { "listing": { "name": "Player 1" } } }),
            json!({ "op": "cards_search", "bridge_id": "brg_x", "name": 1 }),
            json!({ "op": "cards_search", "bridge_id": "brg_x", "name": "Play", "extra": 1 }),
            json!({ "op": "cards_search", "name": "Play" }),
        ] {
            assert!(
                serde_json::from_value::<Request>(bad.clone()).is_err(),
                "{bad}"
            );
        }
    }

    // A save or search the bridge would refuse is refused before any bridge is
    // asked, and one that is well formed is asked only if the bridge is approved.
    #[tokio::test]
    async fn a_malformed_save_or_search_is_refused_before_it_is_asked() {
        let dir = std::env::temp_dir().join(format!("sf4-net-profiles-{}", std::process::id()));
        let shared = shared_pinning(&dir, &[]);
        let card = |change: &dyn Fn(&mut CardSave)| {
            let mut card: CardSave =
                serde_json::from_value(json!({ "listing": { "name": "Player 1" } })).unwrap();
            change(&mut card);
            card
        };
        let refused_save = |card: CardSave| {
            let shared = shared.clone();
            async move {
                save(&shared, "brg_never_approved", card)
                    .await
                    .unwrap_err()
                    .as_str()
                    .into_owned()
            }
        };
        assert_eq!(
            refused_save(card(&|card| card.pins = ["a", "b", "c", "d", "e"].map(String::from).to_vec())).await,
            "invalid_request"
        );
        assert_eq!(
            refused_save(card(&|card| card.style.looking = 0b111)).await,
            "invalid_request"
        );
        assert_eq!(
            refused_save(card(&|card| card.style.region = Some("mars".into()))).await,
            "invalid_request"
        );
        assert_eq!(
            refused_save(card(&|card| card.worn.title = Some("Not An Id".into()))).await,
            "invalid_request"
        );
        assert_eq!(refused_save(card(&|_| {})).await, "bridge_not_approved");

        let refused_search = |name: Option<&str>, code: Option<&str>| {
            let shared = shared.clone();
            let (name, code) = (name.map(str::to_owned), code.map(str::to_owned));
            async move {
                search(&shared, "brg_never_approved", name, code)
                    .await
                    .unwrap_err()
                    .as_str()
                    .into_owned()
            }
        };
        assert_eq!(refused_search(None, None).await, "invalid_request");
        assert_eq!(refused_search(Some("a"), None).await, "invalid_request");
        assert_eq!(refused_search(Some("Play"), Some("4F2A-91CX")).await, "invalid_request");
        assert_eq!(refused_search(None, Some("4F2A")).await, "invalid_request");
        assert_eq!(refused_search(Some("Play"), None).await, "bridge_not_approved");
        assert_eq!(refused_search(None, Some("4f2a 91cx")).await, "bridge_not_approved");
        // A look-up by request or code is asked of no bridge it is malformed for.
        assert_eq!(
            lookup(&shared, "brg_never_approved", vec![CardSelector::Code("4F2A".into())])
                .await
                .unwrap_err()
                .as_str(),
            "invalid_request"
        );
        assert_eq!(
            lookup(&shared, "brg_never_approved", vec![CardSelector::Request(FRIEND.into())])
                .await
                .unwrap_err()
                .as_str(),
            "invalid_request"
        );
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn a_save_is_answered_no_content_and_refused_in_the_words_the_game_reads() {
        assert!(saved(204, b"").is_ok());
        assert!(saved(200, b"{}").is_ok());
        // A bridge without the routes, whether it answers a body or none.
        assert_eq!(saved(404, b"").unwrap_err().as_str(), "profiles_unavailable");
        assert_eq!(
            saved(404, &refusal_body("not_found", None)).unwrap_err().as_str(),
            "profiles_unavailable"
        );
        // No Discord account is a reason the game words; too many saves and
        // a malformed one are the bridge's own codes.
        assert_eq!(
            saved(403, &refusal_body("forbidden", Some("no_discord_account")))
                .unwrap_err()
                .as_str(),
            "no_discord_account"
        );
        assert_eq!(
            saved(429, &refusal_body("rate_limited", None)).unwrap_err().as_str(),
            "rate_limited"
        );
        assert_eq!(
            saved(400, &refusal_body("invalid_request", None)).unwrap_err().as_str(),
            "invalid_request"
        );
        assert_eq!(saved(502, b"").unwrap_err().as_str(), "http_502");
        // Another 403 is no reason the game words.
        assert_eq!(
            saved(403, &refusal_body("forbidden", None)).unwrap_err().as_str(),
            "forbidden"
        );
    }

    #[test]
    fn a_search_without_the_routes_is_told_apart_and_its_answer_is_checked() {
        assert_eq!(
            profile_reply(404, b"").unwrap_err().as_str(),
            "profiles_unavailable"
        );
        assert_eq!(
            profile_reply(403, &refusal_body("forbidden", Some("no_discord_account")))
                .unwrap_err()
                .as_str(),
            "no_discord_account"
        );
        assert_eq!(
            profile_reply(429, &refusal_body("rate_limited", None)).unwrap_err().as_str(),
            "rate_limited"
        );

        // Members a later bridge adds are ignored, and a missing `more` is false.
        let answer = br#"{"players":[{"card":"c1.AbCdEf_-0123","name":"Player 2","country":"FR",
            "main":5,"badge":{"band":7,"settled":true},"since":1748736000,"extra":1}],"extra":1}"#;
        let parsed: SearchAnswer = read(answer).unwrap();
        assert!(parsed.check().is_ok() && !parsed.more);
        let value = to_json(&parsed).unwrap().unwrap();
        assert_eq!(value["players"][0]["name"], "Player 2");
        assert_eq!(value["players"][0]["since"], 1_748_736_000u64);
        assert_eq!(value["more"], false);
        // A hit with no card is no answer; one with a bad token or name does not check.
        assert!(
            serde_json::from_slice::<SearchAnswer>(br#"{"players":[{"name":"Player 2","since":1}]}"#)
                .is_err()
        );
        for bad in [
            br#"{"players":[{"card":"fr_x","name":"Player 2","since":1}]}"#.as_slice(),
            br#"{"players":[{"card":"c1.abc","name":"","since":1}]}"#.as_slice(),
        ] {
            assert!(read_answer(bad).check().is_err());
        }
        // More than twenty players do not check.
        let hit = r#"{"card":"c1.abc","name":"Player 2","since":1}"#;
        let many = format!(r#"{{"players":[{}]}}"#, vec![hit; 21].join(","));
        assert!(read_answer(many.as_bytes()).check().is_err());
        let twenty = format!(r#"{{"players":[{}],"more":true}}"#, vec![hit; 20].join(","));
        assert!(read_answer(twenty.as_bytes()).check().is_ok());
    }

    fn read_answer(body: &[u8]) -> SearchAnswer {
        read::<SearchAnswer>(body).unwrap()
    }

    // A card carries what its player chose to show: read leniently about
    // members and about numbers a later bridge adds, strictly about shapes.
    #[test]
    fn the_profile_fields_of_a_card_pass_through_to_the_game() {
        let card = br#"{"cards":[{"account":{"points":2100,"tier":2},"fighters":[],"relation":"friend",
            "awards":{"games":1200,"wins":700,"best_streak":9,"peak_class":7},
            "name":"Player 2","country":"FR","main":5,"badge":{"band":7,"settled":true},
            "since":1748736000,"worn":{"title":"matches","icon":"streak","extra":1},
            "pins":["rank","wins"],
            "style":{"device":9,"link":7,"region":"sae1","looking":255,"extra":1},
            "versus":{"won":4,"lost":3,"last_at":1731000000},
            "friend":"fr_11111111-1111-4111-8111-111111111111","extra":1}]}"#;
        let answers = read::<CardAnswers>(card).unwrap();
        assert!(answers.check().is_ok());
        let value = to_json(&answers).unwrap().unwrap();
        let card = &value["cards"][0];
        assert_eq!(card["awards"]["games"], 1200);
        assert_eq!(card["awards"]["wins"], 700);
        assert_eq!(card["name"], "Player 2");
        assert_eq!(card["since"], 1_748_736_000u64);
        assert_eq!(card["worn"], json!({ "title": "matches", "icon": "streak" }));
        assert_eq!(card["pins"], json!(["rank", "wins"]));
        assert_eq!(
            card["style"],
            json!({ "device": 9, "link": 7, "region": "sae1", "looking": 255 })
        );
        assert_eq!(card["versus"], json!({ "won": 4, "lost": 3, "last_at": 1_731_000_000u64 }));
        assert_eq!(card["friend"], FRIEND);
        // Shapes are checked: a bad family id or friend id fails the answer.
        for bad in [
            r#"{"cards":[{"account":{"points":0,"tier":0},"relation":"none","pins":["Rank"]}]}"#,
            r#"{"cards":[{"account":{"points":0,"tier":0},"relation":"none","worn":{"icon":"a b"}}]}"#,
            r#"{"cards":[{"account":{"points":0,"tier":0},"relation":"none","friend":"rq_x"}]}"#,
            r#"{"cards":[{"account":{"points":0,"tier":0},"relation":"none","main":44}]}"#,
            r#"{"cards":[{"account":{"points":0,"tier":0},"relation":"none","versus":{"won":0,"lost":0,"last_at":1}}]}"#,
        ] {
            assert!(read::<CardAnswers>(bad.as_bytes()).unwrap().check().is_err(), "{bad}");
        }
        // A card of a bridge before profiles has none of them, and says so by
        // writing none of them.
        let old = read::<CardAnswers>(br#"{"cards":[{"account":{"points":2,"tier":0},"fighters":[],"relation":"none"}]}"#)
            .unwrap();
        let value = to_json(&old).unwrap().unwrap();
        for absent in ["name", "country", "main", "badge", "since", "worn", "pins", "style", "versus", "friend"] {
            assert!(value["cards"][0].get(absent).is_none(), "{absent}");
        }
    }
}
