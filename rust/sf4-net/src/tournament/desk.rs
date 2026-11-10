//! The tournament desk in the game, on the bracket site the approved bridge
//! names (`capabilities.tournaments`, docs/design/BRACKET_SITE.md): the
//! organizer's tournaments and one tournament's sets, its settings and
//! moves, and the desk's actions on a set (hold or let go, call again,
//! decide or void a game). The site is trusted as part of the bridge, so it
//! is reached as the bridge with the site's origin: a session there is a
//! `tournament.act` proof the site checks with the bridge, and it signs in
//! as the player's Discord account. The site checks the tournament is the
//! player's; the helper only keeps the paths to the desk's own.
use std::sync::Arc;

use ember_protocol::{challenge::Action, encoding::check_origin, event};
use serde::Deserialize;
use serde_json::{Map, Value, json};
use zeroize::Zeroizing;

use super::{
    Code, Failure, Outcome, Shared,
    bridges::Approved,
    browser,
    client::{self, Body, BridgeCall, SiteSession, Verb},
};

const TOURNAMENTS: &str = "/tournaments";
const REDEEM: &str = "/organizer-invites/redeem";

/// The most sets an answer carries, and the longest text kept from one: an
/// answer goes to the game in one 64 KiB message.
// ponytail: a bracket past 160 sets shows its first 160; page by round if one ever does.
const MAX_SETS: usize = 160;
const MAX_TEXT: usize = 64;
/// The most a tournament weighs as the site sends it, with its entrants,
/// sets and log.
const MAX_ANSWER: usize = 1024 * 1024;

fn invalid_request() -> Failure {
    Failure::new(Code::InvalidRequest)
}

#[derive(Deserialize)]
struct SessionCreated {
    access_token: String,
    expires_in: u64,
    #[serde(default)]
    organizer: bool,
}

/// A session on the bridge's bracket site, from the cache or a new proof.
async fn session(shared: &Arc<Shared>, bridge_id: &str) -> Result<SiteSession, Failure> {
    let ember_id = client::current_id(shared)?;
    if let Some(session) = shared.client.site_session(bridge_id, &ember_id) {
        return Ok(session);
    }
    let bridge = client::approved(shared, bridge_id)?;
    let offered = shared.client.capabilities(&bridge.origin).await?;
    let origin = offered
        .capabilities
        .tournaments
        .ok_or_else(|| Failure::new(Code::Unsupported))?;
    check_origin(&origin, client::POLICY).map_err(|_| Failure::new(Code::InvalidOrigin))?;
    let site = Approved { origin, ..bridge };
    let command = json!({});
    let call = BridgeCall::new(Verb::Post, "/v1/sessions", &[201]).body(Body::Proven {
        action: Action::TournamentAct,
        command: &command,
    });
    let (status, body) = client::send(shared, &site, None, &call).await?;
    // The site knows players by Discord: one not connected in Ember is told so.
    if status == 409 {
        return Err(Failure::new(Code::DiscordRequired));
    }
    let created: SessionCreated = client::read(status, &body, &[201], &call.refusals)?;
    let session = SiteSession {
        site,
        token: Zeroizing::new(created.access_token),
        expires_at: client::now() + created.expires_in,
        ember_id,
        organizer: created.organizer,
    };
    shared.client.keep_site_session(session.clone());
    Ok(session)
}

/// `call` on the bracket site, its status and body, with a new session once
/// when the site no longer takes the cached one.
async fn site_call(
    shared: &Arc<Shared>,
    bridge_id: &str,
    call: &BridgeCall<'_>,
) -> Result<(SiteSession, u16, Vec<u8>), Failure> {
    let session = session(shared, bridge_id).await?;
    let (status, body) = client::send(shared, &session.site, Some(&session.token), call).await?;
    if status != 401 {
        return Ok((session, status, body));
    }
    shared.client.forget_site_session(bridge_id);
    let session = self::session(shared, bridge_id).await?;
    let (status, body) = client::send(shared, &session.site, Some(&session.token), call).await?;
    Ok((session, status, body))
}

/// The site's answer to a read, as JSON, up to `MAX_ANSWER` bytes.
fn answer(status: u16, body: &[u8]) -> Result<Value, Failure> {
    if status != 200 {
        return Err(client::refused(status, body, &client::Refusals::NONE));
    }
    ember_protocol::json::parse(body, MAX_ANSWER)
        .and_then(|value| ember_protocol::json::from_value::<Value>(&value))
        .map_err(|_| Failure::new(Code::BridgeInvalidResponse))
}

