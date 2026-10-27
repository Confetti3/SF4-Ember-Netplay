//! The helper's bridge client (spec 10). It talks only to approved bridges,
//! checks every challenge against the operation the user started before
//! signing it, and keeps session tokens in memory only.
use std::{
    collections::HashMap,
    sync::{Arc, Mutex},
    time::Duration,
};

use ember_protocol::{
    EmberId,
    api::{ApiError, BridgeProfile, Capabilities, SigningKey, SigningKeys, WELL_KNOWN_PATH},
    challenge::{
        Action, Challenge, Expected, MAX_PROOF_BODY, Method, ProvenRequest, command_digest,
    },
    encoding::{OriginPolicy, check_origin, is_prefixed_id},
    json,
};
use serde::{Deserialize, de::DeserializeOwned};
use serde_json::{Value, json, value::RawValue};
use zeroize::Zeroizing;

use super::{Code, Failure, Locked, Outcome, Shared, bridges::Approved, sites};

const MAX_RESPONSE: usize = 64 * 1024;
/// How long creating a room may take. The bridge waits up to 30 s for the
/// supervisor, and the room host it starts may take up to 25 s to report
/// hosted, so the ordinary request timeout would give up on a room that is
/// then opened.
pub(super) const CREATE_TIMEOUT: Duration = Duration::from_secs(45);
const POLICY: OriginPolicy = OriginPolicy::AllowLoopbackHttp;
/// Renew a cached session this long before it expires.
const SESSION_MARGIN_SECS: u64 = 30;
/// The bridge's code for a session it no longer accepts.
const UNAUTHENTICATED: &str = "unauthenticated";

/// A bridge session, valid only for the origin that issued it and the
/// Ember ID that opened it.
struct Session {
    token: Zeroizing<String>,
    expires_at: u64,
    origin: String,
    ember_id: EmberId,
}

pub struct Client {
    http: Option<reqwest::Client>,
    sessions: Mutex<HashMap<String, Session>>,
}

pub(super) fn now() -> u64 {
    crate::identity::unix_now().unwrap_or(0)
}

fn unreachable() -> Failure {
    Failure::new(Code::BridgeUnreachable)
}

impl Client {
    pub fn new() -> Self {
        // iroh already links rustls with ring; reqwest needs it installed as
        // the process default before building a client.
        let _ = rustls::crypto::ring::default_provider().install_default();
        let http = reqwest::Client::builder()
            .redirect(reqwest::redirect::Policy::none())
            .connect_timeout(Duration::from_secs(5))
            .timeout(Duration::from_secs(15))
            .user_agent(concat!("sf4-net/", env!("CARGO_PKG_VERSION")))
            .build()
            .ok();
        Self {
            http,
            sessions: Mutex::new(HashMap::new()),
        }
    }

    pub fn forget_sessions(&self) {
        if let Ok(mut sessions) = self.sessions.lock() {
            sessions.clear();
        }
    }

    pub fn forget_session(&self, bridge_id: &str) {
        if let Ok(mut sessions) = self.sessions.lock() {
            sessions.remove(bridge_id);
        }
    }

    fn cached(&self, bridge: &Approved, ember_id: &EmberId) -> Option<Zeroizing<String>> {
        let sessions = self.sessions.lock().ok()?;
        sessions
            .get(&bridge.bridge_id)
            .filter(|session| session.origin == bridge.origin && &session.ember_id == ember_id)
            .filter(|session| session.expires_at > now() + SESSION_MARGIN_SECS)
            .map(|session| session.token.clone())
    }

    /// Sends one request and reads at most `MAX_RESPONSE` bytes back.
    pub(super) async fn call(
        &self,
        method: reqwest::Method,
        url: &str,
        token: Option<&str>,
        body: Option<Vec<u8>>,
    ) -> Result<(u16, Vec<u8>), Failure> {
        self.call_within(method, url, token, body, None).await
    }

    /// `call` with `timeout` as the request's total time in place of the
    /// client's own.
    pub(super) async fn call_within(
        &self,
        method: reqwest::Method,
        url: &str,
        token: Option<&str>,
        body: Option<Vec<u8>>,
        timeout: Option<Duration>,
    ) -> Result<(u16, Vec<u8>), Failure> {
        let http = self.http.as_ref().ok_or_else(unreachable)?;
        let mut request = http.request(method, url);
        if let Some(timeout) = timeout {
            request = request.timeout(timeout);
        }
        if let Some(token) = token {
            request = request.bearer_auth(token);
        }
        if let Some(body) = body {
            request = request
                .header("content-type", "application/json")
                .body(body);
        }
        let mut response = request.send().await.map_err(|_| unreachable())?;
        let status = response.status().as_u16();
        let mut bytes = Vec::new();
        while let Some(chunk) = response.chunk().await.map_err(|_| unreachable())? {
            if bytes.len() + chunk.len() > MAX_RESPONSE {
                return Err(Failure::new(Code::BridgeResponseTooLarge));
            }
            bytes.extend_from_slice(&chunk);
        }
        Ok((status, bytes))
    }

