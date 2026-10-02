//! Bridge-run play for `ember-room-v1` matches (spec 14, 15): the fighters
//! claim the match, one hosts its room under a provisioning lease, the bridge
//! signs the room binding once both have claimed, and it issues one permit per
//! official game once both fighters describe the same game. Reports and their
//! reconciliation are in `reports`.
//!
//! Every route here is a fighter's own operation: a session for the assigned
//! Ember ID plus a proof over the exact command.
use axum::{
    extract::{Path, State},
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::{
    EmberId,
    api::ErrorCode,
    challenge::{Action, Method},
    encoding::Counter,
    event::Kind,
    matches::{MatchState, Rules},
    play::{
        self, Binding, BoundFighter, Claim, ClaimAnswer, LEASE_SECS, PERMIT_START_SECS, Permit,
        Prepare, PrepareAnswer, PublishRoom, SignedBinding,
    },
};
use rusqlite::{OptionalExtension, Transaction, params};
use serde_json::json;

use crate::{
    AppState,
    auth::{self, Player},
    error::{ApiFailure, Result},
    http::{Body, PROOF_BODY, json as respond},
    routes::{
        ledger::{Match, Participant, bump, load, match_event, participants},
        links::Ctx,
        sessions::{self, Target},
    },
    util::new_id,
};

/// The scope a player session needs for tournament play.
pub const SCOPE: &str = "tournament:participate";
/// How soon a waiting fighter should ask again.
const RETRY_SECS: u64 = 2;

/// A fighter of an `ember-room-v1` match, checked against the current roster.
pub struct Fighter {
    pub found: Match,
    pub roster: Vec<Participant>,
    pub rules: Rules,
}

/// Loads `match_id` for `ember_id`, which must be one of its two fighters.
/// Anyone else gets `not_found`, as for every match read.
pub fn fighter(tx: &Transaction<'_>, match_id: &str, ember_id: &EmberId) -> Result<Fighter> {
    let found = load(tx, match_id)?.ok_or_else(ApiFailure::not_found)?;
    let roster = participants(tx, &found.id, found.generation)?;
    if !roster.iter().any(|p| p.ember_id == *ember_id) {
        return Err(ApiFailure::not_found());
    }
    let text: String = tx.query_row(
        "SELECT rules FROM matches WHERE id = ?1",
        [&found.id],
        |row| row.get(0),
    )?;
    let rules: Rules = serde_json::from_str(&text).map_err(|_| ApiFailure::unavailable())?;
    if rules.native_rules_profile != play::PROFILE {
        return Err(ApiFailure::new(
            ErrorCode::UnsupportedRules,
            "This match's results are entered by an organizer, not played through Ember.",
        ));
    }
    Ok(Fighter {
        found,
        roster,
        rules,
    })
}

fn finished(found: &Match) -> Result<()> {
    if found.state.is_terminal() {
        return Err(
            ApiFailure::new(ErrorCode::StaleRevision, "The match is finished.")
                .detail("state", found.state.as_str()),
        );
    }
    Ok(())
}

/// A session that may play: `self:read` alone is not enough.
async fn playing(state: &AppState, headers: &HeaderMap) -> Result<Player> {
    let player = auth::player(state, headers).await?;
    if !player.scopes.iter().any(|scope| scope == SCOPE) {
        return Err(ApiFailure::forbidden());
    }
    Ok(player)
}

struct ClaimRow {
    ember_id: EmberId,
    endpoint_id: String,
    build_id: String,
}

fn claims(tx: &Transaction<'_>, match_id: &str) -> Result<Vec<ClaimRow>> {
    tx.prepare("SELECT ember_id, endpoint_id, build_id FROM match_claims WHERE match_id = ?1")?
        .query_map([match_id], |row| {
            Ok((
                row.get::<_, String>(0)?,
                row.get::<_, String>(1)?,
                row.get::<_, String>(2)?,
            ))
        })?
        .map(|row| {
            let (ember_id, endpoint_id, build_id) = row?;
            Ok(ClaimRow {
                ember_id: EmberId::parse(&ember_id).map_err(|_| ApiFailure::unavailable())?,
                endpoint_id,
                build_id,
            })
        })
        .collect()
}

#[derive(Default)]
struct Room {
    lease_id: Option<String>,
    lease_holder: Option<String>,
    lease_fence: u64,
    lease_expires_at: Option<u64>,
    room_id: Option<String>,
    invitation_sealed: Option<Vec<u8>>,
    binding_revision: u64,
    bound_endpoints: Option<String>,
}

fn room(tx: &Transaction<'_>, match_id: &str) -> Result<Room> {
    tx.execute(
        "INSERT OR IGNORE INTO match_rooms (match_id) VALUES (?1)",
        [match_id],
    )?;
    Ok(tx.query_row(
        "SELECT lease_id, lease_holder, lease_fence, lease_expires_at, room_id, invitation_sealed,
                binding_revision, bound_endpoints
         FROM match_rooms WHERE match_id = ?1",
        [match_id],
        |row| {
            Ok(Room {
                lease_id: row.get(0)?,
                lease_holder: row.get(1)?,
                lease_fence: row.get(2)?,
                lease_expires_at: row.get(3)?,
                room_id: row.get(4)?,
                invitation_sealed: row.get(5)?,
                binding_revision: row.get(6)?,
                bound_endpoints: row.get(7)?,
            })
        },
    )?)
}

/// The two claimed endpoints by slot, once both fighters have claimed.
fn endpoints(fighter: &Fighter, claimed: &[ClaimRow]) -> Option<[String; 2]> {
    let of = |slot: usize| {
        claimed
            .iter()
            .find(|claim| claim.ember_id == fighter.roster[slot].ember_id)
            .map(|claim| claim.endpoint_id.clone())
    };
    Some([of(0)?, of(1)?])
}

/// Moves the binding revision when the published room's bound endpoints
/// change, and marks the match ready the first time both are bound.
fn rebind(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    fighter: &mut Fighter,
    claimed: &[ClaimRow],
) -> Result<()> {
    let current = room(tx, &fighter.found.id)?;
    let (Some(_), Some(bound)) = (&current.room_id, endpoints(fighter, claimed)) else {
        return Ok(());
    };
    let text = serde_json::to_string(&bound).unwrap_or_default();
    if current.bound_endpoints.as_deref() == Some(text.as_str()) {
        return Ok(());
    }
    tx.execute(
        "UPDATE match_rooms SET binding_revision = binding_revision + 1, bound_endpoints = ?1 WHERE match_id = ?2",
        params![text, fighter.found.id],
    )?;
    if matches!(
        fighter.found.state,
        MatchState::AwaitingPlayers | MatchState::Provisioning
    ) {
        let revision = bump(tx, &fighter.found, MatchState::Ready, ctx.now)?;
        match_event(
            tx,
            ctx,
            &fighter.found,
            Kind::RoomReady,
            json!({ "match_id": fighter.found.id, "match_revision": revision.to_string(), "state": MatchState::Ready }),
        )?;
        fighter.found = load(tx, &fighter.found.id)?.ok_or_else(ApiFailure::unavailable)?;
    }
    Ok(())
}

/// The signed binding of the current room, when both fighters have claimed.
fn binding(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    fighter: &Fighter,
    claimed: &[ClaimRow],
) -> Result<Option<SignedBinding>> {
    let current = room(tx, &fighter.found.id)?;
    let (Some(room_id), Some(bound)) = (current.room_id, endpoints(fighter, claimed)) else {
        return Ok(None);
    };
    let [p1, p2] = [&fighter.roster[0].ember_id, &fighter.roster[1].ember_id];
    let binding = Binding {
        version: play::VERSION,
        bridge_id: ctx.config.bridge_id.clone(),
        match_id: fighter.found.id.clone(),
        assignment_generation: Counter(fighter.found.generation),
        binding_revision: Counter(current.binding_revision),
        room_id,
        build_id: claimed[0].build_id.clone(),
        games_to_win: fighter.found.games_to_win,
        rules_digest: play::rules_digest(&fighter.rules)?,
        roster_digest: play::roster_digest(p1, p2)?,
        fighters: [
            BoundFighter {
                ember_id: p1.clone(),
                endpoint_id: bound[0].clone(),
            },
            BoundFighter {
                ember_id: p2.clone(),
                endpoint_id: bound[1].clone(),
            },
        ],
        issued_at: ctx.now,
    };
    Ok(Some(
        ctx.keys
            .sign_binding(&binding)
            .map_err(|_| ApiFailure::unavailable())?,
    ))
}

fn room_answer(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    fighter: &Fighter,
    claimed: &[ClaimRow],
) -> Result<ClaimAnswer> {
    let current = room(tx, &fighter.found.id)?;
    let (Some(room_id), Some(sealed)) = (current.room_id, current.invitation_sealed) else {
        return Err(ApiFailure::unavailable());
    };
    let invitation = ctx.keys.open(&sealed).ok_or_else(ApiFailure::unavailable)?;
    Ok(ClaimAnswer::Room {
        room_id,
        invitation: String::from_utf8(invitation.to_vec())
            .map_err(|_| ApiFailure::unavailable())?,
        binding: binding(tx, ctx, fighter, claimed)?.map(Box::new),
    })
}

/// `POST /v1/matches/{id}/claims`: a `match.claim` proof.
pub async fn claim_route(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let player = playing(&state, &headers).await?;
    let (request, command): (_, Claim) = sessions::proven(&body.0)?;
    command.check()?;
    let ctx = Ctx::of(&state);
    let path = format!("/v1/matches/{id}/claims");
    let answer = state
        .db
        .write(move |tx| {
            let (ember_id, _) = sessions::consume_proof(
                tx,
                &ctx.config,
                ctx.now,
                &request,
                Target {
                    action: Action::MatchClaim,
                    method: Method::Post,
                    path: &path,
                },
                Some(&player.ember_id),
            )?;
            claim(tx, &ctx, &id, &ember_id, &command)
        })
        .await?;
    state.committed();
    Ok(respond(StatusCode::OK, &answer))
}

fn claim(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    match_id: &str,
    ember_id: &EmberId,
    command: &Claim,
) -> Result<ClaimAnswer> {
    let mut fighter = fighter(tx, match_id, ember_id)?;
    finished(&fighter.found)?;
    let other = claims(tx, match_id)?
        .into_iter()
        .find(|claim| claim.ember_id != *ember_id);
    // A room admits one build, so both fighters must run the same one.
    if other
        .as_ref()
        .is_some_and(|other| other.build_id != command.build_id)
    {
        return Err(ApiFailure::new(
            ErrorCode::IncompatibleBuild,
            "The two players run different Ember builds. Both need the same version to play this match.",
        ));
    }
    tx.execute(
        "INSERT INTO match_claims (match_id, ember_id, endpoint_id, helper_instance_id, build_id, claimed_at)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6)
         ON CONFLICT (match_id, ember_id) DO UPDATE SET endpoint_id = ?3, helper_instance_id = ?4, build_id = ?5, claimed_at = ?6",
        params![match_id, ember_id.as_str(), command.endpoint_id, command.helper_instance_id, command.build_id, ctx.now],
    )?;
    let claimed = claims(tx, match_id)?;
    let current = room(tx, match_id)?;
    if current.room_id.is_some() {
        rebind(tx, ctx, &mut fighter, &claimed)?;
        return room_answer(tx, ctx, &fighter, &claimed);
    }
    let live = current.lease_expires_at.is_some_and(|at| at > ctx.now);
    let mine = current.lease_holder.as_deref() == Some(ember_id.as_str());
    if live && !mine {
        let left = current
            .lease_expires_at
            .unwrap_or(ctx.now)
            .saturating_sub(ctx.now);
        return Ok(ClaimAnswer::Wait {
            retry_after: left.clamp(1, RETRY_SECS),
        });
    }
    let expires_at = ctx.now + LEASE_SECS;
    let (lease_id, fence) = match (live && mine, current.lease_id) {
        // Claiming again renews the lease it holds.
        (true, Some(lease_id)) => (lease_id, current.lease_fence),
        _ => (new_id("lse"), current.lease_fence + 1),
    };
    tx.execute(
        "UPDATE match_rooms SET lease_id = ?1, lease_holder = ?2, lease_fence = ?3, lease_expires_at = ?4 WHERE match_id = ?5",
        params![lease_id, ember_id.as_str(), fence, expires_at, match_id],
    )?;
    if fighter.found.state == MatchState::AwaitingPlayers {
        let revision = bump(tx, &fighter.found, MatchState::Provisioning, ctx.now)?;
        match_event(
            tx,
            ctx,
            &fighter.found,
            Kind::PlayerPresent,
            json!({ "match_id": match_id, "match_revision": revision.to_string(), "state": MatchState::Provisioning, "ember_id": ember_id }),
        )?;
    }
    Ok(ClaimAnswer::Host {
        lease_id,
        fence: Counter(fence),
        expires_at,
    })
}

/// `POST /v1/matches/{id}/room`: a `room.publish` proof.
pub async fn publish_route(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let player = playing(&state, &headers).await?;
    let (request, command): (_, PublishRoom) = sessions::proven(&body.0)?;
    command.check()?;
    let ctx = Ctx::of(&state);
    let path = format!("/v1/matches/{id}/room");
    let answer = state
        .db
        .write(move |tx| {
            let (ember_id, _) = sessions::consume_proof(
                tx,
                &ctx.config,
                ctx.now,
                &request,
                Target {
                    action: Action::RoomPublish,
                    method: Method::Post,
                    path: &path,
                },
                Some(&player.ember_id),
            )?;
            publish(tx, &ctx, &id, &ember_id, &command)
        })
        .await?;
    state.committed();
    Ok(respond(StatusCode::OK, &answer))
}

fn publish(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    match_id: &str,
    ember_id: &EmberId,
    command: &PublishRoom,
) -> Result<ClaimAnswer> {
    let mut fighter = fighter(tx, match_id, ember_id)?;
    finished(&fighter.found)?;
    let claimed = claims(tx, match_id)?;
    if !claimed.iter().any(|claim| claim.ember_id == *ember_id) {
        return Err(ApiFailure::new(
            ErrorCode::LeaseConflict,
            "Claim the match before publishing its room.",
        ));
    }
    let current = room(tx, match_id)?;
    let sealed = ctx.keys.seal(command.invitation.as_bytes());
    let conflict = |message: &'static str| ApiFailure::new(ErrorCode::LeaseConflict, message);
    match (&command.lease_id, &command.replaces, &current.room_id) {
        (Some(lease_id), None, None) => {
            let holds = current.lease_id.as_deref() == Some(lease_id.as_str())
                && current.lease_holder.as_deref() == Some(ember_id.as_str())
                && command.fence.map(|fence| fence.0) == Some(current.lease_fence)
                && current.lease_expires_at.is_some_and(|at| at > ctx.now);
            if !holds {
                return Err(conflict(
                    "That provisioning lease is not current. Claim the match again.",
                ));
            }
        }
        (Some(_), None, Some(_)) => return Err(conflict("The match already has a room.")),
        (None, None, Some(room_id)) if *room_id == command.room_id => {
            // Refreshing the current room's invitation as it ages.
            tx.execute(
                "UPDATE match_rooms SET invitation_sealed = ?1 WHERE match_id = ?2",
                params![sealed, match_id],
            )?;
            return room_answer(tx, ctx, &fighter, &claimed);
        }
        (None, Some(replaces), Some(room_id)) if replaces == room_id => {
            if open_attempt(tx, match_id)? {
                return Err(conflict(
                    "A game of this match is still being decided. Its room cannot be replaced yet.",
                ));
            }
            replace_room(tx, ctx, &mut fighter)?;
        }
        _ => {
            return Err(conflict(
                "That is not the match's current room. Claim the match again.",
            ));
        }
    }
    tx.execute(
        "UPDATE match_rooms SET room_id = ?1, invitation_sealed = ?2, published_by = ?3, published_at = ?4,
            lease_id = NULL, lease_holder = NULL, lease_expires_at = NULL, bound_endpoints = NULL
         WHERE match_id = ?5",
        params![command.room_id, sealed, ember_id.as_str(), ctx.now, match_id],
    )?;
    rebind(tx, ctx, &mut fighter, &claimed)?;
    room_answer(tx, ctx, &fighter, &claimed)
}

/// A replaced room starts a new assignment generation with the same roster
/// (spec 13.2); accepted games stay in the ledger.
fn replace_room(tx: &Transaction<'_>, ctx: &Ctx, fighter: &mut Fighter) -> Result<()> {
    let next = fighter.found.generation + 1;
    tx.execute(
        "INSERT INTO match_participants (match_id, assignment_generation, slot, participant_id, ember_id)
         SELECT match_id, ?1, slot, participant_id, ember_id FROM match_participants
          WHERE match_id = ?2 AND assignment_generation = ?3",
        params![next, fighter.found.id, fighter.found.generation],
    )?;
    tx.execute(
        "UPDATE matches SET assignment_generation = ?1 WHERE id = ?2",
        params![next, fighter.found.id],
    )?;
    tx.execute(
        "DELETE FROM attempt_preparations WHERE match_id = ?1",
        [&fighter.found.id],
    )?;
    let revision = bump(tx, &fighter.found, fighter.found.state, ctx.now)?;
    match_event(
        tx,
        ctx,
        &fighter.found,
        Kind::AssignmentChanged,
        json!({
            "match_id": fighter.found.id,
            "match_revision": revision.to_string(),
            "assignment_generation": next.to_string(),
            "reason": "room_replaced",
        }),
    )?;
    fighter.found = load(tx, &fighter.found.id)?.ok_or_else(ApiFailure::unavailable)?;
    Ok(())
}

/// Whether the match has a permitted attempt without a result, or one held for review.
pub fn open_attempt(tx: &Transaction<'_>, match_id: &str) -> Result<bool> {
    Ok(tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM attempts WHERE match_id = ?1 AND state IN ('permitted', 'review'))",
        [match_id],
        |row| row.get(0),
    )?)
}

