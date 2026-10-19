//! Tournament play for the native side (spec 14 to 16). The game drives it:
//! it claims an assigned match, hosts or joins the room the claim names,
//! prepares each official game and reports its confirmed result. This module
//! does the parts that need the key and the bridge: proofs, checking the
//! bridge's signed binding and permits against the approved bridge's key, and
//! signing reports into the spool. The game receives checked fields only.
use std::collections::{HashMap, HashSet};
use std::sync::Arc;

use ember_protocol::{
    api::Capabilities,
    challenge::{Action, Method},
    encoding::{Counter, is_prefixed_id, prefixed_id},
    play::{
        Binding, CLAIM_READY_FEATURE, Claim, ClaimAnswer, Observation, Permit, Prepare,
        PrepareAnswer, PublishRoom, SignedBinding, SignedPermit, TABLE,
    },
    report::Outcome as GameOutcome,
};
use serde_json::{Value, json};

use super::{
    Failure, Outcome, Shared,
    bridges::Approved,
    client::{self, answer, now},
    spool::Entry,
    watch::{claims_watch, unlearn_watch},
};

/// What this helper run has verified for each match it plays or watches.
#[derive(Default)]
pub struct State {
    bindings: HashMap<String, Binding>,
    permits: HashMap<(String, u64), Permit>,
    /// The matches whose bridge reads a claim's `ready`.
    reads_ready: HashSet<String>,
    /// The bridges that serve watching (`watch.rs`).
    pub(super) watch_bridges: HashSet<String>,
    /// The matches whose bridge did not serve watching when their first
    /// claim asked (`watch.rs`).
    pub(super) plain_claims: HashSet<String>,
}

pub(super) fn check_match_id(match_id: &str) -> Result<(), Failure> {
    if is_prefixed_id(match_id, "emt") {
        Ok(())
    } else {
        Err(Failure::new("invalid_request"))
    }
}

/// The approved bridge's key `kid`, fetching the bridge's current keys from
/// its approved origin once if this one is not known yet. A built-in's keys
/// are pinned by this build, so one it does not name is never fetched.
pub(super) async fn bridge_key(
    shared: &Arc<Shared>,
    bridge: &Approved,
    kid: &str,
) -> Result<ember_protocol::PublicKey, Failure> {
    if let Some(key) = bridge.key(kid) {
        return Ok(key);
    }
    if client::pinned(shared, &bridge.bridge_id)? {
        return Err(Failure::new("untrusted_signature"));
    }
    let keys = shared.client.signing_keys(&bridge.origin).await?;
    let store = shared.clone();
    let id = bridge.bridge_id.clone();
    let refreshed = tokio::task::spawn_blocking(move || {
        let mut bridges = store.bridges.lock().map_err(|_| Failure::new("internal"))?;
        bridges.set_keys(&id, keys)?;
        bridges
            .get(&id)
            .ok_or_else(|| Failure::new("bridge_not_approved"))
    })
    .await
    .map_err(|_| Failure::new("internal"))??;
    refreshed
        .key(kid)
        .ok_or_else(|| Failure::new("untrusted_signature"))
}

/// Verifies a binding and that it seats this endpoint as this identity.
async fn accept_binding(
    shared: &Arc<Shared>,
    bridge: &Approved,
    match_id: &str,
    room_id: &str,
    signed: &SignedBinding,
) -> Result<Value, Failure> {
    let key = bridge_key(shared, bridge, &signed.kid).await?;
    let binding = signed
        .verify(&key, &signed.kid)
        .map_err(|_| Failure::new("untrusted_signature"))?;
    let me = client::current_id(shared)?;
    let slot = binding.slot_of(&shared.endpoint_id);
    let mine = slot.is_some_and(|slot| binding.fighters[usize::from(slot)].ember_id == me);
    if binding.bridge_id != bridge.bridge_id
        || binding.match_id != match_id
        || binding.room_id != room_id
        || !mine
    {
        return Err(Failure::new("binding_mismatch"));
    }
    let view = binding_view(binding, slot);
    let mut state = shared.play.lock().map_err(|_| Failure::new("internal"))?;
    state.bindings.insert(match_id.to_owned(), binding.clone());
    Ok(view)
}