    /// Fetches a bridge's discovery document and capabilities. This does not
    /// make the bridge trusted; the caller shows it for approval.
    pub async fn inspect(&self, origin: &str) -> Result<(BridgeProfile, Offered), Failure> {
        let profile = self.profile(origin).await?;
        Ok((profile, self.capabilities(origin).await?))
    }

    /// Checks a built-in bridge before each new session: its origin must
    /// still serve the pinned bridge ID, else nothing is proved to it. A
    /// domain now pointing at another bridge fails every request while it
    /// does; one that cannot be reached is not a mismatch.
    async fn check_pin(&self, bridge: &Approved) -> Result<(), Failure> {
        match self.profile(&bridge.origin).await {
            Ok(profile) if profile.bridge_id == bridge.bridge_id => Ok(()),
            Ok(_) => Err(Failure::new(Code::PinnedBridgeMismatch)),
            Err(Failure::Local(Code::BridgeMismatch)) => {
                Err(Failure::new(Code::PinnedBridgeMismatch))
            }
            Err(failure) => Err(failure),
        }
    }

    /// The discovery document at `origin`, which must describe that origin.
    async fn profile(&self, origin: &str) -> Result<BridgeProfile, Failure> {
        check_origin(origin, POLICY).map_err(|_| Failure::new(Code::InvalidOrigin))?;
        let (status, body) = self
            .call(
                reqwest::Method::GET,
                &format!("{origin}{WELL_KNOWN_PATH}"),
                None,
                None,
            )
            .await?;
        if status != 200 {
            return Err(Failure::new(Code::NotABridge));
        }
        let profile: BridgeProfile =
            json::parse_as(&body, MAX_RESPONSE).map_err(|_| Failure::new(Code::NotABridge))?;
        // A bridge must describe the origin it is served from.
        if profile.origin != origin || !is_prefixed_id(&profile.bridge_id, "brg") {
            return Err(Failure::new(Code::BridgeMismatch));
        }
        Ok(profile)
    }

    /// What the bridge at `origin` offers. Its sign-in sites are read on
    /// their own and leniently (`sites::listed`): an entry this build does
    /// not understand is dropped, never failing the document that Discord,
    /// approval and match play also read.
    pub async fn capabilities(&self, origin: &str) -> Result<Offered, Failure> {
        check_origin(origin, POLICY).map_err(|_| Failure::new(Code::InvalidOrigin))?;
        let (status, body) = self
            .call(
                reqwest::Method::GET,
                &format!("{origin}/v1/capabilities"),
                None,
                None,
            )
            .await?;
        if status != 200 {
            return Err(Failure::new(Code::NotABridge));
        }
        let offered = Offered::read(&body).ok_or_else(|| Failure::new(Code::NotABridge))?;
        if offered.capabilities.api_version != "v1" {
            return Err(Failure::new(Code::UnsupportedBridge));
        }
        Ok(offered)
    }

    /// The bridge's public signing keys, from the approved origin only.
    pub async fn signing_keys(&self, origin: &str) -> Result<Vec<SigningKey>, Failure> {
        check_origin(origin, POLICY).map_err(|_| Failure::new(Code::InvalidOrigin))?;
        let (status, body) = self
            .call(
                reqwest::Method::GET,
                &format!("{origin}/v1/signing-keys"),
                None,
                None,
            )
            .await?;
        if status != 200 {
            return Err(Failure::new(Code::NotABridge));
        }
        let keys: SigningKeys =
            json::parse_as(&body, MAX_RESPONSE).map_err(|_| Failure::new(Code::NotABridge))?;
        Ok(keys.keys)
    }
}

/// A bridge's capabilities, with the sign-in sites it lists kept apart:
/// `capabilities.sign_in_sites` is always empty.
pub struct Offered {
    pub capabilities: Capabilities,
    pub sign_in_sites: Vec<sites::Listed>,
}

