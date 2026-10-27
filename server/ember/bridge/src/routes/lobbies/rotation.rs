//! The rotation engine: seats filled from the queue, sets started and
//! announced, and the lobby's answer to a set completing, a player leaving,
//! a player coming free and a link ending. The match ledger and the
//! tournament code call it in their own transactions.
use ember_protocol::{
    EmberId,
    encoding::Counter,
    event::{EventScore, EventStreak, Kind, LobbySetCompletedData},
    json,
    matches::{Participant as Assigned, Rules, Score},
};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::Serialize;
use serde_json::json;

use super::{
    MATCH_PREFIX,
    model::{Entry, Lobby, ids, load, load_listed, lobby_event, queue, seated_ids, seats, touch},
};
use crate::{
    ctx::Ctx,
    error::{ApiFailure, Result},
    routes::ledger::{self, NewMatch, ORGANIZER_PROFILE},
    tournament,
};

pub(super) fn set_entry(
    tx: &Transaction<'_>,
    lobby_id: &str,
    ember_id: &EmberId,
    state: &str,
    slot: Option<u8>,
    now: u64,
) -> Result<()> {
    // Joining the queue (again) takes the next place at the back.
    let position: Option<i64> = if state == "queued" {
        Some(tx.query_row(
            "UPDATE lobbies SET next_position = next_position + 1 WHERE id = ?1 RETURNING next_position - 1",
            [lobby_id],
            |row| row.get(0),
        )?)
    } else {
        None
    };
    tx.execute(
        "UPDATE lobby_entries SET state = ?3, slot = ?4, position = ?5, updated_at = ?6
         WHERE lobby_id = ?1 AND ember_id = ?2",
        params![lobby_id, ember_id.as_str(), state, slot, position, now],
    )?;
    Ok(())
}

/// Busy in another match, or due to play a tournament set: a bracket comes
/// before the lobby, so its set can start as soon as both players are free.
fn unavailable(tx: &Transaction<'_>, connection_id: &str, ember_id: &EmberId) -> Result<bool> {
    Ok(ledger::busy(tx, connection_id, ember_id, "")?
        || tournament::waiting(tx, connection_id, ember_id)?)
}

/// The next set when both seats can be filled and none is running. Empty
/// seats take the first queued players who are still linked and have no other
/// active match on the connection; a player busy elsewhere keeps their place.
/// A player whose link ended leaves the queue. Returns the started set, which
/// the caller announces after its own event.
pub(super) fn advance(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    lobby_id: &str,
    dropped: &mut Vec<EmberId>,
) -> Result<Option<SetStarted>> {
    let lobby = load(tx, lobby_id)?.ok_or_else(ApiFailure::unavailable)?;
    if !lobby.open || lobby.current_match_id.is_some() {
        return Ok(None);
    }
    let mut seated = seats(tx, &lobby.id)?;
    for slot in 0..2u8 {
        if seated[usize::from(slot)].is_some() {
            continue;
        }
        for candidate in queue(tx, &lobby.id)? {
            if !ledger::linked(
                tx,
                &lobby.connection_id,
                &candidate.participant_id,
                &candidate.ember_id,
            )? {
                set_entry(tx, &lobby.id, &candidate.ember_id, "left", None, ctx.now)?;
                dropped.push(candidate.ember_id.clone());
                continue;
            }
            if unavailable(tx, &lobby.connection_id, &candidate.ember_id)? {
                continue;
            }
            set_entry(
                tx,
                &lobby.id,
                &candidate.ember_id,
                "seated",
                Some(slot),
                ctx.now,
            )?;
            seated[usize::from(slot)] = Some(Entry {
                slot: Some(slot),
                ..candidate
            });
            break;
        }
    }
    let [Some(first), Some(second)] = &seated else {
        return Ok(None);
    };
    // A seated player who was given another match while waiting for an
    // opponent keeps the seat; the set starts when that match ends.
    for entry in [first, second] {
        if unavailable(tx, &lobby.connection_id, &entry.ember_id)? {
            return Ok(None);
        }
    }
    let number = lobby.sets_started + 1;
    let roster = [first, second].map(|entry| Assigned {
        participant_id: entry.participant_id.clone(),
        ember_id: entry.ember_id.clone(),
        slot: entry.slot.unwrap_or(0),
    });
    let rules = Rules::standard(lobby.games_to_win, ORGANIZER_PROFILE);
    let external_match_id = format!("{MATCH_PREFIX}{}:{number}", lobby.id);
    let digest = json::digest(&json!({ "lobby_id": lobby.id, "set": number }))
        .map_err(|_| ApiFailure::unavailable())?;
    let mut metadata = lobby.metadata.clone();
    if metadata.len() < ember_protocol::matches::MAX_METADATA_ENTRIES {
        metadata.insert("lobby_set".into(), number.to_string());
    }
    let match_id = ledger::insert_match(
        tx,
        ctx,
        &NewMatch {
            tenant_id: &lobby.tenant_id,
            connection_id: &lobby.connection_id,
            external_match_id: &external_match_id,
            digest: &digest,
            rules: &rules,
            required_build_id: &lobby.required_build_id,
            metadata: &metadata,
            participants: [&roster[0], &roster[1]],
            starting_wins: [0, 0],
            lobby_id: Some(&lobby.id),
            tournament_id: None,
        },
    )?;
    tx.execute(
        "UPDATE lobbies SET current_match_id = ?2, sets_started = ?3 WHERE id = ?1",
        params![lobby.id, match_id, number],
    )?;
    Ok(Some(SetStarted {
        lobby_id: lobby.id,
        match_id,
        set: number,
        games_to_win: lobby.games_to_win,
        participants: roster
            .into_iter()
            .map(|player| StartedPlayer {
                ember_id: player.ember_id,
                slot: player.slot,
            })
            .collect(),
    }))
}

