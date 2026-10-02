//! Short invitation store for SF4 Ember Netplay.
//!
//! A client turns a short code into an opaque locator, a sealing key and a
//! write token on its own machine, then stores a sealed record here under the
//! locator. This service never receives the code, the key or the invitation:
//! it keeps opaque bytes for a bounded time and hands them back to anyone who
//! asks for the same locator. See `docs/design/SHORT_INVITATIONS.md`.
//!
//! Wire format, version 1:
//!
//! - `PUT /s/v1/{locator}` with a body of `version (1) | ttl seconds (u32 LE)
//!   | write token (16) | sealed record (29..=1024)`. The first store of a
//!   locator binds the hash of its write token; later stores must present
//!   the same token. The service sets the expiry from its own clock.
//! - `GET /s/v1/{locator}` returns the sealed record, or 404.
//! - `GET /s/health` returns `ok`.
//!
//! The locator is 16 bytes in unpadded base64url (22 characters).
use std::{
    collections::{HashMap, VecDeque},
    net::{IpAddr, SocketAddr},
    sync::{Arc, Mutex, MutexGuard},
    time::{SystemTime, UNIX_EPOCH},
};

use axum::{
    Router,
    body::Bytes,
    extract::{ConnectInfo, DefaultBodyLimit, FromRequestParts, Path, State},
    http::{HeaderValue, StatusCode, header, request::Parts},
    response::{IntoResponse, Response},
    routing::get,
};
use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD};
use sha2::{Digest, Sha256};
use subtle::ConstantTimeEq;

pub const WIRE_VERSION: u8 = 1;
pub const LOCATOR_BYTES: usize = 16;
pub const WRITE_TOKEN_BYTES: usize = 16;
/// A 12-byte nonce, a 16-byte tag and at least one byte of ciphertext.
pub const MIN_SEALED_BYTES: usize = 29;
pub const MAX_SEALED_BYTES: usize = 1024;
pub const PUT_HEADER_BYTES: usize = 1 + 4 + WRITE_TOKEN_BYTES;
pub const MAX_BODY_BYTES: usize = PUT_HEADER_BYTES + MAX_SEALED_BYTES;
pub const MIN_TTL_SECS: u32 = 60;
/// An invitation lasts an hour and is renewed at half its life, so two
/// hours covers a renewal that arrives late plus clock skew.
pub const MAX_TTL_SECS: u32 = 2 * 60 * 60;
/// How often expired records are swept at the latest.
pub const SWEEP_INTERVAL_SECS: u64 = 30;

#[derive(Clone, Copy, Debug)]
pub struct Limits {
    /// Live records across every client.
    pub max_records: usize,
    /// Live records first stored from one network address (an IPv4 address
    /// or an IPv6 /64). Updating an existing record does not count.
    pub max_records_per_address: usize,
    /// Lookups per address per `read_window_secs`.
    pub reads_per_window: usize,
    pub read_window_secs: u64,
    /// Stores per address per `write_window_secs`.
    pub writes_per_window: usize,
    pub write_window_secs: u64,
}

impl Default for Limits {
    fn default() -> Self {
        Self {
            max_records: 20_000,
            max_records_per_address: 16,
            reads_per_window: 20,
            read_window_secs: 60,
            writes_per_window: 30,
            write_window_secs: 600,
        }
    }
}

/// Seconds on a clock the store trusts. Production uses Unix time; tests
/// move a counter.
pub type Clock = Arc<dyn Fn() -> u64 + Send + Sync>;

pub fn system_clock() -> Clock {
    Arc::new(|| {
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |elapsed| elapsed.as_secs())
    })
}

struct Record {
    sealed: Bytes,
    write_hash: [u8; 32],
    expires: u64,
    owner: u64,
}

#[derive(Clone, Copy, PartialEq, Eq, Hash)]
enum Use {
    Read,
    Write,
}

