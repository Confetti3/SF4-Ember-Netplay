//! Tournament play for the native side (spec 14 to 16). The game drives it:
//! it claims an assigned match, hosts or joins the room the claim names,
//! prepares each official game and reports its confirmed result. This module
//! does the parts that need the key and the bridge: proofs, checking the
//! bridge's signed binding and permits against the approved bridge's key, and
//! signing reports into the spool. The game receives checked fields only.
use std::collections::HashMap;
use std::sync::Arc;

use ember_protocol::{
    api::{Capabilities, ErrorCode},
    challenge::Action,
    encoding::{Counter, is_prefixed_id, prefixed_id},
    play::{
        Binding, CLAIM_READY_FEATURE, CLAIM_UNCHANGED_FEATURE, Claim, ClaimAnswer, Observation,
        Permit, Prepare, PrepareAnswer, PublishRoom, SignedBinding, SignedPermit, TABLE,
        WATCH_FEATURE,
    },
    friends::FRIENDS_FEATURE,
    levels::{POINTS_FEATURE, PROFILES_FEATURE},
    report::Outcome as GameOutcome,
    skill::RANKING_FEATURE,
};
use serde::de::DeserializeOwned;
use serde_json::{Value, json};

use super::{
    Code, Failure, Locked, Outcome, Refusal, Shared,
    bridges::Approved,
    client::{self, Body, BridgeCall, Verb, now},
    spool::Entry,
};

