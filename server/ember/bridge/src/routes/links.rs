//! HTTP side of account linking (spec 11): request parsing, who is acting,
//! and the response bodies. The state changes themselves are in `linking`.
use axum::{
    extract::{Path, State},
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::{
    challenge::{Action, Method},
    encoding::is_prefixed_id,
};
use rusqlite::{OptionalExtension, params};
use serde::Deserialize;
use serde_json::json;

use crate::{
    AppState,
    auth::{self, Actor, Browser, Role},
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, PROOF_BODY, json, ok},
    linking::{
        self, ApproveCommand, ClaimCommand, Owner, Unlinker, account, approve, cancel_intent,
        check_subject, claim, create_intent, reject, unlink, view_intent,
    },
    routes::sessions,
};

const CLAIMS_PER_WINDOW: usize = 10;
const CLAIM_WINDOW_SECS: u64 = 10 * 60;
const MAX_RESOLVE: usize = 100;

fn csrf_header(headers: &HeaderMap) -> Option<&str> {
    headers
        .get("x-csrf-token")
        .and_then(|value| value.to_str().ok())
}

/// The account-side actor for a JSON request: a provider credential or a
/// mock browser session with its CSRF token.
async fn account_owner(state: &AppState, headers: &HeaderMap) -> Result<OwnerActor> {
    match auth::any(state, headers).await? {
        Actor::Service(service) => {
            service.require(Role::Provider)?;
            Ok(OwnerActor::Provider(
                service.provider_connection()?.to_owned(),
            ))
        }
        Actor::Browser(browser) => {
            if !browser.csrf_ok(csrf_header(headers)) {
                return Err(ApiFailure::forbidden());
            }
            Ok(OwnerActor::Browser(browser))
        }
        Actor::Player(_) => Err(ApiFailure::forbidden()),
    }
}

pub enum OwnerActor {
    Browser(Browser),
    Provider(String),
}