#[derive(Default)]
struct Inner {
    records: HashMap<[u8; LOCATOR_BYTES], Record>,
    owners: HashMap<u64, usize>,
    uses: HashMap<(u64, Use), VecDeque<u64>>,
    next_sweep: u64,
}

impl Inner {
    fn sweep(&mut self, now: u64, limits: &Limits) {
        let owners = &mut self.owners;
        self.records.retain(|_, record| {
            let live = record.expires > now;
            if !live && let Some(count) = owners.get_mut(&record.owner) {
                *count = count.saturating_sub(1);
            }
            live
        });
        owners.retain(|_, count| *count > 0);
        let longest = limits.read_window_secs.max(limits.write_window_secs);
        self.uses
            .retain(|_, uses| uses.back().is_some_and(|last| last + longest > now));
        self.next_sweep = now + SWEEP_INTERVAL_SECS;
    }

    /// Records one use. `Err(retry_after)` when the window is already full.
    fn admit(
        &mut self,
        key: u64,
        kind: Use,
        limit: usize,
        window: u64,
        now: u64,
    ) -> Result<(), u64> {
        let uses = self.uses.entry((key, kind)).or_default();
        while uses.front().is_some_and(|first| first + window <= now) {
            uses.pop_front();
        }
        if uses.len() >= limit {
            return Err(uses.front().map_or(window, |first| first + window - now));
        }
        uses.push_back(now);
        Ok(())
    }
}

#[derive(Clone)]
pub struct AppState {
    inner: Arc<Mutex<Inner>>,
    limits: Limits,
    clock: Clock,
    /// Keys the address hash so the store never holds a client address.
    address_key: [u8; 32],
}

impl AppState {
    pub fn new(limits: Limits, clock: Clock) -> Self {
        Self {
            inner: Arc::default(),
            limits,
            clock,
            address_key: rand::random(),
        }
    }

    fn lock(&self) -> MutexGuard<'_, Inner> {
        self.inner
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    /// Drops expired records and idle rate windows.
    pub fn sweep(&self) {
        let now = (self.clock)();
        self.lock().sweep(now, &self.limits);
    }

    /// Live records, for tests and the health check.
    pub fn len(&self) -> usize {
        let now = (self.clock)();
        self.lock()
            .records
            .values()
            .filter(|record| record.expires > now)
            .count()
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    fn address_hash(&self, address: IpAddr) -> u64 {
        // One IPv6 subscriber usually holds a whole /64.
        let bytes = match address {
            IpAddr::V4(v4) => v4.octets().to_vec(),
            IpAddr::V6(v6) => match v6.to_ipv4_mapped() {
                Some(v4) => v4.octets().to_vec(),
                None => v6.octets()[..8].to_vec(),
            },
        };
        let digest = Sha256::new()
            .chain_update(self.address_key)
            .chain_update(&bytes)
            .finalize();
        u64::from_le_bytes(digest[..8].try_into().unwrap_or_default())
    }
}

/// Serves `state` on `listener` until `shutdown` completes.
pub async fn serve_until(
    listener: tokio::net::TcpListener,
    state: AppState,
    shutdown: impl std::future::Future<Output = ()> + Send + 'static,
) -> std::io::Result<()> {
    axum::serve(
        listener,
        router(state).into_make_service_with_connect_info::<SocketAddr>(),
    )
    .with_graceful_shutdown(shutdown)
    .await
}

pub fn router(state: AppState) -> Router {
    Router::new()
        .route("/s/v1/{locator}", get(fetch).put(store))
        .route("/s/health", get(health))
        .layer(DefaultBodyLimit::max(MAX_BODY_BYTES))
        .with_state(state)
}

/// The client's network address. The service listens on loopback behind
/// nginx, which sets `X-Real-IP`; only a local process could forge it.
struct Client(u64);

impl FromRequestParts<AppState> for Client {
    type Rejection = std::convert::Infallible;

