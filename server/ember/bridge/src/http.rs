//! Request and response helpers. Bodies arrive as bytes with a hard cap and
//! are parsed only by the strict JSON profile.
use axum::{
    body::Bytes,
    extract::{FromRequest, Request},
    http::{HeaderMap, HeaderValue, StatusCode, header},
    response::{IntoResponse, Response},
};
use ember_protocol::json;
use serde::{Serialize, de::DeserializeOwned};

use crate::{
    error::{ApiFailure, Result},
    util::html,
};

pub const PROOF_BODY: usize = ember_protocol::challenge::MAX_PROOF_BODY;
pub const GENERAL_BODY: usize = json::MAX_BODY;

/// The raw body, refused above `N` bytes before it is buffered further.
pub struct Body<const N: usize>(pub Bytes);

impl<S: Send + Sync, const N: usize> FromRequest<S> for Body<N> {
    type Rejection = ApiFailure;

    async fn from_request(request: Request, _: &S) -> Result<Self> {
        axum::body::to_bytes(request.into_body(), N)
            .await
            .map(Self)
            .map_err(|_| ApiFailure::invalid("The request body is too large or incomplete."))
    }
}

impl<const N: usize> Body<N> {
    pub fn parse<T: DeserializeOwned>(&self) -> Result<T> {
        Ok(json::parse_as(&self.0, N)?)
    }
}

pub fn json<T: Serialize>(status: StatusCode, value: &T) -> Response {
    match serde_json::to_vec(value) {
        Ok(body) => (status, [(header::CONTENT_TYPE, "application/json")], body).into_response(),
        Err(_) => ApiFailure::unavailable().into_response(),
    }
}

pub fn ok<T: Serialize>(value: &T) -> Response {
    json(StatusCode::OK, value)
}

pub fn bearer(headers: &HeaderMap) -> Option<&str> {
    headers
        .get(header::AUTHORIZATION)?
        .to_str()
        .ok()?
        .strip_prefix("Bearer ")
        .map(str::trim)
}

pub fn cookie(headers: &HeaderMap, name: &str) -> Option<String> {
    headers
        .get_all(header::COOKIE)
        .iter()
        .filter_map(|value| value.to_str().ok())
        .flat_map(|value| value.split(';'))
        .find_map(|pair| {
            let (key, value) = pair.trim().split_once('=')?;
            (key == name).then(|| value.to_owned())
        })
}

/// The `Idempotency-Key` header: 1 to 128 visible ASCII characters.
pub fn idempotency_key(headers: &HeaderMap) -> Result<String> {
    let key = headers
        .get("idempotency-key")
        .and_then(|value| value.to_str().ok())
        .ok_or_else(|| ApiFailure::invalid("An Idempotency-Key header is required."))?;
    if key.is_empty() || key.len() > 128 || !key.bytes().all(|b| b.is_ascii_graphic()) {
        return Err(ApiFailure::invalid(
            "The Idempotency-Key header is invalid.",
        ));
    }
    Ok(key.to_owned())
}

/// `If-Match` or a body `expected_revision`, as a positive integer.
pub fn expected_revision(headers: &HeaderMap, body: Option<u64>) -> Result<u64> {
    let header = headers
        .get(header::IF_MATCH)
        .and_then(|value| value.to_str().ok())
        .map(|value| value.trim_matches('"').parse::<u64>())
        .transpose()
        .map_err(|_| ApiFailure::invalid("If-Match must be a match revision."))?;
    match (header, body) {
        (Some(a), Some(b)) if a != b => Err(ApiFailure::invalid(
            "If-Match and expected_revision disagree.",
        )),
        (Some(value), _) | (None, Some(value)) => Ok(value),
        (None, None) => Err(ApiFailure::invalid("An expected revision is required.")),
    }
}

// Ember's interface palette: charcoal, ivory and ember orange, as in the game
// and on embernetplay.link. Inline only; the page's CSP allows nothing else.
const STYLE: &str = ":root{color-scheme:dark}\
body{font:16px/1.5 system-ui,-apple-system,\"Segoe UI\",sans-serif;max-width:34rem;margin:0 auto;padding:2rem 1rem;color:#f3ebdd;background:#141312}\
h1{font-size:1.4rem;line-height:1.3;color:#ff8738;margin:0 0 1rem;padding-bottom:.6rem;\
background:linear-gradient(#ff8738,#ff8738) 0 100%/72px 3px no-repeat,linear-gradient(#443a31,#443a31) 0 100%/100% 1px no-repeat}\
p{margin:.75rem 0}strong{color:#fff}a{color:#ff8738;text-underline-offset:2px}a:hover{color:#ffb16f}\
code,.code{font:600 2rem ui-monospace,\"Cascadia Mono\",Consolas,monospace;letter-spacing:.1em;color:#f3ebdd;overflow-wrap:anywhere}\
.box{display:block;background:#211e1b;border:1px solid #443a31;border-left:3px solid #ff8738;border-radius:4px;padding:.75rem 1rem;margin:1rem 0}\
.box>:first-child{margin-top:0}.box>:last-child{margin-bottom:0}.warn{background:#2a2119;border-color:#76502f;border-left-color:#f1c477}\
button{font:inherit;font-weight:600;padding:.5rem 1rem;margin:0 .5rem .5rem 0;color:#f3ebdd;background:#382d24;border:1px solid #76502f;border-radius:4px;cursor:pointer}\
button:hover{background:#59402b;border-color:#ff8738}\
label{display:block;margin:.75rem 0;color:#b5a99b}\
input{font:inherit;padding:.4rem .5rem;margin-top:.25rem;width:100%;box-sizing:border-box;color:#f3ebdd;background:#191715;border:1px solid #443a31;border-radius:4px}\
a:focus-visible,button:focus-visible,input:focus-visible{outline:2px solid #ff8738;outline-offset:2px}";

/// A small self-contained HTML page for a person in a browser: the mock
/// provider's pages and the Discord sign-in result. No scripts, no outside
/// loads, never cached or framed. `refresh` reloads it every 3 seconds.
pub fn page(title: &str, body: &str, refresh: bool) -> Response {
    let refresh = if refresh {
        r#"<meta http-equiv="refresh" content="3">"#
    } else {
        ""
    };
    let document = format!(
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">{refresh}<title>{}</title><style>{STYLE}</style></head><body>{body}</body></html>",
        html(title)
    );
    let mut response = (
        StatusCode::OK,
        [(header::CONTENT_TYPE, "text/html; charset=utf-8")],
        document,
    )
        .into_response();
    let headers = response.headers_mut();
    headers.insert(
        header::CONTENT_SECURITY_POLICY,
        HeaderValue::from_static("default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'"),
    );
    headers.insert(header::X_FRAME_OPTIONS, HeaderValue::from_static("DENY"));
    headers.insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
    headers.insert(
        header::REFERRER_POLICY,
        HeaderValue::from_static("no-referrer"),
    );
    response
}