/// The checked fields of a binding the game reads, `local_slot` being this
/// fighter's. The watchers let it name each one in the room.
fn binding_view(binding: &Binding, slot: Option<u8>) -> Value {
    json!({
        "match_id": binding.match_id,
        "assignment_generation": binding.assignment_generation,
        "binding_revision": binding.binding_revision,
        "room_id": binding.room_id,
        "build_id": binding.build_id,
        "games_to_win": binding.games_to_win,
        "starting_wins": binding.starting_wins,
        "rules_digest": binding.rules_digest,
        "roster_digest": binding.roster_digest,
        "fighters": binding.fighters,
        "watchers": binding.watchers,
        "local_slot": slot,
    })
}

/// The claim answer for the game, with the binding checked.
async fn present(
    shared: &Arc<Shared>,
    bridge: &Approved,
    match_id: &str,
    answer: ClaimAnswer,
) -> Outcome {
    let value = match answer {
        ClaimAnswer::Room {
            room_id,
            invitation,
            binding,
        } => {
            let bound = match binding {
                Some(signed) => {
                    Some(accept_binding(shared, bridge, match_id, &room_id, &signed).await?)
                }
                None => None,
            };
            json!({ "role": "room", "room_id": room_id, "invitation": invitation, "binding": bound })
        }
        other => serde_json::to_value(other).map_err(|_| Failure::new("internal"))?,
    };
    Ok(Some(value))
}

/// Sends one proven player operation and reads its JSON answer.
async fn proven(
    shared: &Arc<Shared>,
    bridge: &Approved,
    token: &str,
    action: Action,
    path: &str,
    command: Value,
) -> Result<Value, Failure> {
    let (status, response) = send_proven(shared, bridge, token, action, path, command).await?;
    answer(status, &response, &[200])
}

/// Sends one proven player operation, answering its status and body.
pub(super) async fn send_proven(
    shared: &Arc<Shared>,
    bridge: &Approved,
    token: &str,
    action: Action,
    path: &str,
    command: Value,
) -> Result<(u16, Vec<u8>), Failure> {
    let body = client::prove(
        shared,
        bridge,
        Some(token),
        action,
        Method::Post,
        path,
        command,
    )
    .await?;
    shared
        .client
        .call(
            reqwest::Method::POST,
            &format!("{}{path}", bridge.origin),
            Some(token),
            Some(body),
        )
        .await
}

pub async fn assignments(shared: &Arc<Shared>, bridge_id: &str) -> Outcome {
    client::with_session(shared, bridge_id, |bridge, token| async move {
        let (status, body) = shared
            .client
            .call(
                reqwest::Method::GET,
                &format!("{}/v1/assignments", bridge.origin),
                Some(&token),
                None,
            )
            .await?;
        Ok(Some(answer(status, &body, &[200])?))
    })
    .await
}

/// Claims `match_id` with this helper run's endpoint, saying whether the
/// fighter has pressed Ready at its table. Claiming again renews a
/// provisioning lease and picks up a newer binding.
pub async fn claim_match(
    shared: &Arc<Shared>,
    bridge_id: &str,
    match_id: &str,
    build: &str,
    ready: bool,
) -> Outcome {
    check_match_id(match_id)?;
    let claim = Claim {
        endpoint_id: shared.endpoint_id.clone(),
        helper_instance_id: shared.instance_id.clone(),
        build_id: build.to_owned(),
        ready: None,
        watch: None,
    };
    claim.check().map_err(|_| Failure::new("invalid_request"))?;
    client::with_session(shared, bridge_id, |bridge, token| {
        let claim = claim.clone();
        async move {
            let path = format!("/v1/matches/{match_id}/claims");
            let value = send_claim(shared, &bridge, match_id, claim, ready, |command| {
                proven(shared, &bridge, &token, Action::MatchClaim, &path, command)
            })
            .await?;
            let answer: ClaimAnswer = serde_json::from_value(value)
                .map_err(|_| Failure::new("bridge_invalid_response"))?;
            present(shared, &bridge, match_id, answer).await
        }
    })
    .await
}