/// A set `advance` started. Its `lobby.set.started` event is told after the
/// change that started it, with the lobby's revision and queue then
/// (`SetStartedData`).
pub(super) struct SetStarted {
    lobby_id: String,
    match_id: String,
    set: u64,
    games_to_win: u8,
    participants: Vec<StartedPlayer>,
}

#[derive(Serialize)]
struct StartedPlayer {
    ember_id: EmberId,
    slot: u8,
}

/// Data of `lobby.set.started`.
#[derive(Serialize)]
struct SetStartedData<'a> {
    lobby_id: &'a str,
    lobby_revision: String,
    match_id: &'a str,
    set: u64,
    games_to_win: u8,
    participants: &'a [StartedPlayer],
    queue: Vec<&'a EmberId>,
}

/// Ends a streak whose holder no longer sits at the lobby.
fn keep_streak(tx: &Transaction<'_>, lobby_id: &str) -> Result<()> {
    tx.execute(
        "UPDATE lobbies SET streak_holder = NULL, streak = 0 WHERE id = ?1 AND streak_holder IS NOT NULL
           AND streak_holder NOT IN (SELECT ember_id FROM lobby_entries WHERE lobby_id = ?1 AND state = 'seated')",
        [lobby_id],
    )?;
    Ok(())
}

/// Emits the queue's new order, then the set that change started, if any.
pub(super) fn announce(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    lobby_id: &str,
    reason: &str,
    ember_id: Option<&EmberId>,
    dropped: &[EmberId],
    started: Option<SetStarted>,
) -> Result<u64> {
    announce_change(tx, ctx, lobby_id, started, |lobby, revision, waiting| {
        let seated = seats(tx, lobby_id)?;
        Ok((
            Kind::LobbyQueueChanged,
            json!({
                "lobby_id": lobby.id,
                "lobby_revision": revision.to_string(),
                "reason": reason,
                "ember_id": ember_id,
                "removed": dropped,
                "seated": seated
                    .iter()
                    .flatten()
                    .map(|entry| json!({ "ember_id": entry.ember_id, "slot": entry.slot }))
                    .collect::<Vec<_>>(),
                "queue": ids(waiting),
            }),
        ))
    })
}

/// Every change to a lobby's seats or queue: ends a streak whose holder left
/// its seat, moves the lobby's revision, emits the event `primary` makes of
/// the lobby as it now is (with that revision and the queue), then the set
/// the change started, if any. Returns the new revision.
fn announce_change(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    lobby_id: &str,
    started: Option<SetStarted>,
    primary: impl FnOnce(&Lobby, u64, &[Entry]) -> Result<(Kind, serde_json::Value)>,
) -> Result<u64> {
    keep_streak(tx, lobby_id)?;
    let revision = touch(tx, lobby_id, ctx.now)?;
    let lobby = load(tx, lobby_id)?.ok_or_else(ApiFailure::unavailable)?;
    let waiting = queue(tx, lobby_id)?;
    let (kind, data) = primary(&lobby, revision, &waiting)?;
    lobby_event(tx, ctx, &lobby, kind, data)?;
    if let Some(set) = started {
        let data = serde_json::to_value(SetStartedData {
            lobby_id: &set.lobby_id,
            lobby_revision: revision.to_string(),
            match_id: &set.match_id,
            set: set.set,
            games_to_win: set.games_to_win,
            participants: &set.participants,
            queue: ids(&waiting),
        })
        .map_err(|_| ApiFailure::unavailable())?;
        lobby_event(tx, ctx, &lobby, Kind::LobbySetStarted, data)?;
    }
    Ok(revision)
}

