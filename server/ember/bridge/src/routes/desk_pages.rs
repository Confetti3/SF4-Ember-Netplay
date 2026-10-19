//! The web desk's page (docs/design/TOURNAMENT_DESK.md, 4a), built into the
//! bridge: one HTML page, its style and script, the emblem and the Inter
//! fonts (`desk/`, OFL in `desk/OFL.txt`). The script talks only to this
//! bridge's `/web` and organizer routes, so the policy allows nothing else.
use axum::{
    extract::Path,
    http::{HeaderValue, StatusCode, header},
    response::{IntoResponse, Response},
};

/// Each file the desk serves: its name under `/desk/`, type and bytes.
const FILES: [(&str, &str, &[u8]); 5] = [
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