/// What this helper run has verified for each match it plays or watches, and
/// what each bridge offered when its capabilities were last read.
#[derive(Default)]
pub struct State {
    bindings: HashMap<String, Held>,
    permits: HashMap<(String, u64), Permit>,
    features: HashMap<String, Features>,
    /// The matches the game left, each with the submission order of its
    /// latest `match_leave`. A claim submitted before that leave but run
    /// after it is dropped, so it cannot hold the match again.
    left: HashMap<String, u64>,
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
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(super) struct Features {
    /// It reads a claim's `ready`.
    pub ready: bool,
    /// It serves the watch routes and reads a claim's `watch`.
    pub watch: bool,
    /// It serves the ranking routes and reads a ticket request's
    /// `developer_badge`.
    pub ranking: bool,
    /// It serves the level routes and reads a ticket request's `card`.
    pub levels: bool,
    /// It serves the friends routes.
    pub friends: bool,
    /// It serves the card save and search routes, reads the look-up's
    /// `request` and `code` selectors and the archive's `name`.
    pub profiles: bool,
    /// It reads a claim's `held_binding` and may answer it `Unchanged`.
    pub unchanged: bool,
    /// Where its public replay archive lists its games, when it says
    /// (`Capabilities::archive`).
    pub archive: Option<String>,
}

impl Features {
    fn of(capabilities: &Capabilities) -> Self {
        let lists = |name: &str| capabilities.features.iter().any(|feature| feature == name);
        Self {
            ready: lists(CLAIM_READY_FEATURE),
            watch: lists(WATCH_FEATURE),
            ranking: lists(RANKING_FEATURE),
            levels: lists(POINTS_FEATURE),
            friends: lists(FRIENDS_FEATURE),
            profiles: lists(PROFILES_FEATURE),
            unchanged: lists(CLAIM_UNCHANGED_FEATURE),
            archive: capabilities.archive.clone(),
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
    /// The level routes.
    Levels,
    /// The friends routes.
    Friends,
    /// The profile routes and selectors.
    Profiles,
}

impl Need {
    fn met_by(self, offered: &Features) -> bool {
        match self {
            Self::Nothing => true,
            Self::Ready => offered.ready,
            Self::Watch => offered.watch,
            Self::Levels => offered.levels,
            Self::Friends => offered.friends,
            Self::Profiles => offered.profiles,
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
    let known = play_state(shared)?.features.get(&bridge.bridge_id).cloned();
    if let Some(known) = known.filter(|known| need.met_by(known)) {
        return Ok(known);
    }
    let read = Features::of(&shared.client.capabilities(&bridge.origin).await?);
    play_state(shared)?
        .features
        .insert(bridge.bridge_id.clone(), read.clone());
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
        Err(Failure::new(Code::InvalidRequest))
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
        return Err(Failure::new(Code::UntrustedSignature));
    }
    let keys = shared.client.signing_keys(&bridge.origin).await?;
    let store = shared.clone();
    let id = bridge.bridge_id.clone();
    let refreshed = tokio::task::spawn_blocking(move || {
        let mut bridges = store.bridges.locked()?;
        bridges.set_keys(&id, keys)?;
        bridges
            .get(&id)
            .ok_or_else(|| Failure::new(Code::BridgeNotApproved))
    })
    .await
    .map_err(|_| Failure::new(Code::Internal))??;
    refreshed
        .key(kid)
        .ok_or_else(|| Failure::new(Code::UntrustedSignature))
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
                .map_err(|_| Failure::new(Code::UntrustedSignature))?;
            let digest = signed
                .binding
                .digest()
                .map_err(|_| Failure::new(Code::Internal))?;
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
        return Err(Failure::new(Code::BindingMismatch));
    }
    let view = binding_view(binding, slot);
    let mut state = shared.play.locked()?;
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
            let held = held.ok_or_else(|| Failure::new(Code::BridgeInvalidResponse))?;
            let bound = accept_binding(shared, bridge, match_id, &room_id, &held.signed).await?;
            json!({ "role": "room", "room_id": room_id, "invitation": invitation, "binding": bound })
        }
        other => serde_json::to_value(other).map_err(|_| Failure::new(Code::Internal))?,
    };
    Ok(Some(value))
}

/// Sends one proven player operation and reads its answer as `T`.
pub(super) async fn proven<T: DeserializeOwned>(
    shared: &Arc<Shared>,
    bridge: &Approved,
    token: &str,
    action: Action,
    path: &str,
    command: &Value,
) -> Result<T, Failure> {
    let call = BridgeCall::new(Verb::Post, path, &[200]).body(Body::Proven { action, command });
    client::fetch_as(shared, bridge, Some(token), &call).await
}

pub async fn assignments(shared: &Arc<Shared>, bridge_id: &str) -> Outcome {
    let call = BridgeCall::new(Verb::Get, "/v1/assignments", &[200]);
    client::request(shared, bridge_id, &call).await.map(Some)
}

/// Claims `match_id` with this helper run's endpoint, saying whether the
/// fighter has pressed Ready at its table. Claiming again renews a
/// provisioning lease and picks up a newer binding; it names the binding it
/// holds, so a bridge that has nothing newer answers without it.
///
/// `order` is when the claim was submitted. Bridge requests wait for an HTTP
/// slot while a `match_leave` runs at once, so a claim submitted before the
/// game left the match can reach here after it: that claim fails
/// `match_left` without reaching the bridge, rather than renewing the lease
/// and restoring the binding of a match the game has left. A claim
/// submitted after the leave runs as any other.
pub async fn claim_match(
    shared: &Arc<Shared>,
    bridge_id: &str,
    match_id: &str,
    build: &str,
    ready: bool,
    order: u64,
) -> Outcome {
    check_match_id(match_id)?;
    {
        let mut state = play_state(shared)?;
        match state.left.get(match_id) {
            Some(&left) if left > order => return Err(Failure::new(Code::MatchLeft)),
            // The lane starts its claims in the order they came, so every
            // claim submitted before the leave has started by now.
            Some(_) => {
                state.left.remove(match_id);
            }
            None => {}
        }
    }
    let claim = Claim {
        endpoint_id: shared.endpoint_id.clone(),
        helper_instance_id: shared.instance_id.clone(),
        build_id: build.to_owned(),
        ready: None,
        watch: None,
        held_binding: None,
    };
    claim
        .check()
        .map_err(|_| Failure::new(Code::InvalidRequest))?;
    let claim = &claim;
    client::with_session(shared, bridge_id, |bridge, token| async move {
        let path = format!("/v1/matches/{match_id}/claims");
        let held = held_binding(shared, &bridge, match_id)?;
        let claim = Claim {
            held_binding: held.as_ref().map(|held| held.digest.clone()),
            ..claim.clone()
        };
        let (path, sent_to, sent_with) = (&path, &bridge, &token);
        let (value, named) = send_claim(shared, &bridge, claim, ready, |command| async move {
            proven::<Value>(
                shared,
                sent_to,
                sent_with,
                Action::MatchClaim,
                path,
                &command,
            )
            .await
        })
        .await?;
        let answer: ClaimAnswer = serde_json::from_value(value)
            .map_err(|_| Failure::new(Code::BridgeInvalidResponse))?;
        present(shared, &bridge, match_id, answer, held.filter(|_| named)).await
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
    let command = serde_json::to_value(&claim).map_err(|_| Failure::new(Code::Internal))?;
    let sent = send(command).await;
    if sent.is_err() {
        forget_features(shared, bridge)?;
    }
    sent.map(|answer| (answer, named))
}

pub(super) fn play_state(shared: &Shared) -> Result<std::sync::MutexGuard<'_, State>, Failure> {
    shared.play.locked()
}

/// Publishes the room this game hosts for `match_id`.
pub async fn publish_room(
    shared: &Arc<Shared>,
    bridge_id: &str,
    match_id: &str,
    room: PublishRoom,
) -> Outcome {
    check_match_id(match_id)?;
    room.check()
        .map_err(|_| Failure::new(Code::InvalidRequest))?;
    let command = serde_json::to_value(&room).map_err(|_| Failure::new(Code::Internal))?;
    let command = &command;
    client::with_session(shared, bridge_id, |bridge, token| async move {
        let path = format!("/v1/matches/{match_id}/room");
        let answer: ClaimAnswer =
            proven(shared, &bridge, &token, Action::RoomPublish, &path, command).await?;
        present(shared, &bridge, match_id, answer, None).await
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
        .locked()?
        .bindings
        .get(match_id)
        .map(|held| held.signed.binding.clone())
        .ok_or_else(|| Failure::new(Code::NotBound))?;
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
        .map_err(|_| Failure::new(Code::InvalidRequest))?;
    let command = serde_json::to_value(&prepare).map_err(|_| Failure::new(Code::Internal))?;
    let (command, binding, prepare) = (&command, &binding, &prepare);
    client::with_session(shared, bridge_id, |bridge, token| async move {
        let path = format!("/v1/matches/{match_id}/attempts/prepare");
        let answer: PrepareAnswer = proven(
            shared,
            &bridge,
            &token,
            Action::AttemptPrepare,
            &path,
            command,
        )
        .await?;
        match answer {
            PrepareAnswer::Pending { retry_after } => Ok(Some(
                json!({ "state": "pending", "retry_after": retry_after }),
            )),
            PrepareAnswer::Permitted { permit } => {
                let permit = accept_permit(shared, &bridge, binding, prepare, &permit).await?;
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
        .map_err(|_| Failure::new(Code::UntrustedSignature))?;
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
        return Err(Failure::new(Code::PermitMismatch));
    }
    let mut state = shared.play.locked()?;
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
        .locked()?
        .permits
        .get(&(match_id.to_owned(), match_generation.0))
        .cloned()
        .ok_or_else(|| Failure::new(Code::UnknownPermit))?;
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
            .map_err(|_| Failure::new(Code::IdentityBusy))?;
        let signer = identity
            .signer()
            .ok_or_else(|| Failure::new(Code::IdentityUnavailable))?;
        permit
            .report(signer.ember_id(), observation)
            .and_then(|report| report.sign(signer))
            .map_err(|_| Failure::new(Code::InvalidRequest))?
    };
    let entry = Entry::new(bridge_id, match_id, signed, now());
    let store = shared.clone();
    let queued = entry.clone();
    tokio::task::spawn_blocking(move || store.spool.put(&queued))
        .await
        .map_err(|_| Failure::new(Code::ReportNotSaved))??;
    let delivered = deliver_one(shared, &entry).await;
    Ok(Some(json!({
        "saved": true,
        "delivered": delivered.is_some(),
        "attempt_state": delivered.as_ref().and_then(|receipt| receipt.get("attempt_state")).cloned(),
        "match_state": delivered.as_ref().and_then(|receipt| receipt.get("match_state")).cloned(),
    })))
}

/// Failures after which sending the same report again cannot succeed: the
/// bridge's refusals of the report itself, and the helper's own when the
/// report is invalid or its bridge is no longer approved.
fn settled(failure: &Failure) -> bool {
    match failure {
        Failure::Bridge(Refusal::Error(code)) => matches!(
            code,
            ErrorCode::StaleRevision
                | ErrorCode::IdempotencyConflict
                | ErrorCode::InvalidSignature
                | ErrorCode::InvalidRequest
                | ErrorCode::NotFound
                | ErrorCode::Forbidden
        ),
        Failure::Bridge(_) => false,
        Failure::Local(code) => matches!(
            code,
            Code::InvalidRequest | Code::NotFound | Code::BridgeNotApproved
        ),
        Failure::Identity(_) => false,
    }
}

/// How sending one spooled report ended.
enum Delivery {
    /// The bridge has it, with this receipt.
    Delivered(Value),
    /// The bridge refused it for good, or its bridge is gone: dropped.
    Settled,
    /// Kept for a later try, for this failure.
    Kept(Failure),
}

/// Sends one spooled report. Removes it once the bridge has answered for it
/// either way; keeps it when the answer may change on a later try.
async fn deliver(shared: &Arc<Shared>, entry: &Entry) -> Option<Delivery> {
    let body = serde_json::to_vec(&entry.report).ok()?;
    let path = format!("/v1/matches/{}/reports", entry.match_id);
    let call = BridgeCall::new(Verb::Post, &path, &[200, 201]).body(Body::Json(&body));
    Some(
        match client::request(shared, &entry.bridge_id, &call).await {
            Ok(receipt) => {
                shared.spool.done(entry);
                Delivery::Delivered(receipt)
            }
            Err(failure) if settled(&failure) => {
                shared.spool.done(entry);
                Delivery::Settled
            }
            Err(failure) => Delivery::Kept(failure),
        },
    )
}

/// The receipt, when the bridge has the report.
async fn deliver_one(shared: &Arc<Shared>, entry: &Entry) -> Option<Value> {
    match deliver(shared, entry).await {
        Some(Delivery::Delivered(receipt)) => Some(receipt),
        _ => None,
    }
}

/// Whether a failure says the bridge cannot be reached or is failing, so that
/// sending it another report now would fail the same way.
fn unreachable_bridge(failure: &Failure) -> bool {
    match failure {
        Failure::Local(code) => {
            matches!(code, Code::BridgeUnreachable | Code::BridgeResponseTooLarge)
        }
        Failure::Bridge(refusal) => refusal.is_server_failure(),
        Failure::Identity(_) => false,
    }
}

/// One pass over the spool, oldest report first. A bridge that fails to
/// answer is not sent its other reports in this pass; they wait for the next,
/// still oldest first.
pub async fn deliver_pending(shared: &Arc<Shared>) {
    let store = shared.clone();
    let pending = tokio::task::spawn_blocking(move || store.spool.pending())
        .await
        .unwrap_or_default();
    let mut failing: Vec<String> = Vec::new();
    for entry in pending {
        if failing.contains(&entry.bridge_id) {
            continue;
        }
        if let Some(Delivery::Kept(failure)) = deliver(shared, &entry).await
            && unreachable_bridge(&failure)
        {
            failing.push(entry.bridge_id.clone());
        }
    }
}

/// Drops what this run verified for a match, once the game leaves it, and
/// remembers that the leave was submitted at `order`, so a claim submitted
/// before it cannot hold the match again (`claim_match`).
pub fn forget(shared: &Shared, match_id: &str, order: u64) -> Outcome {
    let mut state = shared.play.locked()?;
    state.bindings.remove(match_id);
    state.permits.retain(|(id, _), _| id != match_id);
    let left = state.left.entry(match_id.to_owned()).or_insert(order);
    *left = (*left).max(order);
    Ok(None)
}

fn random() -> Result<[u8; 16], Failure> {
    let mut bytes = [0; 16];
    getrandom::fill(&mut bytes).map_err(|_| Failure::new(Code::Internal))?;
    Ok(bytes)
}

#[cfg(test)]
mod tests;
