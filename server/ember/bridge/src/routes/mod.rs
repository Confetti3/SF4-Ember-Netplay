pub mod blumint;
pub mod discord;
pub mod discovery;
pub mod events;
pub mod expiry;
pub mod idempotency;
pub mod ledger;
pub mod links;
pub mod lobbies;
pub mod lookup;
pub mod matches;
pub mod play;
pub mod policy;
pub mod records;
pub mod reports;
pub mod rooms;
pub mod sessions;
pub mod tournaments;
pub mod webhooks;

use std::time::{Duration, Instant};

use axum::{
    Router,
    extract::{DefaultBodyLimit, Request},
    middleware::{self, Next},
    response::Response,
    routing::{delete, get, post},
};
use rusqlite::params;

use crate::{AppState, ctx::Ctx, delivery, linking, mock};

/// Form bodies (mock pages only); JSON routes cap their own bodies.
const FORM_LIMIT: usize = 16 * 1024;
/// How often maintenance runs; a lone game report waits at most this much
/// longer than its partner window before review.
const MAINTENANCE_SECS: u64 = 10;
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
        .route(ember_protocol::partner::LOOKUP_PATH, post(lookup::lookup))
        .route("/v1/players/{ember_id}/record", get(records::record))
        .route("/v1/matches", post(matches::create))
        .route("/v1/matches/{id}", get(matches::get))
        .route("/v1/matches/{id}/cancel", post(matches::cancel))
        .route("/v1/matches/{id}/adjudications", post(matches::adjudicate))
        .route("/v1/matches/{id}/claims", post(play::claim_route))
        .route("/v1/matches/{id}/room", post(play::publish_route))
        .route(
            "/v1/matches/{id}/attempts/prepare",
            post(play::prepare_route),
        )
        .route("/v1/matches/{id}/reports", post(reports::submit_route))
        .route("/v1/assignments", get(matches::assignments))
        .route(blumint::LOOKUP_PATH, post(blumint::lookup))
        .route(blumint::MATCHES_PATH, post(blumint::create))
        .route(blumint::CLOSE_PATH, post(blumint::close))
        .route(
            discord::START_PATH,
            post(discord::start).delete(discord::cancel),
        )
        .route(
            discord::CALLBACK_PATH,
            get(discord::callback).post(discord::decide),
        )
        .route(
            discord::ACCOUNT_PATH,
            get(discord::get).delete(discord::remove),
        )
        .route("/v1/rooms", get(rooms::list).post(rooms::create))
        .route("/v1/rooms/public", get(rooms::public_list))
        .route("/v1/rooms/{room_id}", get(rooms::get))
        .route("/v1/rooms/{room_id}/close", post(rooms::close))
        .route("/v1/rooms/{room_id}/tickets", post(rooms::ticket))
        .route("/v1/lobbies", post(lobbies::create))
        .route("/v1/lobbies/{id}", get(lobbies::get))
        .route("/v1/lobbies/{id}/queue", post(lobbies::join))
        .route(
            "/v1/lobbies/{id}/queue/{participant}/leave",
            post(lobbies::leave),
        )
        .route("/v1/lobbies/{id}/close", post(lobbies::close))
        .route("/v1/tournaments", post(tournaments::create))
        .route("/v1/tournaments/{id}", get(tournaments::get))
        .route("/v1/tournaments/{id}/entrants", post(tournaments::register))
        .route(
            "/v1/tournaments/{id}/entrants/{participant}/withdraw",
            post(tournaments::withdraw_route),
        )
        .route("/v1/tournaments/{id}/start", post(tournaments::start))
        .route("/v1/tournaments/{id}/cancel", post(tournaments::cancel))
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
        .layer(middleware::from_fn(access_log))
        .with_state(state)
}

/// One stderr line per request: method, path, status and milliseconds. The
/// query string, headers and bodies are left out; they can carry cursors,
/// credentials and proofs.
async fn access_log(request: Request, next: Next) -> Response {
    let method = request.method().clone();
    let path = request.uri().path().to_owned();
    let started = Instant::now();
    let response = next.run(request).await;
    eprintln!(
        "{method} {path} {} {}ms",
        response.status().as_u16(),
        started.elapsed().as_millis()
    );
    response
}

/// Ends the open BluMint matches an older bridge made, which BluMint cannot
/// close (`blumint::end_unkeyed`), records that the bridge starts again
/// (`delivery::status::resume`), says so on `resumed`, then runs
/// `maintenance_once` forever.
pub async fn maintenance(state: AppState, resumed: tokio::sync::oneshot::Sender<()>) {
    let ctx = Ctx::of(&state);
    match state
        .db
        .write(move |tx| blumint::end_unkeyed(tx, &ctx))
        .await
    {
        Ok(()) => state.committed(),
        Err(error) => {
            eprintln!("maintenance: could not end the unkeyed BluMint matches: {error:?}")
        }
    }
    let now = state.now();
    match state
        .db
        .write(move |tx| delivery::status::resume(tx, now))
        .await
    {
        Ok(()) => state.committed(),
        Err(error) => eprintln!("maintenance: could not record the restart: {error:?}"),
    }
    let _ = resumed.send(());
    loop {
        maintenance_once(&state).await;
        tokio::time::sleep(Duration::from_secs(MAINTENANCE_SECS)).await;
    }
}

/// Expires link intents, holds games whose reports did not arrive, expires
/// matches nobody played, observes the fighters whose claims lapsed, and
/// removes stale challenges, sessions, replay records and refused BluMint
/// keys. Durable domain records (links, matches, attempts, reports, events)
/// stay, and with each BluMint match the key it was made under, its
/// `external_match_id`.
pub async fn maintenance_once(state: &AppState) {
    // One clock read for the whole pass, so its steps agree on the time.
    let ctx = Ctx::of(state);
    let now = ctx.now;
    let cutoff = now.saturating_sub(RETENTION_SECS);
    let done = state
        .db
        .write(move |tx| {
            reports::expire(tx, &ctx)?;
            expiry::expire_stale(tx, &ctx)?;
            delivery::status::observe_lapsed(tx, now)?;
            linking::expire(tx, now)?;
            discord::expire(tx, now)?;
            tx.execute(
                "DELETE FROM auth_challenges WHERE expires_at <= ?1",
                [cutoff],
            )?;
            tx.execute("DELETE FROM sessions WHERE expires_at <= ?1", [cutoff])?;
            tx.execute(
                "DELETE FROM browser_sessions WHERE expires_at <= ?1",
                [cutoff],
            )?;
            tx.execute(
                "DELETE FROM idempotency_records WHERE created_at <= ?1",
                params![cutoff],
            )?;
            Ok(())
        })
        .await;
    if done.is_ok() {
        state.committed();
    }
}
