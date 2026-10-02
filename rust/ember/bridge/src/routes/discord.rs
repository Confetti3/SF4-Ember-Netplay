//! Discord sign-in (optional; on when the configuration has `discord`). A
//! player's Ember asks to start it with a `discord.connect` proof and opens
//! the returned Discord page in the browser; Discord sends the browser back
//! to the callback with a code, which the bridge exchanges for the player's
//! Discord user ID (`identify` scope only). The bridge keeps the user ID and
//! username, so platforms that find players by Discord account get the Ember
//! ID. Nothing else from Discord is stored, and the access token is dropped.
//!
//! A platform's link approved by a Discord sign-in (`approved_via` `discord`,
//! claim `discord:<user ID>`) stands only while that account is connected to
//! that Ember ID: moving or disconnecting the account ends the links its
//! sign-in approved, in the same transaction. The next lookup links the
//! account again where it now belongs.
use axum::{
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
    event::Kind,
};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::Deserialize;
use serde_json::json;

use crate::{
    AppState, auth,
    config::Discord,
    error::{ApiFailure, Result},
    events::{NewEvent, emit},
    http::{Body, PROOF_BODY, json, ok, page},
    routes::{
        links::{Ctx, account, revoke_link},
        sessions::{self, Target},
    },
    util::{html, new_id, outbound_client, random},
};

/// Sign-ins one Ember ID may start per hour.
const STARTS_PER_HOUR: usize = 10;

fn enabled(state: &AppState) -> Result<&Discord> {
    state
        .config
        .discord
        .as_ref()
        .ok_or_else(ApiFailure::not_found)
}

fn state_hash(state: &AppState, text: &str) -> [u8; 32] {
    state.keys.keyed_hash("discord-sign-in", text.as_bytes())
}

fn redirect_uri(state: &AppState) -> String {
    format!("{}{CALLBACK_PATH}", state.config.origin)
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Nothing {}

/// Runs a `discord.connect` or `discord.remove` proof from the session's player.
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
    let ctx = Ctx::of(state);
    state
        .db
        .write(move |tx| {
            let (ember_id, _) = sessions::consume_proof(
                tx,
                &ctx.config,
                ctx.now,
                &request,
                Target {
                    action,
                    method,
                    path,
                },
                Some(&player.ember_id),
            )?;
            work(tx, &ctx, ember_id.as_str())
        })
        .await?;
    state.committed();
    Ok(())
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

/// `GET /v1/discord/callback`: Discord's answer, in the player's browser.
/// The page says what happened; the player goes back to Ember.
pub async fn callback(State(state): State<AppState>, Query(answer): Query<Answer>) -> Response {
    let Ok(discord) = enabled(&state).cloned() else {
        return finished(
            "Discord sign-in is off",
            "This Ember service does not offer Discord sign-in.",
        );
    };
    // The sign-in this browser carries, once, whatever Discord answered.
    let hash = state_hash(&state, answer.state.as_deref().unwrap_or_default());
    let now = state.now();
    let ember_id = state
        .db
        .write(move |tx| {
            let ember_id: Option<String> = tx
                .query_row(
                    "DELETE FROM discord_sign_ins WHERE state_hash = ?1 AND expires_at > ?2 RETURNING ember_id",
                    params![hash.as_slice(), now],
                    |row| row.get(0),
                )
                .optional()?;
            Ok(ember_id)
        })
        .await;
    let Ok(Some(ember_id)) = ember_id else {
        return finished(
            "Sign-in expired",
            "This Discord sign-in is not one Ember is waiting for, or it took longer than ten minutes. Choose Connect Discord in Ember again.",
        );
    };
    let code = match (answer.error.as_deref(), answer.code) {
        (None, Some(code)) if !code.is_empty() && code.len() <= 256 => code,
        _ => {
            return finished(
                "Not connected",
                "Discord did not connect the account. Choose Connect Discord in Ember to try again.",
            );
        }
    };
    let Some(user) = discord_user(&state, &discord, &code).await else {
        return finished(
            "Discord did not answer",
            "Discord did not confirm the account. Choose Connect Discord in Ember to try again.",
        );
    };
    let shown = user.username.clone();
    let fingerprint = EmberId::parse(&ember_id)
        .map(|id| id.fingerprint())
        .unwrap_or_default();
    let ctx = Ctx::of(&state);
    let stored = state
        .db
        .write(move |tx| {
            // The latest sign-in wins: this account leaves any other Ember ID,
            // and this Ember ID leaves any other account.
            let replaced: Vec<(String, String)> = tx
                .prepare(
                    "DELETE FROM discord_accounts WHERE user_id = ?1 OR ember_id = ?2 RETURNING user_id, ember_id",
                )?
                .query_map(params![user.id, ember_id], |row| Ok((row.get(0)?, row.get(1)?)))?
                .collect::<rusqlite::Result<_>>()?;
            for (user_id, owner) in replaced {
                if (user_id.as_str(), owner.as_str()) != (user.id.as_str(), ember_id.as_str()) {
                    end_links(tx, &ctx, &user_id, "replaced")?;
                }
            }
            tx.execute(
                "INSERT INTO discord_accounts (user_id, ember_id, username, connected_at) VALUES (?1, ?2, ?3, ?4)",
                params![user.id, ember_id, user.username, ctx.now],
            )?;
            Ok(())
        })
        .await;
    if stored.is_err() {
        return finished(
            "Not connected",
            "The Ember service could not save the account. Try again later.",
        );
    }
    state.committed();
    finished(
        "Discord connected",
        &format!(
            "Discord account <strong>{}</strong> is now connected to Ember ID <strong>{}</strong>. \
             Tournament sites that use this Ember service can find your Ember ID from this Discord account. \
             You can close this tab and go back to Ember.",
            html(&shown),
            html(&fingerprint)
        ),
    )
}

fn finished(title: &str, body: &str) -> Response {
    page(
        title,
        &format!("<h1>{}</h1><p>{body}</p>", html(title)),
        false,
    )
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
    enabled(&state)?;
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
    enabled(&state)?;
    let player = auth::player(&state, &headers).await?;
    proven(
        &state,
        player,
        &body,
        Action::DiscordRemove,
        Method::Delete,
        ACCOUNT_PATH,
        |tx, ctx, ember_id| {
            let removed: Option<String> = tx
                .query_row(
                    "DELETE FROM discord_accounts WHERE ember_id = ?1 RETURNING user_id",
                    [ember_id],
                    |row| row.get(0),
                )
                .optional()?;
            match removed {
                Some(user_id) => end_links(tx, ctx, &user_id, "unlinked_by_player"),
                None => Ok(()),
            }
        },
    )
    .await?;
    Ok(ok(&Connection { account: None }))
}

/// The Ember IDs connected to these Discord user IDs, in order and without
/// repeats, each linked on `connection_id` so its platform can create
/// matches for it. Anything that is not a Discord user ID (BluMint also sends
/// emails) is skipped.
pub fn find_and_link(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    connection_id: &str,
    user_ids: &[String],
) -> Result<Vec<EmberId>> {
    let mut found = Vec::new();
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
        if !found.contains(&ember_id) {
            link(tx, ctx, connection_id, user_id, &username, &ember_id)?;
            found.push(ember_id);
        }
    }
    Ok(found)
}