impl Offered {
    /// The capabilities document `body`, held to the strict JSON rules
    /// (`json::parse`) everywhere but inside `sign_in_sites`. The document's
    /// own members are split out as written, their values not yet read
    /// (`Members`). Every member but `sign_in_sites` is put back byte for
    /// byte and read strictly, so a duplicate key, a fraction, an unsafe
    /// integer, a value nested too deep or trailing data there fails the
    /// read as it always has, and so does a second `sign_in_sites`. The one
    /// `sign_in_sites` is read entry by entry (`sites::listed`): whatever a
    /// newer bridge puts in it, it never fails the document that Discord,
    /// approval and match play read.
    fn read(body: &[u8]) -> Option<Self> {
        let Members(members) = serde_json::from_slice(body).ok()?;
        let mut rest = Vec::with_capacity(body.len());
        let mut listed = None;
        rest.push(b'{');
        for (key, value) in members {
            if key == "sign_in_sites" {
                if listed.replace(value).is_some() {
                    return None;
                }
                continue;
            }
            if rest.len() > 1 {
                rest.push(b',');
            }
            serde_json::to_writer(&mut rest, &key).ok()?;
            rest.push(b':');
            rest.extend_from_slice(value.get().as_bytes());
        }
        rest.push(b'}');
        Some(Self {
            capabilities: json::parse_as(&rest, MAX_RESPONSE).ok()?,
            sign_in_sites: sites::listed(listed),
        })
    }

    /// The document the game reads: the capabilities, with the sites listed
    /// only when there are any, so a bridge without them answers as before.
    pub fn to_json(&self) -> Value {
        let mut document = json!(self.capabilities);
        if !self.sign_in_sites.is_empty()
            && let Some(fields) = document.as_object_mut()
        {
            fields.insert("sign_in_sites".into(), json!(self.sign_in_sites));
        }
        document
    }
}

/// A JSON object's members in the order written, a repeated key kept each
/// time, each value only checked to be JSON and kept as its text.
struct Members<'a>(Vec<(String, &'a RawValue)>);

impl<'de> Deserialize<'de> for Members<'de> {
    fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct Visit;
        impl<'de> serde::de::Visitor<'de> for Visit {
            type Value = Members<'de>;
            fn expecting(&self, formatter: &mut std::fmt::Formatter) -> std::fmt::Result {
                formatter.write_str("a JSON object")
            }
            fn visit_map<A: serde::de::MapAccess<'de>>(
                self,
                mut map: A,
            ) -> Result<Self::Value, A::Error> {
                let mut members = Vec::new();
                while let Some(member) = map.next_entry::<String, &'de RawValue>()? {
                    members.push(member);
                }
                Ok(Members(members))
            }
        }
        deserializer.deserialize_map(Visit)
    }
}

/// An HTTP verb a bridge route is called with.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Verb {
    Get,
    Post,
    Put,
    Delete,
}

impl Verb {
    fn http(self) -> reqwest::Method {
        match self {
            Self::Get => reqwest::Method::GET,
            Self::Post => reqwest::Method::POST,
            Self::Put => reqwest::Method::PUT,
            Self::Delete => reqwest::Method::DELETE,
        }
    }

    /// The method a proof of a command is made for: the one verb the request
    /// is sent with, so the two cannot differ. Routes with other verbs take
    /// no proof.
    fn proven(self) -> Option<Method> {
        match self {
            Self::Post => Some(Method::Post),
            Self::Delete => Some(Method::Delete),
            Self::Get | Self::Put => None,
        }
    }
}

/// What a bridge call carries.
#[derive(Clone, Copy)]
pub(super) enum Body<'a> {
    None,
    /// A JSON body the session token alone authorizes.
    Json(&'a [u8]),
    /// A command proven by the identity for `action`, sent with its proof.
    Proven {
        action: Action,
        command: &'a Value,
    },
}

/// How a route's refusals are worded: a bridge's `details.reason` that is one
/// of `reasons` is the failure as it is, and a 404 naming none of them is a
/// bridge without the route, `missing`, when there is one to name.
#[derive(Clone, Copy)]
pub(super) struct Refusals<'a> {
    pub reasons: &'a [&'a str],
    pub missing: Option<Code>,
}

impl Refusals<'static> {
    /// The bridge's error code or `http_<status>`, nothing worded apart.
    pub(super) const NONE: Self = Self {
        reasons: &[],
        missing: None,
    };
}

/// One authenticated call to a bridge: the verb and path, what it carries, the
/// statuses that are its answer and how it is refused.
pub(super) struct BridgeCall<'a> {
    verb: Verb,
    path: &'a str,
    body: Body<'a>,
    success: &'a [u16],
    refusals: Refusals<'a>,
    timeout: Option<Duration>,
}

impl<'a> BridgeCall<'a> {
    pub(super) fn new(verb: Verb, path: &'a str, success: &'a [u16]) -> Self {
        Self {
            verb,
            path,
            body: Body::None,
            success,
            refusals: Refusals::NONE,
            timeout: None,
        }
    }

