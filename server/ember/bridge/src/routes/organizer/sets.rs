//! A running event's sets: the connector's snapshot, served to the desk
//! and, when public, to anyone, with the desk's actions on a set.
use super::*;

/// The largest set snapshot the connector sends: `MAX_SETS` sets of a few
/// hundred bytes each.
const SETS_BODY: usize = 512 * 1024;

/// The most sets a snapshot keeps (the connector's `snapshot::MAX_SETS`).
const MAX_SETS: usize = 1024;

#[derive(Deserialize)]
struct Snapshot {
    sets: Vec<Value>,
}

/// `PUT /v1/desk/events/{id}/sets`: the bracket connector's latest view of
/// a running event's sets, kept as sent until the next.
pub async fn put_sets(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<SETS_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let desk = state
        .config
        .desk
        .as_ref()
        .ok_or_else(ApiFailure::not_found)?;
    if service.provider_connection()? != desk.connection {
        return Err(ApiFailure::forbidden());
    }
    let snapshot: Snapshot = body.parse()?;
    if snapshot.sets.len() > MAX_SETS || snapshot.sets.iter().any(|set| !set.is_object()) {
        return Err(ApiFailure::invalid(
            "sets is a list of at most 1024 objects.",
        ));
    }
    let stored = serde_json::to_string(&snapshot.sets).map_err(|_| ApiFailure::unavailable())?;
    let now = state.now();
    let bridge_id = state.config.bridge_id.clone();
    let dms = crate::delivery::dms::on(&state);
    let kept = state
        .db
        .write(move |tx| {
            let kept = tx.execute(
                "UPDATE organizer_events SET sets = ?1, sets_at = ?2
                 WHERE id = ?3 AND ended_at IS NULL AND status = 'ready'",
                params![stored, now, id],
            )?;
            if kept == 1 {
                post_news(tx, now, &bridge_id, &id, &stored, dms)?;
            }
            Ok(kept)
        })
        .await?;
    if kept == 0 {
        return Err(ApiFailure::not_found());
    }
    state.committed();
    Ok(ok(&json!({})))
}