    async fn from_request_parts(
        parts: &mut Parts,
        state: &AppState,
    ) -> Result<Self, Self::Rejection> {
        let forwarded = parts
            .headers
            .get("x-real-ip")
            .and_then(|value| value.to_str().ok())
            .and_then(|value| value.trim().parse::<IpAddr>().ok());
        let peer = parts
            .extensions
            .get::<ConnectInfo<SocketAddr>>()
            .map(|info| info.0.ip());
        let address = forwarded
            .or(peer)
            .unwrap_or(IpAddr::V4(std::net::Ipv4Addr::UNSPECIFIED));
        Ok(Self(state.address_hash(address)))
    }
}

fn parse_locator(text: &str) -> Option<[u8; LOCATOR_BYTES]> {
    if text.len() != 22 {
        return None;
    }
    URL_SAFE_NO_PAD.decode(text).ok()?.try_into().ok()
}

fn status(code: StatusCode) -> Response {
    let mut response = code.into_response();
    response
        .headers_mut()
        .insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
    response
}

fn too_many(retry_after: u64) -> Response {
    let mut response = status(StatusCode::TOO_MANY_REQUESTS);
    if let Ok(value) = HeaderValue::from_str(&retry_after.max(1).to_string()) {
        response.headers_mut().insert(header::RETRY_AFTER, value);
    }
    response
}

async fn health() -> Response {
    let mut response = "ok".into_response();
    response
        .headers_mut()
        .insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
    response
}

async fn fetch(
    State(state): State<AppState>,
    Client(client): Client,
    Path(locator): Path<String>,
) -> Response {
    let now = (state.clock)();
    let limits = state.limits;
    let mut inner = state.lock();
    if let Err(retry) = inner.admit(
        client,
        Use::Read,
        limits.reads_per_window,
        limits.read_window_secs,
        now,
    ) {
        return too_many(retry);
    }
    let Some(locator) = parse_locator(&locator) else {
        return status(StatusCode::BAD_REQUEST);
    };
    match inner.records.get(&locator) {
        Some(record) if record.expires > now => {
            let mut response = record.sealed.clone().into_response();
            let headers = response.headers_mut();
            headers.insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
            headers.insert(
                header::CONTENT_TYPE,
                HeaderValue::from_static("application/octet-stream"),
            );
            response
        }
        _ => status(StatusCode::NOT_FOUND),
    }
}

async fn store(
    State(state): State<AppState>,
    Client(client): Client,
    Path(locator): Path<String>,
    body: Bytes,
) -> Response {
    let now = (state.clock)();
    let limits = state.limits;
    let mut inner = state.lock();
    if let Err(retry) = inner.admit(
        client,
        Use::Write,
        limits.writes_per_window,
        limits.write_window_secs,
        now,
    ) {
        return too_many(retry);
    }
    let Some(locator) = parse_locator(&locator) else {
        return status(StatusCode::BAD_REQUEST);
    };
    if body.len() > MAX_BODY_BYTES {
        return status(StatusCode::PAYLOAD_TOO_LARGE);
    }
    if body.len() < PUT_HEADER_BYTES + MIN_SEALED_BYTES || body[0] != WIRE_VERSION {
        return status(StatusCode::BAD_REQUEST);
    }
    let ttl = u32::from_le_bytes([body[1], body[2], body[3], body[4]]);
    if !(MIN_TTL_SECS..=MAX_TTL_SECS).contains(&ttl) {
        return status(StatusCode::BAD_REQUEST);
    }
    let write_hash: [u8; 32] = Sha256::digest(&body[5..PUT_HEADER_BYTES]).into();
    let sealed = body.slice(PUT_HEADER_BYTES..);
    let expires = now + u64::from(ttl);
    if now >= inner.next_sweep {
        inner.sweep(now, &limits);
    }
    if let Some(record) = inner
        .records
        .get_mut(&locator)
        .filter(|record| record.expires > now)
    {
        if !bool::from(record.write_hash.ct_eq(&write_hash)) {
            return status(StatusCode::FORBIDDEN);
        }
        record.sealed = sealed;
        record.expires = expires;
        return status(StatusCode::NO_CONTENT);
    }
    // An expired record still in the map gives its slot back first.
    if let Some(old) = inner.records.remove(&locator)
        && let Some(count) = inner.owners.get_mut(&old.owner)
    {
        *count = count.saturating_sub(1);
    }
    if inner.records.len() >= limits.max_records {
        inner.sweep(now, &limits);
        if inner.records.len() >= limits.max_records {
            return status(StatusCode::SERVICE_UNAVAILABLE);
        }
    }
    let owned = inner.owners.get(&client).copied().unwrap_or(0);
    if owned >= limits.max_records_per_address {
        return too_many(u64::from(MIN_TTL_SECS));
    }
    *inner.owners.entry(client).or_default() += 1;
    inner.records.insert(
        locator,
        Record {
            sealed,
            write_hash,
            expires,
            owner: client,
        },
    );
    status(StatusCode::NO_CONTENT)
}

#[cfg(test)]
mod tests {
    use std::sync::atomic::{AtomicU64, Ordering};

