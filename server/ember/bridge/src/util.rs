use std::{
    sync::{Arc, Mutex},
    time::{SystemTime, UNIX_EPOCH},
};

use ember_protocol::encoding::{b64u, prefixed_id};

/// Unix seconds that tests can move. It follows the wall clock plus what
/// `advance` added until `freeze` stops it where it is; after that only
/// `advance` moves it, so a test decides exactly how much time passes however
/// busy the machine is.
#[derive(Clone, Default)]
pub struct Clock(Arc<Mutex<ClockState>>);

#[derive(Default)]
struct ClockState {
    offset: i64,
    frozen: bool,
}

impl Clock {
    pub fn now(&self) -> u64 {
        let state = self.state();
        let base = if state.frozen { 0 } else { wall_seconds() };
        (base + state.offset).max(0) as u64
    }

    pub fn advance(&self, seconds: i64) {
        self.state().offset += seconds;
    }

    pub fn freeze(&self) {
        let mut state = self.state();
        if !state.frozen {
            state.offset += wall_seconds();
            state.frozen = true;
        }
    }

    fn state(&self) -> std::sync::MutexGuard<'_, ClockState> {
        self.0
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }
}

fn wall_seconds() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |elapsed| elapsed.as_secs() as i64)
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