/// Called by the match code in the transaction that completes a lobby set.
pub fn on_set_completed(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    lobby_id: &str,
    match_id: &str,
    winner_slot: u8,
    scores: &[Score],
) -> Result<()> {
    let lobby = load(tx, lobby_id)?.ok_or_else(ApiFailure::unavailable)?;
    if lobby.current_match_id.as_deref() != Some(match_id) {
        return Ok(());
    }
    let seated = seats(tx, &lobby.id)?;
    let (Some(winner), Some(loser)) = (
        seated[usize::from(winner_slot)].clone(),
        seated[usize::from(1 - winner_slot)].clone(),
    ) else {
        return Err(ApiFailure::unavailable());
    };
    let streak = if lobby.streak_holder.as_deref() == Some(winner.ember_id.as_str()) {
        lobby.streak.saturating_add(1)
    } else {
        1
    };
    tx.execute(
        "UPDATE lobbies SET current_match_id = NULL, sets_completed = sets_completed + 1, streak_holder = ?2, streak = ?3
         WHERE id = ?1",
        params![lobby.id, winner.ember_id.as_str(), streak],
    )?;
    tx.execute(
        "UPDATE lobby_entries SET best_streak = MAX(best_streak, ?3) WHERE lobby_id = ?1 AND ember_id = ?2",
        params![lobby.id, winner.ember_id.as_str(), streak],
    )?;
    for slot in lobby.rotation.leaving(winner_slot) {
        if let Some(leaving) = &seated[usize::from(slot)] {
            set_entry(tx, &lobby.id, &leaving.ember_id, "queued", None, ctx.now)?;
        }
    }
    let mut dropped = Vec::new();
    let started = advance(tx, ctx, &lobby.id, &mut dropped)?;
    announce_change(tx, ctx, &lobby.id, started, |after, revision, waiting| {
        Ok((
            Kind::LobbySetCompleted,
            serde_json::to_value(LobbySetCompletedData {
                lobby_id: after.id.clone(),
                lobby_revision: Counter(revision),
                match_id: match_id.to_owned(),
                set: lobby.sets_started,
                games_to_win: lobby.games_to_win,
                rotation: Some(lobby.rotation),
                winner_id: winner.ember_id.clone(),
                loser_id: Some(loser.ember_id.clone()),
                scores: scores.iter().map(EventScore::from).collect(),
                streak: after.streak_holder.as_ref().map(|holder| EventStreak {
                    ember_id: holder.clone(),
                    sets: after.streak,
                }),
                removed: dropped,
                queue: ids(waiting).into_iter().cloned().collect(),
            })
            .unwrap_or_default(),
        ))
    })?;
    Ok(())
}

/// Takes a player out of a lobby. A seated player's running set is
/// cancelled; the other player keeps the seat and the next queued player sits.
pub(super) fn remove(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    lobby: &Lobby,
    ember_id: &EmberId,
    reason: &str,
) -> Result<u64> {
    let state: Option<String> = tx
        .query_row(
            "SELECT state FROM lobby_entries WHERE lobby_id = ?1 AND ember_id = ?2",
            params![lobby.id, ember_id.as_str()],
            |row| row.get(0),
        )
        .optional()?;
    if state.as_deref() == Some("seated")
        && let Some(current) = &lobby.current_match_id
    {
        ledger::cancel_lobby_set(tx, ctx, current, reason)?;
        tx.execute(
            "UPDATE lobbies SET current_match_id = NULL WHERE id = ?1",
            [&lobby.id],
        )?;
    }
    set_entry(tx, &lobby.id, ember_id, "left", None, ctx.now)?;
    let mut dropped = Vec::new();
    let started = advance(tx, ctx, &lobby.id, &mut dropped)?;
    announce(
        tx,
        ctx,
        &lobby.id,
        reason,
        Some(ember_id),
        &dropped,
        started,
    )
}

/// Called when a match outside any lobby completes or is cancelled. Its
/// players are free again, which may be all an idle lobby was waiting for.
pub fn on_players_free(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    connection_id: &str,
    players: &[EmberId],
) -> Result<()> {
    for ember_id in players {
        let lobbies = tx
            .prepare(
                "SELECT l.id FROM lobbies l JOIN lobby_entries e ON e.lobby_id = l.id
                 WHERE l.connection_id = ?1 AND l.state = 'open' AND l.current_match_id IS NULL
                   AND e.ember_id = ?2 AND e.state != 'left'",
            )?
            .query_map(params![connection_id, ember_id.as_str()], |row| {
                row.get::<_, String>(0)
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        for id in lobbies {
            if load_listed(tx, &id)?.is_none() {
                continue;
            }
            // A freed player can take a seat even when the set cannot start
            // yet; that is a change the lobby announces like any other.
            let before = seated_ids(tx, &id)?;
            let mut dropped = Vec::new();
            let started = advance(tx, ctx, &id, &mut dropped)?;
            if started.is_some() || !dropped.is_empty() || seated_ids(tx, &id)? != before {
                announce(
                    tx,
                    ctx,
                    &id,
                    "player_available",
                    Some(ember_id),
                    &dropped,
                    started,
                )?;
            }
        }
    }
    Ok(())
}

/// An ended link ends that player's places in the connection's open lobbies.
pub fn on_unlink(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    connection_id: &str,
    ember_id: &EmberId,
) -> Result<()> {
    let lobbies = tx
        .prepare(
            "SELECT l.id FROM lobbies l JOIN lobby_entries e ON e.lobby_id = l.id
             WHERE l.connection_id = ?1 AND l.state = 'open' AND e.ember_id = ?2 AND e.state != 'left'",
        )?
        .query_map(params![connection_id, ember_id.as_str()], |row| row.get::<_, String>(0))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    for id in lobbies {
        let Some(lobby) = load_listed(tx, &id)? else {
            continue;
        };
        remove(tx, ctx, &lobby, ember_id, "identity_unlinked")?;
    }
    Ok(())
}