    use axum::{
        body::Body,
        http::{Method, Request},
    };
    use tower::ServiceExt;

    use super::*;

    struct Harness {
        state: AppState,
        time: Arc<AtomicU64>,
    }

    impl Harness {
        fn new(limits: Limits) -> Self {
            let time = Arc::new(AtomicU64::new(1_000_000));
            let clock_time = time.clone();
            let state = AppState::new(limits, Arc::new(move || clock_time.load(Ordering::SeqCst)));
            Self { state, time }
        }

        fn advance(&self, seconds: u64) {
            self.time.fetch_add(seconds, Ordering::SeqCst);
        }

        async fn send(
            &self,
            method: Method,
            locator: &str,
            body: Vec<u8>,
            address: &str,
        ) -> (StatusCode, Vec<u8>) {
            let request = Request::builder()
                .method(method)
                .uri(format!("/s/v1/{locator}"))
                .header("x-real-ip", address)
                .body(Body::from(body))
                .unwrap();
            let response = router(self.state.clone()).oneshot(request).await.unwrap();
            let code = response.status();
            let bytes = axum::body::to_bytes(response.into_body(), usize::MAX)
                .await
                .unwrap();
            (code, bytes.to_vec())
        }

        async fn put(&self, locator: &str, body: Vec<u8>, address: &str) -> StatusCode {
            self.send(Method::PUT, locator, body, address).await.0
        }

        async fn get(&self, locator: &str, address: &str) -> (StatusCode, Vec<u8>) {
            self.send(Method::GET, locator, Vec::new(), address).await
        }
    }

    fn locator(seed: u8) -> String {
        URL_SAFE_NO_PAD.encode([seed; LOCATOR_BYTES])
    }

    fn body(ttl: u32, write: u8, sealed: &[u8]) -> Vec<u8> {
        let mut body = vec![WIRE_VERSION];
        body.extend_from_slice(&ttl.to_le_bytes());
        body.extend_from_slice(&[write; WRITE_TOKEN_BYTES]);
        body.extend_from_slice(sealed);
        body
    }

    #[tokio::test]
    async fn a_record_is_returned_until_it_expires() {
        let harness = Harness::new(Limits::default());
        let sealed = vec![7; 64];
        assert_eq!(
            harness
                .put(&locator(1), body(600, 1, &sealed), "203.0.113.5")
                .await,
            StatusCode::NO_CONTENT
        );
        let (code, bytes) = harness.get(&locator(1), "198.51.100.9").await;
        assert_eq!(code, StatusCode::OK);
        assert_eq!(bytes, sealed);
        assert_eq!(
            harness.get(&locator(2), "198.51.100.9").await.0,
            StatusCode::NOT_FOUND
        );
        harness.advance(600);
        assert_eq!(
            harness.get(&locator(1), "198.51.100.9").await.0,
            StatusCode::NOT_FOUND
        );
        harness.state.sweep();
        assert!(harness.state.is_empty());
    }

