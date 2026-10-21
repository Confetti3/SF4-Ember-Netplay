//! The helper's bridge client (spec 10). It talks only to approved bridges,
//! checks every challenge against the operation the user started before
//! signing it, and keeps session tokens in memory only.
use std::{
    collections::HashMap,
    sync::{Arc, Mutex},
    time::{Duration, SystemTime, UNIX_EPOCH},
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
use serde::Deserialize;
use serde_json::{Value, json, value::RawValue};
use zeroize::Zeroizing;

use super::{Failure, Outcome, Shared, bridges::Approved, sites};

const MAX_RESPONSE: usize = 64 * 1024;
/// How long creating a room may take. The bridge waits up to 30 s for the
/// supervisor, and the room host it starts may take up to 25 s to report
/// hosted, so the ordinary request timeout would give up on a room that is
/// then opened.
pub(super) const CREATE_TIMEOUT: Duration = Duration::from_secs(45);
const POLICY: OriginPolicy = OriginPolicy::AllowLoopbackHttp;
/// Renew a cached session this long before it expires.
const SESSION_MARGIN_SECS: u64 = 30;
/// A built-in bridge whose origin no longer serves its pinned ID.
const PIN_MISMATCH: &str = "pinned_bridge_mismatch";

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
        self.send(
            method,
            url,
            token,
            body,
            "application/json",
            MAX_RESPONSE,
            timeout,
        )
        .await
    }

    /// `call` with a body of `content_type` and at most `max` bytes back.
    pub(super) async fn call_bounded(
        &self,
        method: reqwest::Method,
        url: &str,
        token: Option<&str>,
        body: Option<Vec<u8>>,
        content_type: &str,
        max: usize,
    ) -> Result<(u16, Vec<u8>), Failure> {
        self.send(method, url, token, body, content_type, max, None)
            .await
    }

    #[allow(clippy::too_many_arguments)]
    async fn send(
        &self,
        method: reqwest::Method,
        url: &str,
        token: Option<&str>,
        body: Option<Vec<u8>>,
        content_type: &str,
        max: usize,
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
            request = request.header("content-type", content_type).body(body);
        }
        let mut response = request.send().await.map_err(|_| unreachable())?;
        let status = response.status().as_u16();
        let mut bytes = Vec::new();
        while let Some(chunk) = response.chunk().await.map_err(|_| unreachable())? {
            if bytes.len() + chunk.len() > max {
                return Err(Failure::new("bridge_response_too_large"));
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
            Ok(_) => Err(Failure::new(PIN_MISMATCH)),
            Err(Failure(code)) if code == "bridge_mismatch" => Err(Failure::new(PIN_MISMATCH)),
            Err(failure) => Err(failure),
        }
    }

    /// The discovery document at `origin`, which must describe that origin.
    async fn profile(&self, origin: &str) -> Result<BridgeProfile, Failure> {
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
        Ok(profile)
    }

    /// What the bridge at `origin` offers. Its sign-in sites are read on
    /// their own and leniently (`sites::listed`): an entry this build does
    /// not understand is dropped, never failing the document that Discord,
    /// approval and match play also read.
    pub async fn capabilities(&self, origin: &str) -> Result<Offered, Failure> {
        check_origin(origin, POLICY).map_err(|_| Failure::new("invalid_origin"))?;
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
        let offered = Offered::read(&body).ok_or_else(|| Failure::new("not_a_bridge"))?;
        if offered.capabilities.api_version != "v1" {
            return Err(Failure::new("unsupported_bridge"));
        }
        Ok(offered)
    }

    /// The bridge's public signing keys, from the approved origin only.
    pub async fn signing_keys(&self, origin: &str) -> Result<Vec<SigningKey>, Failure> {
        check_origin(origin, POLICY).map_err(|_| Failure::new("invalid_origin"))?;
        let (status, body) = self
            .call(
                reqwest::Method::GET,
                &format!("{origin}/v1/signing-keys"),
                None,
                None,
            )
            .await?;
        if status != 200 {
            return Err(Failure::new("not_a_bridge"));
        }
        let keys: SigningKeys =
            json::parse_as(&body, MAX_RESPONSE).map_err(|_| Failure::new("not_a_bridge"))?;
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

/// The bridge's answer, as a failure code when it is not a success.
pub(super) fn answer(status: u16, body: &[u8], expected: &[u16]) -> Result<Value, Failure> {
    if expected.contains(&status) {
        return json::parse(body, MAX_RESPONSE)
            .and_then(|value| json::from_value::<Value>(&value))
            .map_err(|_| Failure::new("bridge_invalid_response"));
    }
    Err(refusal(status, body, &[]))
}

/// The answer of a route an older bridge may not serve: `answer` for
/// `success`, a refusal as `refusal` words it with `reasons`, except that a
/// 404 naming none of them is a bridge without the route, `missing`.
pub(super) fn feature_answer(
    status: u16,
    body: &[u8],
    success: u16,
    reasons: &[&str],
    missing: &str,
) -> Result<Value, Failure> {
    if status == success {
        return answer(status, body, &[success]);
    }
    let failure = refusal(status, body, reasons);
    if status == 404 && !reasons.contains(&failure.0.as_str()) {
        return Err(Failure::new(missing));
    }
    Err(failure)
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
    Failure(reason.unwrap_or_else(code))
}

/// Whether this build pins `bridge_id` (builtin.rs).
pub(super) fn pinned(shared: &Shared, bridge_id: &str) -> Result<bool, Failure> {
    Ok(shared
        .bridges
        .lock()
        .map_err(|_| Failure::new("internal"))?
        .pins(bridge_id))
}

pub(super) fn approved(shared: &Shared, bridge_id: &str) -> Result<Approved, Failure> {
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
pub(super) async fn prove(
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

/// The unlocked identity's ID, without waiting on key work.
pub(super) fn current_id(shared: &Shared) -> Result<EmberId, Failure> {
    let identity = shared
        .identity
        .try_lock()
        .map_err(|_| Failure::new("identity_busy"))?;
    identity
        .signer()
        .map(|signer| signer.ember_id().clone())
        .ok_or_else(|| Failure::new("identity_unavailable"))
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
/// bridge no longer accepts the cached one.
pub(super) async fn with_session<F, Fut>(shared: &Arc<Shared>, bridge_id: &str, operation: F) -> Outcome
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
}