    pub(super) fn body(self, body: Body<'a>) -> Self {
        Self { body, ..self }
    }

    pub(super) fn refusals(self, refusals: Refusals<'a>) -> Self {
        Self { refusals, ..self }
    }

    /// The call's total time in place of the client's own.
    pub(super) fn timeout(self, timeout: Duration) -> Self {
        Self {
            timeout: Some(timeout),
            ..self
        }
    }
}

/// Sends `call` to `bridge`, proving its command first when it carries one,
/// and answers the bridge's status and body. `token` is the session's, or
/// none for the call that opens one.
pub(super) async fn send(
    shared: &Arc<Shared>,
    bridge: &Approved,
    token: Option<&str>,
    call: &BridgeCall<'_>,
) -> Result<(u16, Vec<u8>), Failure> {
    let body = match call.body {
        Body::None => None,
        Body::Json(bytes) => Some(bytes.to_vec()),
        Body::Proven { action, command } => {
            let method = call
                .verb
                .proven()
                .ok_or_else(|| Failure::new(Code::Internal))?;
            Some(prove(shared, bridge, token, action, method, call.path, command).await?)
        }
    };
    shared
        .client
        .call_within(
            call.verb.http(),
            &format!("{}{}", bridge.origin, call.path),
            token,
            body,
            call.timeout,
        )
        .await
}

/// `send`, and the bridge's answer as JSON.
pub(super) async fn fetch(
    shared: &Arc<Shared>,
    bridge: &Approved,
    token: Option<&str>,
    call: &BridgeCall<'_>,
) -> Result<Value, Failure> {
    let (status, body) = send(shared, bridge, token, call).await?;
    answer(status, &body, call.success, &call.refusals)
}

/// `send`, and the bridge's answer read as `T`.
pub(super) async fn fetch_as<T: DeserializeOwned>(
    shared: &Arc<Shared>,
    bridge: &Approved,
    token: Option<&str>,
    call: &BridgeCall<'_>,
) -> Result<T, Failure> {
    let (status, body) = send(shared, bridge, token, call).await?;
    read(status, &body, call.success, &call.refusals)
}

/// `call` under a session on `bridge_id`, and the bridge's answer as JSON.
pub(super) async fn request(
    shared: &Arc<Shared>,
    bridge_id: &str,
    call: &BridgeCall<'_>,
) -> Result<Value, Failure> {
    with_session(shared, bridge_id, |bridge, token| async move {
        fetch(shared, &bridge, Some(&token), call).await
    })
    .await
}

/// `call` under a session on `bridge_id`, and the bridge's answer read as `T`.
pub(super) async fn request_as<T: DeserializeOwned>(
    shared: &Arc<Shared>,
    bridge_id: &str,
    call: &BridgeCall<'_>,
) -> Result<T, Failure> {
    with_session(shared, bridge_id, |bridge, token| async move {
        fetch_as(shared, &bridge, Some(&token), call).await
    })
    .await
}

/// The bridge's answer, as a failure code when it is not a success.
pub(super) fn answer(
    status: u16,
    body: &[u8],
    success: &[u16],
    refusals: &Refusals<'_>,
) -> Result<Value, Failure> {
    if success.contains(&status) {
        return json::parse(body, MAX_RESPONSE)
            .and_then(|value| json::from_value::<Value>(&value))
            .map_err(|_| Failure::new(Code::BridgeInvalidResponse));
    }
    Err(refused(status, body, refusals))
}

/// The bridge's answer read as `T`, as a failure code when it is not a
/// success or is not a `T`.
pub(super) fn read<T: DeserializeOwned>(
    status: u16,
    body: &[u8],
    success: &[u16],
    refusals: &Refusals<'_>,
) -> Result<T, Failure> {
    if success.contains(&status) {
        return json::parse_as(body, MAX_RESPONSE)
            .map_err(|_| Failure::new(Code::BridgeInvalidResponse));
    }
    Err(refused(status, body, refusals))
}

/// The failure for an answer that is not a success of its route.
fn refused(status: u16, body: &[u8], refusals: &Refusals<'_>) -> Failure {
    let failure = refusal(status, body, refusals.reasons);
    match refusals.missing {
        Some(missing) if status == 404 && !refusals.reasons.contains(&failure.as_str()) => {
            Failure::new(missing)
        }
        _ => failure,
    }
}

