//! A play match's room as the bridge keeps it: the fighters' claims, the
//! `match_rooms` row (provisioning lease, published room, what the current
//! binding names) and the signed binding built from them. What the routes
//! decide with these is in the parent module; which watchers the binding
//! names is chosen in `watch`.
use ember_protocol::{
    EmberId,
    encoding::Counter,
    play::{self, Binding, BoundFighter, BoundWatcher, ClaimAnswer, SignedBinding},
};
use rusqlite::{
    OptionalExtension, Transaction,
    types::{FromSql, FromSqlError, FromSqlResult, ToSql, ToSqlOutput, ValueRef},
};
use serde::{Serialize, de::DeserializeOwned};

use super::PlayedMatch;
use crate::{
    ctx::Ctx,
    error::{ApiFailure, Result},
};

/// A value kept in a TEXT column as JSON and decoded when the row is read, so
/// a stored value is compared as the value it is, not as the text it was
/// written as.
struct Json<T>(T);

impl<T: DeserializeOwned> FromSql for Json<T> {
    fn column_result(value: ValueRef<'_>) -> FromSqlResult<Self> {
        serde_json::from_str(value.as_str()?)
            .map(Self)
            .map_err(|error| FromSqlError::Other(Box::new(error)))
    }
}

impl<T: Serialize> ToSql for Json<T> {
    fn to_sql(&self) -> rusqlite::Result<ToSqlOutput<'_>> {
        serde_json::to_string(&self.0)
            .map(ToSqlOutput::from)
            .map_err(|error| rusqlite::Error::ToSqlConversionFailure(Box::new(error)))
    }
}

pub(super) struct ClaimRow {
    pub ember_id: EmberId,
    pub endpoint_id: String,
    pub build_id: String,
    /// Whether the fighter's latest claim said its helper reads watchers.
    pub watch: bool,
}

pub(super) fn claims(tx: &Transaction<'_>, match_id: &str) -> Result<Vec<ClaimRow>> {
    tx.prepare(
        "SELECT ember_id, endpoint_id, build_id, watch FROM match_claims WHERE match_id = ?1",
    )?
    .query_map([match_id], |row| {
        Ok((
            row.get::<_, String>(0)?,
            row.get::<_, String>(1)?,
            row.get::<_, String>(2)?,
            row.get::<_, bool>(3)?,
        ))
    })?
    .map(|row| {
        let (ember_id, endpoint_id, build_id, watch) = row?;
        Ok(ClaimRow {
            ember_id: EmberId::parse(&ember_id).map_err(|_| ApiFailure::unavailable())?,
            endpoint_id,
            build_id,
            watch,
        })
    })
    .collect()
}

#[derive(Default)]
pub(super) struct Room {
    pub lease_id: Option<String>,
    pub lease_holder: Option<String>,
    pub lease_fence: u64,
    pub lease_expires_at: Option<u64>,
    pub room_id: Option<String>,
    pub invitation_sealed: Option<Vec<u8>>,
    pub binding_revision: u64,
    pub bound_endpoints: Option<[String; 2]>,
    /// The `binding_revision` of the last fighter-endpoint change: a prepare
    /// is checked between it and `binding_revision`, so watcher changes in
    /// between never fail one.
    pub fighters_revision: u64,
    /// The watchers the binding names. Stored as no value when there are none.
    pub bound_watchers: Vec<BoundWatcher>,
    /// When the current room was published: the binding's `issued_at`.
    pub published_at: Option<u64>,
}

/// The match's room, made if the match has none yet.
pub(super) fn room(tx: &Transaction<'_>, match_id: &str) -> Result<Room> {
    tx.execute(
        "INSERT OR IGNORE INTO match_rooms (match_id) VALUES (?1)",
        [match_id],
    )?;
    peek(tx, match_id)
}

/// The match's room as it is, for a reader that writes nothing: a match
/// nobody has claimed has none, which reads as a room with nothing in it.
pub(super) fn peek(tx: &Transaction<'_>, match_id: &str) -> Result<Room> {
    Ok(tx
        .query_row(
            "SELECT lease_id, lease_holder, lease_fence, lease_expires_at, room_id, invitation_sealed,
                    binding_revision, bound_endpoints, fighters_revision, bound_watchers, published_at
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
                    bound_endpoints: row.get::<_, Option<Json<_>>>(7)?.map(|stored| stored.0),
                    fighters_revision: row.get(8)?,
                    bound_watchers: row
                        .get::<_, Option<Json<_>>>(9)?
                        .map(|stored| stored.0)
                        .unwrap_or_default(),
                    published_at: row.get(10)?,
                })
            },
        )
        .optional()?
        .unwrap_or_default())
}

