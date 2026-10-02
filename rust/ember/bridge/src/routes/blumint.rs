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
//! organizer review; a restart is its way to replay. `register` tells BluMint
//! where the three endpoints are.
use std::collections::BTreeMap;

use axum::{
    extract::{Query, State},
    http::HeaderMap,
    response::Response,
};
use ember_protocol::{
    EmberId, json as wire,
    matches::{MatchState, Participant, Rules},
    play::{PROFILE, play_url},
};
use rusqlite::{Transaction, params};
use serde::Deserialize;
use serde_json::{Value, json};
use zeroize::Zeroizing;

use crate::{
    AppState, auth,
    config::{BLUMINT, Connection},
    delivery::{BATCH, Leased, Outcome, Queue, Record, Settled},
    error::{ApiFailure, Result},
    http::{Body, GENERAL_BODY, ok},
    routes::{
        discord,
        ledger::{Match, load, participants, scores},
        links::{Ctx, audit},
        matches::{NewMatch, insert_match},
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
/// Ember. `matchSettings.gamesToWin` (1, 2, 3 or 5) sets its length; other
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
            .filter(|n| matches!(n, 1 | 2 | 3 | 5))
            .ok_or_else(|| ApiFailure::invalid("matchSettings.gamesToWin must be 1, 2, 3 or 5."))?,
    };
    let ctx = Ctx::of(&state);
    let bridge_id = state.config.bridge_id.clone();
    let match_id = state
        .db
        .write(move |tx| {
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

/// `GET /v1/blumint/matches/status?matchId=`.
pub async fn status(
    State(state): State<AppState>,
    headers: HeaderMap,
    Query(query): Query<StatusQuery>,
) -> Result<Response> {
    let connection_id = connection(&state, &headers).await?;
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
        Cancelled | Failed => "cancelled",
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

/// Whether the connection's record is enabled.
fn enabled(tx: &Transaction<'_>, connection_id: &str) -> Result<bool> {
    Ok(tx.query_row(
        "SELECT enabled FROM provider_connections WHERE id = ?1",
        [connection_id],
        |row| row.get(0),
    )?)
}

async fn stored_enabled(state: &AppState, connection_id: &str) -> bool {
    let id = connection_id.to_owned();
    state
        .db
        .read(move |tx| enabled(tx, &id))
        .await
        .unwrap_or(false)
}

/// What the bridge sends BluMint for a match, or None while it is still on.
fn submission(tx: &Transaction<'_>, found: &Match) -> Result<Option<Value>> {
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
        _ => None,
    })
}

/// Where one connection's results go, with BluMint's API key for it.
struct Destination {
    submit_url: String,
    key: Zeroizing<String>,
}

/// Finished matches' results (a `delivery::Queue`), for the enabled BluMint
/// connections whose API key is configured. Matches of a connection still
/// waiting for its key, or disabled, are never leased, so they hold nobody
/// else's back, and go out once it has its key and is enabled again.
/// Leasing marks a result `delivering`; from then on it cannot be corrected
/// (`ledger::settle`), so what is sent is what the match keeps.
pub struct Results {
    destinations: BTreeMap<String, Destination>,
}

impl Results {
    pub fn new(state: &AppState) -> Self {
        let destinations = state
            .config
            .tenants
            .iter()
            .flat_map(|tenant| &tenant.connections)
            .filter(|connection| connection.kind == BLUMINT && connection.enabled)
            .filter_map(|connection| {
                let key = state.integrations.api_keys.get(&connection.id)?;
                let destination = Destination {
                    submit_url: format!("{}/tournaments/match/submit", api_base(connection)),
                    key: key.clone(),
                };
                Some((connection.id.clone(), destination))
            })
            .collect();
        Self { destinations }
    }
}

pub struct Due {
    match_id: String,
    connection_id: String,
}

impl Queue for Results {
    type Item = Due;

    fn lease(&self, tx: &Transaction<'_>, now: u64, until: u64) -> Result<Vec<Leased<Due>>> {
        let connections: Vec<&String> = self.destinations.keys().collect();
        let rows = tx
            .prepare(
                "SELECT m.id, m.connection_id, m.delivery_attempts, m.delivery_first_at
                 FROM matches m JOIN provider_connections c ON c.id = m.connection_id
                 WHERE m.delivery_state IN ('queued', 'retrying', 'delivering') AND m.delivery_next_at <= ?1
                   AND m.state IN ('completed', 'cancelled', 'failed')
                   AND m.connection_id IN (SELECT value FROM json_each(?2)) AND c.enabled = 1
                 ORDER BY m.delivery_next_at, m.id LIMIT ?3",
            )?
            .query_map(params![now, json!(connections).to_string(), BATCH], |row| {
                Ok(Leased {
                    item: Due {
                        match_id: row.get(0)?,
                        connection_id: row.get(1)?,
                    },
                    attempt: row.get::<_, u32>(2)? + 1,
                    first_attempt_at: row.get(3)?,
                })
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        for row in &rows {
            tx.execute(
                "UPDATE matches SET delivery_state = 'delivering', delivery_next_at = ?1, delivery_attempts = ?2
                 WHERE id = ?3",
                params![until, row.attempt, row.item.match_id],
            )?;
        }
        Ok(rows)
    }

    async fn attempt(&self, state: &AppState, item: &Due) -> Outcome {
        let retry = |error: String, retry_after: Option<u64>| Outcome::Retry { error, retry_after };
        let Some(destination) = self.destinations.get(&item.connection_id) else {
            return Outcome::Cancelled;
        };
        let id = item.match_id.clone();
        // Checked again as late as possible, as webhooks are.
        let body = state
            .db
            .read(move |tx| match load(tx, &id)? {
                Some(found) if enabled(tx, &found.connection_id)? => submission(tx, &found),
                _ => Ok(None),
            })
            .await;
        let body = match body {
            Ok(Some(body)) => body,
            // Its connection was disabled since the lease; it is sent once the
            // connection is enabled again, its attempts one higher.
            Ok(None) => return Outcome::Cancelled,
            Err(_) => return retry("database unavailable".into(), None),
        };
        let Ok(client) = outbound_client().build() else {
            return retry("client".into(), None);
        };
        let response = client
            .post(&destination.submit_url)
            .header("x-api-key", destination.key.as_str())
            .header("content-type", "application/json")
            .body(body.to_string())
            .send()
            .await;
        let response = match response {
            Ok(response) => response,
            Err(error) if error.is_timeout() => return retry("timeout".into(), None),
            Err(_) => return retry("connection failed".into(), None),
        };
        let status = response.status().as_u16();
        let retry_after = response
            .headers()
            .get("retry-after")
            .and_then(|value| value.to_str().ok())
            .and_then(|value| value.parse::<u64>().ok());
        match status {
            // 409: BluMint already has this match's result.
            200..=299 | 409 => Outcome::Delivered,
            429 => retry(format!("http {status}"), retry_after),
            // Any other refusal is the same on a retry.
            400..=499 => Outcome::Refused(format!("http {status}")),
            _ => retry(format!("http {status}"), retry_after),
        }
    }

    fn record(&self, tx: &Transaction<'_>, leased: &Leased<Due>, record: &Record) -> Result<()> {
        let (state, next, error) = match &record.settled {
            Settled::Delivered => ("delivered", record.at, None),
            Settled::Retry { at, error } => ("retrying", *at, Some(error.as_str())),
            Settled::Refused(error) | Settled::Expired(error) => {
                ("failed", record.at, Some(error.as_str()))
            }
        };
        let changed = tx.execute(
            "UPDATE matches SET delivery_state = ?1, delivery_next_at = ?2,
                delivery_first_at = COALESCE(delivery_first_at, ?3)
             WHERE id = ?4 AND delivery_state = 'delivering' AND delivery_attempts = ?5",
            params![
                state,
                next,
                record.first_attempt_at,
                leased.item.match_id,
                leased.attempt
            ],
        )?;
        if changed == 1 {
            audit(
                tx,
                record.at,
                ("bridge", BLUMINT.into()),
                "blumint.submit",
                &leased.item.match_id,
                state,
                error,
            )?;
        }
        Ok(())
    }
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
    if !stored_enabled(state, connection_id).await {
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
        if !stored_enabled(state, connection_id).await {
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
        ];
        for (state, status) in table {
            assert_eq!(match_status(state), status, "{state:?}");
        }
    }

    #[tokio::test]
    async fn only_the_latest_lease_records_its_outcome() {
        let path = std::env::temp_dir().join(format!(
            "ember-bridge-lease-{}.sqlite3",
            ember_protocol::encoding::b64u(&crate::util::random::<12>())
        ));
        let db = crate::Db::open(&path).unwrap();
        db.write(|tx| {
            tx.execute_batch(
                "INSERT INTO tenants (id, name) VALUES ('bm', 'BluMint');
                 INSERT INTO provider_connections (id, tenant_id, kind, environment, display_name, enabled)
                   VALUES ('bm-partner', 'bm', 'blumint', 'staging', 'BluMint', 1);
                 INSERT INTO matches (id, tenant_id, connection_id, external_match_id, create_digest, revision,
                   assignment_generation, state, games_to_win, rules, required_build_id, metadata,
                   delivery_state, created_at, updated_at)
                   VALUES ('emt_1', 'bm', 'bm-partner', 'blumint_1', '', 1, 1, 'completed', 2, '{}', '', '{}',
                   'queued', 0, 0);",
            )?;
            Ok(())
        })
        .await
        .unwrap();
        let results = std::sync::Arc::new(Results {
            destinations: [(
                "bm-partner".to_owned(),
                Destination {
                    submit_url: String::new(),
                    key: Zeroizing::new(String::new()),
                },
            )]
            .into(),
        });
        let lease = |now: u64| {
            let results = results.clone();
            db.write(move |tx| results.lease(tx, now, now + 60))
        };
        let record = |leased: Leased<Due>, settled: Settled| {
            let results = results.clone();
            db.write(move |tx| {
                results.record(
                    tx,
                    &leased,
                    &Record {
                        at: 100,
                        first_attempt_at: 0,
                        settled,
                    },
                )
            })
        };
        let first = lease(0).await.unwrap().pop().unwrap();
        assert!(lease(30).await.unwrap().is_empty(), "leased twice");
        let marked: String = db
            .read(|tx| {
                Ok(tx.query_row(
                    "SELECT delivery_state FROM matches WHERE id = 'emt_1'",
                    [],
                    |row| row.get(0),
                )?)
            })
            .await
            .unwrap();
        assert_eq!(marked, "delivering");
        // The first attempt outlives its lease and is leased again.
        let second = lease(61).await.unwrap().pop().unwrap();
        assert_eq!((first.attempt, second.attempt), (1, 2));
        record(
            first,
            Settled::Retry {
                at: 101,
                error: "timeout".into(),
            },
        )
        .await
        .unwrap();
        record(second, Settled::Delivered).await.unwrap();
        let stored: (String, u32) = db
            .read(|tx| {
                Ok(tx.query_row(
                    "SELECT delivery_state, delivery_attempts FROM matches WHERE id = 'emt_1'",
                    [],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )?)
            })
            .await
            .unwrap();
        assert_eq!(stored, ("delivered".to_owned(), 2));
        drop(db);
        for suffix in ["", "-wal", "-shm"] {
            let _ = std::fs::remove_file(format!("{}{suffix}", path.display()));
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
