//! Event replay and the SSE stream (spec 17.3, 20.6). Both serve the stored
//! event bytes, filtered by what the caller may see.
use std::{collections::VecDeque, convert::Infallible, time::Duration};

use axum::{
    extract::{Query, State},
    http::{HeaderMap, StatusCode, header},
    response::{
        IntoResponse, Response,
        sse::{Event, KeepAlive, Sse},
    },
};
use futures_util::stream;
use serde::Deserialize;

use crate::{
    AppState,
    auth::{self, Actor},
    error::{ApiFailure, Result},
    events::{self, MAX_PAGE, Viewer},
    routes::matches::viewer_of,
};

pub const KEEPALIVE_SECS: u64 = 20;

#[derive(Deserialize)]
pub struct Cursor {
    #[serde(default)]
    after: Option<String>,
    #[serde(default)]
    limit: Option<usize>,
}

fn parse_cursor(text: Option<&str>) -> Result<i64> {
    match text {
        None | Some("") => Ok(0),
        Some(text) => ember_protocol::encoding::Counter::parse(text)
            .ok()
            .and_then(|counter| i64::try_from(counter.0).ok())
            .ok_or_else(|| ApiFailure::invalid("The cursor is invalid.")),
    }
}

/// `GET /v1/events?after=<cursor>&limit=<n>`.
pub async fn list(
    State(state): State<AppState>,
    headers: HeaderMap,
    Query(cursor): Query<Cursor>,
) -> Result<Response> {
    let viewer = viewer_of(&auth::any(&state, &headers).await?)?;
    let after = parse_cursor(cursor.after.as_deref())?;
    let limit = cursor.limit.unwrap_or(100).min(MAX_PAGE);
    let rows = state
        .db
        .read(move |tx| events::list(tx, &viewer, after, limit))
        .await?;
    Ok((
        StatusCode::OK,
        [(header::CONTENT_TYPE, "application/json")],
        events::page_body(&rows, after),
    )
        .into_response())
}

struct Stream {
    state: AppState,
    viewer: Viewer,
    cursor: i64,
    pending: VecDeque<(i64, Vec<u8>)>,
    signal: tokio::sync::watch::Receiver<u64>,
    /// Player streams end when the session does.
    expires_at: Option<u64>,
    /// Rechecked with every page, so revoking the credential ends the stream.
    standing: auth::Standing,
}

/// `GET /v1/events/stream`. Resumes from `Last-Event-ID` or `?after=`.
pub async fn stream(
    State(state): State<AppState>,
    headers: HeaderMap,
    Query(cursor): Query<Cursor>,
) -> Result<Response> {
    let actor = auth::any(&state, &headers).await?;
    let expires_at = match &actor {
        Actor::Player(player) => Some(player.expires_at),
        _ => None,
    };
    let viewer = viewer_of(&actor)?;
    let standing = actor.standing();
    let resume = headers
        .get("last-event-id")
        .and_then(|value| value.to_str().ok())
        .map(str::to_owned)
        .or(cursor.after);
    let after = parse_cursor(resume.as_deref())?;
    // Fail fast on a cursor that needs a resync instead of opening a stream.
    let probe = viewer.clone();
    state
        .db
        .read(move |tx| events::list(tx, &probe, after, 1))
        .await?;
    let signal = state.stream_signal.subscribe();
    let initial = Stream {
        state,
        viewer,
        cursor: after,
        pending: VecDeque::new(),
        signal,
        expires_at,
        standing,
    };
    let events = stream::unfold(initial, |mut stream| async move {
        loop {
            if let Some((seq, body)) = stream.pending.pop_front() {
                let event = Event::default()
                    .id(seq.to_string())
                    .event("ember")
                    .data(String::from_utf8_lossy(&body));
                return Some((Ok::<_, Infallible>(event), stream));
            }
            if stream
                .expires_at
                .is_some_and(|expiry| stream.state.now() >= expiry)
            {
                return None;
            }
            let viewer = stream.viewer.clone();
            let standing = stream.standing.clone();
            let after = stream.cursor;
            let now = stream.state.now();
            match stream
                .state
                .db
                .read(move |tx| {
                    if !auth::still_valid(tx, &standing, now)? {
                        return Ok(None);
                    }
                    events::list(tx, &viewer, after, MAX_PAGE).map(Some)
                })
                .await
            {
                Ok(None) => return None,
                Ok(Some(rows)) if !rows.is_empty() => {
                    stream.cursor = rows.last().map_or(after, |(seq, _)| *seq);
                    stream.pending.extend(rows);
                    continue;
                }
                Ok(Some(_)) => {}
                Err(_) => return None,
            }
            // Wait for a commit, or recheck now and then for expiry and revocation.
            let _ = tokio::time::timeout(Duration::from_secs(5), stream.signal.changed()).await;
        }
    });
    Ok(Sse::new(events)
        .keep_alive(KeepAlive::new().interval(Duration::from_secs(KEEPALIVE_SECS)))
        .into_response())
}