/// Sends `claim` through `send`, carrying `ready` when the fighter is Ready
/// and the bridge reads it. A claim without `ready` reads not ready on every
/// bridge, so only a Ready fighter's claim needs the bridge to read it, and
/// one carrying it that fails forgets that the bridge does (`unlearn_ready`).
/// Every claim says this helper reads watchers to a bridge that serves
/// watching (`watch::claims_watch`).
pub(super) async fn send_claim<F, Sent>(
    shared: &Arc<Shared>,
    bridge: &Approved,
    match_id: &str,
    claim: Claim,
    ready: bool,
    send: F,
) -> Result<Value, Failure>
where
    F: FnOnce(Value) -> Sent,
    Sent: std::future::Future<Output = Result<Value, Failure>>,
{
    let says_ready = ready && reads_ready(shared, bridge, match_id).await?;
    let says_watch = claims_watch(shared, bridge, match_id).await?;
    let claim = Claim {
        ready: says_ready.then_some(true),
        watch: says_watch.then_some(true),
        ..claim
    };
    let command = serde_json::to_value(&claim).map_err(|_| Failure::new("internal"))?;
    let sent = send(command).await;
    if sent.is_err() && says_ready {
        unlearn_ready(shared, match_id)?;
    }
    if sent.is_err() && says_watch {
        unlearn_watch(shared, bridge)?;
    }
    sent
}

pub(super) fn play_state(shared: &Shared) -> Result<std::sync::MutexGuard<'_, State>, Failure> {
    shared.play.lock().map_err(|_| Failure::new("internal"))
}

/// Whether the bridge reads a claim's `ready`. An older bridge refuses a
/// claim carrying it, so the helper asks the bridge's capabilities. A bridge
/// that reads it is remembered for the match; one that does not is asked
/// again on the next Ready claim, since it may be upgraded meanwhile. A
/// capabilities read that fails fails the claim, which the game retries,
/// rather than sending a Ready fighter's claim that reads not ready.
async fn reads_ready(
    shared: &Arc<Shared>,
    bridge: &Approved,
    match_id: &str,
) -> Result<bool, Failure> {
    if play_state(shared)?.reads_ready.contains(match_id) {
        return Ok(true);
    }
    let offered = shared.client.capabilities(&bridge.origin).await?;
    let reads = lists_ready(&offered.capabilities);
    if reads {
        play_state(shared)?.reads_ready.insert(match_id.to_owned());
    }
    Ok(reads)
}

/// Forgets that the match's bridge reads `ready`, after a claim carrying it
/// failed: the bridge may have been put back to one that refuses the field,
/// so the next Ready claim asks its capabilities again.
fn unlearn_ready(shared: &Shared, match_id: &str) -> Result<(), Failure> {
    play_state(shared)?.reads_ready.remove(match_id);
    Ok(())
}

fn lists_ready(capabilities: &Capabilities) -> bool {
    capabilities
        .features
        .iter()
        .any(|feature| feature == CLAIM_READY_FEATURE)
}

/// Publishes the room this game hosts for `match_id`.
pub async fn publish_room(
    shared: &Arc<Shared>,
    bridge_id: &str,
    match_id: &str,
    room: PublishRoom,
) -> Outcome {
    check_match_id(match_id)?;
    room.check().map_err(|_| Failure::new("invalid_request"))?;
    let command = serde_json::to_value(&room).map_err(|_| Failure::new("internal"))?;
    client::with_session(shared, bridge_id, |bridge, token| {
        let command = command.clone();
        async move {
            let path = format!("/v1/matches/{match_id}/room");
            let value =
                proven(shared, &bridge, &token, Action::RoomPublish, &path, command).await?;
            let answer: ClaimAnswer = serde_json::from_value(value)
                .map_err(|_| Failure::new("bridge_invalid_response"))?;
            present(shared, &bridge, match_id, answer).await
        }
    })
    .await
}

