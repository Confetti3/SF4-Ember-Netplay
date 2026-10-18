//! Discord sign-in (optional; on when the configuration has `discord`). A
//! player's Ember asks to start it with a `discord.connect` proof and opens
//! the returned Discord page in the browser; Discord sends the browser back
//! to the callback with a code, which the bridge exchanges for the player's
//! Discord user ID (`identify` scope only). The bridge keeps the user ID and
//! username, so platforms that find players by Discord account get the Ember
//! ID. Nothing else from Discord is stored, and the access token is dropped.
//!
//! A platform's link approved by a Discord sign-in (`approved_via` `discord`,
//! claim `discord:<user ID>`) follows the account as `linking::Site`
//! describes: it ends when the account moves or is disconnected, and a link
//! the player removes is not made again until a newer sign-in.
//!
//! A sign-in connects nothing by itself unless the account is already this
//! Ember ID's: the browser page names the Ember ID it would connect to and
//! asks first, so an authorize link someone else started cannot tie the
//! person who opens it to their Ember ID. For an account connected to
//! another Ember ID the page also says what moving it ends. The answer holds
//! only for the owner the page named: if the account was connected or moved
//! elsewhere meanwhile, it changes nothing.
//! Starting a sign-in ends the Ember ID's earlier ones, and the player can
//! end the one in flight (`cancel`), so only the latest can connect. The
//! pages the browser shows are in `pages`.
mod pages;

use axum::{
    Form,
    extract::{Query, State},
    http::{HeaderMap, StatusCode},
    response::Response,
};
pub use ember_protocol::discord::{ACCOUNT_PATH, CALLBACK_PATH, START_PATH};
use ember_protocol::{
    EmberId,
    challenge::{Action, Method},
    discord::{Account, Connection, SIGN_IN_SECS, SignInStarted, is_user_id},
    encoding::b64u,
    partner::FoundPlayer,
};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::Deserialize;
use serde_json::json;

use self::pages::{Sites, connected, connecting, finished, moving, not_saved, not_waiting, off};
use crate::{
    AppState, auth,
    config::Discord,
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, PROOF_BODY, json, ok},
    linking::Site,
    routes::sessions::{self, Nothing, Target},
    util::{html, outbound_client, random},
};

/// Sign-ins one Ember ID may start per hour.
const STARTS_PER_HOUR: usize = 10;

/// Discord sign-in, offered when the configuration has `discord` and the
/// integration secrets hold its client secret. Reading and disconnecting a
/// connected account need neither, so a player can always withdraw one.
pub fn sign_in(state: &AppState) -> Option<&Discord> {
    state
        .config
        .discord
        .as_ref()
        .filter(|_| state.integrations.discord_client_secret.is_some())
}

fn enabled(state: &AppState) -> Result<&Discord> {
    sign_in(state).ok_or_else(ApiFailure::not_found)
}

fn state_hash(state: &AppState, text: &str) -> [u8; 32] {
    state.keys.keyed_hash("discord-sign-in", text.as_bytes())
}

/// The key of a sign-in waiting on the page's question: a secret of its own,
/// made when the question is asked and shown only on that page. The sign-in's
/// state was handed to the Ember that started it, so it does not answer the
/// question.
fn confirmation_hash(state: &AppState, text: &str) -> [u8; 32] {
    state.keys.keyed_hash("discord-move", text.as_bytes())
}

fn redirect_uri(state: &AppState) -> String {
    format!("{}{CALLBACK_PATH}", state.config.origin)
}

