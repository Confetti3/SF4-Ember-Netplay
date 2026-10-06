//! BluMint's game partner API (v1.3.0), for connections of kind `blumint`.
//! BluMint calls three endpoints here, with the connection's provider
//! credential as `Authorization: Bearer`:
//!
//! - player lookup: the sign-in IDs of a BluMint user, answered with the Ember
//!   IDs connected to those Discord accounts (`discord` is the one sign-in
//!   method Ember supports);
//! - match creation: two teams of one Ember ID each, answered with the match ID
//!   and its Play link, which only those two players can use;
//! - match status: pending, running, complete or cancelled, with each player's
//!   presence and the score.
//!
//! The bridge sends each finished match's score to BluMint, or asks it to
//! restart a cancelled match, once BluMint's API key for the connection is
//! configured. A match whose result the two games disputed is cancelled where
//! it would wait for review (`matches::needs_review`), since BluMint has no
//! organizer review; a restart is its way to replay. A match nobody played
//! expires (`expiry::expire_stale`) and is sent nothing: BluMint's result
//! format cannot say it, and a restart would be a request it never made; its
//! status reads `cancelled`. `register` tells BluMint
//! where the three endpoints are.
use std::collections::BTreeMap;

use axum::{
    extract::{Query, State, rejection::QueryRejection},
    http::HeaderMap,
    response::Response,
};
use ember_protocol::{
    EmberId, json as wire,
    matches::{MAX_GAMES_TO_WIN, MatchState, Participant, Rules, games_to_win_valid},
    play::{PROFILE, play_url},
};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::Deserialize;
use serde_json::{Value, json};

use crate::{
    AppState, auth,
    config::{BLUMINT, Connection},
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, GENERAL_BODY, ok},
    routes::{
        discord,
        ledger::{Match, load, participants, scores},
        matches::{NewMatch, insert_match},
        policy,
    },
    util::{new_id, outbound_client},
};

pub const LOOKUP_PATH: &str = "/v1/blumint/lookup";
pub const MATCHES_PATH: &str = "/v1/blumint/matches";
pub const STATUS_PATH: &str = "/v1/blumint/matches/status";
/// Set length when BluMint's match settings name none: first to 2.
const DEFAULT_GAMES_TO_WIN: u8 = 2;

/// BluMint's API for a connection: `api_base`, or by its environment.
pub fn api_base(connection: &Connection) -> &str {
    match (&connection.api_base, connection.environment.as_str()) {
        (Some(base), _) => base,
        (None, "production") => "https://app.blumint.com/api",
        (None, _) => "https://staging.blumint.io/api",
    }
}

/// BluMint's bodies as plain JSON: its match settings carry decimals
/// (`"gravity": 1.1`), which the bridge's strict profile refuses. The size cap
/// still applies, and nothing here is signed or stored as sent.
fn parse<T: serde::de::DeserializeOwned>(body: &Body<GENERAL_BODY>) -> Result<T> {
    serde_json::from_slice(&body.0)
        .map_err(|_| ApiFailure::invalid("The request body is not the JSON BluMint documents."))
}

/// The BluMint connection a provider credential speaks for.
async fn connection(state: &AppState, headers: &HeaderMap) -> Result<String> {
    let service = auth::service(state, headers).await?;
    let id = service.provider_connection()?.to_owned();
    match state.config.connection(&id) {
        Some((_, connection)) if connection.kind == BLUMINT => Ok(id),
        _ => Err(ApiFailure::not_found()),
    }
}

/// `POST /v1/blumint/lookup`: every sign-in method of one BluMint user. Only
/// `discord` is answered, with `[]` when none of its IDs is connected.
pub async fn lookup(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let connection_id = connection(&state, &headers).await?;
    let request: BTreeMap<String, Value> = parse(&body)?;
    let user_ids: Vec<String> = request
        .get("discord")
        .and_then(Value::as_array)
        .map(|ids| {
            ids.iter()
                .filter_map(Value::as_str)
                .take(32)
                .map(str::to_owned)
                .collect()
        })
        .unwrap_or_default();
    let ctx = Ctx::of(&state);
    let found = state
        .db
        .write(move |tx| discord::find_and_link(tx, &ctx, &connection_id, &user_ids))
        .await?;
    let found: Vec<EmberId> = found.into_iter().map(|player| player.ember_id).collect();
    state.committed();
    Ok(ok(&json!({ "discord": found })))
}

