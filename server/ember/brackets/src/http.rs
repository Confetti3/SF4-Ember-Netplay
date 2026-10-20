//! The HTTP client every call out of the connector uses, and what the
//! site adapters share in reading and sending.
use std::time::Duration;

use reqwest::{RequestBuilder, StatusCode, header::HeaderValue};
use serde::{Deserialize, Deserializer};

use crate::site::SiteError;

/// Sends a site's request and reads its answer, at most `limit` bytes, for
/// the adapter to read by its status. `who` names the site in an error.
pub(crate) async fn exchange(
    request: RequestBuilder,
    limit: usize,
    who: &str,
) -> Result<(StatusCode, Vec<u8>), SiteError> {
    let response = request
        .send()
        .await
        .map_err(|_| SiteError::Unavailable(format!("{who} could not be reached")))?;
    let status = response.status();
    let body = bounded_body(response, limit)
        .await
        .ok_or_else(|| SiteError::Unavailable(format!("{who}'s answer was cut off or too long")))?;
    Ok((status, body))
}

/// A credential as a header value, kept out of debug output. `what` names
/// it in the error.
pub(crate) fn secret_header(value: &str, what: &str) -> Result<HeaderValue, String> {
    let mut header =
        HeaderValue::from_str(value).map_err(|_| format!("{what} is not a valid header value"))?;
    header.set_sensitive(true);
    Ok(header)
}

/// `Authorization: Bearer <token>`.
pub(crate) fn bearer(token: &str, what: &str) -> Result<HeaderValue, String> {
    secret_header(&format!("Bearer {token}"), what)
}

/// An ID a site gives as a string or as a whole number (never negative).
#[derive(Deserialize)]
#[serde(untagged)]
enum WireId {
    Number(u64),
    Text(String),
}

impl WireId {
    fn into_text(self) -> String {
        match self {
            Self::Number(number) => number.to_string(),
            Self::Text(text) => text,
        }
    }
}

/// An ID, string or integer, as text.
pub(crate) fn id<'de, D: Deserializer<'de>>(deserializer: D) -> Result<String, D::Error> {
    WireId::deserialize(deserializer).map(WireId::into_text)
}

/// An ID that may be `null` or absent, as text.
pub(crate) fn optional_id<'de, D: Deserializer<'de>>(
    deserializer: D,
) -> Result<Option<String>, D::Error> {
    Ok(Option::<WireId>::deserialize(deserializer)?.map(WireId::into_text))
}

/// No redirects, short timeouts.
pub(crate) fn http_client() -> Result<reqwest::Client, String> {
    reqwest::Client::builder()
        .redirect(reqwest::redirect::Policy::none())
        .connect_timeout(Duration::from_secs(5))
        .timeout(Duration::from_secs(20))
        .user_agent(concat!("ember-brackets/", env!("CARGO_PKG_VERSION")))
        .build()
        .map_err(|error| error.to_string())
}

/// The body, or `None` when it fails or is longer than `limit`.
pub(crate) async fn bounded_body(mut response: reqwest::Response, limit: usize) -> Option<Vec<u8>> {
    let mut body = Vec::new();
    while let Some(chunk) = response.chunk().await.ok()? {
        if body.len() + chunk.len() > limit {
            return None;
        }
        body.extend_from_slice(&chunk);
    }
    Some(body)
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::*;

    #[derive(Deserialize)]
    struct Ids {
        #[serde(deserialize_with = "id")]
        id: String,
        #[serde(default, deserialize_with = "optional_id")]
        other: Option<String>,
    }

    #[test]
    fn ids_read_as_text_whatever_their_type() {
        let ids = |value| serde_json::from_value::<Ids>(value).map(|ids| (ids.id, ids.other));
        assert_eq!(
            ids(json!({ "id": 65089253, "other": "7" })).unwrap(),
            ("65089253".into(), Some("7".into()))
        );
        assert_eq!(
            ids(json!({ "id": "7", "other": null })).unwrap(),
            ("7".into(), None)
        );
        assert!(ids(json!({ "id": -2 })).is_err());
        assert_eq!(ids(json!({ "id": "s-1" })).unwrap(), ("s-1".into(), None));
        assert!(ids(json!({ "id": 1.5 })).is_err());
        assert!(bearer("bad\ntoken", "the token").is_err());
        assert!(bearer("good", "the token").unwrap().is_sensitive());
    }
}
