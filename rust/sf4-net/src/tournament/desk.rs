//! The tournament desk in the game: the organizer's events and one event's
//! sets, read with the player's session, and the desk's actions on a set
//! (hold or let go, call again, decide or void a game) and a player's
//! sign-up for an event, each proven for its
//! path as the bridge asks (`site.connect`). The bridge checks the event is
//! the player's; the helper only keeps the paths to the desk's own.
use std::sync::Arc;

use ember_protocol::{
    challenge::{Action, Method},
    encoding::is_prefixed_id,
};
use serde_json::{Map, Value, json};

use super::{
    Failure, Outcome, Shared, browser,
    client::{answer, feature_answer, prove, with_session},
};

const EVENTS: &str = "/v1/organizer/events";
const SIGN_UP: &str = "/v1/events/";
const WEB: &str = "/v1/web-sessions";
const HANDOFF: &str = "/v1/web-sessions/handoff";
const APPROVE: &str = "/v1/web-sessions/approve";
const DEVICE: &str = "/v1/web-sessions/device";
const ACCESS: &str = "/v1/organizer/access";
const REDEEM: &str = "/v1/organizer/invites/redeem";

/// The most sets an answer carries, and the longest text kept from one: an
/// answer goes to the game in one 64 KiB message.
// ponytail: a bracket past 160 sets shows its first 160; page by phase if one ever does.
const MAX_SETS: usize = 160;
const MAX_TEXT: usize = 64;
/// The most an event's sets weigh as the bridge sends them, with every
/// set's players, score and games: the bridge keeps up to 1024 sets.
const MAX_SETS_ANSWER: usize = 1024 * 1024;

/// The organizer's events, or `event`'s sets with what the desk shows of
/// each, when `event` is given.
pub async fn read(shared: &Arc<Shared>, bridge_id: &str, event: Option<&str>) -> Outcome {
    let path = match event {
        None => EVENTS.to_owned(),
        Some(event) if is_prefixed_id(event, "oev") => format!("{EVENTS}/{event}/sets"),
        Some(_) => return Err(Failure::new("invalid_request")),
    };
    with_session(shared, bridge_id, |bridge, token| {
        let path = path.clone();
        async move {
            // An event's sets outgrow the 64 KiB of other answers from
            // about 90 sets on; what goes to the game is cut down below.
            let (status, body) = shared
                .client
                .call_bounded(
                    reqwest::Method::GET,
                    &format!("{}{path}", bridge.origin),
                    Some(&token),
                    None,
                    "application/json",
                    if event.is_some() { MAX_SETS_ANSWER } else { 64 * 1024 },
                )
                .await?;
            let found = if status == 200 {
                large_answer(&body)?
            } else {
                answer(status, &body, &[200])?
            };
            if event.is_some() {
                return Ok(Some(json!({ "event": event, "sets": sets(&found["sets"]) })));
            }
            // Whether Tournament Mode is the player's yet, on a bridge that
            // takes organizers by invitation; a bridge without the route
            // takes everyone.
            let (status, body) = shared
                .client
                .call(
                    reqwest::Method::GET,
                    &format!("{}{ACCESS}", bridge.origin),
                    Some(&token),
                    None,
                )
                .await?;
            let access = match feature_answer(status, &body, 200, &[], "unsupported") {
                Ok(found) => json!({
                    "invite_only": found["invite_only"].as_bool().unwrap_or(false),
                    "allowed": found["allowed"].as_bool().unwrap_or(true),
                }),
                Err(failure) if failure.0 == "unsupported" => {
                    json!({ "invite_only": false, "allowed": true })
                }
                Err(failure) => return Err(failure),
            };
            Ok(Some(json!({ "events": events(&found["events"]), "access": access })))
        }
    })
    .await
}