/// Describes the game about to start from the verified binding. Answers
/// `pending` until the other fighter has described the same game, then the
/// checked permit's identifiers.
pub async fn prepare_game(
    shared: &Arc<Shared>,
    bridge_id: &str,
    match_id: &str,
    match_generation: Counter,
) -> Outcome {
    check_match_id(match_id)?;
    let binding = shared
        .play
        .lock()
        .map_err(|_| Failure::new("internal"))?
        .bindings
        .get(match_id)
        .cloned()
        .ok_or_else(|| Failure::new("not_bound"))?;
    let prepare = Prepare {
        assignment_generation: binding.assignment_generation,
        binding_revision: binding.binding_revision,
        room_id: binding.room_id.clone(),
        table_id: TABLE,
        match_generation,
        rules_digest: binding.rules_digest.clone(),
        roster_digest: binding.roster_digest.clone(),
        build_id: binding.build_id.clone(),
        endpoint_id: shared.endpoint_id.clone(),
    };
    prepare
        .check()
        .map_err(|_| Failure::new("invalid_request"))?;
    let command = serde_json::to_value(&prepare).map_err(|_| Failure::new("internal"))?;
    client::with_session(shared, bridge_id, |bridge, token| {
        let command = command.clone();
        let (binding, prepare) = (binding.clone(), prepare.clone());
        async move {
            let path = format!("/v1/matches/{match_id}/attempts/prepare");
            let value = proven(
                shared,
                &bridge,
                &token,
                Action::AttemptPrepare,
                &path,
                command,
            )
            .await?;
            let answer: PrepareAnswer = serde_json::from_value(value)
                .map_err(|_| Failure::new("bridge_invalid_response"))?;
            match answer {
                PrepareAnswer::Pending { retry_after } => Ok(Some(
                    json!({ "state": "pending", "retry_after": retry_after }),
                )),
                PrepareAnswer::Permitted { permit } => {
                    let permit =
                        accept_permit(shared, &bridge, &binding, &prepare, &permit).await?;
                    Ok(Some(json!({
                        "state": "permitted",
                        "permit_id": permit.permit_id,
                        "attempt_id": permit.attempt_id,
                        "match_generation": permit.match_generation,
                        "issued_at": permit.issued_at,
                        "start_by": permit.start_by,
                    })))
                }
            }
        }
    })
    .await
}

/// Verifies a permit and that it is for exactly the described game.
async fn accept_permit(
    shared: &Arc<Shared>,
    bridge: &Approved,
    binding: &Binding,
    prepare: &Prepare,
    signed: &SignedPermit,
) -> Result<Permit, Failure> {
    let key = bridge_key(shared, bridge, &signed.kid).await?;
    let permit = signed
        .verify(&key, &signed.kid)
        .map_err(|_| Failure::new("untrusted_signature"))?;
    let same = permit.bridge_id == bridge.bridge_id
        && permit.match_id == binding.match_id
        && permit.assignment_generation == binding.assignment_generation
        && permit.room_id == binding.room_id
        && permit.table_id == prepare.table_id
        && permit.match_generation == prepare.match_generation
        && permit.rules_digest == binding.rules_digest
        && permit.roster_digest == binding.roster_digest
        && permit.build_id == binding.build_id
        && permit.p1_id == binding.fighters[0].ember_id
        && permit.p2_id == binding.fighters[1].ember_id;
    if !same {
        return Err(Failure::new("permit_mismatch"));
    }
    let mut state = shared.play.lock().map_err(|_| Failure::new("internal"))?;
    state.permits.insert(
        (permit.match_id.clone(), permit.match_generation.0),
        permit.clone(),
    );
    Ok(permit.clone())
}

