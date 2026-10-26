//! Tournament play for the native side (spec 14 to 16). The game drives it:
//! it claims an assigned match, hosts or joins the room the claim names,
//! prepares each official game and reports its confirmed result. This module
//! does the parts that need the key and the bridge: proofs, checking the
//! bridge's signed binding and permits against the approved bridge's key, and
//! signing reports into the spool. The game receives checked fields only.
use std::collections::HashMap;
use std::sync::Arc;

use ember_protocol::{
    api::Capabilities,
    challenge::{Action, Method},
    encoding::{Counter, is_prefixed_id, prefixed_id},
    play::{
        Binding, CLAIM_READY_FEATURE, CLAIM_UNCHANGED_FEATURE, Claim, ClaimAnswer, Observation,
        Permit, Prepare, PrepareAnswer, PublishRoom, SignedBinding, SignedPermit, TABLE,
        WATCH_FEATURE,
    },
    report::Outcome as GameOutcome,
    skill::RANKING_FEATURE,
};
use serde_json::{Value, json};

use super::{
    Failure, Outcome, Shared,
    bridges::Approved,
    client::{self, answer, now},
    spool::Entry,
};

/// What this helper run has verified for each match it plays or watches, and
/// what each bridge offered when its capabilities were last read.
#[derive(Default)]
pub struct State {
    bindings: HashMap<String, Held>,
    permits: HashMap<(String, u64), Permit>,
    features: HashMap<String, Features>,
}

/// A binding this run verified, as the bridge signed it, with the key that
/// verified it and its digest, which a claim names (`Claim::held_binding`).
#[derive(Clone)]
struct Held {
    signed: SignedBinding,
    key: ember_protocol::PublicKey,
    digest: String,
}

/// The optional features of a bridge that a claim, a watch request or a room
/// ticket depends on, from its capabilities.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(super) struct Features {
    /// It reads a claim's `ready`.
    pub ready: bool,
    /// It serves the watch routes and reads a claim's `watch`.
    pub watch: bool,
    /// It serves the ranking routes and reads a ticket request's
    /// `developer_badge`.
    pub ranking: bool,
    /// It reads a claim's `held_binding` and may answer it `Unchanged`.
    pub unchanged: bool,
}

impl Features {
    fn of(capabilities: &Capabilities) -> Self {
        let lists = |name: &str| capabilities.features.iter().any(|feature| feature == name);
        Self {
            ready: lists(CLAIM_READY_FEATURE),
            watch: lists(WATCH_FEATURE),
            ranking: lists(RANKING_FEATURE),
            unchanged: lists(CLAIM_UNCHANGED_FEATURE),
        }
    }
}

/// The feature a caller cannot do without, if any.
#[derive(Clone, Copy)]
pub(super) enum Need {
    /// None: whatever the bridge was last seen to offer will do.
    Nothing,
    /// A claim's `ready`.
    Ready,
    /// The watch routes.
    Watch,
}

impl Need {
    fn met_by(self, offered: Features) -> bool {
        match self {
            Self::Nothing => true,
            Self::Ready => offered.ready,
            Self::Watch => offered.watch,
        }
    }
}

/// What the bridge offers, from the one capabilities read this run keeps for
/// it. A bridge that lacked what `need` names is asked again, since it may be
/// upgraded meanwhile, and one that has it is remembered until a claim to it
/// fails (`forget_features`), since it may have been put back to one that
/// refuses the field. A read that fails fails the caller, which the game
/// retries, rather than carrying on without a field the bridge would have
/// read.
pub(super) async fn features(
    shared: &Arc<Shared>,
    bridge: &Approved,
    need: Need,
) -> Result<Features, Failure> {
    let known = play_state(shared)?.features.get(&bridge.bridge_id).copied();
    if let Some(known) = known.filter(|known| need.met_by(*known)) {
        return Ok(known);
    }
    let read = Features::of(
        &shared
            .client
            .capabilities(&bridge.origin)
            .await?
            .capabilities,
    );
    play_state(shared)?
        .features
        .insert(bridge.bridge_id.clone(), read);
    Ok(read)
}

