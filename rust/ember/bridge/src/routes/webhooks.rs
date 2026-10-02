//! Webhook subscriptions (spec 20.3 to 20.5). Destinations are set by an
//! authenticated provider or organizer, never by match data.
use axum::{
    extract::{Path, State},
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::{event::Kind, json, webhook::Secret};
use reqwest::Url;
use rusqlite::{OptionalExtension, params};
use serde::{Deserialize, Serialize};
use serde_json::json;

use crate::{
    AppState,
    auth::{self, Role, Service},
    delivery,
    error::{ApiFailure, Result},
    http::{Body, PROOF_BODY, idempotency_key, ok},
    routes::{
        links::audit,
        matches::{idempotent as idempotent_request, into_response},
    },
    util::{new_id, random},
};

const MAX_URL: usize = 2048;
const MAX_SUBSCRIPTIONS: i64 = 20;
const DEFAULT_OVERLAP_SECS: u64 = 24 * 60 * 60;
const MAX_OVERLAP_SECS: u64 = 7 * 24 * 60 * 60;

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct CreateCommand {
    url: String,
    event_types: Vec<String>,
}

/// Checks a destination at registration. Delivery checks it again against
/// the addresses DNS returns at connection time.
pub fn check_url(text: &str, allow_private: bool) -> Result<Url> {
    let invalid = || {
        ApiFailure::invalid(
            "The webhook URL must be an https URL with a host name and no credentials.",
        )
    };
    if text.len() > MAX_URL {
        return Err(invalid());
    }
    let url = Url::parse(text).map_err(|_| invalid())?;
    let scheme_ok = url.scheme() == "https" || (allow_private && url.scheme() == "http");
    let port_ok = allow_private || url.port().is_none_or(|port| port == 443);
    if !scheme_ok
        || !port_ok
        || url.host_str().is_none()
        || !url.username().is_empty()
        || url.password().is_some()
        || url.fragment().is_some()
    {
        return Err(invalid());
    }
    if let Some(ip) = delivery::literal_ip(&url)
        && !delivery::allowed_address(ip, allow_private)
    {
        return Err(ApiFailure::invalid(
            "That webhook destination is not allowed.",
        ));
    }
    Ok(url)
}

fn check_types(service: &Service, types: &[String]) -> Result<Vec<String>> {
    if types.is_empty() || types.len() > Kind::ALL.len() {
        return Err(ApiFailure::invalid(
            "Choose between one and all of the event types.",
        ));
    }
    let mut chosen = Vec::new();
    for name in types {
        let kind = Kind::from_full(name).ok_or_else(|| {
            ApiFailure::invalid("Unknown event type.").detail("type", name.clone())
        })?;
        // Identity events stay with the provider connection they belong to.
        if kind.is_identity() && service.role != Role::Provider {
            return Err(ApiFailure::forbidden());
        }
        if !chosen.contains(name) {
            chosen.push(name.clone());
        }
    }
    Ok(chosen)
}

/// `POST /v1/webhook-subscriptions`. The secret is returned once; a retried
/// request gets the same subscription with the secret withheld.
pub async fn create(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let key = idempotency_key(&headers)?;
    let command: CreateCommand = body.parse()?;
    let url = check_url(&command.url, state.config.allow_private_webhooks)?;
    let types = check_types(&service, &command.event_types)?;
    let digest = json::digest(&command)?;
    let keys = state.keys.clone();
    let now = state.now();
    let secret = Secret::from_bytes(random());
    let revealed = secret.reveal();
    let sealed = keys.seal(revealed.as_bytes());
    let ((status, mut body), fresh) = state
        .db
        .write(move |tx| {
            let mut fresh = false;
            let result = idempotent_request(tx, &service.id, "/v1/webhook-subscriptions", &key, &digest, now, |tx| {
                // Authentication ran in an earlier transaction; a revocation
                // committed since then must not be outlived by a new webhook.
                let live: bool = tx.query_row(
                    "SELECT EXISTS (SELECT 1 FROM service_credentials WHERE id = ?1 AND revoked_at IS NULL)",
                    [&service.id],
                    |row| row.get(0),
                )?;
                if !live {
                    return Err(ApiFailure::unauthenticated());
                }
                let count: i64 = tx.query_row(
                    "SELECT COUNT(*) FROM webhook_subscriptions WHERE owner_credential = ?1 AND enabled = 1",
                    [&service.id],
                    |row| row.get(0),
                )?;
                if count >= MAX_SUBSCRIPTIONS {
                    return Err(ApiFailure::invalid("This credential has too many webhook subscriptions."));
                }
                let id = new_id("whs");
                tx.execute(
                    "INSERT INTO webhook_subscriptions
                        (id, tenant_id, owner_credential, connection_id, url, event_types, secret_sealed, enabled, created_at)
                     VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, 1, ?8)",
                    params![
                        id,
                        service.tenant_id,
                        service.id,
                        service.connection_id,
                        url.as_str(),
                        serde_json::to_string(&types).unwrap_or_default(),
                        sealed,
                        now
                    ],
                )?;
                audit(tx, now, ("service", service.id.clone()), "webhook.create", &id, "ok", None)?;
                fresh = true;
                Ok((
                    StatusCode::CREATED,
                    json!({ "subscription_id": id, "url": url.as_str(), "event_types": types, "secret": null }),
                ))
            })?;
            Ok((result, fresh))
        })
        .await?;
    // Only the request that created the subscription sees its secret; the
    // stored replay never contains it.
    if fresh {
        body["secret"] = revealed.as_str().into();
    } else {
        body["secret_redacted"] = true.into();
    }
    Ok(into_response((status, body)))
}