/// The two claimed endpoints by slot, once both fighters have claimed.
pub(super) fn endpoints(fighter: &PlayedMatch, claimed: &[ClaimRow]) -> Option<[String; 2]> {
    let of = |slot: usize| {
        claimed
            .iter()
            .find(|claim| claim.ember_id == fighter.roster[slot].ember_id)
            .map(|claim| claim.endpoint_id.clone())
    };
    Some([of(0)?, of(1)?])
}

/// Stores what the binding now names: `seated`, the fighters' endpoints once
/// the room is published and both have claimed, and the `watchers`. A change
/// of the fighters' endpoints moves both `binding_revision` and
/// `fighters_revision` (prepares are checked against the latter); a change of
/// watchers alone moves only `binding_revision`. Returns whether the
/// fighters' endpoints changed.
pub(super) fn bind(
    tx: &Transaction<'_>,
    match_id: &str,
    current: &Room,
    seated: Option<&[String; 2]>,
    watchers: &[BoundWatcher],
) -> Result<bool> {
    let fighters_moved =
        seated.is_some_and(|seated| current.bound_endpoints.as_ref() != Some(seated));
    if !fighters_moved && watchers == current.bound_watchers {
        return Ok(false);
    }
    let endpoints = if fighters_moved {
        seated
    } else {
        current.bound_endpoints.as_ref()
    };
    tx.execute(
        "UPDATE match_rooms SET binding_revision = binding_revision + 1,
            fighters_revision = CASE WHEN ?1 THEN binding_revision + 1 ELSE fighters_revision END,
            bound_endpoints = ?2, bound_watchers = ?3 WHERE match_id = ?4",
        rusqlite::params![
            fighters_moved,
            endpoints.map(Json),
            (!watchers.is_empty()).then_some(Json(watchers)),
            match_id
        ],
    )?;
    Ok(fighters_moved)
}

/// A claim's answer once the match has a room: the room, its invitation and
/// the signed binding once both fighters have claimed. When the claim says
/// it holds exactly that binding (`held`, its `held_binding`), the answer is
/// `Unchanged`, without the binding and without signing it again. A claim
/// that names none gets `Room`, as every claim did before.
pub(super) fn claim_answer(
    ctx: &Ctx,
    fighter: &PlayedMatch,
    claimed: &[ClaimRow],
    current: Room,
    held: Option<&str>,
) -> Result<ClaimAnswer> {
    let (room_id, invitation) = invitation(ctx, &current)?;
    let binding = match unsigned(ctx, fighter, claimed, current)? {
        Some(binding) => binding,
        None => {
            return Ok(ClaimAnswer::Room {
                room_id,
                invitation,
                binding: None,
            });
        }
    };
    if let Some(held) = held {
        let digest = binding.digest().map_err(|_| ApiFailure::unavailable())?;
        if digest == held {
            return Ok(ClaimAnswer::Unchanged {
                room_id,
                invitation,
            });
        }
    }
    Ok(ClaimAnswer::Room {
        room_id,
        invitation,
        binding: Some(Box::new(sign(ctx, &binding)?)),
    })
}

fn sign(ctx: &Ctx, binding: &Binding) -> Result<SignedBinding> {
    ctx.keys
        .sign_binding(binding)
        .map_err(|_| ApiFailure::unavailable())
}

/// The binding of `current` when both fighters have claimed, unsigned. Its
/// `issued_at` is when the room was published, not when this answer is
/// made, so the binding stays the same bytes, and its signature the same,
/// until something it names changes (each such change also moves its
/// revision or generation). A helper holding it can then tell a renewal
/// that changes nothing from a new binding. Nothing reads a binding's
/// `issued_at`; a permit carries its own.
fn unsigned(
    ctx: &Ctx,
    fighter: &PlayedMatch,
    claimed: &[ClaimRow],
    current: Room,
) -> Result<Option<Binding>> {
    let (Some(room_id), Some(bound)) = (current.room_id, endpoints(fighter, claimed)) else {
        return Ok(None);
    };
    let [p1, p2] = [&fighter.roster[0].ember_id, &fighter.roster[1].ember_id];
    let starting_wins = fighter.found.starting_wins;
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
        issued_at: current.published_at.unwrap_or(ctx.now),
        starting_wins,
        watchers: current.bound_watchers,
    };
    Ok(Some(binding))
}

/// The published room's id and its current invitation.
pub(super) fn invitation(ctx: &Ctx, room: &Room) -> Result<(String, String)> {
    let (Some(room_id), Some(sealed)) = (&room.room_id, &room.invitation_sealed) else {
        return Err(ApiFailure::unavailable());
    };
    let invitation = ctx.keys.open(sealed).ok_or_else(ApiFailure::unavailable)?;
    let invitation =
        String::from_utf8(invitation.to_vec()).map_err(|_| ApiFailure::unavailable())?;
    Ok((room_id.clone(), invitation))
}