/// Links the Discord account's subject on the connection to `ember_id`,
/// approved by the player's Discord sign-in, unless it is linked already or
/// the Ember ID has a link there from a code the player claimed, which it
/// keeps. Moving or disconnecting the account ended every other link its
/// sign-in approved (`end_links`), so it has none elsewhere; the links
/// table's unique indexes would refuse a second one.
fn link(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    connection_id: &str,
    user_id: &str,
    username: &str,
    ember_id: &EmberId,
) -> Result<()> {
    let subject = format!("discord:{user_id}");
    let linked: bool = tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM links WHERE ember_id = ?1 AND connection_id = ?2 AND revoked_at IS NULL
           AND (approved_via != 'discord' OR claim_id = ?3))",
        params![ember_id.as_str(), connection_id, subject],
        |row| row.get(0),
    )?;
    if linked {
        return Ok(());
    }
    let tenant = ctx.tenant_of(connection_id)?;
    let (account_id, participant_id) = account(tx, connection_id, &subject, username, ctx.now)?;
    let link_id = new_id("lnk");
    tx.execute(
        "INSERT INTO links (id, account_id, connection_id, ember_id, approved_via, claim_id, consented_at, approved_at)
         VALUES (?1, ?2, ?3, ?4, 'discord', ?5, ?6, ?6)",
        params![link_id, account_id, connection_id, ember_id.as_str(), subject, ctx.now],
    )?;
    emit(
        tx,
        &ctx.config,
        ctx.now,
        NewEvent {
            kind: Kind::LinkCompleted,
            tenant_id: &tenant,
            connection_id: Some(connection_id),
            subject: format!("links/{link_id}"),
            match_id: None,
            ember_id: Some(ember_id),
            lobby_id: None,
            tournament_id: None,
            data: json!({
                "link_id": link_id,
                "ember_id": ember_id,
                "connection_id": connection_id,
                "participant_id": participant_id,
                "approved_via": "discord",
            }),
        },
    )?;
    Ok(())
}

/// Ends every link this Discord account's sign-in approved, on every
/// connection, when the account moves to another Ember ID or is disconnected.
fn end_links(tx: &Transaction<'_>, ctx: &Ctx, user_id: &str, reason: &str) -> Result<()> {
    let links: Vec<(String, String, String, String, String)> = tx
        .prepare(
            "SELECT l.id, l.ember_id, l.connection_id, a.participant_id, c.tenant_id
             FROM links l
             JOIN external_accounts a ON a.id = l.account_id
             JOIN provider_connections c ON c.id = l.connection_id
             WHERE l.approved_via = 'discord' AND l.claim_id = ?1 AND l.revoked_at IS NULL",
        )?
        .query_map([format!("discord:{user_id}")], |row| {
            Ok((
                row.get(0)?,
                row.get(1)?,
                row.get(2)?,
                row.get(3)?,
                row.get(4)?,
            ))
        })?
        .collect::<rusqlite::Result<_>>()?;
    for (link_id, ember_id, connection_id, participant_id, tenant) in links {
        let ember_id = EmberId::parse(&ember_id).map_err(|_| ApiFailure::unavailable())?;
        revoke_link(
            tx,
            ctx,
            &tenant,
            &link_id,
            &ember_id,
            &connection_id,
            &participant_id,
            reason,
        )?;
    }
    Ok(())
}

/// Drops sign-ins Discord never answered.
pub fn expire(tx: &Transaction<'_>, now: u64) -> Result<()> {
    tx.execute("DELETE FROM discord_sign_ins WHERE expires_at <= ?1", [now])?;
    Ok(())
}