    #[tokio::test]
    async fn only_the_first_write_token_may_replace_a_record() {
        let harness = Harness::new(Limits::default());
        assert_eq!(
            harness
                .put(&locator(1), body(600, 1, &[1; 40]), "203.0.113.5")
                .await,
            StatusCode::NO_CONTENT
        );
        assert_eq!(
            harness
                .put(&locator(1), body(600, 2, &[2; 40]), "203.0.113.6")
                .await,
            StatusCode::FORBIDDEN
        );
        assert_eq!(harness.get(&locator(1), "203.0.113.7").await.1, vec![1; 40]);
        // Another member with the same token refreshes it from elsewhere.
        harness.advance(500);
        assert_eq!(
            harness
                .put(&locator(1), body(600, 1, &[3; 40]), "203.0.113.6")
                .await,
            StatusCode::NO_CONTENT
        );
        harness.advance(500);
        assert_eq!(harness.get(&locator(1), "203.0.113.7").await.1, vec![3; 40]);
        // Once it has expired the locator is free again.
        harness.advance(600);
        assert_eq!(
            harness
                .put(&locator(1), body(600, 2, &[4; 40]), "203.0.113.6")
                .await,
            StatusCode::NO_CONTENT
        );
    }

    #[tokio::test]
    async fn malformed_and_oversized_requests_are_refused() {
        let harness = Harness::new(Limits {
            writes_per_window: 100,
            reads_per_window: 100,
            ..Limits::default()
        });
        let address = "203.0.113.5";
        for bad in [
            "short",
            "!!!!!!!!!!!!!!!!!!!!!!",
            &format!("{}A", locator(1)),
        ] {
            assert_eq!(
                harness.put(bad, body(600, 1, &[1; 40]), address).await,
                StatusCode::BAD_REQUEST
            );
            assert_eq!(harness.get(bad, address).await.0, StatusCode::BAD_REQUEST);
        }
        for ttl in [0, MIN_TTL_SECS - 1, MAX_TTL_SECS + 1, u32::MAX] {
            assert_eq!(
                harness
                    .put(&locator(1), body(ttl, 1, &[1; 40]), address)
                    .await,
                StatusCode::BAD_REQUEST
            );
        }
        assert_eq!(
            harness
                .put(
                    &locator(1),
                    body(600, 1, &[1; MIN_SEALED_BYTES - 1]),
                    address
                )
                .await,
            StatusCode::BAD_REQUEST
        );
        let mut wrong_version = body(600, 1, &[1; 40]);
        wrong_version[0] = 2;
        assert_eq!(
            harness.put(&locator(1), wrong_version, address).await,
            StatusCode::BAD_REQUEST
        );
        assert_eq!(
            harness
                .put(
                    &locator(1),
                    body(600, 1, &vec![1; MAX_SEALED_BYTES + 1]),
                    address
                )
                .await,
            StatusCode::PAYLOAD_TOO_LARGE
        );
        assert_eq!(
            harness
                .put(
                    &locator(1),
                    body(MAX_TTL_SECS, 1, &vec![1; MAX_SEALED_BYTES]),
                    address
                )
                .await,
            StatusCode::NO_CONTENT
        );
        assert_eq!(harness.state.len(), 1);
    }