/// Signs this fighter's report of a permitted game and spools it before
/// sending. `saved` means it survives a restart; `delivered` that the bridge
/// has it.
#[allow(clippy::too_many_arguments)]
pub async fn report_game(
    shared: &Arc<Shared>,
    bridge_id: &str,
    match_id: &str,
    match_generation: Counter,
    result: GameOutcome,
    capture_frame: Option<Counter>,
    confirmed_input_frame: Option<Counter>,
) -> Outcome {
    check_match_id(match_id)?;
    let permit = shared
        .play
        .lock()
        .map_err(|_| Failure::new("internal"))?
        .permits
        .get(&(match_id.to_owned(), match_generation.0))
        .cloned()
        .ok_or_else(|| Failure::new("unknown_permit"))?;
    let observation = Observation {
        observation_id: prefixed_id("obs", random()?),
        result,
        capture_frame,
        confirmed_input_frame,
        observed_at: now(),
        helper_instance_id: shared.instance_id.clone(),
    };
    let signed = {
        let identity = shared
            .identity
            .try_lock()
            .map_err(|_| Failure::new("identity_busy"))?;
        let signer = identity
            .signer()
            .ok_or_else(|| Failure::new("identity_unavailable"))?;
        permit
            .report(signer.ember_id(), observation)
            .and_then(|report| report.sign(signer))
            .map_err(|_| Failure::new("invalid_request"))?
    };
    let entry = Entry::new(bridge_id, match_id, signed, now());
    let store = shared.clone();
    let queued = entry.clone();
    tokio::task::spawn_blocking(move || store.spool.put(&queued))
        .await
        .map_err(|_| Failure::new("report_not_saved"))??;
    let delivered = deliver_one(shared, &entry).await;
    Ok(Some(json!({
        "saved": true,
        "delivered": delivered.is_some(),
        "attempt_state": delivered.as_ref().and_then(|receipt| receipt.get("attempt_state")).cloned(),
        "match_state": delivered.as_ref().and_then(|receipt| receipt.get("match_state")).cloned(),
    })))
}

/// Codes after which sending the same report again cannot succeed.
fn settled(code: &str) -> bool {
    matches!(
        code,
        "stale_revision"
            | "idempotency_conflict"
            | "invalid_signature"
            | "invalid_request"
            | "not_found"
            | "forbidden"
            | "bridge_not_approved"
    )
}

/// Sends one spooled report. Removes it once the bridge has answered for it
/// either way; keeps it when the answer may change on a later try.
async fn deliver_one(shared: &Arc<Shared>, entry: &Entry) -> Option<Value> {
    let body = serde_json::to_vec(&entry.report).ok()?;
    let match_id = entry.match_id.clone();
    let outcome = client::with_session(shared, &entry.bridge_id, |bridge, token| {
        let body = body.clone();
        let match_id = match_id.clone();
        async move {
            let (status, response) = shared
                .client
                .call(
                    reqwest::Method::POST,
                    &format!("{}/v1/matches/{match_id}/reports", bridge.origin),
                    Some(&token),
                    Some(body),
                )
                .await?;
            Ok(Some(answer(status, &response, &[200, 201])?))
        }
    })
    .await;
    match outcome {
        Ok(receipt) => {
            shared.spool.done(entry);
            receipt
        }
        Err(Failure(code)) if settled(&code) => {
            shared.spool.done(entry);
            None
        }
        Err(_) => None,
    }
}

/// One pass over the spool, oldest report first.
pub async fn deliver_pending(shared: &Arc<Shared>) {
    let store = shared.clone();
    let pending = tokio::task::spawn_blocking(move || store.spool.pending())
        .await
        .unwrap_or_default();
    for entry in pending {
        deliver_one(shared, &entry).await;
    }
}

