//! The helper's bridge client (spec 10). It talks only to approved bridges,
//! checks every challenge against the operation the user started before
//! signing it, and keeps session tokens in memory only.
use std::{
    collections::HashMap,
    sync::{Arc, Mutex},
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use ember_protocol::{
    api::{ApiError, BridgeProfile, Capabilities, WELL_KNOWN_PATH},
    challenge::{
        Action, Challenge, Expected, MAX_PROOF_BODY, Method, ProvenRequest, command_digest,
    },
    encoding::{OriginPolicy, check_origin, is_prefixed_id},
    json,
};
use serde::Deserialize;
use serde_json::{Value, json};
use zeroize::Zeroizing;

use super::{Failure, Outcome, Shared, bridges::Approved};

const MAX_RESPONSE: usize = 64 * 1024;
const POLICY: OriginPolicy = OriginPolicy::AllowLoopbackHttp;
/// Renew a cached session this long before it expires.
const SESSION_MARGIN_SECS: u64 = 30;

struct Session {
    token: Zeroizing<String>,
    expires_at: u64,
}

pub struct Client {
    http: Option<reqwest::Client>,
    sessions: Mutex<HashMap<String, Session>>,
}

fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |elapsed| elapsed.as_secs())
}

fn unreachable() -> Failure {
    Failure::new("bridge_unreachable")
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

    fn cached(&self, bridge_id: &str) -> Option<Zeroizing<String>> {
        let sessions = self.sessions.lock().ok()?;
        sessions
            .get(bridge_id)
            .filter(|session| session.expires_at > now() + SESSION_MARGIN_SECS)
            .map(|session| session.token.clone())
    }

    /// Sends one request and reads at most `MAX_RESPONSE` bytes back.
    async fn call(
        &self,
        method: reqwest::Method,
        url: &str,
        token: Option<&str>,
        body: Option<Vec<u8>>,
    ) -> Result<(u16, Vec<u8>), Failure> {
        let http = self.http.as_ref().ok_or_else(unreachable)?;
        let mut request = http.request(method, url);
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
                return Err(Failure::new("bridge_response_too_large"));
            }
            bytes.extend_from_slice(&chunk);
        }
        Ok((status, bytes))
    }

    /// Fetches a bridge's discovery document and capabilities. This does not
    /// make the bridge trusted; the caller shows it for approval.
    pub async fn inspect(&self, origin: &str) -> Result<(BridgeProfile, Capabilities), Failure> {
        check_origin(origin, POLICY).map_err(|_| Failure::new("invalid_origin"))?;
        let (status, body) = self
            .call(
                reqwest::Method::GET,
                &format!("{origin}{WELL_KNOWN_PATH}"),
                None,
                None,
            )
            .await?;
        if status != 200 {
            return Err(Failure::new("not_a_bridge"));
        }
        let profile: BridgeProfile =
            json::parse_as(&body, MAX_RESPONSE).map_err(|_| Failure::new("not_a_bridge"))?;
        // A bridge must describe the origin it is served from.
        if profile.origin != origin || !is_prefixed_id(&profile.bridge_id, "brg") {
            return Err(Failure::new("bridge_mismatch"));
        }
        let (status, body) = self
            .call(
                reqwest::Method::GET,
                &format!("{origin}/v1/capabilities"),
                None,
                None,
            )
            .await?;
        if status != 200 {
            return Err(Failure::new("not_a_bridge"));
        }
        let capabilities: Capabilities =
            json::parse_as(&body, MAX_RESPONSE).map_err(|_| Failure::new("not_a_bridge"))?;
        if capabilities.api_version != "v1" {
            return Err(Failure::new("unsupported_bridge"));
        }
        Ok((profile, capabilities))
    }
}

/// The bridge's answer, as a failure code when it is not a success.
fn answer(status: u16, body: &[u8], expected: &[u16]) -> Result<Value, Failure> {
    if expected.contains(&status) {
        return json::parse(body, MAX_RESPONSE)
            .and_then(|value| json::from_value::<Value>(&value))
            .map_err(|_| Failure::new("bridge_invalid_response"));
    }
    let code = serde_json::from_slice::<ApiError>(body)
        .map(|error| {
            serde_json::to_value(error.error.code)
                .ok()
                .and_then(|v| v.as_str().map(str::to_owned))
        })
        .ok()
        .flatten()
        .unwrap_or_else(|| format!("http_{status}"));
    Err(Failure(code))
}

fn approved(shared: &Shared, bridge_id: &str) -> Result<Approved, Failure> {
    shared
        .bridges
        .lock()
        .map_err(|_| Failure::new("internal"))?
        .get(bridge_id)
        .ok_or_else(|| Failure::new("bridge_not_approved"))
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
    command: Value,
) -> Result<Vec<u8>, Failure> {
    let command = json::to_value(&command).map_err(|_| Failure::new("internal"))?;
    let public_key = {
        let identity = shared
            .identity
            .try_lock()
            .map_err(|_| Failure::new("identity_busy"))?;
        identity
            .signer()
            .ok_or_else(|| Failure::new("identity_unavailable"))?
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
            Some(serde_json::to_vec(&request).map_err(|_| Failure::new("internal"))?),
        )
        .await?;
    if status != 201 {
        return Err(answer(status, &body, &[]).unwrap_err());
    }
    let challenge: Challenge = json::parse_as(&body, MAX_PROOF_BODY)
        .map_err(|_| Failure::new("bridge_invalid_response"))?;
    let proof = {
        let identity = shared
            .identity
            .try_lock()
            .map_err(|_| Failure::new("identity_busy"))?;
        let signer = identity
            .signer()
            .ok_or_else(|| Failure::new("identity_unavailable"))?;
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
            .map_err(|_| Failure::new("challenge_mismatch"))?
    };
    ProvenRequest { command, proof }
        .to_json()
        .map_err(|_| Failure::new("internal"))
}