/// The organizer's tournaments, or `event`'s sets with what the desk shows
/// of each, when `event` is given; with `set`, that set's games too.
pub async fn read(
    shared: &Arc<Shared>,
    bridge_id: &str,
    event: Option<&str>,
    set: Option<&str>,
) -> Outcome {
    let Some(event) = event else {
        let call = BridgeCall::new(Verb::Get, TOURNAMENTS, &[200]).limit(MAX_ANSWER);
        let (session, status, body) = site_call(shared, bridge_id, &call).await?;
        let found = answer(status, &body)?;
        return Ok(Some(json!({
            "events": events(&found["tournaments"]),
            "access": { "invite_only": true, "allowed": session.organizer },
        })));
    };
    if !is_slug(event) || set.is_some_and(|set| !is_slug(set)) {
        return Err(invalid_request());
    }
    let path = format!("{TOURNAMENTS}/{event}");
    let call = BridgeCall::new(Verb::Get, &path, &[200]).limit(MAX_ANSWER);
    let (_, status, body) = site_call(shared, bridge_id, &call).await?;
    let mut sets = sets(&answer(status, &body)?);
    // The open set's games, from its Ember match, for the organizer to decide.
    if let Some(set) = set
        && let Some(shown) = sets.iter_mut().find(|shown| shown["id"] == set)
        && shown.get("match_id").is_some()
    {
        let path = format!("{TOURNAMENTS}/{event}/sets/{set}/games");
        let call = BridgeCall::new(Verb::Get, &path, &[200]);
        let (_, status, body) = site_call(shared, bridge_id, &call).await?;
        games(shown, &answer(status, &body)?);
    }
    Ok(Some(json!({ "event": event, "sets": sets })))
}

/// A desk action at `path`, one of the site's routes for the organizer's
/// tournaments and their sets, with `command` as its body. A `starts_at`
/// given in Unix seconds goes to the site as its date and time.
pub async fn act(shared: &Arc<Shared>, bridge_id: &str, path: &str, mut command: Value) -> Outcome {
    let Some(verb) = verb_of(path) else {
        return Err(invalid_request());
    };
    let Some(fields) = command.as_object_mut() else {
        return Err(invalid_request());
    };
    if let Some(at) = fields.get("starts_at").and_then(Value::as_u64) {
        fields.insert("starts_at".into(), json!(event::rfc3339(at)));
    }
    let bytes = serde_json::to_vec(&command).map_err(|_| Failure::new(Code::Internal))?;
    let call = BridgeCall::new(verb, path, &[200, 201]).body(Body::Json(&bytes));
    let (_, status, body) = site_call(shared, bridge_id, &call).await?;
    if !(200..300).contains(&status) {
        return Err(client::refused(status, &body, &client::Refusals::NONE));
    }
    // A new tournament's ID, for the game to go on with it.
    if path != TOURNAMENTS {
        return Ok(None);
    }
    let made = answer(200, &body)?;
    Ok(made["id"]
        .as_str()
        .filter(|id| is_slug(id))
        .map(|id| json!({ "id": id })))
}

/// Deletes the organizer's tournament `event` on the site.
pub async fn stop(shared: &Arc<Shared>, bridge_id: &str, event: &str) -> Outcome {
    if !is_slug(event) {
        return Err(invalid_request());
    }
    let path = format!("{TOURNAMENTS}/{event}");
    let call = BridgeCall::new(Verb::Delete, &path, &[204]);
    let (_, status, body) = site_call(shared, bridge_id, &call).await?;
    if !(200..300).contains(&status) {
        return Err(client::refused(status, &body, &client::Refusals::NONE));
    }
    Ok(None)
}

/// Opens the bracket site in this PC's browser, where the organizer signs
/// in with Discord.
pub async fn open(shared: &Arc<Shared>, bridge_id: &str) -> Outcome {
    let session = session(shared, bridge_id).await?;
    browser::open(&format!("{}/", session.site.origin))?;
    Ok(None)
}

/// Redeems `code`, an invite to organize on the site.
pub async fn redeem(shared: &Arc<Shared>, bridge_id: &str, code: &str) -> Outcome {
    let code: String = code.trim().chars().take(64).collect();
    if code.is_empty() || !is_slug(&code) {
        return Err(Failure::new(Code::InviteInvalid));
    }
    let bytes =
        serde_json::to_vec(&json!({ "code": code })).map_err(|_| Failure::new(Code::Internal))?;
    let call = BridgeCall::new(Verb::Post, REDEEM, &[200]).body(Body::Json(&bytes));
    let (_, status, body) = site_call(shared, bridge_id, &call).await?;
    match status {
        200 => {
            // The session said who organizes when it opened.
            shared.client.forget_site_session(bridge_id);
            Ok(None)
        }
        404 => Err(Failure::new(Code::InviteInvalid)),
        _ => Err(client::refused(status, &body, &client::Refusals::NONE)),
    }
}