/// Drops what this run verified for a match, once the game leaves it.
pub fn forget(shared: &Shared, match_id: &str) -> Outcome {
    let mut state = shared.play.lock().map_err(|_| Failure::new("internal"))?;
    state.bindings.remove(match_id);
    state.permits.retain(|(id, _), _| id != match_id);
    state.reads_ready.remove(match_id);
    state.plain_claims.remove(match_id);
    Ok(None)
}

fn random() -> Result<[u8; 16], Failure> {
    let mut bytes = [0; 16];
    getrandom::fill(&mut bytes).map_err(|_| Failure::new("internal"))?;
    Ok(bytes)
}

#[cfg(test)]
pub(super) mod tests {
    use super::*;
    use crate::tournament::{Request, builtin::Builtin};

    fn capabilities_json(features: &[&str]) -> Value {
        json!({
            "api_version": "v1", "event_version": "v1", "games": ["usf4"], "games_to_win": [1],
            "native_rules_profiles": [], "native_play": true, "result_sources": [],
            "features": features,
            "limits": {
                "max_body_bytes": 1, "max_proof_body_bytes": 1, "max_json_depth": 1,
                "challenge_lifetime_secs": 1, "session_lifetime_secs": 1, "link_code_lifetime_secs": 1,
            },
        })
    }

    fn capabilities(features: &[&str]) -> Capabilities {
        serde_json::from_value(capabilities_json(features)).unwrap()
    }

    #[test]
    fn the_game_says_whether_its_fighter_is_ready_with_each_claim() {
        let claim = |extra: Value| {
            let mut value = json!({ "op": "match_claim", "bridge_id": "brg_x", "match_id": "emt_x", "build": "b1" });
            if let Value::Object(extra) = extra {
                value.as_object_mut().unwrap().extend(extra);
            }
            serde_json::from_value::<Request>(value)
        };
        let ready = |request: Request| match request {
            Request::MatchClaim { ready, .. } => ready,
            _ => panic!("not a claim"),
        };
        assert!(ready(claim(json!({ "ready": true })).unwrap()));
        assert!(!ready(claim(json!({ "ready": false })).unwrap()));
        assert!(!ready(claim(json!({})).unwrap()));
        assert!(claim(json!({ "ready": "yes" })).is_err());
    }

    /// A bridge origin on loopback whose `/v1/capabilities` answers as
    /// `mode` says: 0 fails, 1 lists neither `ready` nor watching, 2 lists
    /// both. It answers every path so, and counts each request.
    pub(in crate::tournament) async fn capabilities_server(
        mode: Arc<std::sync::atomic::AtomicU8>,
    ) -> (String, Arc<std::sync::atomic::AtomicUsize>) {
        use tokio::io::{AsyncReadExt, AsyncWriteExt};
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let origin = format!("http://{}", listener.local_addr().unwrap());
        let asked = Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let counted = asked.clone();
        tokio::spawn(async move {
            loop {
                let Ok((mut socket, _)) = listener.accept().await else {
                    return;
                };
                let mut request = Vec::new();
                let mut buffer = [0; 1024];
                while !request.windows(4).any(|end| end == b"\r\n\r\n") {
                    match socket.read(&mut buffer).await {
                        Ok(0) | Err(_) => break,
                        Ok(read) => request.extend_from_slice(&buffer[..read]),
                    }
                }
                counted.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
                let features: &[&str] = match mode.load(std::sync::atomic::Ordering::SeqCst) {
                    1 => &["matches.play"],
                    2 => &[
                        "matches.play",
                        CLAIM_READY_FEATURE,
                        ember_protocol::play::WATCH_FEATURE,
                    ],
                    _ => {
                        let _ = socket
                            .write_all(b"HTTP/1.1 503 Service Unavailable\r\ncontent-length: 0\r\nconnection: close\r\n\r\n")
                            .await;
                        continue;
                    }
                };
                let body = serde_json::to_vec(&capabilities_json(features)).unwrap();
                let head = format!(
                    "HTTP/1.1 200 OK\r\ncontent-type: application/json\r\ncontent-length: {}\r\nconnection: close\r\n\r\n",
                    body.len()
                );
                let _ = socket.write_all(head.as_bytes()).await;
                let _ = socket.write_all(&body).await;
            }
        });
        (origin, asked)
    }

