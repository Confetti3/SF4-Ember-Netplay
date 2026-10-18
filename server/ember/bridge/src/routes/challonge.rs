//! Challonge sign-in (optional; on when the configuration has `challonge`),
//! through Challonge's device grant, so nothing has to come back to the
//! bridge in a browser. A player's Ember asks to start it with a
//! `challonge.connect` proof and shows the code the bridge answers with; the
//! player enters it on Challonge's page. The bridge polls Challonge until the
//! player approves (`poll`), then reads the player's Challonge user ID and
//! username (`me` scope) and drops the token. No Challonge token is stored.
//!
//! An account no other Ember ID holds is connected at once. One another
//! Ember ID holds stays there while that Ember ID is in use: the sign-in
//! ends `held`, and the player disconnects it there first. Only from an
//! Ember ID that has opened no session for `MOVE_IDLE_SECS` can it move: the
//! sign-in waits in `moving` until the player chooses (`choose`), and the
//! choice holds only while that Ember ID still has it and is still unused.
//! Whoever enters a sign-in's code on Challonge's page connects their account
//! to the Ember ID that started it, so a code someone else was handed cannot
//! take an account from an Ember ID its owner still uses. One sign-in per
//! Ember ID: starting one replaces the earlier one, and the player can end it
//! (`cancel`).
//!
//! Sign-ins are bounded bridge-wide, by those waiting at once and by
//! Challonge requests per month (`budget`), since Challonge's free tier
//! allows 500 requests a month.
//!
//! Bracket connectors find players by the Challonge username a tournament
//! entry shows (`find_and_link`); the links that makes follow the account
//! as `linking::Site` describes.
mod api;
mod budget;
mod poll;

use axum::{
    extract::State,
    http::{HeaderMap, StatusCode},
    response::Response,
};
pub use ember_protocol::challonge::{ACCOUNT_PATH, MOVE_PATH, START_PATH};
use ember_protocol::{
    EmberId,
    api::ErrorCode,
    challenge::{Action, Method},
    challonge::{
        Account, Choice, Connection, MOVE_IDLE_SECS, MoveChoice, SIGN_IN_SECS, SignIn,
        SignInStarted, SignInState, username_key,
    },
    partner::FoundChallongePlayer,
};
use rusqlite::{OptionalExtension, Transaction, params};
use serde_json::json;

pub use self::poll::{poll, poll_forever};
use crate::{
    AppState, auth,
    config::Challonge,
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, PROOF_BODY, json, ok},
    linking::Site,
    routes::sessions::{self, Nothing, Target},
};

/// Sign-ins one Ember ID may start per hour.
const STARTS_PER_HOUR: usize = 10;
/// The interval RFC 8628 gives a device code that names none.
const DEFAULT_POLL_SECS: u64 = 5;
/// The fastest the bridge polls Challonge for one sign-in, whatever interval
/// Challonge asks for: its free tier allows 500 requests a month.
const MIN_POLL_SECS: u64 = 5;
/// How long a sign-in that ended stays readable after its code lapsed, so
/// the game can say how it ended.
const ENDED_KEEP_SECS: u64 = SIGN_IN_SECS;

/// Challonge sign-in, offered when the configuration has `challonge`; its
/// client secret is optional. Reading, cancelling and disconnecting need
/// neither, so a player can always withdraw an account.
pub fn sign_in(state: &AppState) -> Option<&Challonge> {
    state.config.challonge.as_ref()
}

fn enabled(state: &AppState) -> Result<&Challonge> {
    sign_in(state).ok_or_else(ApiFailure::not_found)
}

/// A start refused for the bridge-wide limits.
fn busy() -> ApiFailure {
    ApiFailure::new(
        ErrorCode::ServiceUnavailable,
        "Challonge sign-in is busy. Try again later.",
    )
}

/// Whether `max_waiting_sign_ins` other Ember IDs' sign-ins wait already.
fn waiting_full(
    tx: &Transaction<'_>,
    challonge: &Challonge,
    ember_id: &str,
    now: u64,
) -> Result<bool> {
    let waiting: u64 = tx.query_row(
        "SELECT COUNT(*) FROM challonge_sign_ins WHERE state = 'waiting' AND expires_at > ?1 AND ember_id != ?2",
        params![now, ember_id],
        |row| row.get(0),
    )?;
    Ok(waiting >= challonge.max_waiting_sign_ins)
}