/// Forgets what the bridge offered, so the next claim or watch request reads
/// its capabilities again.
fn forget_features(shared: &Shared, bridge: &Approved) -> Result<(), Failure> {
    play_state(shared)?.features.remove(&bridge.bridge_id);
    Ok(())
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

/// Verifies a binding and that it seats this endpoint as this identity. The
/// signature of the very binding this run holds for the match, byte for
/// byte, is not checked again while the bridge still has the key that
/// verified it; every other check runs each time.
async fn accept_binding(
    shared: &Arc<Shared>,
    bridge: &Approved,
    match_id: &str,
    room_id: &str,
    signed: &SignedBinding,
) -> Result<Value, Failure> {
    let verified = play_state(shared)?
        .bindings
        .get(match_id)
        .filter(|held| held.signed == *signed && bridge.key(&signed.kid) == Some(held.key))
        .map(|held| (held.key, held.digest.clone()));
    let (key, digest) = match verified {
        Some(verified) => verified,
        None => {
            let key = bridge_key(shared, bridge, &signed.kid).await?;
            signed
                .verify(&key, &signed.kid)
                .map_err(|_| Failure::new("untrusted_signature"))?;
            let digest = signed
                .binding
                .digest()
                .map_err(|_| Failure::new("internal"))?;
            (key, digest)
        }
    };
    let binding = &signed.binding;
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
    // An answer can arrive after a newer one (a claim sent before its room
    // was replaced): the newer binding stays the one permits are checked against.
    let order = |binding: &Binding| (binding.assignment_generation.0, binding.binding_revision.0);
    if state
        .bindings
        .get(match_id)
        .is_none_or(|held| order(binding) >= order(&held.signed.binding))
    {
        state.bindings.insert(
            match_id.to_owned(),
            Held {
                signed: signed.clone(),
                key,
                digest,
            },
        );
    }
    Ok(view)
}

/// The binding this run holds for `match_id` from `bridge`, while the bridge
/// still has the key that verified it: the one a claim names, so a bridge
/// with nothing new can answer without it. One whose key the bridge no
/// longer lists is not named, so the bridge sends the binding again.
fn held_binding(
    shared: &Shared,
    bridge: &Approved,
    match_id: &str,
) -> Result<Option<Held>, Failure> {
    Ok(play_state(shared)?
        .bindings
        .get(match_id)
        .filter(|held| {
            held.signed.binding.bridge_id == bridge.bridge_id
                && bridge.key(&held.signed.kid) == Some(held.key)
        })
        .cloned())
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

/// The claim answer for the game, with the binding checked. `held` is the
/// binding the claim named (`Claim::held_binding`), if it named one: an
/// `Unchanged` answer means that binding, and the game reads the same answer
/// as if the bridge had sent it again. Only a claim that named one may be
/// answered so.
async fn present(
    shared: &Arc<Shared>,
    bridge: &Approved,
    match_id: &str,
    answer: ClaimAnswer,
    held: Option<Held>,
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
        ClaimAnswer::Unchanged {
            room_id,
            invitation,
        } => {
            let held = held.ok_or_else(|| Failure::new("bridge_invalid_response"))?;
            let bound = accept_binding(shared, bridge, match_id, &room_id, &held.signed).await?;
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
/// provisioning lease and picks up a newer binding; it names the binding it
/// holds, so a bridge that has nothing newer answers without it.
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
        held_binding: None,
    };
    claim.check().map_err(|_| Failure::new("invalid_request"))?;
    client::with_session(shared, bridge_id, |bridge, token| {
        let claim = claim.clone();
        async move {
            let path = format!("/v1/matches/{match_id}/claims");
            let held = held_binding(shared, &bridge, match_id)?;
            let claim = Claim {
                held_binding: held.as_ref().map(|held| held.digest.clone()),
                ..claim
            };
            let (value, named) = send_claim(shared, &bridge, claim, ready, |command| {
                proven(shared, &bridge, &token, Action::MatchClaim, &path, command)
            })
            .await?;
            let answer: ClaimAnswer = serde_json::from_value(value)
                .map_err(|_| Failure::new("bridge_invalid_response"))?;
            present(shared, &bridge, match_id, answer, held.filter(|_| named)).await
        }
    })
    .await
}

/// Sends `claim` through `send`, carrying what the bridge reads of it
/// (`features`): `ready` when the fighter is Ready, since a claim without it
/// reads not ready on every bridge, and `watch` always, since the bridge names
/// watchers only while both fighters' claims say so. An older bridge refuses a
/// claim carrying a field it does not know, hence the capabilities read. A
/// claim that fails makes the next one read them again.
///
/// Only a Ready claim needs a feature. A claim never needs `watch`: one to a
/// bridge last seen without it goes without, and an upgrade is heard once a
/// claim to the bridge fails, as one does while it restarts. The same holds
/// for `held_binding`, which goes only to a bridge that answers it; the
/// answer comes back with whether the claim carried it.
pub(super) async fn send_claim<F, Sent>(
    shared: &Arc<Shared>,
    bridge: &Approved,
    claim: Claim,
    ready: bool,
    send: F,
) -> Result<(Value, bool), Failure>
where
    F: FnOnce(Value) -> Sent,
    Sent: std::future::Future<Output = Result<Value, Failure>>,
{
    let need = if ready { Need::Ready } else { Need::Nothing };
    let offered = features(shared, bridge, need).await?;
    let claim = Claim {
        ready: (ready && offered.ready).then_some(true),
        watch: offered.watch.then_some(true),
        held_binding: claim.held_binding.filter(|_| offered.unchanged),
        ..claim
    };
    let named = claim.held_binding.is_some();
    let command = serde_json::to_value(&claim).map_err(|_| Failure::new("internal"))?;
    let sent = send(command).await;
    if sent.is_err() {
        forget_features(shared, bridge)?;
    }
    sent.map(|answer| (answer, named))
}

pub(super) fn play_state(shared: &Shared) -> Result<std::sync::MutexGuard<'_, State>, Failure> {
    shared.play.lock().map_err(|_| Failure::new("internal"))
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
            present(shared, &bridge, match_id, answer, None).await
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
        .map(|held| held.signed.binding.clone())
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
    Ok(None)
}

fn random() -> Result<[u8; 16], Failure> {
    let mut bytes = [0; 16];
    getrandom::fill(&mut bytes).map_err(|_| Failure::new("internal"))?;
    Ok(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::tournament::{
        Request,
        builtin::Builtin,
        test_support::{capabilities_json, capabilities_server, shared_pinning},
    };

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

    fn shared_in(dir: &std::path::Path) -> Arc<Shared> {
        shared_pinning(dir, &[])
    }

    // A Ready claim is not sent without its `ready` when the capabilities
    // read fails: the claim fails and the game retries it. A bridge that does
    // not read the field is asked again on the next Ready claim, so one
    // upgraded while the match is played is heard; one that reads it is
    // remembered, until a claim to it fails, since the bridge may have been
    // put back to one that refuses it.
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
        let reads = || async {
            features(&shared, &bridge, Need::Ready)
                .await
                .map(|offered| offered.ready)
        };
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
        // `ready` carries it without asking again; once a claim to it fails,
        // the next asks again and, the bridge put back, sends none.
        let claim = Claim {
            endpoint_id: "a".repeat(64),
            helper_instance_id: "ins_test".into(),
            build_id: "b1".into(),
            ready: None,
            watch: None,
            held_binding: None,
        };
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
            send_claim(&shared, &bridge, claim.clone(), ready, send(fails))
        };
        assert!(claimed(true, false).await.is_ok());
        assert!(claimed(true, true).await.is_err());
        assert_eq!(count(), 4);
        set(1);
        assert!(claimed(true, false).await.is_ok());
        assert_eq!(count(), 5);
        // One that is not Ready asks nothing of a bridge it knows.
        set(2);
        assert!(claimed(true, false).await.is_ok());
        assert!(claimed(false, false).await.is_ok());
        assert_eq!(count(), 6);
        // Its failure forgets, so the next claim asks, and one asking fails
        // when the read does rather than going out without its `ready`.
        assert!(claimed(false, true).await.is_err());
        set(0);
        assert!(claimed(true, false).await.is_err());
        assert_eq!(count(), 7);
        set(2);
        assert!(claimed(true, false).await.is_ok());
        assert_eq!(count(), 8);
        assert_eq!(
            *sent.lock().unwrap(),
            vec![
                Some(json!(true)),
                Some(json!(true)),
                None,
                Some(json!(true)),
                None,
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
    fn a_bridge_offers_what_its_capabilities_list() {
        let ready = Features::of(&capabilities(&["matches.play", CLAIM_READY_FEATURE]));
        assert_eq!(
            ready,
            Features {
                ready: true,
                watch: false,
                ranking: false,
                unchanged: false,
            }
        );
        let watch = Features::of(&capabilities(&["matches.play", WATCH_FEATURE]));
        assert_eq!(
            watch,
            Features {
                ready: false,
                watch: true,
                ranking: false,
                unchanged: false,
            }
        );
        assert!(Features::of(&capabilities(&[RANKING_FEATURE])).ranking);
        assert!(Features::of(&capabilities(&[CLAIM_UNCHANGED_FEATURE])).unchanged);
        assert_eq!(
            Features::of(&capabilities(&["matches.play"])),
            Features::default()
        );
    }

    // A claim names the binding it holds only to a bridge that lists the
    // feature, since an older one refuses the field, and says whether it did.
    // A claim that fails makes the next one read the capabilities again, so
    // a bridge put back to one without the feature is heard.
    #[tokio::test]
    async fn a_claim_names_its_binding_only_to_a_bridge_that_answers_it() {
        let dir = std::env::temp_dir().join(prefixed_id("sf4-net-held", random().unwrap()));
        let shared = shared_in(&dir);
        let mode = Arc::new(std::sync::atomic::AtomicU8::new(3));
        let (origin, _) = capabilities_server(mode.clone()).await;
        let bridge = Approved {
            bridge_id: "brg_x".into(),
            origin,
            display_name: "Bridge".into(),
            approved_at: 0,
            keys: Vec::new(),
        };
        let digest = ember_protocol::encoding::b64u(&[7; 32]);
        let claim = Claim {
            endpoint_id: "a".repeat(64),
            helper_instance_id: "ins_test".into(),
            build_id: "b1".into(),
            ready: None,
            watch: None,
            held_binding: Some(digest.clone()),
        };
        let sent = std::sync::Mutex::new(Vec::new());
        let send = |fails: bool| {
            let sent = &sent;
            move |command: Value| async move {
                sent.lock()
                    .unwrap()
                    .push(command.get("held_binding").cloned());
                if fails {
                    Err(Failure::new("bridge_unavailable"))
                } else {
                    Ok(json!({}))
                }
            }
        };
        let claimed =
            |claim: Claim, fails: bool| send_claim(&shared, &bridge, claim, false, send(fails));
        assert!(claimed(claim.clone(), false).await.unwrap().1);
        let unnamed = Claim {
            held_binding: None,
            ..claim.clone()
        };
        assert!(!claimed(unnamed, false).await.unwrap().1);
        assert!(claimed(claim.clone(), true).await.is_err());
        mode.store(2, std::sync::atomic::Ordering::SeqCst);
        assert!(!claimed(claim, false).await.unwrap().1);
        assert_eq!(
            *sent.lock().unwrap(),
            vec![Some(json!(digest)), None, Some(json!(digest)), None]
        );
        let _ = std::fs::remove_dir_all(dir);
    }

    // The binding a claim named comes back as `Unchanged`, and the game reads
    // the same answer as when the bridge sent the binding. A bridge that
    // answers so to a claim that named none is refused. The very binding
    // this run verified is not verified again while the bridge lists its
    // key; one the bridge's keys no longer cover is neither named nor taken
    // on trust.
    #[tokio::test]
    async fn an_unchanged_answer_is_the_binding_the_claim_named() {
        let dir = std::env::temp_dir().join(prefixed_id("sf4-net-unchanged", random().unwrap()));
        let shared = shared_in(&dir);
        shared
            .identity
            .lock()
            .unwrap()
            .enable(crate::identity::Protection::Passphrase("a test passphrase"))
            .unwrap();
        let me = client::current_id(&shared).unwrap();
        let closed = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let origin = format!("http://{}", closed.local_addr().unwrap());
        drop(closed);
        let signer =
            ember_protocol::SigningIdentity::from_seed(&zeroize::Zeroizing::new([5; 32])).unwrap();
        let bridge = Approved {
            bridge_id: prefixed_id("brg", [1; 16]),
            origin,
            display_name: "Bridge".into(),
            approved_at: 0,
            keys: vec![ember_protocol::api::SigningKey {
                kty: "OKP".into(),
                crv: "Ed25519".into(),
                alg: "EdDSA".into(),
                usage: "sig".into(),
                kid: "k5".into(),
                x: signer.public_key().to_b64u(),
            }],
        };
        let other = ember_protocol::SigningIdentity::from_seed(&zeroize::Zeroizing::new([6; 32]))
            .unwrap()
            .ember_id()
            .clone();
        let match_id = prefixed_id("emt", [2; 16]);
        let room_id = "0123456789abcdef0123456789abcdef".to_owned();
        let binding = Binding {
            version: ember_protocol::play::VERSION,
            bridge_id: bridge.bridge_id.clone(),
            match_id: match_id.clone(),
            assignment_generation: Counter(1),
            binding_revision: Counter(1),
            room_id: room_id.clone(),
            build_id: "b1".into(),
            games_to_win: 2,
            rules_digest: ember_protocol::encoding::b64u(&[3; 32]),
            roster_digest: ember_protocol::play::roster_digest(&me, &other).unwrap(),
            fighters: [
                ember_protocol::play::BoundFighter {
                    ember_id: me.clone(),
                    endpoint_id: shared.endpoint_id.clone(),
                },
                ember_protocol::play::BoundFighter {
                    ember_id: other,
                    endpoint_id: "b".repeat(64),
                },
            ],
            issued_at: 100,
            starting_wins: [0, 0],
            watchers: Vec::new(),
        };
        let signed = binding.sign(&signer, "k5").unwrap();
        let room = || ClaimAnswer::Room {
            room_id: room_id.clone(),
            invitation: "sf4e3:x".into(),
            binding: Some(Box::new(signed.clone())),
        };
        let unchanged = || ClaimAnswer::Unchanged {
            room_id: room_id.clone(),
            invitation: "sf4e3:x".into(),
        };

        // Nothing held yet: nothing to name, and an unchanged answer to a
        // claim that named nothing is refused.
        assert!(held_binding(&shared, &bridge, &match_id).unwrap().is_none());
        let refused = present(&shared, &bridge, &match_id, unchanged(), None).await;
        assert_eq!(refused.unwrap_err().0, "bridge_invalid_response");

        let sent = present(&shared, &bridge, &match_id, room(), None)
            .await
            .unwrap()
            .unwrap();
        assert_eq!(sent["binding"]["local_slot"], json!(0));
        let held = held_binding(&shared, &bridge, &match_id).unwrap().unwrap();
        assert_eq!(held.digest, binding.digest().unwrap());
        let kept = present(&shared, &bridge, &match_id, unchanged(), Some(held.clone()))
            .await
            .unwrap()
            .unwrap();
        assert_eq!(kept, sent);

        // The held bytes are taken without a second signature check: here
        // they carry a signature that would fail one. Any other bytes are
        // checked and refused.
        let forged = SignedBinding {
            signature: binding
                .sign(
                    &ember_protocol::SigningIdentity::from_seed(&zeroize::Zeroizing::new([9; 32]))
                        .unwrap(),
                    "k5",
                )
                .unwrap()
                .signature,
            ..signed.clone()
        };
        let check = |signed: &SignedBinding| {
            let answer = ClaimAnswer::Room {
                room_id: room_id.clone(),
                invitation: "sf4e3:x".into(),
                binding: Some(Box::new(signed.clone())),
            };
            present(&shared, &bridge, &match_id, answer, None)
        };
        assert_eq!(check(&forged).await.unwrap_err().0, "untrusted_signature");
        play_state(&shared)
            .unwrap()
            .bindings
            .get_mut(&match_id)
            .unwrap()
            .signed = forged.clone();
        assert!(check(&forged).await.is_ok());

        // A bridge whose keys no longer cover the held binding: it is not
        // named, and the same bytes are checked again, which here means
        // asking the bridge for its keys.
        let rotated = Approved {
            keys: Vec::new(),
            ..bridge.clone()
        };
        assert!(
            held_binding(&shared, &rotated, &match_id)
                .unwrap()
                .is_none()
        );
        let answer = ClaimAnswer::Room {
            room_id: room_id.clone(),
            invitation: "sf4e3:x".into(),
            binding: Some(Box::new(forged)),
        };
        let refetched = present(&shared, &rotated, &match_id, answer, None).await;
        assert_eq!(refetched.unwrap_err().0, "bridge_unreachable");

        // Leaving the match drops it.
        forget(&shared, &match_id).unwrap();
        assert!(held_binding(&shared, &bridge, &match_id).unwrap().is_none());
        let _ = std::fs::remove_dir_all(dir);
    }
}