    fn shared_in(dir: &std::path::Path) -> Arc<Shared> {
        shared_pinning(dir, &[])
    }

    pub(in crate::tournament) fn shared_pinning(dir: &std::path::Path, builtins: &[Builtin]) -> Arc<Shared> {
        Arc::new(Shared {
            identity: std::sync::Mutex::new(crate::identity::Identity::open(
                dir.join("identity"),
                false,
                crate::identity::Cost::DEFAULT,
            )),
            bridges: std::sync::Mutex::new(super::super::bridges::Store::open(
                dir.join("bridges"),
                builtins,
            )),
            client: client::Client::new(),
            http: Arc::new(tokio::sync::Semaphore::new(1)),
            play: std::sync::Mutex::new(State::default()),
            spool: super::super::spool::Spool::open(dir.join("reports")),
            endpoint_id: "a".repeat(64),
            instance_id: prefixed_id("ins", [1; 16]),
            home_region: Box::new(|| None),
        })
    }

    // A Ready claim is not sent without its `ready` when the capabilities
    // read fails: the claim fails and the game retries it. A bridge that does
    // not read the field is asked again on the next Ready claim, so one
    // upgraded while the match is played is heard; one that reads it is
    // remembered, until a claim carrying it fails, since the bridge may have
    // been put back to one that refuses it.
    #[tokio::test]
    async fn a_ready_claim_asks_until_the_bridge_is_known_to_read_ready() {
        let dir = std::env::temp_dir().join(prefixed_id("sf4-net-ready", random().unwrap()));
        let shared = shared_in(&dir);
        let mode = Arc::new(std::sync::atomic::AtomicU8::new(0));
        let (origin, asked) = capabilities_server(mode.clone()).await;
        let bridge = Approved {
            bridge_id: "brg_x".into(),
            origin,
            display_name: "Bridge".into(),
            approved_at: 0,
            keys: Vec::new(),
        };
        let reads = || reads_ready(&shared, &bridge, "emt_x");
        let count = || asked.load(std::sync::atomic::Ordering::SeqCst);
        let set = |value| mode.store(value, std::sync::atomic::Ordering::SeqCst);

        assert!(reads().await.is_err());
        set(1);
        assert!(!reads().await.unwrap());
        assert!(!reads().await.unwrap());
        assert_eq!(count(), 3);
        set(2);
        assert!(reads().await.unwrap());
        set(0);
        assert!(reads().await.unwrap());
        assert_eq!(count(), 4);

        // The claims themselves: a Ready claim to a bridge known to read
        // `ready` carries it without asking again; once one carrying it
        // fails, the next asks again and, the bridge put back, sends none.
        let claim = Claim {
            endpoint_id: "a".repeat(64),
            helper_instance_id: "ins_test".into(),
            build_id: "b1".into(),
            ready: None,
            watch: None,
        };
        // This test is about `ready` alone: the match's claims ask nothing
        // about watching.
        play_state(&shared)
            .unwrap()
            .plain_claims
            .insert("emt_x".into());
        let sent = std::sync::Mutex::new(Vec::new());
        let send = |fails: bool| {
            let sent = &sent;
            move |command: Value| async move {
                sent.lock().unwrap().push(command.get("ready").cloned());
                if fails {
                    Err(Failure::new("bridge_unavailable"))
                } else {
                    Ok(json!({}))
                }
            }
        };
        let claimed = |ready: bool, fails: bool| {
            send_claim(&shared, &bridge, "emt_x", claim.clone(), ready, send(fails))
        };
        assert!(claimed(true, false).await.is_ok());
        assert!(claimed(true, true).await.is_err());
        assert_eq!(count(), 4);
        set(1);
        assert!(claimed(true, false).await.is_ok());
        assert_eq!(count(), 5);
        // A claim that is not Ready asks nothing, and its failure forgets nothing.
        set(2);
        assert!(claimed(true, false).await.is_ok());
        assert!(claimed(false, true).await.is_err());
        set(0);
        assert!(claimed(true, false).await.is_ok());
        assert_eq!(count(), 6);
        assert_eq!(
            *sent.lock().unwrap(),
            vec![
                Some(json!(true)),
                Some(json!(true)),
                None,
                Some(json!(true)),
                None,
                Some(json!(true)),
            ]
        );
        let _ = std::fs::remove_dir_all(dir);
    }