/// `POST /v1/challonge/start`: the code to show and where to enter it.
pub async fn start(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let challonge = enabled(&state)?.clone();
    let player = auth::player(&state, &headers).await?;
    state
        .rate
        .check(
            &format!("challonge-start:{}", player.ember_id),
            STARTS_PER_HOUR,
            3600,
            state.now(),
        )
        .map_err(ApiFailure::rate_limited)?;
    let (request, _): (_, Nothing) = sessions::proven(&body.0)?;
    // Room for one more, and a request from the month's budget to ask with.
    let (limits, ember_id, now) = (challonge.clone(), player.ember_id.to_string(), state.now());
    state
        .db
        .write(move |tx| {
            if waiting_full(tx, &limits, &ember_id, now)? || budget::take(tx, &limits, now, 1)? == 0
            {
                return Err(busy());
            }
            Ok(())
        })
        .await?;
    let device = api::authorize_device(&challonge).await.ok_or_else(|| {
        ApiFailure::new(
            ErrorCode::ServiceUnavailable,
            "Challonge did not start a sign-in. Try again later.",
        )
    })?;
    let now = state.now();
    let interval = device
        .interval
        .unwrap_or(DEFAULT_POLL_SECS)
        .max(MIN_POLL_SECS);
    let started = SignInStarted {
        user_code: device.user_code,
        verification_uri: device.verification_uri,
        verification_uri_complete: device.verification_uri_complete,
        expires_at: now + device.expires_in.min(SIGN_IN_SECS),
    };
    let stored = started.clone();
    let target = Target {
        action: Action::ChallongeConnect,
        method: Method::Post,
        path: START_PATH,
    };
    sessions::as_proven(&state, player, request, target, move |tx, ctx, ember_id| {
        // Others may have started meanwhile.
        if waiting_full(tx, &challonge, ember_id, ctx.now)? {
            return Err(busy());
        }
        // Only the latest sign-in is polled.
        Site::Challonge.end_sign_ins(tx, ember_id)?;
        tx.execute(
            "INSERT INTO challonge_sign_ins (ember_id, state, device_code, user_code, verification_uri,
               verification_uri_complete, expires_at, interval_secs, next_poll_at)
             VALUES (?1, 'waiting', ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
            params![
                ember_id,
                device.device_code.as_str(),
                stored.user_code,
                stored.verification_uri,
                stored.verification_uri_complete,
                stored.expires_at,
                interval,
                now + interval
            ],
        )?;
        Ok(())
    })
    .await?;
    Ok(json(StatusCode::CREATED, &started))
}

/// `DELETE /v1/challonge/start`: ends the player's sign-in, so a code still
/// shown connects nothing. Needs no sign-in offer, like disconnecting.
pub async fn cancel(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let (request, _): (_, Nothing) = sessions::proven(&body.0)?;
    let target = Target {
        action: Action::ChallongeCancel,
        method: Method::Delete,
        path: START_PATH,
    };
    sessions::as_proven(&state, player, request, target, |tx, _, ember_id| {
        Site::Challonge.end_sign_ins(tx, ember_id)
    })
    .await?;
    Ok(ok(&json!({})))
}

/// Why a move was refused.
enum Refused {
    /// The account was disconnected or moved since it signed in; the
    /// sign-in is done.
    Changed,
    /// The Ember ID that has it was used since; the sign-in ends `held`.
    InUse,
}