#[derive(Deserialize)]
struct SessionCreated {
    session_token: String,
    expires_at: u64,
}

async fn session(shared: &Shared, bridge: &Approved) -> Result<Zeroizing<String>, Failure> {
    if let Some(token) = shared.client.cached(&bridge.bridge_id) {
        return Ok(token);
    }
    let body = prove(
        shared,
        bridge,
        None,
        Action::SessionCreate,
        Method::Post,
        "/v1/sessions",
        json!({ "requested_scopes": ["self:read", "tournament:participate"] }),
    )
    .await?;
    let (status, response) = shared
        .client
        .call(
            reqwest::Method::POST,
            &format!("{}/v1/sessions", bridge.origin),
            None,
            Some(body),
        )
        .await?;
    let created: SessionCreated = serde_json::from_value(answer(status, &response, &[201])?)
        .map_err(|_| Failure::new("bridge_invalid_response"))?;
    let token = Zeroizing::new(created.session_token);
    if let Ok(mut sessions) = shared.client.sessions.lock() {
        sessions.insert(
            bridge.bridge_id.clone(),
            Session {
                token: token.clone(),
                expires_at: created.expires_at,
            },
        );
    }
    Ok(token)
}

/// Runs `operation` with a session, retrying once with a new session if the
/// bridge no longer accepts the cached one.
async fn with_session<F, Fut>(shared: &Arc<Shared>, bridge_id: &str, operation: F) -> Outcome
where
    F: Fn(Approved, Zeroizing<String>) -> Fut,
    Fut: std::future::Future<Output = Outcome>,
{
    let bridge = approved(shared, bridge_id)?;
    let token = session(shared, &bridge).await?;
    match operation(bridge.clone(), token).await {
        Err(Failure(code)) if code == "unauthenticated" => {
            shared.client.forget_session(bridge_id);
            let token = session(shared, &bridge).await?;
            operation(bridge, token).await
        }
        other => other,
    }
}

pub async fn links(shared: &Arc<Shared>, bridge_id: &str) -> Outcome {
    with_session(shared, bridge_id, |bridge, token| async move {
        let (status, body) = shared
            .client
            .call(
                reqwest::Method::GET,
                &format!("{}/v1/links", bridge.origin),
                Some(&token),
                None,
            )
            .await?;
        Ok(Some(answer(status, &body, &[200])?))
    })
    .await
}

pub async fn claim(
    shared: &Arc<Shared>,
    bridge_id: &str,
    connection_id: &str,
    code: &str,
) -> Outcome {
    if code.len() > 16 || connection_id.is_empty() || connection_id.len() > 64 {
        return Err(Failure::new("invalid_request"));
    }
    with_session(shared, bridge_id, |bridge, token| async move {
        let command = json!({ "code": code, "connection_id": connection_id, "consent": true });
        let body = prove(
            shared,
            &bridge,
            Some(&token),
            Action::LinkClaim,
            Method::Post,
            "/v1/link-claims",
            command,
        )
        .await?;
        let (status, response) = shared
            .client
            .call(
                reqwest::Method::POST,
                &format!("{}/v1/link-claims", bridge.origin),
                Some(&token),
                Some(body),
            )
            .await?;
        Ok(Some(answer(status, &response, &[201])?))
    })
    .await
}

pub async fn cancel_claim(shared: &Arc<Shared>, bridge_id: &str, claim_id: &str) -> Outcome {
    if !is_prefixed_id(claim_id, "lkc") {
        return Err(Failure::new("invalid_request"));
    }
    with_session(shared, bridge_id, |bridge, token| async move {
        let path = format!("/v1/link-claims/{claim_id}/cancel");
        let body = prove(
            shared,
            &bridge,
            Some(&token),
            Action::LinkCancel,
            Method::Post,
            &path,
            json!({ "claim_id": claim_id }),
        )
        .await?;
        let (status, response) = shared
            .client
            .call(
                reqwest::Method::POST,
                &format!("{}{path}", bridge.origin),
                Some(&token),
                Some(body),
            )
            .await?;
        Ok(Some(answer(status, &response, &[200])?))
    })
    .await
}

pub async fn remove_link(shared: &Arc<Shared>, bridge_id: &str, link_id: &str) -> Outcome {
    if !is_prefixed_id(link_id, "lnk") {
        return Err(Failure::new("invalid_request"));
    }
    with_session(shared, bridge_id, |bridge, token| async move {
        let path = format!("/v1/links/{link_id}");
        let body = prove(
            shared,
            &bridge,
            Some(&token),
            Action::LinkRemove,
            Method::Delete,
            &path,
            json!({ "link_id": link_id }),
        )
        .await?;
        let (status, response) = shared
            .client
            .call(
                reqwest::Method::DELETE,
                &format!("{}{path}", bridge.origin),
                Some(&token),
                Some(body),
            )
            .await?;
        Ok(Some(answer(status, &response, &[200])?))
    })
    .await
}