fn owned(tx: &rusqlite::Transaction<'_>, service: &Service, id: &str) -> Result<()> {
    let owner: Option<String> = tx
        .query_row(
            "SELECT owner_credential FROM webhook_subscriptions WHERE id = ?1",
            [id],
            |row| row.get(0),
        )
        .optional()?;
    if owner.as_deref() != Some(service.id.as_str()) {
        return Err(ApiFailure::not_found());
    }
    Ok(())
}

/// `GET /v1/webhook-subscriptions`: this credential's subscriptions.
pub async fn list(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let rows = state
        .db
        .read(move |tx| {
            Ok(tx
                .prepare(
                    "SELECT id, url, event_types, enabled, created_at,
                        (SELECT COUNT(*) FROM delivery_outbox o WHERE o.subscription_id = s.id AND o.state = 'pending'),
                        (SELECT COUNT(*) FROM delivery_outbox o WHERE o.subscription_id = s.id AND o.state = 'dead')
                     FROM webhook_subscriptions s WHERE owner_credential = ?1 ORDER BY created_at",
                )?
                .query_map([&service.id], |row| {
                    Ok(json!({
                        "subscription_id": row.get::<_, String>(0)?,
                        "url": row.get::<_, String>(1)?,
                        "event_types": serde_json::from_str::<serde_json::Value>(&row.get::<_, String>(2)?).unwrap_or_default(),
                        "enabled": row.get::<_, bool>(3)?,
                        "created_at": row.get::<_, u64>(4)?,
                        "pending_deliveries": row.get::<_, u64>(5)?,
                        "dead_letters": row.get::<_, u64>(6)?,
                    }))
                })?
                .collect::<rusqlite::Result<Vec<_>>>()?)
        })
        .await?;
    Ok(ok(&json!({ "subscriptions": rows })))
}

/// `DELETE /v1/webhook-subscriptions/{id}`: stops future delivery and drops
/// pending attempts and both secrets.
pub async fn remove(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let now = state.now();
    state
        .db
        .write(move |tx| {
            owned(tx, &service, &id)?;
            tx.execute(
                "UPDATE webhook_subscriptions SET enabled = 0, disabled_at = ?1, secret_sealed = x'',
                    previous_secret_sealed = NULL, previous_expires_at = NULL WHERE id = ?2",
                params![now, id],
            )?;
            tx.execute(
                "UPDATE delivery_outbox SET state = 'cancelled' WHERE subscription_id = ?1 AND state = 'pending'",
                [&id],
            )?;
            audit(tx, now, ("service", service.id.clone()), "webhook.delete", &id, "ok", None)?;
            Ok(())
        })
        .await?;
    Ok(ok(&json!({ "state": "disabled" })))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RotateCommand {
    #[serde(default)]
    overlap_seconds: Option<u64>,
}

/// `POST /v1/webhook-subscriptions/{id}/rotate-secret`. Deliveries carry
/// both signatures until the overlap ends, then only the new one (WEB-03).
pub async fn rotate(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let command: RotateCommand = if body.0.is_empty() {
        RotateCommand {
            overlap_seconds: None,
        }
    } else {
        body.parse()?
    };
    let overlap = command.overlap_seconds.unwrap_or(DEFAULT_OVERLAP_SECS);
    if overlap > MAX_OVERLAP_SECS {
        return Err(ApiFailure::invalid(
            "The overlap can be at most seven days.",
        ));
    }
    let secret = Secret::from_bytes(random());
    let revealed = secret.reveal();
    let sealed = state.keys.seal(revealed.as_bytes());
    let now = state.now();
    let target = id.clone();
    state
        .db
        .write(move |tx| {
            owned(tx, &service, &target)?;
            let changed = tx.execute(
                "UPDATE webhook_subscriptions SET previous_secret_sealed = secret_sealed, previous_expires_at = ?1,
                    secret_sealed = ?2 WHERE id = ?3 AND enabled = 1",
                params![now + overlap, sealed, target],
            )?;
            if changed != 1 {
                return Err(ApiFailure::not_found());
            }
            audit(tx, now, ("service", service.id.clone()), "webhook.rotate", &target, "ok", None)?;
            Ok(())
        })
        .await?;
    Ok(crate::http::json(
        StatusCode::OK,
        &json!({ "subscription_id": id, "secret": revealed.as_str(), "previous_valid_until": now + overlap }),
    ))
}