/// An answer of up to `MAX_SETS_ANSWER` bytes, as `client::answer` reads
/// the smaller ones.
fn large_answer(body: &[u8]) -> Result<Value, Failure> {
    ember_protocol::json::parse(body, MAX_SETS_ANSWER)
        .and_then(|value| ember_protocol::json::from_value::<Value>(&value))
        .map_err(|_| Failure::new("bridge_invalid_response"))
}

/// A desk action on `path`, one of an event's set or match routes, proven
/// and sent with `command` as its body.
pub async fn act(shared: &Arc<Shared>, bridge_id: &str, path: &str, command: Value) -> Outcome {
    if !is_action(path) || !command.is_object() {
        return Err(Failure::new("invalid_request"));
    }
    with_session(shared, bridge_id, |bridge, token| {
        let command = command.clone();
        async move {
            let body = prove(
                shared,
                &bridge,
                Some(&token),
                Action::SiteConnect,
                Method::Post,
                path,
                command,
            )
            .await?;
            let (status, response) = shared
                .client
                .call(
                    reqwest::Method::POST,
                    &format!("{}{path}", bridge.origin),
                    Some(&token),
                    Some(body),
                )
                .await?;
            // A sign-up the bridge took is 201 Created; a set's action 200.
            answer(status, &response, &[200, 201])?;
            Ok(None)
        }
    })
    .await
}

/// The player's web desk sessions, after noting them seen (`seen`) or
/// ending them all (`end`, proven with `site.remove`).
pub async fn web(shared: &Arc<Shared>, bridge_id: &str, seen: bool, end: bool) -> Outcome {
    with_session(shared, bridge_id, |bridge, token| async move {
        let url = format!("{}{WEB}", bridge.origin);
        if end {
            let body = prove(
                shared,
                &bridge,
                Some(&token),
                Action::SiteRemove,
                Method::Delete,
                WEB,
                json!({}),
            )
            .await?;
            let (status, response) = shared
                .client
                .call(reqwest::Method::DELETE, &url, Some(&token), Some(body))
                .await?;
            answer(status, &response, &[200])?;
        } else if seen {
            let (status, response) = shared
                .client
                .call(
                    reqwest::Method::POST,
                    &format!("{url}/seen"),
                    Some(&token),
                    Some(b"{}".to_vec()),
                )
                .await?;
            answer(status, &response, &[200])?;
        }
        let (status, body) = shared
            .client
            .call(reqwest::Method::GET, &url, Some(&token), None)
            .await?;
        let found = answer(status, &body, &[200])?;
        let list = found["sessions"]
            .as_array()
            .map(Vec::as_slice)
            .unwrap_or_default();
        let sessions: Vec<Value> = list
            .iter()
            .take(20)
            .map(|session| {
                kept(
                    session,
                    &["site", "browser", "started_at", "last_used_at", "new"],
                )
            })
            .collect();
        Ok(Some(json!({ "sessions": sessions })))
    })
    .await
}

/// A one-time handoff to the web desk, proven with `site.connect`: its link
/// opens in this PC's browser, and its token stays in the helper.
pub async fn handoff(shared: &Arc<Shared>, bridge_id: &str) -> Outcome {
    with_session(shared, bridge_id, |bridge, token| async move {
        let body = prove(
            shared,
            &bridge,
            Some(&token),
            Action::SiteConnect,
            Method::Post,
            HANDOFF,
            json!({}),
        )
        .await?;
        let (status, response) = shared
            .client
            .call(
                reqwest::Method::POST,
                &format!("{}{HANDOFF}", bridge.origin),
                Some(&token),
                Some(body),
            )
            .await?;
        let made = answer(status, &response, &[201])?;
        let link = made["token"].as_str().unwrap_or_default();
        let well_formed = (16..=64).contains(&link.len())
            && link
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_');
        if !well_formed {
            return Err(Failure::new("bridge_invalid_response"));
        }
        browser::open(&format!("{}/desk/h/{link}", bridge.origin))?;
        Ok(Some(json!({ "desk": format!("{}/desk", bridge.origin) })))
    })
    .await
}

