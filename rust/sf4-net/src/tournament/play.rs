//! Tournament play for the native side (spec 14 to 16). The game drives it:
//! it claims an assigned match, hosts or joins the room the claim names,
//! prepares each official game and reports its confirmed result. This module
//! does the parts that need the key and the bridge: proofs, checking the
//! bridge's signed binding and permits against the approved bridge's key, and
//! signing reports into the spool. The game receives checked fields only.
use std::collections::HashMap;
use std::sync::Arc;

use ember_protocol::{
    challenge::{Action, Method},
    encoding::{Counter, is_prefixed_id, prefixed_id},
    play::{
        Binding, Claim, ClaimAnswer, HandoffRedeemed, Observation, Permit, Prepare, PrepareAnswer,
        PublishRoom, RedeemHandoff, SignedBinding, SignedPermit, TABLE,
    },
    report::Outcome as GameOutcome,
};
use serde_json::{Value, json};

use super::{
    Failure, Outcome, Shared,
    bridges::Approved,
    client::{self, answer, now},
    spool::Entry,
};

/// What this helper run has verified for each match it plays.
#[derive(Default)]
pub struct State {
    bindings: HashMap<String, Binding>,
    permits: HashMap<(String, u64), Permit>,
}

fn check_match_id(match_id: &str) -> Result<(), Failure> {
    if is_prefixed_id(match_id, "emt") {
        Ok(())
    } else {
        Err(Failure::new("invalid_request"))
    }
}

/// The approved bridge's key `kid`, fetching the bridge's current keys from
/// its approved origin once if this one is not known yet.
async fn bridge_key(
    shared: &Arc<Shared>,
    bridge: &Approved,
    kid: &str,
) -> Result<ember_protocol::PublicKey, Failure> {
    if let Some(key) = bridge.key(kid) {
        return Ok(key);
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
    let view = json!({
        "match_id": binding.match_id,
        "assignment_generation": binding.assignment_generation,
        "binding_revision": binding.binding_revision,
        "room_id": binding.room_id,
        "build_id": binding.build_id,
        "games_to_win": binding.games_to_win,
        "rules_digest": binding.rules_digest,
        "roster_digest": binding.roster_digest,
        "fighters": binding.fighters,
        "local_slot": slot,
    });
    let mut state = shared.play.lock().map_err(|_| Failure::new("internal"))?;
    state.bindings.insert(match_id.to_owned(), binding.clone());
    Ok(view)
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
    let (status, response) = shared
        .client
        .call(
            reqwest::Method::POST,
            &format!("{}{path}", bridge.origin),
            Some(token),
            Some(body),
        )
        .await?;
    answer(status, &response, &[200])
}

/// Redeems a browser handoff for the match it names. Only this player's
/// identity can, once, within its minute; the answer is just the match ID.
pub async fn redeem_handoff(shared: &Arc<Shared>, bridge_id: &str, handoff: &str) -> Outcome {
    let command = RedeemHandoff {
        handoff: handoff.to_owned(),
    };
    command
        .check()
        .map_err(|_| Failure::new("invalid_request"))?;
    let value = serde_json::to_value(&command).map_err(|_| Failure::new("internal"))?;
    client::with_session(shared, bridge_id, |bridge, token| {
        let value = value.clone();
        async move {
            let answer = proven(
                shared,
                &bridge,
                &token,
                Action::HandoffRedeem,
                "/v1/handoffs/redeem",
                value,
            )
            .await?;
            let redeemed: HandoffRedeemed = serde_json::from_value(answer)
                .map_err(|_| Failure::new("bridge_invalid_response"))?;
            if !is_prefixed_id(&redeemed.match_id, "emt") {
                return Err(Failure::new("bridge_invalid_response"));
            }
            Ok(Some(json!({ "match_id": redeemed.match_id })))
        }
    })
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

/// Claims `match_id` with this helper run's endpoint. Claiming again renews
/// a provisioning lease and picks up a newer binding.
pub async fn claim_match(
    shared: &Arc<Shared>,
    bridge_id: &str,
    match_id: &str,
    build: &str,
) -> Outcome {
    check_match_id(match_id)?;
    let claim = Claim {
        endpoint_id: shared.endpoint_id.clone(),
        helper_instance_id: shared.instance_id.clone(),
        build_id: build.to_owned(),
    };
    claim.check().map_err(|_| Failure::new("invalid_request"))?;
    let command = serde_json::to_value(&claim).map_err(|_| Failure::new("internal"))?;
    client::with_session(shared, bridge_id, |bridge, token| {
        let command = command.clone();
        async move {
            let path = format!("/v1/matches/{match_id}/claims");
            let value = proven(shared, &bridge, &token, Action::MatchClaim, &path, command).await?;
            let answer: ClaimAnswer = serde_json::from_value(value)
                .map_err(|_| Failure::new("bridge_invalid_response"))?;
            present(shared, &bridge, match_id, answer).await
        }
    })
    .await
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