/// The failure for a bridge's refusal: its `details.reason` when that is one
/// of `reasons`, which a caller words apart, else its error code, or
/// `http_<status>` when the body names none.
pub(super) fn refusal(status: u16, body: &[u8], reasons: &[&str]) -> Failure {
    let error = serde_json::from_slice::<ApiError>(body).ok();
    let reason = error.as_ref().and_then(|error| {
        let reason = error.error.details.get("reason")?.as_str()?;
        reasons.contains(&reason).then(|| reason.to_owned())
    });
    let code = || {
        error
            .and_then(|error| serde_json::to_value(error.error.code).ok())
            .and_then(|code| code.as_str().map(str::to_owned))
            .unwrap_or_else(|| format!("http_{status}"))
    };
    Failure::Bridge(reason.unwrap_or_else(code))
}

/// Whether this build pins `bridge_id` (builtin.rs).
pub(super) fn pinned(shared: &Shared, bridge_id: &str) -> Result<bool, Failure> {
    Ok(shared.bridges.locked()?.pins(bridge_id))
}

pub(super) fn approved(shared: &Shared, bridge_id: &str) -> Result<Approved, Failure> {
    shared
        .bridges
        .locked()?
        .get(bridge_id)
        .ok_or_else(|| Failure::new(Code::BridgeNotApproved))
}

/// Requests a challenge for `command`, checks it, and signs it. The identity
/// lock is only tried: a key operation in progress makes this `identity_busy`
/// rather than blocking a runtime thread.
async fn prove(
    shared: &Shared,
    bridge: &Approved,
    token: Option<&str>,
    action: Action,
    method: Method,
    path: &str,
    command: &Value,
) -> Result<Vec<u8>, Failure> {
    let command = json::to_value(command).map_err(|_| Failure::new(Code::Internal))?;
    let public_key = {
        let identity = shared
            .identity
            .try_lock()
            .map_err(|_| Failure::new(Code::IdentityBusy))?;
        identity
            .signer()
            .ok_or_else(|| Failure::new(Code::IdentityUnavailable))?
            .public_key()
    };
    let request = json!({
        "public_key": public_key,
        "action": action,
        "method": method,
        "path": path,
        "request_digest": command_digest(&command),
    });
    let (status, body) = shared
        .client
        .call(
            reqwest::Method::POST,
            &format!("{}/v1/auth/challenges", bridge.origin),
            token,
            Some(serde_json::to_vec(&request).map_err(|_| Failure::new(Code::Internal))?),
        )
        .await?;
    if status != 201 {
        return Err(refusal(status, &body, &[]));
    }
    let challenge: Challenge = json::parse_as(&body, MAX_PROOF_BODY)
        .map_err(|_| Failure::new(Code::BridgeInvalidResponse))?;
    let proof = {
        let identity = shared
            .identity
            .try_lock()
            .map_err(|_| Failure::new(Code::IdentityBusy))?;
        let signer = identity
            .signer()
            .ok_or_else(|| Failure::new(Code::IdentityUnavailable))?;
        // The challenge must name the approved bridge, our ID, and exactly
        // this operation and command; anything else is never signed.
        let expected = Expected {
            bridge_id: &bridge.bridge_id,
            audience: &bridge.origin,
            ember_id: signer.ember_id(),
            action,
            method,
            path,
            command: &command,
        };
        challenge
            .sign(signer, &expected, now(), POLICY)
            .map_err(|_| Failure::new(Code::ChallengeMismatch))?
    };
    ProvenRequest { command, proof }
        .to_json()
        .map_err(|_| Failure::new(Code::Internal))
}

#[derive(Deserialize)]
struct SessionCreated {
    session_token: String,
    expires_at: u64,
}

/// The unlocked identity's ID, without waiting on key work.
pub(super) fn current_id(shared: &Shared) -> Result<EmberId, Failure> {
    let identity = shared
        .identity
        .try_lock()
        .map_err(|_| Failure::new(Code::IdentityBusy))?;
    identity
        .signer()
        .map(|signer| signer.ember_id().clone())
        .ok_or_else(|| Failure::new(Code::IdentityUnavailable))
}

async fn session(shared: &Arc<Shared>, bridge: &Approved) -> Result<Zeroizing<String>, Failure> {
    // A session opened by an identity the player has since replaced is never reused.
    let ember_id = current_id(shared)?;
    if let Some(token) = shared.client.cached(bridge, &ember_id) {
        return Ok(token);
    }
    let pinned = pinned(shared, &bridge.bridge_id)?;
    if pinned {
        shared.client.check_pin(bridge).await?;
    }
    let command = json!({ "requested_scopes": ["self:read", "tournament:participate"] });
    let call = BridgeCall::new(Verb::Post, "/v1/sessions", &[201]).body(Body::Proven {
        action: Action::SessionCreate,
        command: &command,
    });
    let created: SessionCreated = fetch_as(shared, bridge, None, &call).await?;
    let token = Zeroizing::new(created.session_token);
    if let Ok(mut sessions) = shared.client.sessions.lock() {
        sessions.insert(
            bridge.bridge_id.clone(),
            Session {
                token: token.clone(),
                expires_at: created.expires_at,
                origin: bridge.origin.clone(),
                ember_id,
            },
        );
    }
    if pinned {
        activate(shared, &bridge.bridge_id).await;
    }
    Ok(token)
}

