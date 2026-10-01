pub mod discovery;
pub mod events;
pub mod links;
pub mod matches;
pub mod sessions;
pub mod webhooks;

use std::time::Duration;

use axum::{
    Router,
    extract::DefaultBodyLimit,
    routing::{delete, get, post},
};
use rusqlite::params;

use crate::{AppState, mock};

/// Form bodies (mock pages only); JSON routes cap their own bodies.
const FORM_LIMIT: usize = 16 * 1024;
/// Expired secret-bearing rows are removed after this (spec 24.3).
const RETENTION_SECS: u64 = 24 * 60 * 60;

pub fn router(state: AppState) -> Router {
    let mut app = Router::new()
        .route("/.well-known/ember-bridge.json", get(discovery::well_known))
        .route("/v1/capabilities", get(discovery::capabilities))
        .route("/v1/signing-keys", get(discovery::signing_keys))
        .route("/v1/auth/challenges", post(sessions::issue))
        .route("/v1/sessions", post(sessions::create))
        .route("/v1/sessions/current", delete(sessions::revoke))
        .route("/v1/identity", get(sessions::identity))
        .route("/v1/link-intents", post(links::create))
        .route("/v1/link-intents/{id}", get(links::get))
        .route("/v1/link-intents/{id}/approve", post(links::approve_route))
        .route("/v1/link-intents/{id}/reject", post(links::reject_route))
        .route("/v1/link-intents/{id}/cancel", post(links::cancel_route))
        .route("/v1/link-claims", post(links::claim_route))
        .route(
            "/v1/link-claims/{id}/cancel",
            post(links::cancel_claim_route),
        )
        .route("/v1/links", get(links::list))
        .route("/v1/links/{id}", delete(links::remove))
        .route("/v1/players/resolve", post(links::resolve))
        .route("/v1/matches", post(matches::create))
        .route("/v1/matches/{id}", get(matches::get))
        .route("/v1/matches/{id}/cancel", post(matches::cancel))
        .route("/v1/matches/{id}/adjudications", post(matches::adjudicate))
        .route("/v1/assignments", get(matches::assignments))
        .route("/v1/events", get(events::list))
        .route("/v1/events/stream", get(events::stream))
        .route(
            "/v1/webhook-subscriptions",
            post(webhooks::create).get(webhooks::list),
        )
        .route("/v1/webhook-subscriptions/{id}", delete(webhooks::remove))
        .route(
            "/v1/webhook-subscriptions/{id}/rotate-secret",
            post(webhooks::rotate),
        );
    if state.config.mock_browser {
        app = app
            .route(
                "/mock/{connection}/login",
                get(mock::login_form).post(mock::login),
            )
            .route("/mock/{connection}/link", get(mock::link_page))
            .route("/mock/{connection}/link/create", post(mock::create))
            .route(
                "/mock/{connection}/link/{intent}/approve",
                post(mock::approve),
            )
            .route(
                "/mock/{connection}/link/{intent}/reject",
                post(mock::reject),
            )
            .route("/mock/{connection}/links/{link}/remove", post(mock::remove))
            .route("/mock/{connection}/logout", post(mock::logout));
    }
    app.layer(DefaultBodyLimit::max(FORM_LIMIT))
        .with_state(state)
}

/// Expires link intents and removes stale challenges, sessions and replay
/// records. Durable domain records (links, matches, attempts, events) stay.
pub async fn maintenance(state: AppState) {
    loop {
        let now = state.now();
        let cutoff = now.saturating_sub(RETENTION_SECS);
        let _ = state
            .db
            .write(move |tx| {
                tx.execute(
                    "UPDATE link_intents SET state = 'expired' WHERE state IN ('created', 'claim_pending') AND expires_at <= ?1",
                    [now],
                )?;
                tx.execute(
                    "UPDATE link_claims SET state = 'expired', decided_at = ?1 WHERE state = 'pending'
                       AND intent_id IN (SELECT id FROM link_intents WHERE state = 'expired')",
                    [now],
                )?;
                tx.execute("DELETE FROM auth_challenges WHERE expires_at <= ?1", [cutoff])?;
                tx.execute("DELETE FROM sessions WHERE expires_at <= ?1", [cutoff])?;
                tx.execute("DELETE FROM browser_sessions WHERE expires_at <= ?1", [cutoff])?;
                tx.execute("DELETE FROM idempotency_records WHERE created_at <= ?1", params![cutoff])?;
                Ok(())
            })
            .await;
        tokio::time::sleep(Duration::from_secs(60)).await;
    }
}