#[derive(Deserialize)]
struct CreateRequest {
    teams: Vec<Team>,
    #[serde(default, rename = "matchSettings")]
    settings: BTreeMap<String, Value>,
}

#[derive(Deserialize)]
struct Team {
    players: Vec<TeamPlayer>,
}

/// The guide names the field `playerId`, the OpenAPI schema `inGameId`.
#[derive(Deserialize)]
struct TeamPlayer {
    #[serde(alias = "playerId", rename = "inGameId")]
    in_game_id: EmberId,
}

/// `POST /v1/blumint/matches`: a set between two teams of one, played in
/// Ember. `matchSettings.gamesToWin` (1 to 10, best of 1 to 19) sets its length; other
/// settings are BluMint's own and ignored.
pub async fn create(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let connection_id = connection(&state, &headers).await?;
    let request: CreateRequest = parse(&body)?;
    let players: Vec<EmberId> = match request.teams.as_slice() {
        [a, b] if a.players.len() == 1 && b.players.len() == 1 => {
            vec![
                a.players[0].in_game_id.clone(),
                b.players[0].in_game_id.clone(),
            ]
        }
        _ => {
            return Err(ApiFailure::invalid(
                "Ember matches are two teams of one player.",
            ));
        }
    };
    let games_to_win = match request.settings.get("gamesToWin") {
        None => DEFAULT_GAMES_TO_WIN,
        Some(value) => value
            .as_u64()
            .and_then(|n| u8::try_from(n).ok())
            .filter(|n| games_to_win_valid(*n))
            .ok_or_else(|| {
                ApiFailure::invalid(format!(
                    "matchSettings.gamesToWin must be 1 to {MAX_GAMES_TO_WIN}."
                ))
            })?,
    };
    let ctx = Ctx::of(&state);
    let bridge_id = state.config.bridge_id.clone();
    let match_id = state
        .db
        .write(move |tx| {
            if let Some(id) = active(tx, &connection_id, &players, games_to_win)? {
                return Ok(id);
            }
            let tenant_id = ctx.tenant_of(&connection_id)?;
            let mut roster = Vec::new();
            for (slot, ember_id) in players.iter().enumerate() {
                roster.push(Participant {
                    participant_id: participant(tx, &connection_id, ember_id)?,
                    ember_id: ember_id.clone(),
                    slot: slot as u8,
                });
            }
            let rules = Rules::standard(games_to_win, PROFILE);
            let external_match_id = new_id("blumint");
            let digest = wire::digest(&json!({ "blumint": external_match_id }))?;
            insert_match(
                tx,
                &ctx,
                &NewMatch {
                    tenant_id: &tenant_id,
                    connection_id: &connection_id,
                    external_match_id: &external_match_id,
                    digest: &digest,
                    rules: &rules,
                    required_build_id: "",
                    metadata: &BTreeMap::new(),
                    participants: [&roster[0], &roster[1]],
                    lobby_id: None,
                    tournament_id: None,
                },
            )
        })
        .await?;
    state.committed();
    Ok(ok(
        &json!({ "matchId": match_id, "matchUrl": play_url(&bridge_id, &match_id) }),
    ))
}

/// The active match the connection already has between these players, in
/// this order, of this length. BluMint sends no request ID, so this is how
/// a creation it retries after losing the answer gets the same match; any
/// other request for a busy player is refused as busy.
fn active(
    tx: &Transaction<'_>,
    connection_id: &str,
    players: &[EmberId],
    games_to_win: u8,
) -> Result<Option<String>> {
    Ok(tx
        .query_row(
            "SELECT m.id FROM matches m
             JOIN match_participants a
               ON a.match_id = m.id AND a.assignment_generation = m.assignment_generation AND a.slot = 0
             JOIN match_participants b
               ON b.match_id = m.id AND b.assignment_generation = m.assignment_generation AND b.slot = 1
             WHERE m.connection_id = ?1 AND a.ember_id = ?2 AND b.ember_id = ?3 AND m.games_to_win = ?4
               AND m.state NOT IN ('completed', 'cancelled', 'failed', 'expired')",
            params![connection_id, players[0].as_str(), players[1].as_str(), games_to_win],
            |row| row.get(0),
        )
        .optional()?)
}