/// 1 to 64 letters, digits, `_` or `-`: the site's tournament and set IDs.
fn is_slug(text: &str) -> bool {
    (1..=64).contains(&text.len())
        && text
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'-')
}

/// The verb of a desk action at `path`, or none for a path not the desk's.
fn verb_of(path: &str) -> Option<Verb> {
    if path == TOURNAMENTS {
        return Some(Verb::Post);
    }
    let rest = path.strip_prefix(TOURNAMENTS)?.strip_prefix('/')?;
    let parts: Vec<&str> = rest.split('/').collect();
    match parts.as_slice() {
        [event] if is_slug(event) => Some(Verb::Patch),
        [event, "state"] if is_slug(event) => Some(Verb::Put),
        [event, "sets", set, "hold" | "call" | "adjudications"]
            if is_slug(event) && is_slug(set) =>
        {
            Some(Verb::Post)
        }
        _ => None,
    }
}

fn text(value: &Value) -> Value {
    match value.as_str() {
        Some(text) => json!(text.chars().take(MAX_TEXT).collect::<String>()),
        None => Value::Null,
    }
}

/// The site's `2026-11-10T20:00:00.000Z` in Unix seconds, or none.
fn unix(value: &Value) -> Option<u64> {
    let at = value.as_str()?;
    let whole = at.get(..19)?;
    matches!(at.get(19..)?, "Z" | ".000Z")
        .then(|| event::parse_rfc3339(&format!("{whole}Z")))
        .flatten()
}

/// What the desk shows of each tournament.
fn events(found: &Value) -> Value {
    let list = found.as_array().map(Vec::as_slice).unwrap_or_default();
    list.iter()
        .take(32)
        .map(|t| {
            let actions: Vec<Value> = t["actions"]
                .as_array()
                .map(|actions| actions.iter().take(12).map(text).collect())
                .unwrap_or_default();
            let mut event = Map::new();
            event.insert("id".into(), text(&t["id"]));
            event.insert("title".into(), text(&t["title"]));
            event.insert("status".into(), text(&t["state"]));
            event.insert(
                "starts_at".into(),
                json!(unix(&t["starts_at"]).unwrap_or(0)),
            );
            event.insert("games_to_win".into(), t["games_to_win"].clone());
            event.insert(
                "check_in".into(),
                json!(t["check_in"].as_bool().unwrap_or(false)),
            );
            event.insert("cap".into(), json!(t["cap"].as_u64().unwrap_or(0)));
            let entrants = t["entrants"].as_array().map_or(0, Vec::len);
            event.insert("signed_up".into(), json!(entrants));
            event.insert(
                "discord".into(),
                json!(t["discord"].as_bool().unwrap_or(false)),
            );
            event.insert("actions".into(), json!(actions));
            Value::Object(event)
        })
        .collect()
}

/// What the desk shows of each set: its players by name, its score, and its
/// state in the desk's words.
fn sets(found: &Value) -> Vec<Value> {
    let names: Map<String, Value> = found["entrants"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|e| Some((e["ember_id"].as_str()?.to_owned(), text(&e["name"]))))
        .collect();
    let list = found["sets"]
        .as_array()
        .map(Vec::as_slice)
        .unwrap_or_default();
    list.iter()
        .take(MAX_SETS)
        .map(|set| {
            let players: Vec<Value> = (0..2)
                .map(|slot| {
                    set["entries"][slot]
                        .as_str()
                        .and_then(|id| names.get(id).cloned())
                        .unwrap_or(json!(""))
                })
                .collect();
            let held = set["held"].as_bool().unwrap_or(false);
            let state = if set["winner"].is_string() {
                "done"
            } else if held {
                "held"
            } else if set["underway"].as_bool().unwrap_or(false) {
                "playing"
            } else if set["status"] == "open" {
                "waiting"
            } else {
                "not_ready"
            };
            let mut shown = Map::new();
            shown.insert("id".into(), text(&set["id"]));
            shown.insert("round".into(), set["round"].clone());
            shown.insert("round_label".into(), text(&set["round_label"]));
            shown.insert("players".into(), json!(players));
            if set["scores"].is_array() {
                shown.insert("score".into(), set["scores"].clone());
            }
            shown.insert("desk_state".into(), json!(state));
            shown.insert("held".into(), json!(held));
            if set["ember_match"].is_string() {
                shown.insert("match_id".into(), text(&set["ember_match"]));
            }
            Value::Object(shown)
        })
        .collect()
}