/// `POST /v1/matches/{id}/attempts/prepare`: an `attempt.prepare` proof.
pub async fn prepare_route(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let player = playing(&state, &headers).await?;
    let (request, command): (_, Prepare) = sessions::proven(&body.0)?;
    command.check()?;
    let ctx = Ctx::of(&state);
    let path = format!("/v1/matches/{id}/attempts/prepare");
    let answer = state
        .db
        .write(move |tx| {
            let (ember_id, _) = sessions::consume_proof(
                tx,
                &ctx.config,
                ctx.now,
                &request,
                Target {
                    action: Action::AttemptPrepare,
                    method: Method::Post,
                    path: &path,
                },
                Some(&player.ember_id),
            )?;
            prepare(tx, &ctx, &id, &ember_id, &command)
        })
        .await?;
    state.committed();
    Ok(respond(StatusCode::OK, &answer))
}

fn prepare(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    match_id: &str,
    ember_id: &EmberId,
    command: &Prepare,
) -> Result<PrepareAnswer> {
    let fighter = fighter(tx, match_id, ember_id)?;
    finished(&fighter.found)?;
    if fighter.found.state == MatchState::NeedsReview {
        return Err(ApiFailure::new(
            ErrorCode::LeaseConflict,
            "An organizer is reviewing this match. The next game waits for their decision.",
        )
        .detail("reason", "needs_review"));
    }
    // The open attempt answers both fighters' retries with the same permit.
    let open: Option<(String, u64)> = tx
        .query_row(
            "SELECT permit, match_generation FROM attempts WHERE match_id = ?1 AND state = 'permitted'",
            [match_id],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .optional()?;
    if let Some((permit, generation)) = open {
        if generation != command.match_generation.0 {
            return Err(ApiFailure::new(
                ErrorCode::LeaseConflict,
                "The previous game has no result yet. Report it before preparing another.",
            )
            .detail("reason", "attempt_open"));
        }
        let permit = serde_json::from_str(&permit).map_err(|_| ApiFailure::unavailable())?;
        return Ok(PrepareAnswer::Permitted { permit });
    }
    check_descriptor(tx, &fighter, ember_id, command)?;
    tx.execute(
        "INSERT INTO attempt_preparations (match_id, ember_id, descriptor, prepared_at) VALUES (?1, ?2, ?3, ?4)
         ON CONFLICT (match_id, ember_id) DO UPDATE SET descriptor = ?3, prepared_at = ?4",
        params![match_id, ember_id.as_str(), serde_json::to_string(command).unwrap_or_default(), ctx.now],
    )?;
    let other: Option<String> = tx
        .query_row(
            "SELECT descriptor FROM attempt_preparations WHERE match_id = ?1 AND ember_id != ?2 AND prepared_at > ?3",
            params![match_id, ember_id.as_str(), ctx.now.saturating_sub(PERMIT_START_SECS)],
            |row| row.get(0),
        )
        .optional()?;
    let agreed = other
        .and_then(|text| serde_json::from_str::<Prepare>(&text).ok())
        .is_some_and(|other| other.same_game(command));
    if !agreed {
        return Ok(PrepareAnswer::Pending {
            retry_after: RETRY_SECS,
        });
    }
    let permit = issue(tx, ctx, &fighter, command)?;
    Ok(PrepareAnswer::Permitted {
        permit: Box::new(permit),
    })
}

/// A descriptor must name the match's current binding, room and rules, and
/// a native game newer than any this match has already used.
fn check_descriptor(
    tx: &Transaction<'_>,
    fighter: &Fighter,
    ember_id: &EmberId,
    command: &Prepare,
) -> Result<()> {
    let current = room(tx, &fighter.found.id)?;
    let claimed = claims(tx, &fighter.found.id)?;
    let mine = claimed.iter().find(|claim| claim.ember_id == *ember_id);
    let [p1, p2] = [&fighter.roster[0].ember_id, &fighter.roster[1].ember_id];
    let used: u64 = tx.query_row(
        "SELECT COALESCE(MAX(match_generation), 0) FROM attempts WHERE match_id = ?1 AND assignment_generation = ?2",
        params![fighter.found.id, fighter.found.generation],
        |row| row.get(0),
    )?;
    let current_binding = current.room_id.as_deref() == Some(command.room_id.as_str())
        && current.binding_revision == command.binding_revision.0
        && current.bound_endpoints.is_some()
        && fighter.found.generation == command.assignment_generation.0
        && mine.is_some_and(|claim| {
            claim.endpoint_id == command.endpoint_id && claim.build_id == command.build_id
        })
        && command.rules_digest == play::rules_digest(&fighter.rules)?
        && command.roster_digest == play::roster_digest(p1, p2)?;
    if !current_binding {
        return Err(ApiFailure::new(
            ErrorCode::StaleRevision,
            "That game does not match the match's current room. Claim the match again.",
        )
        .detail("reason", "stale_binding"));
    }
    if command.match_generation.0 <= used {
        return Err(ApiFailure::new(
            ErrorCode::StaleRevision,
            "That game was already used for this match.",
        )
        .detail("reason", "generation_used"));
    }
    Ok(())
}

fn issue(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    fighter: &Fighter,
    command: &Prepare,
) -> Result<ember_protocol::play::SignedPermit> {
    let match_id = &fighter.found.id;
    let seq: u64 = tx.query_row(
        "SELECT COALESCE(MAX(seq), 0) + 1 FROM attempts WHERE match_id = ?1",
        [match_id],
        |row| row.get(0),
    )?;
    let permit = Permit {
        version: play::VERSION,
        bridge_id: ctx.config.bridge_id.clone(),
        match_id: match_id.clone(),
        assignment_generation: command.assignment_generation,
        attempt_id: new_id("ega"),
        permit_id: new_id("per"),
        room_id: command.room_id.clone(),
        table_id: command.table_id,
        match_generation: command.match_generation,
        p1_id: fighter.roster[0].ember_id.clone(),
        p2_id: fighter.roster[1].ember_id.clone(),
        rules_digest: command.rules_digest.clone(),
        roster_digest: command.roster_digest.clone(),
        build_id: command.build_id.clone(),
        issued_at: ctx.now,
        start_by: ctx.now + PERMIT_START_SECS,
    };
    let signed = ctx
        .keys
        .sign_permit(&permit)
        .map_err(|_| ApiFailure::unavailable())?;
    tx.execute(
        "INSERT INTO attempts (id, match_id, assignment_generation, seq, outcome, source, state, created_at,
            permit_id, permit, match_generation, start_by)
         VALUES (?1, ?2, ?3, ?4, NULL, 'player_agreement', 'permitted', ?5, ?6, ?7, ?8, ?9)",
        params![
            permit.attempt_id,
            match_id,
            permit.assignment_generation.0,
            seq,
            ctx.now,
            permit.permit_id,
            serde_json::to_string(&signed).unwrap_or_default(),
            permit.match_generation.0,
            permit.start_by
        ],
    )?;
    tx.execute(
        "DELETE FROM attempt_preparations WHERE match_id = ?1",
        [match_id],
    )?;
    let revision = bump(tx, &fighter.found, MatchState::Running, ctx.now)?;
    match_event(
        tx,
        ctx,
        &fighter.found,
        Kind::AttemptAuthorized,
        json!({
            "match_id": match_id,
            "match_revision": revision.to_string(),
            "attempt_id": permit.attempt_id,
            "seq": seq,
            "state": MatchState::Running,
        }),
    )?;
    Ok(signed)
}