/// The participant an Ember ID is linked as on the connection.
fn participant(tx: &Transaction<'_>, connection_id: &str, ember_id: &EmberId) -> Result<String> {
    tx.query_row(
        "SELECT a.participant_id FROM links l JOIN external_accounts a ON a.id = l.account_id
         WHERE l.ember_id = ?1 AND l.connection_id = ?2 AND l.revoked_at IS NULL",
        params![ember_id.as_str(), connection_id],
        |row| row.get(0),
    )
    .map_err(|_| {
        ApiFailure::invalid("That in-game ID was not found by a player lookup on this connection.")
            .detail("inGameId", ember_id.as_str())
    })
}

#[derive(Deserialize)]
pub struct StatusQuery {
    #[serde(rename = "matchId")]
    match_id: String,
}

/// `GET /v1/blumint/matches/status?matchId=`. The query is read after the
/// credential, so a caller without one gets 401 whatever it sent, and a
/// missing `matchId` gets the usual JSON error.
pub async fn status(
    State(state): State<AppState>,
    headers: HeaderMap,
    query: std::result::Result<Query<StatusQuery>, QueryRejection>,
) -> Result<Response> {
    let connection_id = connection(&state, &headers).await?;
    let Query(query) = query.map_err(|_| ApiFailure::invalid("The query needs a matchId."))?;
    let body = state
        .db
        .read(move |tx| {
            let found = load(tx, &query.match_id)?
                .filter(|found| found.connection_id == connection_id)
                .ok_or_else(ApiFailure::not_found)?;
            let (wins, _) = scores(tx, &found.id)?;
            let mut teams = Vec::new();
            for player in participants(tx, &found.id, found.generation)? {
                let claimed: bool = tx.query_row(
                    "SELECT EXISTS (SELECT 1 FROM match_claims WHERE match_id = ?1 AND ember_id = ?2)",
                    params![found.id, player.ember_id.as_str()],
                    |row| row.get(0),
                )?;
                teams.push(json!({
                    "score": wins[usize::from(player.slot)],
                    "players": [{ "inGameId": player.ember_id, "status": presence(found.state, claimed) }],
                }));
            }
            Ok(json!({ "status": match_status(found.state), "teams": teams }))
        })
        .await?;
    Ok(ok(&body))
}

/// BluMint's word for a match's state: players gathering, playing, done, or
/// ended without a result.
pub fn match_status(state: MatchState) -> &'static str {
    use MatchState::*;
    match state {
        Created | AwaitingPlayers | Provisioning | Ready => "pending",
        Running | BetweenGames | AwaitingReports | NeedsReview => "running",
        Completed => "complete",
        // BluMint has no word for a match nobody played; "cancelled" is its
        // only ended-without-a-result status. It is never sent a restart.
        Cancelled | Failed | Expired => "cancelled",
    }
}

/// A player is present once their Ember claimed the match, and ready once the
/// room both fighters sit in is bound (from `ready` on).
pub fn presence(state: MatchState, claimed: bool) -> &'static str {
    use MatchState::*;
    match (claimed, state) {
        (false, _) => "absent",
        (true, Created | AwaitingPlayers | Provisioning) => "present-not-ready",
        (true, _) => "present-ready",
    }
}

