//! The tournament desk in the game: the organizer's events and one event's
//! sets, read with the player's session, and the desk's actions on a set
//! (hold or let go, call again, decide or void a game), each proven for its
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
    client::{answer, prove, with_session},
};

const EVENTS: &str = "/v1/organizer/events";
const WEB: &str = "/v1/web-sessions";
const HANDOFF: &str = "/v1/web-sessions/handoff";

/// The most sets an answer carries, and the longest text kept from one: an
/// answer goes to the game in one 64 KiB message.
// ponytail: a bracket past 160 sets shows its first 160; page by phase if one ever does.
const MAX_SETS: usize = 160;
const MAX_TEXT: usize = 64;

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
            let (status, body) = shared
                .client
                .call(
                    reqwest::Method::GET,
                    &format!("{}{path}", bridge.origin),
                    Some(&token),
                    None,
                )
                .await?;
            let found = answer(status, &body, &[200])?;
            Ok(Some(if event.is_some() {
                json!({ "event": event, "sets": sets(&found["sets"]) })
            } else {
                json!({ "events": events(&found["events"]) })
            }))
        }
    })
    .await
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
            answer(status, &response, &[200])?;
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

/// A one-time handoff to the web desk, proven with `site.connect`. With
/// `open`, the desk's link opens in the browser; the answer is the code to
/// type elsewhere, when it lapses, and the desk's address. The link's
/// token stays in the helper.
pub async fn handoff(shared: &Arc<Shared>, bridge_id: &str, open: bool) -> Outcome {
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
        let (Some(link), Some(code)) = (made["token"].as_str(), made["code"].as_str()) else {
            return Err(Failure::new("bridge_invalid_response"));
        };
        let well_formed = |text: &str, len: std::ops::RangeInclusive<usize>| {
            len.contains(&text.len())
                && text
                    .bytes()
                    .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_')
        };
        if !well_formed(link, 16..=64) || !well_formed(code, 8..=8) {
            return Err(Failure::new("bridge_invalid_response"));
        }
        if open {
            browser::open(&format!("{}/desk/h/{link}", bridge.origin))?;
        }
        Ok(Some(json!({
            "code": code,
            "expires_at": made["expires_at"],
            "desk": format!("{}/desk", bridge.origin),
        })))
    })
    .await
}

/// `/v1/organizer/events/{event}/sets/{set}` or
/// `/v1/organizer/events/{event}/matches/{match}/adjudications`.
fn is_action(path: &str) -> bool {
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
        for path in [
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