/// Runs `operation` with a session, retrying once with a new session if the
/// bridge no longer accepts the cached one. The operation is called again, so
/// a caller borrows what it sends (a reference is copied into each future)
/// and clones none of it.
pub(super) async fn with_session<T, F, Fut>(
    shared: &Arc<Shared>,
    bridge_id: &str,
    mut operation: F,
) -> Result<T, Failure>
where
    F: FnMut(Approved, Zeroizing<String>) -> Fut,
    Fut: std::future::Future<Output = Result<T, Failure>>,
{
    let bridge = approved(shared, bridge_id)?;
    let token = session(shared, &bridge).await?;
    match operation(bridge.clone(), token).await {
        Err(Failure::Bridge(code)) if code == UNAUTHENTICATED => {
            shared.client.forget_session(bridge_id);
            let token = session(shared, &bridge).await?;
            operation(bridge, token).await
        }
        other => other,
    }
}

/// A dormant built-in becomes one of the player's services once a session
/// to it opens. It counts as used for the rest of the run even when that
/// cannot be recorded (`Store::activate`).
async fn activate(shared: &Arc<Shared>, bridge_id: &str) {
    let store = shared.clone();
    let id = bridge_id.to_owned();
    let _ = tokio::task::spawn_blocking(move || {
        if let Ok(mut bridges) = store.bridges.lock() {
            let _ = bridges.activate(&id);
        }
    })
    .await;
}

pub async fn links(shared: &Arc<Shared>, bridge_id: &str) -> Outcome {
    let call = BridgeCall::new(Verb::Get, "/v1/links", &[200]);
    request(shared, bridge_id, &call).await.map(Some)
}

pub async fn claim(
    shared: &Arc<Shared>,
    bridge_id: &str,
    connection_id: &str,
    code: &str,
) -> Outcome {
    if code.len() > 16 || connection_id.is_empty() || connection_id.len() > 64 {
        return Err(Failure::new(Code::InvalidRequest));
    }
    let command = json!({ "code": code, "connection_id": connection_id, "consent": true });
    let call = BridgeCall::new(Verb::Post, "/v1/link-claims", &[201]).body(Body::Proven {
        action: Action::LinkClaim,
        command: &command,
    });
    request(shared, bridge_id, &call).await.map(Some)
}

pub async fn cancel_claim(shared: &Arc<Shared>, bridge_id: &str, claim_id: &str) -> Outcome {
    if !is_prefixed_id(claim_id, "lkc") {
        return Err(Failure::new(Code::InvalidRequest));
    }
    let path = format!("/v1/link-claims/{claim_id}/cancel");
    let command = json!({ "claim_id": claim_id });
    let call = BridgeCall::new(Verb::Post, &path, &[200]).body(Body::Proven {
        action: Action::LinkCancel,
        command: &command,
    });
    request(shared, bridge_id, &call).await.map(Some)
}