/// `POST /v1/challonge/move`: the player's choice for a sign-in in `moving`.
/// Keeping it ends the sign-in. Moving it answers `link_conflict`, and moves
/// nothing, if the account was disconnected or moved since it signed in, or
/// if the Ember ID that has it opened a session since (the sign-in then ends
/// `held`).
pub async fn choose(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    enabled(&state)?;
    let player = auth::player(&state, &headers).await?;
    let (request, MoveChoice { choice }): (_, MoveChoice) = sessions::proven(&body.0)?;
    let target = Target {
        action: Action::ChallongeMove,
        method: Method::Post,
        path: MOVE_PATH,
    };
    let chosen = sessions::as_proven(&state, player, request, target, move |tx, ctx, ember_id| {
        let (user_id, username, asked_owner): (String, String, String) = tx
            .query_row(
                "SELECT pending_user_id, pending_username, pending_owner FROM challonge_sign_ins
                 WHERE ember_id = ?1 AND state = 'moving' AND expires_at > ?2",
                params![ember_id, ctx.now],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
            )
            .optional()?
            .ok_or_else(ApiFailure::not_found)?;
        if choice == Choice::Move {
            if holder(tx, &user_id)?.as_deref() != Some(asked_owner.as_str()) {
                Site::Challonge.end_sign_ins(tx, ember_id)?;
                return Ok(Err(Refused::Changed));
            }
            if in_use(tx, &asked_owner, ctx.now)? {
                hold(tx, ember_id, &username)?;
                return Ok(Err(Refused::InUse));
            }
            store(tx, ctx, ember_id, &user_id, &username)?;
        }
        Site::Challonge.end_sign_ins(tx, ember_id)?;
        read(tx, ember_id, ctx.now).map(Ok)
    })
    .await?;
    let message = match chosen {
        Ok(connection) => return Ok(ok(&connection)),
        Err(Refused::Changed) => {
            "That Challonge account was disconnected or moved after it signed in, so nothing moved."
        }
        Err(Refused::InUse) => {
            "The Ember ID that has that Challonge account was used again, so it stays there. \
             Disconnect it there first."
        }
    };
    Err(ApiFailure::new(ErrorCode::LinkConflict, message))
}

/// `GET /v1/challonge`: the session player's connected account and sign-in.
pub async fn get(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let now = state.now();
    let connection = state
        .db
        .read(move |tx| read(tx, player.ember_id.as_str(), now))
        .await?;
    Ok(ok(&connection))
}

/// `DELETE /v1/challonge`: disconnects the session player's account.
pub async fn remove(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let (request, _): (_, Nothing) = sessions::proven(&body.0)?;
    let target = Target {
        action: Action::ChallongeRemove,
        method: Method::Delete,
        path: ACCOUNT_PATH,
    };
    sessions::as_proven(&state, player, request, target, |tx, ctx, ember_id| {
        Site::Challonge.disconnect(tx, ctx, ember_id)
    })
    .await?;
    Ok(ok(&Connection {
        account: None,
        sign_in: None,
    }))
}

/// The Ember ID's account and sign-in. A sign-in still waiting or moving
/// when its code lapsed reads as expired before maintenance records it.
fn read(tx: &Transaction<'_>, ember_id: &str, now: u64) -> Result<Connection> {
    let account = tx
        .query_row(
            "SELECT user_id, username, connected_at FROM challonge_accounts WHERE ember_id = ?1",
            [ember_id],
            |row| {
                Ok(Account {
                    user_id: row.get(0)?,
                    username: row.get(1)?,
                    connected_at: row.get(2)?,
                })
            },
        )
        .optional()?;
    let row: Option<(String, SignIn)> = tx
        .query_row(
            "SELECT state, user_code, verification_uri, verification_uri_complete, expires_at, pending_username
             FROM challonge_sign_ins WHERE ember_id = ?1",
            [ember_id],
            |row| {
                let sign_in = SignIn {
                    state: SignInState::Waiting,
                    user_code: row.get(1)?,
                    verification_uri: row.get(2)?,
                    verification_uri_complete: row.get(3)?,
                    expires_at: row.get(4)?,
                    username: row.get(5)?,
                };
                Ok((row.get(0)?, sign_in))
            },
        )
        .optional()?;
    let sign_in = match row {
        Some((state, mut sign_in)) => {
            sign_in.state = match SignInState::parse(&state).ok_or_else(ApiFailure::unavailable)? {
                SignInState::Waiting | SignInState::Moving if sign_in.expires_at <= now => {
                    SignInState::Expired
                }
                state => state,
            };
            // The account that signed in is named only while it waits for
            // the player's choice, or to say where it is held.
            if !matches!(sign_in.state, SignInState::Moving | SignInState::Held) {
                sign_in.username = None;
            }
            Some(sign_in)
        }
        None => None,
    };
    Ok(Connection { account, sign_in })
}

