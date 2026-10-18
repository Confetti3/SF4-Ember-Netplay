//! The HTTP client every call out of the connector uses.
use std::time::Duration;

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