impl OwnerActor {
    pub fn owner(&self) -> Owner<'_> {
        match self {
            Self::Browser(browser) => Owner::Browser(browser),
            Self::Provider(connection_id) => Owner::Provider { connection_id },
        }
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct IntentRequest {
    #[serde(default)]
    subject: Option<String>,
    #[serde(default)]
    display_label: Option<String>,
}

/// `POST /v1/link-intents`.
pub async fn create(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let request: IntentRequest = if body.0.is_empty() {
        IntentRequest {
            subject: None,
            display_label: None,
        }
    } else {
        body.parse()?
    };
    let actor = account_owner(&state, &headers).await?;
    let ctx = Ctx::of(&state);
    let created = state
        .db
        .write(move |tx| match &actor {
            OwnerActor::Provider(connection_id) => {
                let subject = request.subject.as_deref().ok_or_else(|| {
                    ApiFailure::invalid("A provider must name the verified subject.")
                })?;
                let (account_id, _) = account(
                    tx,
                    connection_id,
                    subject,
                    request.display_label.as_deref().unwrap_or(""),
                    ctx.now,
                )?;
                create_intent(tx, &ctx, connection_id, &account_id, None)
            }
            OwnerActor::Browser(browser) => {
                if request.subject.is_some() {
                    return Err(ApiFailure::invalid(
                        "A browser session links its own signed-in account.",
                    ));
                }
                create_intent(
                    tx,
                    &ctx,
                    &browser.connection_id,
                    &browser.account_id,
                    Some(&browser.token_hash),
                )
            }
        })
        .await?;
    state.committed();
    Ok(json(StatusCode::CREATED, &created))
}

/// `GET /v1/link-intents/{id}`.
pub async fn get(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> Result<Response> {
    let actor = match auth::any(&state, &headers).await? {
        Actor::Service(service) => OwnerActor::Provider(service.provider_connection()?.to_owned()),
        Actor::Browser(browser) => OwnerActor::Browser(browser),
        Actor::Player(_) => return Err(ApiFailure::forbidden()),
    };
    let ctx = Ctx::of(&state);
    let view = state
        .db
        .read(move |tx| view_intent(tx, &ctx, &actor.owner(), &id))
        .await?;
    Ok(ok(&view))
}

/// `POST /v1/link-intents/{id}/approve`.
pub async fn approve_route(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let command: ApproveCommand = body.parse()?;
    let actor = account_owner(&state, &headers).await?;
    let ctx = Ctx::of(&state);
    let link = state
        .db
        .write(move |tx| approve(tx, &ctx, &actor.owner(), &id, &command))
        .await?;
    state.committed();
    Ok(ok(&link))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RejectCommand {
    claim_id: String,
}

/// `POST /v1/link-intents/{id}/reject`.
pub async fn reject_route(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let command: RejectCommand = body.parse()?;
    let actor = account_owner(&state, &headers).await?;
    let ctx = Ctx::of(&state);
    let view = state
        .db
        .write(move |tx| reject(tx, &ctx, &actor.owner(), &id, &command.claim_id))
        .await?;
    Ok(ok(&view))
}

/// `POST /v1/link-intents/{id}/cancel`.
pub async fn cancel_route(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> Result<Response> {
    let actor = account_owner(&state, &headers).await?;
    let ctx = Ctx::of(&state);
    let view = state
        .db
        .write(move |tx| cancel_intent(tx, &ctx, &actor.owner(), &id))
        .await?;
    Ok(ok(&view))
}

/// `POST /v1/link-claims`: player session plus a `link.claim` proof.
pub async fn claim_route(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let now = state.now();
    state
        .rate
        .check(
            &format!("link-claim:{}", player.ember_id),
            CLAIMS_PER_WINDOW,
            CLAIM_WINDOW_SECS,
            now,
        )
        .map_err(ApiFailure::rate_limited)?;
    let (request, command): (_, ClaimCommand) = sessions::proven(&body.0)?;
    let ctx = Ctx::of(&state);
    let outcome = state
        .db
        .write(move |tx| {
            let (ember_id, _) = sessions::consume_proof(
                tx,
                &ctx.config,
                ctx.now,
                &request,
                sessions::Target {
                    action: Action::LinkClaim,
                    method: Method::Post,
                    path: "/v1/link-claims",
                },
                Some(&player.ember_id),
            )?;
            claim(tx, &ctx, &ember_id, &request.proof.challenge_id, &command)
        })
        .await?;
    state.committed();
    Ok(json(StatusCode::CREATED, &outcome?))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct CancelClaimCommand {
    claim_id: String,
}

/// `POST /v1/link-claims/{id}/cancel`: player session plus a `link.cancel` proof.
pub async fn cancel_claim_route(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let (request, command): (_, CancelClaimCommand) = sessions::proven(&body.0)?;
    if command.claim_id != id || !is_prefixed_id(&id, "lkc") {
        return Err(ApiFailure::invalid("The command does not match the path."));
    }
    let ctx = Ctx::of(&state);
    let path = format!("/v1/link-claims/{id}/cancel");
    state
        .db
        .write(move |tx| {
            let (ember_id, _) = sessions::consume_proof(
                tx,
                &ctx.config,
                ctx.now,
                &request,
                sessions::Target {
                    action: Action::LinkCancel,
                    method: Method::Post,
                    path: &path,
                },
                Some(&player.ember_id),
            )?;
            linking::cancel_claim(tx, &ctx, &ember_id, &id)
        })
        .await?;
    Ok(ok(
        &json!({ "claim_id": command.claim_id, "state": "cancelled" }),
    ))
}

/// `GET /v1/links`: the caller's own links and pending claims.
pub async fn list(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let config = state.config.clone();
    let now = state.now();
    let body = state
        .db
        .read(move |tx| {
            let links = tx
                .prepare(
                    "SELECT l.id, l.connection_id, a.display_label, l.approved_at FROM links l
                     JOIN external_accounts a ON a.id = l.account_id
                     WHERE l.ember_id = ?1 AND l.revoked_at IS NULL ORDER BY l.approved_at",
                )?
                .query_map([player.ember_id.as_str()], |row| {
                    let connection: String = row.get(1)?;
                    let label: String = row.get(2)?;
                    Ok(json!({
                        "link_id": row.get::<_, String>(0)?,
                        "connection_id": connection,
                        "provider": config.connection(&connection).map(|(_, c)| c.display_name.clone()),
                        "account_label": mask(&label),
                        "state": "linked",
                        "approved_at": row.get::<_, u64>(3)?,
                    }))
                })?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            let pending = tx
                .prepare(
                    "SELECT c.id, i.connection_id, i.expires_at FROM link_claims c JOIN link_intents i ON i.id = c.intent_id
                     WHERE c.ember_id = ?1 AND c.state = 'pending' AND i.expires_at > ?2
                       AND i.state IN ('created', 'claim_pending')",
                )?
                .query_map(params![player.ember_id.as_str(), now], |row| {
                    let connection: String = row.get(1)?;
                    Ok(json!({
                        "claim_id": row.get::<_, String>(0)?,
                        "connection_id": connection,
                        "provider": config.connection(&connection).map(|(_, c)| c.display_name.clone()),
                        "state": "waiting_for_browser_approval",
                        "expires_at": row.get::<_, u64>(2)?,
                    }))
                })?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            Ok(json!({ "links": links, "pending": pending }))
        })
        .await?;
    Ok(ok(&body))
}

/// Shows the first two characters of an account label and masks the rest.
fn mask(label: &str) -> String {
    let visible: String = label.chars().take(2).collect();
    if label.chars().count() <= 2 {
        visible
    } else {
        format!("{visible}***")
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RemoveCommand {
    link_id: String,
}

/// `DELETE /v1/links/{id}`: the linked player with a `link.remove` proof, the
/// account's browser session, or its provider.
pub async fn remove(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let ctx = Ctx::of(&state);
    let result = match auth::any(&state, &headers).await? {
        Actor::Player(player) => {
            let (request, command): (_, RemoveCommand) = sessions::proven(&body.0)?;
            if command.link_id != id || !is_prefixed_id(&id, "lnk") {
                return Err(ApiFailure::invalid("The command does not match the path."));
            }
            let path = format!("/v1/links/{id}");
            state
                .db
                .write(move |tx| {
                    let (ember_id, _) = sessions::consume_proof(
                        tx,
                        &ctx.config,
                        ctx.now,
                        &request,
                        sessions::Target {
                            action: Action::LinkRemove,
                            method: Method::Delete,
                            path: &path,
                        },
                        Some(&player.ember_id),
                    )?;
                    unlink(tx, &ctx, &Unlinker::Player(&ember_id), &id)
                })
                .await?
        }
        _ => {
            let actor = account_owner(&state, &headers).await?;
            state
                .db
                .write(move |tx| unlink(tx, &ctx, &Unlinker::Account(actor.owner()), &id))
                .await?
        }
    };
    state.committed();
    Ok(ok(&result))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ResolveRequest {
    subjects: Vec<String>,
}

/// `POST /v1/players/resolve`: this provider connection's own subjects only.
/// Unknown subjects are reported unlinked and nothing is created (LINK-12).
pub async fn resolve(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<{ crate::http::GENERAL_BODY }>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let connection = service.provider_connection()?.to_owned();
    let request: ResolveRequest = body.parse()?;
    if request.subjects.is_empty() || request.subjects.len() > MAX_RESOLVE {
        return Err(ApiFailure::invalid("Resolve between 1 and 100 subjects."));
    }
    for subject in &request.subjects {
        check_subject(subject)?;
    }
    let players = state
        .db
        .read(move |tx| {
            let mut statement = tx.prepare(
                "SELECT a.participant_id, l.ember_id FROM external_accounts a
                 LEFT JOIN links l ON l.account_id = a.id AND l.revoked_at IS NULL
                 WHERE a.connection_id = ?1 AND a.subject = ?2",
            )?;
            request
                .subjects
                .iter()
                .map(|subject| {
                    let row: Option<(String, Option<String>)> = statement
                        .query_row(params![connection, subject], |row| Ok((row.get(0)?, row.get(1)?)))
                        .optional()?;
                    Ok(match row {
                        Some((participant, Some(ember))) => {
                            json!({ "subject": subject, "linked": true, "participant_id": participant, "ember_id": ember })
                        }
                        _ => json!({ "subject": subject, "linked": false }),
                    })
                })
                .collect::<Result<Vec<_>>>()
        })
        .await?;
    Ok(ok(&json!({ "players": players })))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn labels_are_masked() {
        assert_eq!(mask("Kate"), "Ka***");
        assert_eq!(mask("K"), "K");
        assert_eq!(mask("日本語"), "日本***");
    }
}