/// Runs a `discord.*` proof from the session's player.
async fn proven(
    state: &AppState,
    player: auth::Player,
    body: &Body<PROOF_BODY>,
    action: Action,
    method: Method,
    path: &'static str,
    work: impl FnOnce(&Transaction<'_>, &Ctx, &str) -> Result<()> + Send + 'static,
) -> Result<()> {
    let (request, _): (_, Nothing) = sessions::proven(&body.0)?;
    let target = Target {
        action,
        method,
        path,
    };
    sessions::as_proven(state, player, request, target, work).await
}

/// `POST /v1/discord/start`: where to send the player's browser.
pub async fn start(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let discord = enabled(&state)?.clone();
    let player = auth::player(&state, &headers).await?;
    state
        .rate
        .check(
            &format!("discord-start:{}", player.ember_id),
            STARTS_PER_HOUR,
            3600,
            state.now(),
        )
        .map_err(ApiFailure::rate_limited)?;
    let secret = b64u(&random::<32>());
    let hash = state_hash(&state, &secret);
    let expires_at = state.now() + SIGN_IN_SECS;
    proven(
        &state,
        player,
        &body,
        Action::DiscordConnect,
        Method::Post,
        START_PATH,
        move |tx, ctx, ember_id| {
            expire(tx, ctx.now)?;
            // Only the latest sign-in can connect an account.
            Site::Discord.end_sign_ins(tx, ember_id)?;
            tx.execute(
                "INSERT INTO discord_sign_ins (state_hash, ember_id, expires_at) VALUES (?1, ?2, ?3)",
                params![hash.as_slice(), ember_id, expires_at],
            )?;
            Ok(())
        },
    )
    .await?;
    let authorize = format!(
        "{}/oauth2/authorize",
        discord.api_base.trim_end_matches("/api")
    );
    let authorize_url = url::Url::parse_with_params(
        &authorize,
        [
            ("response_type", "code"),
            ("client_id", discord.client_id.as_str()),
            ("scope", "identify"),
            ("redirect_uri", redirect_uri(&state).as_str()),
            ("state", secret.as_str()),
            ("prompt", "consent"),
        ],
    )
    .map_err(|_| ApiFailure::unavailable())?;
    Ok(json(
        StatusCode::CREATED,
        &SignInStarted {
            authorize_url: authorize_url.into(),
            expires_at,
        },
    ))
}

#[derive(Deserialize)]
pub struct Answer {
    #[serde(default)]
    code: Option<String>,
    #[serde(default)]
    state: Option<String>,
    #[serde(default)]
    error: Option<String>,
}

/// `DELETE /v1/discord/start`: ends the player's sign-ins in flight, so a
/// page left open in the browser connects nothing. Needs no sign-in offer,
/// like disconnecting.
pub async fn cancel(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    proven(
        &state,
        player,
        &body,
        Action::DiscordCancel,
        Method::Delete,
        START_PATH,
        |tx, _, ember_id| Site::Discord.end_sign_ins(tx, ember_id),
    )
    .await?;
    Ok(ok(&json!({})))
}

/// What the callback did with a sign-in Discord confirmed.
enum Stored {
    Connected(String),
    /// The account is not `to`'s yet: connected to `from`, or to no Ember
    /// ID. `replaced` is the account `to` has now, which connecting this one
    /// disconnects. The player decides.
    Asking {
        from: Option<String>,
        to: String,
        replaced: Option<String>,
        confirmation: String,
    },
    Gone,
}

/// `GET /v1/discord/callback`: Discord's answer, in the player's browser.
/// The page says what happened, or asks before connecting an account that
/// is not this Ember ID's yet; the player goes back to the site that sent
/// them.
pub async fn callback(State(state): State<AppState>, Query(answer): Query<Answer>) -> Response {
    let Ok(discord) = enabled(&state).cloned() else {
        return off();
    };
    // Only a sign-in Ember is waiting for reaches Discord, and only for the
    // first answer that claims it; another answer for it is turned away. The
    // claimer takes it, whatever Discord answered, in the same write that
    // stores the account, so a sign-in a disconnect ended meanwhile connects
    // nothing.
    let secret = answer.state.unwrap_or_default();
    let hash = state_hash(&state, &secret);
    let now = state.now();
    let claimed = state
        .db
        .write(move |tx| {
            Ok(tx.execute(
                "UPDATE discord_sign_ins SET claimed = 1
                 WHERE state_hash = ?1 AND expires_at > ?2 AND claimed = 0",
                params![hash.as_slice(), now],
            )? == 1)
        })
        .await;
    if !matches!(claimed, Ok(true)) {
        return not_waiting();
    }
    let code = match (answer.error.as_deref(), answer.code) {
        (None, Some(code)) if !code.is_empty() && code.len() <= 256 => code,
        _ => {
            let _ = state.db.write(move |tx| take(tx, &hash, now)).await;
            return finished(
                "Not connected",
                "Discord did not connect your account. Choose Connect Discord in Ember to try again.",
            );
        }
    };
    let Some(user) = discord_user(&state, &discord, &code).await else {
        let _ = state.db.write(move |tx| take(tx, &hash, now)).await;
        return finished(
            "Discord did not answer",
            "Discord did not confirm your account. Choose Connect Discord in Ember to try again.",
        );
    };
    let shown = user.username.clone();
    let confirmation = b64u(&random::<32>());
    let confirmation_key = confirmation_hash(&state, &confirmation);
    let ctx = Ctx::of(&state);
    let stored = state
        .db
        .write(move |tx| {
            let Some(ember_id) = tx
                .query_row(
                    "SELECT ember_id FROM discord_sign_ins
                     WHERE state_hash = ?1 AND expires_at > ?2 AND claimed = 1 AND pending_user_id IS NULL",
                    params![hash.as_slice(), ctx.now],
                    |row| row.get::<_, String>(0),
                )
                .optional()?
            else {
                return Ok(Stored::Gone);
            };
            let owner: Option<String> = tx
                .query_row(
                    "SELECT ember_id FROM discord_accounts WHERE user_id = ?1",
                    [&user.id],
                    |row| row.get(0),
                )
                .optional()?;
            if owner.as_deref() != Some(ember_id.as_str()) {
                let replaced: Option<String> = tx
                    .query_row(
                        "SELECT username FROM discord_accounts WHERE ember_id = ?1",
                        [&ember_id],
                        |row| row.get(0),
                    )
                    .optional()?;
                // The question is answered with its own secret from here on;
                // the state stops naming this sign-in. The owner it names is
                // kept, so the answer holds only while it still is.
                tx.execute(
                    "UPDATE discord_sign_ins SET state_hash = ?4, pending_user_id = ?2, pending_username = ?3, pending_owner = ?5
                     WHERE state_hash = ?1",
                    params![
                        hash.as_slice(),
                        user.id,
                        user.username,
                        confirmation_key.as_slice(),
                        owner
                    ],
                )?;
                return Ok(Stored::Asking {
                    from: owner,
                    to: ember_id,
                    replaced,
                    confirmation,
                });
            }
            take(tx, &hash, ctx.now)?;
            store(tx, &ctx, &ember_id, &user.id, &user.username)?;
            Ok(Stored::Connected(ember_id))
        })
        .await;
    let sites = Sites::of(&state.config);
    match stored {
        Ok(Stored::Connected(ember_id)) => {
            state.committed();
            connected(&sites, &shown, &ember_id)
        }
        Ok(Stored::Asking {
            from,
            to,
            replaced,
            confirmation,
        }) => {
            state.committed();
            let replaced = replaced.as_deref();
            match from {
                Some(from) => moving(&sites, &confirmation, &shown, &from, &to, replaced),
                None => connecting(&sites, &confirmation, &shown, &to, replaced),
            }
        }
        Ok(Stored::Gone) => not_waiting(),
        Err(_) => not_saved(),
    }
}

#[derive(Deserialize)]
pub struct MoveAnswer {
    confirmation: String,
    choice: String,
}

/// What the player's answer did.
enum Decided {
    /// Connected, or left where it was: the Ember ID asked about and the account.
    Answered(String, String),
    /// The account was connected or moved elsewhere after the page asked.
    Changed,
}

/// `POST /v1/discord/callback`: the player's answer to connecting or moving
/// an account, from the page `callback` showed. Either way the sign-in is
/// done.
pub async fn decide(State(state): State<AppState>, Form(answer): Form<MoveAnswer>) -> Response {
    if enabled(&state).is_err() {
        return off();
    }
    let moves = match answer.choice.as_str() {
        "move" | "connect" => true,
        "keep" | "refuse" => false,
        _ => return not_waiting(),
    };
    let refused = answer.choice == "refuse";
    let hash = confirmation_hash(&state, &answer.confirmation);
    let ctx = Ctx::of(&state);
    let decided = state
        .db
        .write(move |tx| {
            let Some((ember_id, user_id, username, asked_owner)) = tx
                .query_row(
                    "DELETE FROM discord_sign_ins
                     WHERE state_hash = ?1 AND expires_at > ?2 AND claimed = 1 AND pending_user_id IS NOT NULL
                     RETURNING ember_id, pending_user_id, pending_username, pending_owner",
                    params![hash.as_slice(), ctx.now],
                    |row| {
                        Ok((
                            row.get::<_, String>(0)?,
                            row.get::<_, String>(1)?,
                            row.get::<_, String>(2)?,
                            row.get::<_, Option<String>>(3)?,
                        ))
                    },
                )
                .optional()?
            else {
                return Ok(None);
            };
            if moves {
                let owner: Option<String> = tx
                    .query_row(
                        "SELECT ember_id FROM discord_accounts WHERE user_id = ?1",
                        [&user_id],
                        |row| row.get(0),
                    )
                    .optional()?;
                if owner != asked_owner {
                    return Ok(Some(Decided::Changed));
                }
                store(tx, &ctx, &ember_id, &user_id, &username)?;
            }
            Ok(Some(Decided::Answered(ember_id, username)))
        })
        .await;
    let (ember_id, username) = match decided {
        Ok(Some(Decided::Answered(ember_id, username))) => (ember_id, username),
        Ok(Some(Decided::Changed)) => {
            state.committed();
            return finished(
                "Not connected",
                "Your Discord account was connected to an Ember ID after this page asked, so nothing changed. \
                 To connect it here, choose Connect Discord in Ember again.",
            );
        }
        Ok(None) => return not_waiting(),
        Err(_) => return not_saved(),
    };
    state.committed();
    if moves {
        connected(&Sites::of(&state.config), &username, &ember_id)
    } else if refused {
        finished(
            "Not connected",
            &format!(
                "Your Discord account <strong>{}</strong> was not connected to any Ember ID. \
                 If you did not choose Connect Discord in Ember, someone may have sent you that link, \
                 so do not sign in from it again. You can close this tab.",
                html(&username)
            ),
        )
    } else {
        finished(
            "Nothing moved",
            &format!(
                "Your Discord account <strong>{}</strong> stays connected to your other Ember ID. \
                 You can close this tab.",
                html(&username)
            ),
        )
    }
}

/// Connects Discord account `user_id` to `ember_id`. The latest sign-in wins
/// (`Site::release`).
fn store(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    ember_id: &str,
    user_id: &str,
    username: &str,
) -> Result<()> {
    Site::Discord.release(tx, ctx, ember_id, user_id)?;
    tx.execute(
        "INSERT INTO discord_accounts (user_id, ember_id, username, connected_at) VALUES (?1, ?2, ?3, ?4)",
        params![user_id, ember_id, username, ctx.now],
    )?;
    Ok(())
}

/// Takes the claimed sign-in `hash` names if it is still there: the Ember ID
/// that started it.
fn take(tx: &Transaction<'_>, hash: &[u8; 32], now: u64) -> Result<Option<String>> {
    Ok(tx
        .query_row(
            "DELETE FROM discord_sign_ins WHERE state_hash = ?1 AND expires_at > ?2 AND claimed = 1
             RETURNING ember_id",
            params![hash.as_slice(), now],
            |row| row.get(0),
        )
        .optional()?)
}

struct DiscordUser {
    id: String,
    username: String,
}

/// Exchanges the code and reads who signed in, or None on any failure.
async fn discord_user(state: &AppState, discord: &Discord, code: &str) -> Option<DiscordUser> {
    #[derive(Deserialize)]
    struct Token {
        access_token: String,
    }
    #[derive(Deserialize)]
    struct User {
        id: String,
        username: String,
    }
    let secret = state.integrations.discord_client_secret.as_ref()?;
    let client = outbound_client().build().ok()?;
    let form = url::form_urlencoded::Serializer::new(String::new())
        .append_pair("grant_type", "authorization_code")
        .append_pair("code", code)
        .append_pair("redirect_uri", &redirect_uri(state))
        .append_pair("client_id", &discord.client_id)
        .append_pair("client_secret", secret)
        .finish();
    let response = client
        .post(format!("{}/oauth2/token", discord.api_base))
        .header("content-type", "application/x-www-form-urlencoded")
        .body(form)
        .send()
        .await
        .ok()?;
    if !response.status().is_success() {
        return None;
    }
    let token: Token = serde_json::from_slice(&response.bytes().await.ok()?).ok()?;
    let response = client
        .get(format!("{}/users/@me", discord.api_base))
        .bearer_auth(&token.access_token)
        .send()
        .await
        .ok()?;
    if !response.status().is_success() {
        return None;
    }
    let user: User = serde_json::from_slice(&response.bytes().await.ok()?).ok()?;
    let username: String = user
        .username
        .chars()
        .filter(|c| !c.is_control())
        .take(64)
        .collect();
    (is_user_id(&user.id) && !username.is_empty()).then_some(DiscordUser {
        id: user.id,
        username,
    })
}

/// `GET /v1/discord`: the session player's connected account.
pub async fn get(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let account = state
        .db
        .read(move |tx| {
            Ok(tx
                .query_row(
                    "SELECT user_id, username, connected_at FROM discord_accounts WHERE ember_id = ?1",
                    [player.ember_id.as_str()],
                    |row| {
                        Ok(Account {
                            user_id: row.get(0)?,
                            username: row.get(1)?,
                            connected_at: row.get(2)?,
                        })
                    },
                )
                .optional()?)
        })
        .await?;
    Ok(ok(&Connection { account }))
}

/// `DELETE /v1/discord`: disconnects the session player's account.
pub async fn remove(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    proven(
        &state,
        player,
        &body,
        Action::DiscordRemove,
        Method::Delete,
        ACCOUNT_PATH,
        |tx, ctx, ember_id| Site::Discord.disconnect(tx, ctx, ember_id),
    )
    .await?;
    Ok(ok(&Connection { account: None }))
}

/// The players whose Ember IDs are connected to these Discord user IDs and
/// linked on `connection_id`, so its platform can create matches for them, in
/// order and without repeats. Anything that is not a Discord user ID (BluMint
/// also sends emails) is skipped, and so is an account `Site::link` cannot link.
pub fn find_and_link(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    connection_id: &str,
    user_ids: &[String],
) -> Result<Vec<FoundPlayer>> {
    let mut found: Vec<FoundPlayer> = Vec::new();
    for user_id in user_ids.iter().filter(|id| is_user_id(id)) {
        let account: Option<(String, String)> = tx
            .query_row(
                "SELECT ember_id, username FROM discord_accounts WHERE user_id = ?1",
                [user_id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .optional()?;
        let Some((ember_id, username)) = account else {
            continue;
        };
        let ember_id = EmberId::parse(&ember_id).map_err(|_| ApiFailure::unavailable())?;
        if found.iter().any(|player| player.ember_id == ember_id) {
            continue;
        }
        let Some(participant_id) =
            Site::Discord.link(tx, ctx, connection_id, user_id, &username, &ember_id)?
        else {
            continue;
        };
        found.push(FoundPlayer {
            discord_user_id: user_id.clone(),
            ember_id,
            participant_id,
        });
    }
    Ok(found)
}

/// Drops sign-ins Discord never answered.
pub fn expire(tx: &Transaction<'_>, now: u64) -> Result<()> {
    tx.execute("DELETE FROM discord_sign_ins WHERE expires_at <= ?1", [now])?;
    Ok(())
}