    // Bindings, permits and room tickets all take their key here. A built-in
    // that does not name the key is refused at once; an approved bridge asks
    // its origin, which here refuses the connection.
    #[tokio::test]
    async fn a_builtins_unknown_key_stays_untrusted() {
        let dir = std::env::temp_dir().join(prefixed_id("sf4-net-pinned", random().unwrap()));
        let closed = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let origin = format!("http://{}", closed.local_addr().unwrap());
        drop(closed);
        let x = ember_protocol::SigningIdentity::from_seed(&zeroize::Zeroizing::new([9; 32]))
            .unwrap()
            .public_key()
            .to_b64u();
        let pin = Builtin {
            bridge_id: prefixed_id("brg", [9; 16]),
            origin,
            display_name: "Pinned".into(),
            keys: vec![("k9".into(), x)],
        };
        let shared = shared_pinning(&dir, std::slice::from_ref(&pin));
        let mut bridge = pin.entry();
        assert!(bridge_key(&shared, &bridge, "k9").await.is_ok());
        let refused = bridge_key(&shared, &bridge, "k5").await.unwrap_err();
        assert_eq!(refused.0, "untrusted_signature");
        // The same entry under an ID this build does not pin.
        bridge.bridge_id = prefixed_id("brg", [8; 16]);
        let fetched = bridge_key(&shared, &bridge, "k5").await.unwrap_err();
        assert_eq!(fetched.0, "bridge_unreachable");
        let _ = std::fs::remove_dir_all(dir);
    }

    // The game names each watcher in the room from the binding it applies.
    #[test]
    fn the_game_sees_the_watchers_its_binding_names() {
        let id = |byte: u8| {
            ember_protocol::SigningIdentity::from_seed(&zeroize::Zeroizing::new([byte; 32]))
                .unwrap()
                .ember_id()
                .clone()
        };
        let fighter = |byte: u8| ember_protocol::play::BoundFighter {
            ember_id: id(byte),
            endpoint_id: format!("{byte:02x}").repeat(32),
        };
        let mut binding = Binding {
            version: ember_protocol::play::VERSION,
            bridge_id: prefixed_id("brg", [1; 16]),
            match_id: prefixed_id("emt", [2; 16]),
            assignment_generation: Counter(1),
            binding_revision: Counter(3),
            room_id: "0123456789abcdef0123456789abcdef".into(),
            build_id: "b1".into(),
            games_to_win: 2,
            rules_digest: String::new(),
            roster_digest: String::new(),
            fighters: [fighter(1), fighter(2)],
            issued_at: 100,
            starting_wins: [0, 0],
            watchers: Vec::new(),
        };
        assert_eq!(binding_view(&binding, Some(0))["watchers"], json!([]));
        binding.watchers = vec![ember_protocol::play::BoundWatcher {
            ember_id: id(3),
            endpoint_id: "03".repeat(32),
        }];
        let view = binding_view(&binding, Some(1));
        assert_eq!(
            view["watchers"],
            json!([{ "ember_id": id(3).to_string(), "endpoint_id": "03".repeat(32) }])
        );
        assert_eq!(view["local_slot"], json!(1));
    }

    #[test]
    fn a_claim_says_ready_only_to_a_bridge_that_reads_it() {
        assert!(lists_ready(&capabilities(&[
            "matches.play",
            CLAIM_READY_FEATURE
        ])));
        assert!(!lists_ready(&capabilities(&["matches.play"])));
    }
}