/// Calls each set whose Ember match the desk made since its last call, and
/// posts the score of each finished set the desk called, once per match
/// (`delivery::calls`).
fn post_news(
    tx: &Transaction<'_>,
    now: u64,
    bridge_id: &str,
    id: &str,
    stored: &str,
    dms: bool,
) -> Result<()> {
    let site: Option<String> = tx.query_row(
        "SELECT site FROM organizer_events WHERE id = ?1",
        [id],
        |row| row.get(0),
    )?;
    for set in desk_sets(tx, id, site.as_deref(), Some(stored), true)? {
        let (Some(set_id), Some(match_id)) = (set["id"].as_str(), set["match_id"].as_str()) else {
            continue;
        };
        let (called, reported): (Option<String>, Option<String>) = tx
            .query_row(
                "SELECT called_match, result_match FROM desk_marks
                 WHERE event_id = ?1 AND set_id = ?2",
                params![id, set_id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .optional()?
            .unwrap_or_default();
        let called_set = Called {
            event_id: id,
            set: &set,
        };
        let state = set["desk_state"].as_str().unwrap_or_default();
        if called.as_deref() != Some(match_id)
            && matches!(state, "called" | "playing")
            && calls::call(tx, now, bridge_id, &called_set, dms)?
        {
            tx.execute(
                "INSERT INTO desk_marks (event_id, set_id, called_match, called_at)
                 VALUES (?1, ?2, ?3, ?4)
                 ON CONFLICT (event_id, set_id)
                 DO UPDATE SET called_match = ?3, called_at = ?4",
                params![id, set_id, match_id, now],
            )?;
        }
        if called.as_deref() == Some(match_id)
            && reported.as_deref() != Some(match_id)
            && matches!(state, "reporting" | "done")
            && calls::result(tx, now, &called_set)?
        {
            tx.execute(
                "UPDATE desk_marks SET result_match = ?1 WHERE event_id = ?2 AND set_id = ?3",
                params![match_id, id, set_id],
            )?;
        }
    }
    Ok(())
}

/// `GET /v1/organizer/events/{id}/sets`: the organizer's event's sets, each
/// with its Ember match's score and state and the desk state they make.
pub async fn sets(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> Result<Response> {
    let ember_id = reader(&state, &headers).await?;
    let view = state
        .db
        .read(move |tx| {
            let found: Option<(Option<String>, Option<String>, Option<u64>)> = tx
                .query_row(
                    "SELECT site, sets, sets_at FROM organizer_events
                     WHERE id = ?1 AND ember_id = ?2",
                    params![id, ember_id],
                    |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
                )
                .optional()?;
            let Some((site, stored, at)) = found else {
                return Ok(None);
            };
            Ok(Some(json!({
                "sets": desk_sets(tx, &id, site.as_deref(), stored.as_deref(), true)?,
                "updated_at": at,
            })))
        })
        .await?;
    view.map(|view| ok(&view)).ok_or_else(ApiFailure::not_found)
}

/// `GET /v1/public/events/{id}`: an event its organizer made public and has
/// not stopped: its title, start, links and sets, without Ember IDs, and
/// whether it takes sign-ups now.
pub async fn public(State(state): State<AppState>, Path(id): Path<String>) -> Result<Response> {
    let view = state
        .db
        .read(move |tx| {
            type Row = (
                String,
                Option<u64>,
                Option<String>,
                String,
                Option<String>,
                Option<u64>,
                bool,
            );
            let found: Option<Row> = tx
                .query_row(
                    "SELECT title, starts_at, site, links, sets, sets_at,
                         sign_ups = 1 AND status = 'draft' AND tournament IS NOT NULL
                     FROM organizer_events
                     WHERE id = ?1 AND public = 1 AND ended_at IS NULL",
                    [&id],
                    |row| {
                        Ok((
                            row.get(0)?,
                            row.get(1)?,
                            row.get(2)?,
                            row.get(3)?,
                            row.get(4)?,
                            row.get(5)?,
                            row.get(6)?,
                        ))
                    },
                )
                .optional()?;
            let Some((title, starts_at, site, links, stored, at, sign_ups_open)) = found else {
                return Ok(None);
            };
            Ok(Some(json!({
                "title": title,
                "starts_at": starts_at,
                "site": site,
                "links": settings(&links),
                "sets": desk_sets(tx, &id, site.as_deref(), stored.as_deref(), false)?,
                "updated_at": at,
                "sign_ups_open": sign_ups_open,
            })))
        })
        .await?;
    view.map(|view| ok(&view)).ok_or_else(ApiFailure::not_found)
}

/// The sets an event's organizer holds from the desk.
pub(super) fn held_sets(tx: &Transaction<'_>, event: &str) -> Result<Vec<String>> {
    Ok(tx
        .prepare("SELECT set_id FROM desk_marks WHERE event_id = ?1 AND held = 1 ORDER BY set_id")?
        .query_map([event], |row| row.get(0))?
        .collect::<rusqlite::Result<_>>()?)
}

/// Whether `path` is a desk action on one of an event's sets or matches,
/// which a `site.connect` proof authorizes (`sessions::route_matches`).
pub fn is_desk_action(path: &str) -> bool {
    let Some(rest) = path.strip_prefix("/v1/organizer/events/") else {
        return false;
    };
    let parts: Vec<&str> = rest.split('/').collect();
    match parts.as_slice() {
        [event, "sets", set] => is_prefixed_id(event, "oev") && is_set_id(set),
        [event, "matches", id, "adjudications"] => {
            is_prefixed_id(event, "oev") && is_prefixed_id(id, "emt")
        }
        _ => false,
    }
}

/// A set ID as the connector sends it: a site's ID, 1 to 64 letters,
/// digits, `-` or `_`.
fn is_set_id(id: &str) -> bool {
    (1..=64).contains(&id.len())
        && id
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_')
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Mark {
    /// Holds the set (it is not played in Ember) or lets it go.
    #[serde(default)]
    held: Option<bool>,
    /// Calls the set again: the desk shows when.
    #[serde(default)]
    call: bool,
}

/// `POST /v1/organizer/events/{id}/sets/{set}`: holds or lets go a set of
/// the organizer's running event, or calls it again. Takes a `site.connect`
/// The shortest time between two calls of one set.
const CALL_AGAIN_SECS: u64 = 120;

/// proof for this path.
pub async fn mark(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((id, set)): Path<(String, String)>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let (caller, mark): (_, Mark) = Caller::of(&state, &headers, &body).await?;
    if !is_set_id(&set) {
        return Err(ApiFailure::not_found());
    }
    let path = format!("{EVENTS_PATH}/{id}/sets/{set}");
    let ctx = Ctx::of(&state);
    let dms = crate::delivery::dms::on(&state);
    let marked = state
        .db
        .write(move |tx| {
            let ember_id = caller.prove(tx, &ctx, Action::SiteConnect, Method::Post, &path)?;
            if open_status(tx, &id, &ember_id)?.is_none() {
                return Ok(false);
            }
            tx.execute(
                "INSERT INTO desk_marks (event_id, set_id) VALUES (?1, ?2)
                 ON CONFLICT (event_id, set_id) DO NOTHING",
                params![id, set],
            )?;
            if let Some(held) = mark.held {
                tx.execute(
                    "UPDATE desk_marks SET held = ?1 WHERE event_id = ?2 AND set_id = ?3",
                    params![held, id, set],
                )?;
            }
            if mark.call {
                // A set is called at most once in CALL_AGAIN_SECS, so a
                // desk cannot flood the channel or the players' messages.
                let called_at: Option<u64> = tx
                    .query_row(
                        "SELECT called_at FROM desk_marks WHERE event_id = ?1 AND set_id = ?2",
                        params![id, set],
                        |row| row.get(0),
                    )
                    .optional()?
                    .flatten();
                if let Some(wait) = called_at
                    .map(|at| (at + CALL_AGAIN_SECS).saturating_sub(ctx.now))
                    .filter(|&wait| wait > 0)
                {
                    return Err(ApiFailure::rate_limited(wait));
                }
                tx.execute(
                    "UPDATE desk_marks SET called_at = ?1 WHERE event_id = ?2 AND set_id = ?3",
                    params![ctx.now, id, set],
                )?;
                let (site, stored): (Option<String>, Option<String>) = tx.query_row(
                    "SELECT site, sets FROM organizer_events WHERE id = ?1",
                    [&id],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )?;
                let found = desk_sets(tx, &id, site.as_deref(), stored.as_deref(), true)?
                    .into_iter()
                    .find(|shown| shown["id"] == json!(set))
                    .unwrap_or_else(|| json!({ "id": set }));
                let called = Called {
                    event_id: &id,
                    set: &found,
                };
                calls::call(tx, ctx.now, &ctx.config.bridge_id, &called, dms)?;
            }
            Ok(true)
        })
        .await?;
    state.committed();
    if !marked {
        return Err(ApiFailure::not_found());
    }
    Ok(ok(&json!({})))
}

/// `POST /v1/organizer/events/{id}/matches/{match}/adjudications`: the
/// organizer decides or voids a game of an Ember match one of their events
/// created, as an organizer credential would (`matches::adjudicate`),
/// recorded as theirs. Takes a `site.connect` proof for this path.
pub async fn adjudicate(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((id, match_id)): Path<(String, String)>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let (caller, command): (_, crate::routes::matches::AdjudicationCommand) =
        Caller::of(&state, &headers, &body).await?;
    let desk = state
        .config
        .desk
        .as_ref()
        .ok_or_else(ApiFailure::not_found)?
        .connection
        .clone();
    let path = format!("{EVENTS_PATH}/{id}/matches/{match_id}/adjudications");
    let ctx = Ctx::of(&state);
    let result = state
        .db
        .write(move |tx| {
            let ember_id = caller.prove(tx, &ctx, Action::SiteConnect, Method::Post, &path)?;
            // The match must be one this organizer's event made: on the
            // desk connection, and in the event's sets.
            let stored: Option<Option<String>> = tx
                .query_row(
                    "SELECT sets FROM organizer_events WHERE id = ?1 AND ember_id = ?2",
                    params![id, ember_id],
                    |row| row.get(0),
                )
                .optional()?;
            let sets: Vec<Value> = stored
                .flatten()
                .and_then(|stored| serde_json::from_str(&stored).ok())
                .unwrap_or_default();
            let on_desk: bool = tx.query_row(
                "SELECT EXISTS (SELECT 1 FROM matches WHERE id = ?1 AND connection_id = ?2)",
                params![match_id, desk],
                |row| row.get(0),
            )?;
            if !on_desk || !sets.iter().any(|set| set["match_id"] == json!(match_id)) {
                return Err(ApiFailure::not_found());
            }
            crate::routes::matches::adjudicate_for(
                tx,
                &ctx,
                &format!("desk:{ember_id}"),
                &match_id,
                &command,
            )
        })
        .await?;
    state.committed();
    let (status, body) = result;
    Ok(json(status, &body))
}

/// A kept snapshot's sets with what the bridge knows of each one's Ember
/// match: score, players (by their account on the event's site, else the
/// name the site shows for the entry; their Ember IDs when `with_ids`), the
/// round its metadata names when the snapshot has none, and the desk state.
/// Without `with_ids` no ID of the site's or Ember's is left in a set.
fn desk_sets(
    tx: &Transaction<'_>,
    event: &str,
    site: Option<&str>,
    stored: Option<&str>,
    with_ids: bool,
) -> Result<Vec<Value>> {
    let sets: Vec<Value> = stored
        .and_then(|stored| serde_json::from_str(stored).ok())
        .unwrap_or_default();
    let posted = calls::calls(tx, event)?;
    let messaged = crate::delivery::dms::status(tx, event)?;
    let marks: std::collections::HashMap<String, (bool, Option<u64>)> = tx
        .prepare("SELECT set_id, held, called_at FROM desk_marks WHERE event_id = ?1")?
        .query_map([event], |row| Ok((row.get(0)?, (row.get(1)?, row.get(2)?))))?
        .collect::<rusqlite::Result<_>>()?;
    sets.into_iter()
        .map(|mut set| {
            // Held from the desk shows held before the connector's next read.
            if let Some((held, called_at)) = set["id"].as_str().and_then(|id| marks.get(id)) {
                if *held {
                    set["held"] = json!(true);
                }
                set["called_at"] = json!(called_at);
            }
            if let Some(call) = set["id"].as_str().and_then(|id| posted.get(id)) {
                set["call"] = call.clone();
            }
            if let Some(dms) = set["id"].as_str().and_then(|id| messaged.get(id)) {
                set["dms"] = dms.clone();
            }
            let found = match set["match_id"].as_str() {
                Some(match_id) => desk_match(tx, match_id, site)?,
                None => None,
            };
            // The site's names, each replaced by the player's account
            // name once the set has an Ember match.
            let mut players = match set["names"].take() {
                Value::Array(names) => names,
                _ => vec![Value::Null; 2],
            };
            if let Some(found) = &found {
                for key in ["round_label", "round", "phase"] {
                    if set[key].is_null() {
                        set[key] = found.metadata[key].clone();
                    }
                }
                set["score"] = json!(found.wins);
                // Drawn games are replayed and never scored, so the score
                // alone would hide them.
                set["draws"] = json!(
                    found
                        .games
                        .iter()
                        .filter(|game| game["state"] == "accepted" && game["outcome"] == "draw")
                        .count()
                );
                for (slot, name) in found.names.iter().enumerate() {
                    if let (Some(name), Some(player)) = (name, players.get_mut(slot)) {
                        *player = json!(name);
                    }
                }
                if with_ids {
                    set["ember_ids"] = json!(found.ember_ids);
                    // What a decision from the desk names as its
                    // `expected_revision`.
                    set["revision"] = json!(found.revision.to_string());
                    set["games"] = json!(found.games);
                }
            }
            if let Some(set) = set.as_object_mut() {
                set.remove("names");
            }
            set["players"] = json!(players);
            set["side"] = json!(side(&set));
            set["desk_state"] = json!(desk_state(&set, found.as_ref().map(|f| f.state.as_str())));
            if !with_ids && let Some(set) = set.as_object_mut() {
                set.remove("match_id");
                set.remove("entries");
                set.remove("discord");
            }
            Ok(set)
        })
        .collect()
}

struct DeskMatch {
    state: String,
    revision: u64,
    /// The games played or decided, oldest first: what the desk can void,
    /// and a game waiting for a result it can decide.
    games: Vec<Value>,
    metadata: Value,
    wins: [u8; 2],
    ember_ids: Vec<String>,
    names: Vec<Option<String>>,
}

fn desk_match(tx: &Transaction<'_>, id: &str, site: Option<&str>) -> Result<Option<DeskMatch>> {
    let found: Option<(String, String, i64, u64)> = tx
        .query_row(
            "SELECT state, metadata, assignment_generation, revision FROM matches WHERE id = ?1",
            [id],
            |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?)),
        )
        .optional()?;
    let Some((state, metadata, generation, revision)) = found else {
        return Ok(None);
    };
    let (wins, _) = crate::routes::ledger::scores(tx, id)?;
    let ember_ids = tx
        .prepare(
            "SELECT ember_id FROM match_participants
             WHERE match_id = ?1 AND assignment_generation = ?2 ORDER BY slot",
        )?
        .query_map(params![id, generation], |row| row.get(0))?
        .collect::<rusqlite::Result<Vec<String>>>()?;
    let games = tx
        .prepare(
            "SELECT id, outcome, state FROM attempts
             WHERE match_id = ?1 AND state IN ('accepted', 'review', 'permitted') ORDER BY seq",
        )?
        .query_map([id], |row| {
            Ok(json!({
                "attempt_id": row.get::<_, String>(0)?,
                "outcome": row.get::<_, Option<String>>(1)?,
                "state": row.get::<_, String>(2)?,
            }))
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    let mut names = Vec::new();
    for ember_id in &ember_ids {
        names.push(
            tx.query_row(
                "SELECT username FROM site_accounts WHERE site = ?1 AND ember_id = ?2",
                params![site, ember_id],
                |row| row.get(0),
            )
            .optional()?,
        );
    }
    Ok(Some(DeskMatch {
        state,
        revision,
        games,
        metadata: serde_json::from_str(&metadata).unwrap_or_else(|_| json!({})),
        wins,
        ember_ids,
        names,
    }))
}

/// Which part of the bracket a set is in: a grand final by its label, the
/// losers bracket below round 0, else winners.
fn side(set: &Value) -> &'static str {
    let label = set["round_label"].as_str().unwrap_or_default();
    if label.to_ascii_lowercase().contains("grand final") {
        "grand_final"
    } else if set["round"].as_i64().is_some_and(|round| round < 0) {
        "losers"
    } else {
        "winners"
    }
}

/// What the desk shows for a set: what the connector and the set's Ember
/// match say together.
fn desk_state(set: &Value, match_state: Option<&str>) -> &'static str {
    if set["held"] == json!(true) {
        return "held";
    }
    match (set["progress"].as_str(), match_state) {
        (Some("stuck"), _) => "needs_organizer",
        (Some("reported" | "closed"), _) => "done",
        (_, Some("needs_review")) => "needs_review",
        (_, Some("running" | "between_games" | "awaiting_reports")) => "playing",
        (_, Some("created" | "awaiting_players" | "provisioning" | "ready")) => "called",
        (_, Some("completed")) => "reporting",
        _ => match set["status"].as_str() {
            Some("decided") => "done",
            Some("pending") => "not_ready",
            _ => "waiting",
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_sets_desk_state_and_side_come_from_the_connector_and_its_match() {
        let set = |fields: Value| fields;
        assert_eq!(
            desk_state(&set(json!({ "held": true })), Some("running")),
            "held"
        );
        assert_eq!(
            desk_state(&set(json!({ "progress": "stuck" })), None),
            "needs_organizer"
        );
        assert_eq!(
            desk_state(&set(json!({ "progress": "reported" })), Some("completed")),
            "done"
        );
        assert_eq!(
            desk_state(&set(json!({ "progress": "live" })), Some("needs_review")),
            "needs_review"
        );
        assert_eq!(
            desk_state(&set(json!({ "progress": "live" })), Some("between_games")),
            "playing"
        );
        assert_eq!(
            desk_state(
                &set(json!({ "progress": "live" })),
                Some("awaiting_players")
            ),
            "called"
        );
        assert_eq!(
            desk_state(&set(json!({ "progress": "live" })), Some("completed")),
            "reporting"
        );
        assert_eq!(
            desk_state(&set(json!({ "progress": "live" })), Some("cancelled")),
            "waiting"
        );
        assert_eq!(
            desk_state(&set(json!({ "status": "pending" })), None),
            "not_ready"
        );
        assert_eq!(
            desk_state(&set(json!({ "status": "decided" })), None),
            "done"
        );
        assert_eq!(side(&json!({ "round": -2 })), "losers");
        assert_eq!(
            side(&json!({ "round": 5, "round_label": "Grand Final Reset" })),
            "grand_final"
        );
        assert_eq!(side(&json!({ "round": 3 })), "winners");
    }
}