    #[tokio::test]
    async fn one_address_cannot_fill_the_store() {
        let harness = Harness::new(Limits {
            max_records: 6,
            max_records_per_address: 2,
            writes_per_window: 100,
            ..Limits::default()
        });
        for seed in 0..2 {
            assert_eq!(
                harness
                    .put(&locator(seed), body(600, 1, &[1; 40]), "203.0.113.5")
                    .await,
                StatusCode::NO_CONTENT
            );
        }
        assert_eq!(
            harness
                .put(&locator(9), body(600, 1, &[1; 40]), "203.0.113.5")
                .await,
            StatusCode::TOO_MANY_REQUESTS
        );
        // An update of its own record still goes through.
        assert_eq!(
            harness
                .put(&locator(0), body(600, 1, &[2; 40]), "203.0.113.5")
                .await,
            StatusCode::NO_CONTENT
        );
        // One IPv6 /64 counts as one address.
        for (seed, address) in [(10, "2001:db8:1:2::1"), (11, "2001:db8:1:2::ffff")] {
            assert_eq!(
                harness
                    .put(&locator(seed), body(600, 1, &[1; 40]), address)
                    .await,
                StatusCode::NO_CONTENT
            );
        }
        assert_eq!(
            harness
                .put(&locator(12), body(600, 1, &[1; 40]), "2001:db8:1:2:abcd::1")
                .await,
            StatusCode::TOO_MANY_REQUESTS
        );
        // The global cap holds whatever the addresses.
        for (seed, address) in [(20, "198.51.100.1"), (21, "198.51.100.2")] {
            assert_eq!(
                harness
                    .put(&locator(seed), body(600, 1, &[1; 40]), address)
                    .await,
                StatusCode::NO_CONTENT
            );
        }
        assert_eq!(
            harness
                .put(&locator(22), body(600, 1, &[1; 40]), "198.51.100.3")
                .await,
            StatusCode::SERVICE_UNAVAILABLE
        );
        // Expiry frees both the address and the store.
        harness.advance(600);
        assert_eq!(
            harness
                .put(&locator(22), body(600, 1, &[1; 40]), "198.51.100.3")
                .await,
            StatusCode::NO_CONTENT
        );
        assert_eq!(
            harness
                .put(&locator(9), body(600, 1, &[1; 40]), "203.0.113.5")
                .await,
            StatusCode::NO_CONTENT
        );
    }

    #[tokio::test]
    async fn lookups_and_stores_are_rate_limited_per_address() {
        let harness = Harness::new(Limits {
            reads_per_window: 3,
            read_window_secs: 60,
            writes_per_window: 2,
            write_window_secs: 600,
            ..Limits::default()
        });
        for _ in 0..3 {
            assert_eq!(
                harness.get(&locator(1), "203.0.113.5").await.0,
                StatusCode::NOT_FOUND
            );
        }
        assert_eq!(
            harness.get(&locator(1), "203.0.113.5").await.0,
            StatusCode::TOO_MANY_REQUESTS
        );
        assert_eq!(
            harness.get(&locator(1), "203.0.113.6").await.0,
            StatusCode::NOT_FOUND
        );
        harness.advance(60);
        assert_eq!(
            harness.get(&locator(1), "203.0.113.5").await.0,
            StatusCode::NOT_FOUND
        );
        for seed in 0..2 {
            assert_eq!(
                harness
                    .put(&locator(seed), body(600, 1, &[1; 40]), "203.0.113.5")
                    .await,
                StatusCode::NO_CONTENT
            );
        }
        assert_eq!(
            harness
                .put(&locator(2), body(600, 1, &[1; 40]), "203.0.113.5")
                .await,
            StatusCode::TOO_MANY_REQUESTS
        );
    }

    #[tokio::test]
    async fn responses_are_never_cached() {
        let harness = Harness::new(Limits::default());
        harness
            .put(&locator(1), body(600, 1, &[1; 40]), "203.0.113.5")
            .await;
        let request = Request::builder()
            .uri(format!("/s/v1/{}", locator(1)))
            .body(Body::empty())
            .unwrap();
        let response = router(harness.state.clone())
            .oneshot(request)
            .await
            .unwrap();
        assert_eq!(response.headers()[header::CACHE_CONTROL], "no-store");
        let request = Request::builder()
            .uri("/s/health")
            .body(Body::empty())
            .unwrap();
        let response = router(harness.state.clone())
            .oneshot(request)
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::OK);
    }
}