/// Another device's code as typed: its six digits, or nothing.
fn code_of(code: &str) -> Option<String> {
    let digits: String = code.chars().filter(char::is_ascii_digit).collect();
    (digits.len() == 6
        && code
            .chars()
            .all(|c| c.is_ascii_digit() || c == ' ' || c == '-'))
    .then_some(digits)
}

/// What asked for `code`, another device's code at the desk, and when, with
/// the three numbers the organizer picks from: that device shows one of them
/// now.
pub async fn code_info(shared: &Arc<Shared>, bridge_id: &str, code: &str) -> Outcome {
    let Some(code) = code_of(code) else {
        return Err(Failure::new("desk_code_refused"));
    };
    let code = &code;
    with_session(shared, bridge_id, |bridge, token| async move {
        let (status, body) = shared
            .client
            .call(
                reqwest::Method::GET,
                &format!("{}{DEVICE}?code={code}", bridge.origin),
                Some(&token),
                None,
            )
            .await?;
        if status == 404 {
            return Err(Failure::new("desk_code_refused"));
        }
        let found = answer(status, &body, &[200])?;
        info(&found).map(Some)
    })
    .await
}

/// What the game shows of a device's code: its name kept short, when it
/// asked, and exactly three numbers from 10 to 99.
fn info(found: &Value) -> Result<Value, Failure> {
    let choices: Vec<u64> = found["choices"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(Value::as_u64)
        .filter(|n| (10..100).contains(n))
        .collect();
    let Some(requested_at) = found["requested_at"].as_u64() else {
        return Err(Failure::new("bridge_invalid_response"));
    };
    if choices.len() != 3 {
        return Err(Failure::new("bridge_invalid_response"));
    }
    Ok(json!({
        "device": text(&found["device"]),
        "requested_at": requested_at,
        "choices": choices,
    }))
}

/// Approves `code`, shown by another device's browser at the desk, with
/// `number`, the one that device shows now (`code_info`), proven with
/// `site.connect`: that browser, and only it, signs in to the desk. A wrong
/// number ends the code.
pub async fn approve(shared: &Arc<Shared>, bridge_id: &str, code: &str, number: u8) -> Outcome {
    let Some(code) = code_of(code) else {
        return Err(Failure::new("desk_code_refused"));
    };
    if !(10..100).contains(&number) {
        return Err(Failure::new("desk_number_wrong"));
    }
    let code = &code;
    with_session(shared, bridge_id, |bridge, token| async move {
        let body = prove(
            shared,
            &bridge,
            Some(&token),
            Action::SiteConnect,
            Method::Post,
            APPROVE,
            json!({ "code": code, "number": number }),
        )
        .await?;
        let (status, response) = shared
            .client
            .call(
                reqwest::Method::POST,
                &format!("{}{APPROVE}", bridge.origin),
                Some(&token),
                Some(body),
            )
            .await?;
        if status == 404 {
            let wrong = serde_json::from_slice::<Value>(&response)
                .is_ok_and(|refused| refused["error"]["details"]["reason"] == "number_wrong");
            return Err(Failure::new(if wrong {
                "desk_number_wrong"
            } else {
                "desk_code_refused"
            }));
        }
        answer(status, &response, &[200])?;
        Ok(Some(json!({ "approved": true })))
    })
    .await
}

/// Redeems `code`, an invite to Tournament Mode, for the player's Ember ID.
pub async fn redeem(shared: &Arc<Shared>, bridge_id: &str, code: &str) -> Outcome {
    let code: String = code.trim().chars().take(16).collect();
    if code.is_empty() {
        return Err(Failure::new("invite_invalid"));
    }
    let code = &code;
    with_session(shared, bridge_id, |bridge, token| async move {
        let body = prove(
            shared,
            &bridge,
            Some(&token),
            Action::SiteConnect,
            Method::Post,
            REDEEM,
            json!({ "code": code }),
        )
        .await?;
        let (status, response) = shared
            .client
            .call(
                reqwest::Method::POST,
                &format!("{}{REDEEM}", bridge.origin),
                Some(&token),
                Some(body),
            )
            .await?;
        feature_answer(status, &response, 200, &["invite_invalid"], "invites_unsupported")?;
        Ok(Some(json!({ "allowed": true })))
    })
    .await
}

/// `/v1/organizer/events/{event}/sets/{set}`,
/// `/v1/organizer/events/{event}/matches/{match}/adjudications`, or a
/// player signing up for an event, `/v1/events/{event}/sign-up`.
fn is_action(path: &str) -> bool {
    if let Some(event) = path
        .strip_prefix(SIGN_UP)
        .and_then(|rest| rest.strip_suffix("/sign-up"))
    {
        return is_prefixed_id(event, "oev");
    }
    let Some(rest) = path
        .strip_prefix(EVENTS)
        .and_then(|rest| rest.strip_prefix('/'))
    else {
        return false;
    };
    match rest.split('/').collect::<Vec<_>>().as_slice() {
        [event, "sets", set] => is_prefixed_id(event, "oev") && is_set_id(set),
        [event, "matches", id, "adjudications"] => {
            is_prefixed_id(event, "oev") && is_prefixed_id(id, "emt")
        }
        _ => false,
    }
}

/// A set ID as the bridge takes it: 1 to 64 letters, digits, `-` or `_`.
fn is_set_id(id: &str) -> bool {
    (1..=64).contains(&id.len())
        && id
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_')
}

fn text(value: &Value) -> Value {
    match value.as_str() {
        Some(text) => json!(text.chars().take(MAX_TEXT).collect::<String>()),
        None => Value::Null,
    }
}

/// Only the fields of `value` named in `keys`, each text cut to `MAX_TEXT`.
fn kept(value: &Value, keys: &[&str]) -> Value {
    let mut out = Map::new();
    for key in keys {
        let field = &value[*key];
        let field = if field.is_string() {
            text(field)
        } else {
            field.clone()
        };
        if !field.is_null() {
            out.insert((*key).to_owned(), field);
        }
    }
    Value::Object(out)
}

fn events(found: &Value) -> Value {
    let list = found.as_array().map(Vec::as_slice).unwrap_or_default();
    list.iter()
        .take(32)
        .map(|event| kept(event, &["id", "title", "status", "site", "starts_at"]))
        .collect()
}

fn sets(found: &Value) -> Value {
    let list = found.as_array().map(Vec::as_slice).unwrap_or_default();
    list.iter()
        .take(MAX_SETS)
        .map(|set| {
            let mut kept = kept(
                set,
                &[
                    "id",
                    "round_label",
                    "round",
                    "phase",
                    "side",
                    "score",
                    "desk_state",
                    "held",
                    "called_at",
                    "match_id",
                    "revision",
                ],
            );
            let players: Vec<Value> = set["players"]
                .as_array()
                .map(|players| players.iter().take(2).map(text).collect())
                .unwrap_or_default();
            kept["players"] = json!(players);
            kept["posted"] = text(&set["call"]["posted"]);
            let games: Vec<Value> = set["games"]
                .as_array()
                .map(|games| games.iter().take(16).map(kept_game).collect())
                .unwrap_or_default();
            kept["games"] = json!(games);
            kept
        })
        .collect()
}

fn kept_game(game: &Value) -> Value {
    kept(game, &["attempt_id", "outcome", "state"])
}

#[cfg(test)]
mod tests {
    use super::*;

    const EVENT: &str = "oev_0f8fad5b-d9cb-469f-a165-70867728950e";
    const MATCH: &str = "emt_7c9e6679-7425-40de-944b-e07fc1f90ae7";

    #[test]
    fn only_the_desks_own_action_paths_are_sent() {
        assert!(is_action(&format!("{EVENTS}/{EVENT}/sets/1001")));
        assert!(is_action(&format!(
            "{EVENTS}/{EVENT}/matches/{MATCH}/adjudications"
        )));
        assert!(is_action(&format!("/v1/events/{EVENT}/sign-up")));
        for path in [
            format!("/v1/events/{EVENT}/sign-up/x"),
            format!("/v1/events/oev_1/sign-up"),
            format!("/v1/events/{EVENT}/../x/sign-up"),
            format!("{EVENTS}/{EVENT}"),
            format!("{EVENTS}/{EVENT}/sets/../x"),
            format!("{EVENTS}/{EVENT}/matches/{MATCH}/cancel"),
            format!("{EVENTS}/oev_1/sets/1"),
            format!("/v1/matches/{MATCH}/adjudications"),
        ] {
            assert!(!is_action(&path), "{path}");
        }
    }

    #[test]
    fn a_code_is_six_digits_however_spaced() {
        assert_eq!(code_of("123 456").as_deref(), Some("123456"));
        assert_eq!(code_of("123-456").as_deref(), Some("123456"));
        for wrong in ["12345", "1234567", "ABCD EFGH", "12a456", "../123456"] {
            assert!(code_of(wrong).is_none(), "{wrong}");
        }
    }

    #[test]
    fn a_device_shows_its_name_its_time_and_three_numbers() {
        let shown = info(&json!({
            "device": "Chrome on Android", "requested_at": 5, "choices": [47, 12, 83], "extra": 1,
        }))
        .unwrap();
        assert_eq!(
            shown,
            json!({ "device": "Chrome on Android", "requested_at": 5, "choices": [47, 12, 83] })
        );
        assert!(info(&json!({ "device": "x", "requested_at": 5, "choices": [47, 12] })).is_err());
        assert!(
            info(&json!({ "device": "x", "requested_at": 5, "choices": [47, 12, 100] })).is_err()
        );
        assert!(info(&json!({ "device": "x", "choices": [47, 12, 83] })).is_err());
    }

    #[test]
    fn a_large_event_is_read_whole_and_cut_down_for_the_game() {
        // A thousand sets as the bridge sends them: well past 64 KiB.
        let one = json!({
            "id": "set-0000000", "round": 3, "round_label": "Winners Round 3", "phase": "Top 64",
            "status": "open", "players": ["A player name", "Another player"], "score": [1, 0],
            "draws": 0, "side": "winners", "desk_state": "playing", "held": false,
            "match_id": "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a01", "revision": "4",
            "ember_ids": ["emb1_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "emb1_bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"],
            "games": [{ "attempt_id": "att_1", "outcome": "p1_win", "state": "accepted" }],
        });
        let body = serde_json::to_vec(&json!({ "sets": vec![one; 1000] })).unwrap();
        assert!(body.len() > 64 * 1024);
        let found = large_answer(&body).unwrap();
        assert_eq!(sets(&found["sets"]).as_array().unwrap().len(), MAX_SETS);
    }

    #[test]
    fn a_set_keeps_what_the_desk_shows_and_cuts_long_text() {
        let found = json!([{
            "id": "1001", "round_label": "x".repeat(200), "players": ["Kuro", "Velvet", "extra"],
            "score": [2, 1], "desk_state": "playing", "entries": ["355", "354"], "ember_ids": ["a", "b"],
            "call": { "at": 5, "posted": "posted" },
            "games": [{ "attempt_id": "att_1", "outcome": "p1_win", "state": "accepted", "seq": 1 }],
        }]);
        let set = &sets(&found)[0];
        assert_eq!(set["round_label"].as_str().unwrap().len(), MAX_TEXT);
        assert_eq!(set["players"], json!(["Kuro", "Velvet"]));
        assert_eq!(set["posted"], "posted");
        assert!(set.get("entries").is_none() && set.get("ember_ids").is_none());
        assert_eq!(
            set["games"][0],
            json!({ "attempt_id": "att_1", "outcome": "p1_win", "state": "accepted" })
        );
    }
}