/// What the bridge sends BluMint for a match, or None while it is still on
/// (`delivery::results`).
pub fn submission(tx: &Transaction<'_>, found: &Match) -> Result<Option<Value>> {
    Ok(match found.state {
        MatchState::Completed => {
            let (wins, _) = scores(tx, &found.id)?;
            let teams: Vec<Value> = participants(tx, &found.id, found.generation)?
                .into_iter()
                .map(|p| json!({ "score": wins[usize::from(p.slot)], "players": [{ "inGameId": p.ember_id }] }))
                .collect();
            Some(json!({ "matchId": found.id, "teams": teams }))
        }
        MatchState::Cancelled | MatchState::Failed => {
            Some(json!({ "matchId": found.id, "mustRestart": true }))
        }
        // Nothing BluMint accepts says "not played", and a restart would
        // start a match nobody asked for; it is told through its status
        // (`match_status`) only.
        MatchState::Expired => None,
        _ => None,
    })
}

/// Tells BluMint where this bridge's three endpoints are, with a new provider
/// credential for BluMint to call them with. The credential goes straight to
/// BluMint and is never shown.
pub async fn register(state: &AppState, connection_id: &str) -> std::result::Result<(), String> {
    let Some((_, connection)) = state
        .config
        .connection(connection_id)
        .filter(|(_, c)| c.kind == BLUMINT && c.enabled)
    else {
        return Err(format!(
            "{connection_id} is not an enabled blumint connection"
        ));
    };
    let key = state
        .integrations
        .api_keys
        .get(connection_id)
        .ok_or_else(|| {
            format!("no BluMint API key for {connection_id} in the integration secrets")
        })?;
    // The connection's record decides, as it does for result posts, checked
    // again before each call so a connection disabled meanwhile stops them.
    let disabled = || format!("{connection_id} is disabled");
    if !policy::stored_enabled(state, connection_id).await {
        return Err(disabled());
    }
    let token = crate::issue_credential(state, Some(connection_id), None, "BluMint callbacks")
        .await
        .map_err(|_| "could not issue the callback credential".to_owned())?;
    let authentication = json!({
        "config": { "location": "header", "key": "Authorization", "valuePrefix": "Bearer " },
        "apiKey": token,
    });
    let origin = &state.config.origin;
    let client = outbound_client()
        .build()
        .map_err(|error| error.to_string())?;
    for (path, field, ours) in [
        (
            "/tournaments/setLookupPlayer",
            "lookupPlayerUrl",
            LOOKUP_PATH,
        ),
        (
            "/tournaments/match/setCreate",
            "createMatchUrl",
            MATCHES_PATH,
        ),
        (
            "/tournaments/match/setRetrieveStatus",
            "retrieveMatchStatusUrl",
            STATUS_PATH,
        ),
    ] {
        if !policy::stored_enabled(state, connection_id).await {
            return Err(disabled());
        }
        let body = json!({ field: format!("{origin}{ours}"), "authentication": authentication });
        let response = client
            .post(format!("{}{path}", api_base(connection)))
            .header("x-api-key", key.as_str())
            .header("content-type", "application/json")
            .body(body.to_string())
            .send()
            .await
            .map_err(|error| format!("{path}: {error}"))?;
        if !response.status().is_success() {
            return Err(format!("{path}: BluMint answered {}", response.status()));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_match_state_reads_as_one_blumint_status() {
        use MatchState::*;
        let table = [
            (Created, "pending"),
            (AwaitingPlayers, "pending"),
            (Provisioning, "pending"),
            (Ready, "pending"),
            (Running, "running"),
            (BetweenGames, "running"),
            (AwaitingReports, "running"),
            (NeedsReview, "running"),
            (Completed, "complete"),
            (Cancelled, "cancelled"),
            (Failed, "cancelled"),
            (Expired, "cancelled"),
        ];
        for (state, status) in table {
            assert_eq!(match_status(state), status, "{state:?}");
        }
    }

    #[test]
    fn a_player_is_present_once_their_ember_claimed_and_ready_once_the_room_is_bound() {
        assert_eq!(presence(MatchState::Running, false), "absent");
        assert_eq!(
            presence(MatchState::AwaitingPlayers, true),
            "present-not-ready"
        );
        assert_eq!(
            presence(MatchState::Provisioning, true),
            "present-not-ready"
        );
        assert_eq!(presence(MatchState::Ready, true), "present-ready");
        assert_eq!(presence(MatchState::Completed, true), "present-ready");
    }
}
