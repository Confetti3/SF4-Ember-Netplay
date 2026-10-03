//! How a send went, and what a rate-limited answer asks for.
use std::time::Duration;

pub(crate) enum Sent {
    Done,
    Retry(String, u64),
    Failed(String),
}

/// How a service says when its rate limit resets, besides `Retry-After`.
#[derive(Clone, Copy)]
pub(crate) enum Reset {
    /// Seconds from now (Discord's `X-RateLimit-Reset-After`).
    After(&'static str),
    /// A Unix time in seconds (Twitch's `Ratelimit-Reset`).
    At(&'static str),
}

/// The wait in seconds a rate-limited answer asks for, at most an hour.
pub(crate) fn retry_delay(headers: &reqwest::header::HeaderMap, reset: Reset, now: u64) -> u64 {
    let number = |name: &str| {
        headers
            .get(name)
            .and_then(|value| value.to_str().ok())
            .and_then(|value| value.trim().parse::<f64>().ok())
            .filter(|value| value.is_finite())
    };
    let seconds = number("retry-after").or_else(|| match reset {
        Reset::After(name) => number(name),
        Reset::At(name) => number(name).map(|at| at - now as f64),
    });
    seconds.map_or(0, |seconds| seconds.ceil().clamp(0.0, 3600.0) as u64)
}

pub(crate) async fn classify(
    response: reqwest::Result<reqwest::Response>,
    reset: Reset,
    now: u64,
) -> Sent {
    let response = match response {
        Ok(response) => response,
        Err(_) => return Sent::Retry("unreachable".into(), 0),
    };
    let status = response.status();
    let after = retry_delay(response.headers(), reset, now);
    if status.is_success() {
        Sent::Done
    } else if status.as_u16() == 429 || status.is_server_error() {
        Sent::Retry(format!("status {}", status.as_u16()), after)
    } else {
        Sent::Failed(format!("status {}", status.as_u16()))
    }
}

/// The client every call out of the notifier uses: no redirects, short
/// timeouts.
pub(crate) fn http_client() -> Result<reqwest::Client, String> {
    reqwest::Client::builder()
        .redirect(reqwest::redirect::Policy::none())
        .connect_timeout(Duration::from_secs(5))
        .timeout(Duration::from_secs(15))
        .user_agent(concat!("ember-notifier/", env!("CARGO_PKG_VERSION")))
        .build()
        .map_err(|error| error.to_string())
}

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
    use super::*;

    #[test]
    fn rate_limit_resets_are_read_by_their_meaning() {
        use reqwest::header::{HeaderMap, HeaderValue};
        let now = 1_800_000_000;
        let mut twitch = HeaderMap::new();
        twitch.insert("ratelimit-reset", HeaderValue::from_static("1800000010"));
        assert_eq!(retry_delay(&twitch, Reset::At("ratelimit-reset"), now), 10);
        let mut discord = HeaderMap::new();
        discord.insert("x-ratelimit-reset-after", HeaderValue::from_static("1.2"));
        assert_eq!(
            retry_delay(&discord, Reset::After("x-ratelimit-reset-after"), now),
            2
        );
        // Retry-After wins, and a reset in the past means no wait.
        twitch.insert("retry-after", HeaderValue::from_static("3"));
        assert_eq!(retry_delay(&twitch, Reset::At("ratelimit-reset"), now), 3);
        let mut past = HeaderMap::new();
        past.insert("ratelimit-reset", HeaderValue::from_static("1700000000"));
        assert_eq!(retry_delay(&past, Reset::At("ratelimit-reset"), now), 0);
    }
}
