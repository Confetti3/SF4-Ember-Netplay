//! The web desk's page (docs/design/TOURNAMENT_DESK.md, 4a), built into the
//! bridge: one HTML page, its style and script, the emblem and the Inter
//! fonts (`desk/`, OFL in `desk/OFL.txt`). The script talks only to this
//! bridge's `/web` and organizer routes, so the policy allows nothing else.
//! `/t/{id}` is an event's public page, the phone viewer players open from
//! the Discord post: the same files, with its title and start in the page
//! itself for Discord's link preview.
use axum::{
    extract::{Path, State},
    http::{HeaderValue, StatusCode, header},
    response::{IntoResponse, Response},
};
use rusqlite::OptionalExtension;

use crate::{AppState, error::Result, util::html};

/// Each file the desk serves: its name under `/desk/`, type and bytes.
const FILES: [(&str, &str, &[u8]); 7] = [
    (
        "desk.css",
        "text/css; charset=utf-8",
        include_bytes!("../../desk/desk.css"),
    ),
    (
        "desk.js",
        "text/javascript; charset=utf-8",
        include_bytes!("../../desk/desk.js"),
    ),
    (
        "sets.js",
        "text/javascript; charset=utf-8",
        include_bytes!("../../desk/sets.js"),
    ),
    (
        "public.js",
        "text/javascript; charset=utf-8",
        include_bytes!("../../desk/public.js"),
    ),
    (
        "ember-emblem.png",
        "image/png",
        include_bytes!("../../desk/ember-emblem.png"),
    ),
    (
        "inter-latin-400.woff2",
        "font/woff2",
        include_bytes!("../../desk/inter-latin-400.woff2"),
    ),
    (
        "inter-latin-600.woff2",
        "font/woff2",
        include_bytes!("../../desk/inter-latin-600.woff2"),
    ),
];

const POLICY: &str = "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self'; \
     font-src 'self'; connect-src 'self'; form-action 'self'; base-uri 'none'; \
     frame-ancestors 'none'";

/// `GET /desk`: the page.
pub async fn page() -> Response {
    serve(
        "text/html; charset=utf-8",
        include_bytes!("../../desk/index.html"),
    )
}

/// `GET /t/{id}`: the public page of an event its organizer made public,
/// with its title and start written in for link previews; the script reads
/// the rest from `GET /v1/public/events/{id}`.
pub async fn public(State(state): State<AppState>, Path(id): Path<String>) -> Result<Response> {
    let found: Option<(String, Option<u64>)> = state
        .db
        .read(move |tx| {
            Ok(tx
                .query_row(
                    "SELECT title, starts_at FROM organizer_events
                     WHERE id = ?1 AND public = 1 AND ended_at IS NULL",
                    [&id],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )
                .optional()?)
        })
        .await?;
    let Some((title, starts_at)) = found else {
        return Ok(StatusCode::NOT_FOUND.into_response());
    };
    // The preview has no reader's time zone: UTC, said so.
    let when = starts_at.map_or_else(
        || "Date to be announced".to_owned(),
        |at| {
            let at = ember_protocol::event::rfc3339(at);
            format!("{} {} UTC", &at[..10], &at[11..16])
        },
    );
    let page = include_str!("../../desk/public.html")
        .replace("{{title}}", &html(&title))
        .replace("{{when}}", &html(&when));
    let mut response = serve("text/html; charset=utf-8", b"");
    *response.body_mut() = page.into();
    Ok(response)
}

/// `GET /desk/{file}`: one of its files.
pub async fn file(Path(name): Path<String>) -> Response {
    match FILES.iter().find(|(file, _, _)| *file == name) {
        Some((_, kind, bytes)) => serve(kind, bytes),
        None => StatusCode::NOT_FOUND.into_response(),
    }
}

fn serve(kind: &'static str, bytes: &'static [u8]) -> Response {
    let mut response = ([(header::CONTENT_TYPE, kind)], bytes).into_response();
    let headers = response.headers_mut();
    headers.insert(
        header::CONTENT_SECURITY_POLICY,
        HeaderValue::from_static(POLICY),
    );
    headers.insert(
        header::X_CONTENT_TYPE_OPTIONS,
        HeaderValue::from_static("nosniff"),
    );
    headers.insert(header::X_FRAME_OPTIONS, HeaderValue::from_static("DENY"));
    headers.insert(
        header::REFERRER_POLICY,
        HeaderValue::from_static("no-referrer"),
    );
    // A new bridge build brings new files; nothing is kept past one.
    headers.insert(header::CACHE_CONTROL, HeaderValue::from_static("no-cache"));
    response
}
