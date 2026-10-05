use std::{
    sync::{
        Arc,
        atomic::{AtomicI64, Ordering},
    },
    time::{SystemTime, UNIX_EPOCH},
};

use ember_protocol::encoding::{b64u, prefixed_id};

/// Wall-clock seconds with an adjustable offset, so tests can move time.
#[derive(Clone, Default)]
pub struct Clock(Arc<AtomicI64>);

impl Clock {
    pub fn now(&self) -> u64 {
        let wall = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |elapsed| elapsed.as_secs() as i64);
        (wall + self.0.load(Ordering::Relaxed)).max(0) as u64
    }

    pub fn advance(&self, seconds: i64) {
        self.0.fetch_add(seconds, Ordering::Relaxed);
    }
}

/// OS randomness. Failure here is unrecoverable for a security service.
pub fn random<const N: usize>() -> [u8; N] {
    let mut bytes = [0u8; N];
    getrandom::fill(&mut bytes).expect("operating system RNG");
    bytes
}

pub fn new_id(prefix: &str) -> String {
    prefixed_id(prefix, random())
}

/// A bearer token: `<prefix>_<43 base64url chars>` over 32 random bytes.
pub fn new_token(prefix: &str) -> String {
    format!("{prefix}_{}", b64u(&random::<32>()))
}

/// Escapes text for HTML element content and quoted attributes.
pub fn html(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for ch in text.chars() {
        match ch {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            '\'' => out.push_str("&#39;"),
            ch => out.push(ch),
        }
    }
    out
}

/// A client for the services the bridge calls: no redirects, short timeouts,
/// no proxy. Callers add what their destination needs.
pub fn outbound_client() -> reqwest::ClientBuilder {
    reqwest::Client::builder()
        .redirect(reqwest::redirect::Policy::none())
        .connect_timeout(std::time::Duration::from_secs(5))
        .timeout(std::time::Duration::from_secs(15))
        .user_agent(concat!("ember-bridge/", env!("CARGO_PKG_VERSION")))
        .no_proxy()
}