/// The Ember ID Challonge account `user_id` is connected to, if any.
fn holder(tx: &Transaction<'_>, user_id: &str) -> Result<Option<String>> {
    Ok(tx
        .query_row(
            "SELECT ember_id FROM challonge_accounts WHERE user_id = ?1",
            [user_id],
            |row| row.get(0),
        )
        .optional()?)
}

/// Whether `ember_id` opened a player session within `MOVE_IDLE_SECS`
/// (`identities.last_session_at`, which every session creation sets).
fn in_use(tx: &Transaction<'_>, ember_id: &str, now: u64) -> Result<bool> {
    let last_session_at: u64 = tx.query_row(
        "SELECT last_session_at FROM identities WHERE ember_id = ?1",
        [ember_id],
        |row| row.get(0),
    )?;
    Ok(now < last_session_at + MOVE_IDLE_SECS)
}

/// Ends the Ember ID's sign-in as `held`, naming the account another Ember
/// ID in use has.
fn hold(tx: &Transaction<'_>, ember_id: &str, username: &str) -> Result<()> {
    tx.execute(
        "UPDATE challonge_sign_ins SET state = 'held', device_code = NULL,
           pending_user_id = NULL, pending_username = ?2, pending_owner = NULL
         WHERE ember_id = ?1",
        params![ember_id, username],
    )?;
    Ok(())
}

/// Connects Challonge account `user_id` to `ember_id`. The latest sign-in
/// wins (`Site::release`).
fn store(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    ember_id: &str,
    user_id: &str,
    username: &str,
) -> Result<()> {
    Site::Challonge.release(tx, ctx, ember_id, user_id)?;
    tx.execute(
        "INSERT INTO challonge_accounts (user_id, ember_id, username, username_key, connected_at)
         VALUES (?1, ?2, ?3, ?4, ?5)",
        params![user_id, ember_id, username, username_key(username), ctx.now],
    )?;
    Ok(())
}

/// The players whose Ember IDs are connected to Challonge accounts with these
/// usernames, compared without case, and linked on `connection_id`, in order
/// and without repeats. Where two accounts once had the same username, the
/// one connected most recently answers. An account `Site::link` cannot link
/// is left out. The caller checked the usernames.
pub fn find_and_link(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    connection_id: &str,
    usernames: &[String],
) -> Result<Vec<FoundChallongePlayer>> {
    let mut found: Vec<FoundChallongePlayer> = Vec::new();
    for username in usernames {
        let account: Option<(String, String, String)> = tx
            .query_row(
                "SELECT user_id, ember_id, username FROM challonge_accounts WHERE username_key = ?1
                 ORDER BY connected_at DESC, user_id LIMIT 1",
                [username_key(username)],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
            )
            .optional()?;
        let Some((user_id, ember_id, connected_name)) = account else {
            continue;
        };
        let ember_id = EmberId::parse(&ember_id).map_err(|_| ApiFailure::unavailable())?;
        if found.iter().any(|player| player.ember_id == ember_id) {
            continue;
        }
        let Some(participant_id) =
            Site::Challonge.link(tx, ctx, connection_id, &user_id, &connected_name, &ember_id)?
        else {
            continue;
        };
        found.push(FoundChallongePlayer {
            username: username.clone(),
            challonge_user_id: user_id,
            ember_id,
            participant_id,
        });
    }
    Ok(found)
}

/// Records sign-ins whose code lapsed as expired, dropping what they held,
/// and removes them once the game has had time to read how they ended.
pub fn expire(tx: &Transaction<'_>, now: u64) -> Result<()> {
    tx.execute(
        "UPDATE challonge_sign_ins SET state = 'expired', device_code = NULL,
           pending_user_id = NULL, pending_username = NULL, pending_owner = NULL
         WHERE state IN ('waiting', 'moving') AND expires_at <= ?1",
        [now],
    )?;
    tx.execute(
        "DELETE FROM challonge_sign_ins WHERE expires_at <= ?1",
        [now.saturating_sub(ENDED_KEEP_SECS)],
    )?;
    Ok(())
}