pub async fn remove_link(shared: &Arc<Shared>, bridge_id: &str, link_id: &str) -> Outcome {
    if !is_prefixed_id(link_id, "lnk") {
        return Err(Failure::new(Code::InvalidRequest));
    }
    let path = format!("/v1/links/{link_id}");
    let command = json!({ "link_id": link_id });
    let call = BridgeCall::new(Verb::Delete, &path, &[200]).body(Body::Proven {
        action: Action::LinkRemove,
        command: &command,
    });
    request(shared, bridge_id, &call).await.map(Some)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The capabilities hold to the strict JSON rules everywhere but inside
    /// `sign_in_sites`: a break of them anywhere else fails the read as it
    /// always did, and nothing a site list holds ever does. A bridge without
    /// sites reads back exactly as it was served.
    #[test]
    fn only_the_site_list_is_read_leniently() {
        const FIELDS: &str = concat!(
            r#""api_version":"v1","event_version":"v1","games":["usf4"],"games_to_win":[1],"#,
            r#""native_rules_profiles":[],"native_play":true,"result_sources":[],"#,
            r#""features":["discord","sites","matches.play"],"lobby_rotations":[],"#,
            r#""tournament_formats":[],"connections":[],"limits":{"max_body_bytes":1,"#,
            r#""max_proof_body_bytes":1,"max_json_depth":1,"challenge_lifetime_secs":1,"#,
            r#""session_lifetime_secs":1,"link_code_lifetime_secs":1}"#,
        );
        let read = |extra: &str| Offered::read(format!("{{{FIELDS}{extra}}}").as_bytes());
        let plain = read("").unwrap();
        assert!(plain.sign_in_sites.is_empty());
        let served: Value = serde_json::from_str(&format!("{{{FIELDS}}}")).unwrap();
        assert_eq!(plain.to_json(), served);

        // Outside the site list: a duplicate key, a fraction, an exponent, an
        // unsafe integer, nesting past the limit, a second site list,
        // trailing data, a missing field, or no object at all.
        let deep = format!("{}1{}", "[".repeat(17), "]".repeat(17));
        for extra in [
            r#","features":["discord"]"#.to_owned(),
            r#","weight":0.5"#.to_owned(),
            r#","weight":1e3"#.to_owned(),
            r#","weight":9007199254740993"#.to_owned(),
            format!(r#","nested":{deep}"#),
            r#","sign_in_sites":[],"sign_in_sites":[]"#.to_owned(),
        ] {
            assert!(read(&extra).is_none(), "{extra}");
        }
        assert!(Offered::read(format!("{{{FIELDS}}} {{}}").as_bytes()).is_none());
        assert!(Offered::read(br#"{"api_version":"v1"}"#).is_none());
        assert!(Offered::read(b"[]").is_none());

        // Inside it, the same breaks and any shape at all never fail it.
        let good = concat!(
            r#"{"id":"challonge","display_name":"Challonge","sign_in":true,"lookup_by":"username","#,
            r#""authorize_origin":"https://api.challonge.com","authorize_path":"/oauth/authorize"}"#,
        );
        let later = format!(
            r#"{{"id":"later","display_name":"Later","sign_in":true,"lookup_by":"email","weight":0.5,"rank":1e3,"big":9007199254740993,"deep":{deep},"authorize_origin":"https://later.example","authorize_path":"/sign-in"}}"#
        );
        let twice = concat!(
            r#"{"id":"twice","id":"again","display_name":"Twice","sign_in":true,"#,
            r#""authorize_origin":"https://twice.example","authorize_path":"/x"}"#,
        );
        let listed: &[&str] = &["challonge", "later"];
        for (sites, ids) in [
            (
                format!(r#"[{good},{later},{twice},{{"id":"half"}},7,1.5,"x",null,[{good}]]"#),
                listed,
            ),
            (format!(r#"{{"challonge":{good}}}"#), &[][..]),
            (r#""challonge""#.to_owned(), &[][..]),
            ("2.5e10".to_owned(), &[][..]),
            ("null".to_owned(), &[][..]),
            (format!("[{deep}]"), &[][..]),
        ] {
            let offered =
                read(&format!(r#","sign_in_sites":{sites}"#)).unwrap_or_else(|| panic!("{sites}"));
            assert_eq!(
                offered.capabilities.features,
                ["discord", "sites", "matches.play"]
            );
            assert!(offered.capabilities.sign_in_sites.is_empty());
            let kept: Vec<&str> = offered
                .sign_in_sites
                .iter()
                .map(|site| site.id.as_str())
                .collect();
            assert_eq!(kept, ids, "{sites}");
            let shown = offered.to_json();
            if ids.is_empty() {
                assert_eq!(shown, served, "{sites}");
            } else {
                assert!(shown["sign_in_sites"][0].get("lookup_by").is_none());
                assert_eq!(shown["sign_in_sites"][1]["authorize_path"], "/sign-in");
            }
        }
    }

    #[test]
    fn a_session_is_offered_only_to_its_origin() {
        let client = Client::new();
        let ember_id = ember_protocol::SigningIdentity::from_seed(&Zeroizing::new([7; 32]))
            .unwrap()
            .ember_id()
            .clone();
        let bridge_id = ember_protocol::encoding::prefixed_id("brg", [1; 16]);
        client.sessions.lock().unwrap().insert(
            bridge_id.clone(),
            Session {
                token: Zeroizing::new("token".into()),
                expires_at: now() + 600,
                origin: "https://a.example".into(),
                ember_id: ember_id.clone(),
            },
        );
        let at = |origin: &str| Approved {
            bridge_id: bridge_id.clone(),
            origin: origin.into(),
            display_name: "Bridge".into(),
            approved_at: 0,
            keys: Vec::new(),
        };
        assert!(client.cached(&at("https://a.example"), &ember_id).is_some());
        assert!(client.cached(&at("https://b.example"), &ember_id).is_none());
    }

    /// A proof is made for the method the request is sent with, so the two
    /// cannot differ, and a route that takes no proof has none.
    #[test]
    fn a_proof_is_made_for_the_verb_the_request_is_sent_with() {
        assert_eq!(Verb::Post.proven(), Some(Method::Post));
        assert_eq!(Verb::Delete.proven(), Some(Method::Delete));
        assert_eq!(Verb::Get.proven(), None);
        assert_eq!(Verb::Put.proven(), None);
        assert_eq!(Verb::Post.http(), reqwest::Method::POST);
        assert_eq!(Verb::Delete.http(), reqwest::Method::DELETE);
        assert_eq!(Verb::Get.http(), reqwest::Method::GET);
        assert_eq!(Verb::Put.http(), reqwest::Method::PUT);
    }

    fn refusal_body(code: &str, reason: Option<&str>) -> Vec<u8> {
        let details = reason.map_or(json!({}), |reason| json!({ "reason": reason }));
        serde_json::to_vec(&json!({ "error": {
            "code": code, "message": "m", "retryable": false, "request_id": "req_1",
            "details": details,
        }}))
        .unwrap()
    }

    /// An answer read as a type is held to the same strict rules as one read
    /// as JSON first and then as the type, and refused with the same code.
    #[test]
    fn a_typed_answer_is_read_as_strictly_as_an_untyped_one() {
        #[derive(Debug, Deserialize, PartialEq)]
        #[serde(deny_unknown_fields)]
        struct Shape {
            a: u64,
        }
        let none = Refusals::NONE;
        assert_eq!(
            read::<Shape>(201, br#"{"a":1}"#, &[200, 201], &none).unwrap(),
            Shape { a: 1 }
        );
        assert_eq!(
            answer(201, br#"{"a":1}"#, &[200, 201], &none).unwrap(),
            json!({ "a": 1 })
        );
        for body in [
            &br#"{"a":1,"b":2}"#[..],
            br#"{"a":1,"a":1}"#,
            br#"{"a":1.5}"#,
            br#"{"a":9007199254740993}"#,
            br#"{"a":"1"}"#,
            br#"{"a":1} {}"#,
            b"",
        ] {
            let typed = read::<Shape>(200, body, &[200], &none).unwrap_err();
            assert_eq!(typed.as_str(), "bridge_invalid_response", "{body:?}");
        }
        // The untyped read keeps what the type would refuse, as before.
        assert!(answer(200, br#"{"a":1,"b":2}"#, &[200], &none).is_ok());
        assert!(answer(200, br#"{"a":1,"a":1}"#, &[200], &none).is_err());
        // A status that is not a success is refused alike, by code.
        let body = refusal_body("forbidden", None);
        assert_eq!(
            read::<Shape>(403, &body, &[200], &none)
                .unwrap_err()
                .as_str(),
            "forbidden"
        );
        assert_eq!(
            answer(403, &body, &[200], &none).unwrap_err().as_str(),
            "forbidden"
        );
    }

    /// A refusal is the bridge's code, kept apart from the helper's own: a
    /// code this build does not know is `http_<status>`, and a 404 naming no
    /// reason is a bridge without the route only for a route that says so.
    #[test]
    fn a_refusal_is_the_bridges_code_or_the_status_and_a_404_may_be_a_missing_route() {
        let body = refusal_body("brand_new_code", None);
        let unknown = refusal(409, &body, &[]);
        assert!(matches!(&unknown, Failure::Bridge(code) if code == "http_409"));
        let known = refusal(403, &refusal_body("forbidden", None), &[]);
        assert!(matches!(&known, Failure::Bridge(code) if code == "forbidden"));
        let reasons = ["room_full"];
        let worded = refusal(409, &refusal_body("forbidden", Some("room_full")), &reasons);
        assert!(matches!(&worded, Failure::Bridge(code) if code == "room_full"));

        let route = Refusals {
            reasons: &reasons,
            missing: Some(Code::RoomsUnavailable),
        };
        let missing = answer(404, &refusal_body("not_found", None), &[200], &route);
        assert!(matches!(
            missing,
            Err(Failure::Local(Code::RoomsUnavailable))
        ));
        let named = answer(
            404,
            &refusal_body("not_found", Some("room_full")),
            &[200],
            &route,
        );
        assert!(matches!(named, Err(Failure::Bridge(code)) if code == "room_full"));
        let other = answer(403, &refusal_body("forbidden", None), &[200], &route);
        assert!(matches!(other, Err(Failure::Bridge(code)) if code == "forbidden"));
        let plain = Refusals {
            reasons: &reasons,
            missing: None,
        };
        let not_found = answer(404, &refusal_body("not_found", None), &[200], &plain);
        assert!(matches!(not_found, Err(Failure::Bridge(code)) if code == "not_found"));
    }
}