/// The set's games from its Ember match, and the match's revision a
/// decision names; a game waiting for review says the set needs it.
fn games(shown: &mut Value, found: &Value) {
    let games: Vec<Value> = found["games"]
        .as_array()
        .map(Vec::as_slice)
        .unwrap_or_default()
        .iter()
        .take(16)
        .map(|game| {
            json!({
                "attempt_id": text(&game["attempt_id"]),
                "outcome": text(&game["outcome"]),
                "state": text(&game["state"]),
            })
        })
        .collect();
    if games.iter().any(|game| game["state"] == "review") && shown["desk_state"] == "playing" {
        shown["desk_state"] = json!("needs_review");
    }
    shown["games"] = json!(games);
    shown["revision"] = match &found["revision"] {
        Value::Number(n) => json!(n.to_string()),
        other => text(other),
    };
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_the_desks_own_paths_are_sent() {
        assert_eq!(verb_of("/tournaments"), Some(Verb::Post));
        assert_eq!(verb_of("/tournaments/weekly_3"), Some(Verb::Patch));
        assert_eq!(verb_of("/tournaments/weekly_3/state"), Some(Verb::Put));
        for end in ["hold", "call", "adjudications"] {
            assert_eq!(
                verb_of(&format!("/tournaments/w/sets/r1-2/{end}")),
                Some(Verb::Post),
                "{end}"
            );
        }
        for path in [
            "/tournaments/",
            "/tournaments/w/entrants",
            "/tournaments/w/sets/r1-2/result",
            "/tournaments/w/sets/../hold",
            "/tournaments/a.b",
            "/tournaments/w/state/x",
            "/organizer-invites",
            "/v1/sessions",
        ] {
            assert_eq!(verb_of(path), None, "{path}");
        }
    }

    #[test]
    fn a_tournament_keeps_what_the_desk_shows() {
        let found = json!([{
            "id": "weekly_3", "title": "Weekly", "state": "sign_ups", "starts_at": "2026-11-12T19:00:00.000Z",
            "games_to_win": 3, "check_in": true, "cap": null, "discord": true, "organizers": ["1"],
            "entrants": [{ "discord": "1" }, { "discord": "2" }], "actions": ["close_sign_ups", "start"], "log": [],
        }]);
        assert_eq!(
            events(&found)[0],
            json!({ "id": "weekly_3", "title": "Weekly", "status": "sign_ups", "starts_at": 1_794_510_000,
                "games_to_win": 3, "check_in": true, "cap": 0, "signed_up": 2, "discord": true,
                "actions": ["close_sign_ups", "start"] })
        );
        assert_eq!(unix(&json!("2026-11-12T19:00:00.500Z")), None);
        assert_eq!(unix(&Value::Null), None);
    }

    #[test]
    fn a_set_shows_its_players_by_name_and_its_state() {
        let found = json!({
            "entrants": [{ "ember_id": "emb_a", "name": "Kuro" }, { "ember_id": "emb_b", "name": "Velvet" }],
            "sets": [
                { "id": "r1-1", "round": 1, "round_label": "Semifinals", "entries": ["emb_a", "emb_b"], "winner": null,
                  "scores": null, "underway": true, "held": false, "status": "open", "ember_match": "emt_1" },
                { "id": "r1-2", "round": 1, "entries": ["emb_b", null], "winner": null, "underway": false, "held": true, "status": "pending" },
                { "id": "r2-1", "round": 2, "entries": ["emb_a", "emb_b"], "winner": "emb_a", "scores": [2, 1], "underway": true, "status": "decided" },
            ],
        });
        let shown = sets(&found);
        assert_eq!(shown[0]["players"], json!(["Kuro", "Velvet"]));
        assert_eq!(shown[0]["desk_state"], "playing");
        assert_eq!(shown[0]["match_id"], "emt_1");
        assert!(shown[0].get("score").is_none());
        assert_eq!(
            [&shown[1]["desk_state"], &shown[1]["players"]],
            [&json!("held"), &json!(["Velvet", ""])]
        );
        assert_eq!(
            [&shown[2]["desk_state"], &shown[2]["score"]],
            [&json!("done"), &json!([2, 1])]
        );

        let mut set = shown[0].clone();
        games(
            &mut set,
            &json!({ "revision": 7, "games": [{ "attempt_id": "att_1", "seq": 1, "outcome": null, "state": "review" }] }),
        );
        assert_eq!(set["desk_state"], "needs_review");
        assert_eq!(set["revision"], "7");
        assert_eq!(
            set["games"],
            json!([{ "attempt_id": "att_1", "outcome": null, "state": "review" }])
        );
    }
}
