//! Keeping the bridge's rooms in line with the supervisor: member counts,
//! invitations, kicks and what the room host says about its room (name,
//! capacity, lock, listing details) come in, and an open room it no longer
//! reports is closed. A connection's rooms announce what changed on its
//! stream.
use std::{collections::BTreeSet, sync::atomic::Ordering, time::Duration};

use ember_protocol::{
    EmberId,
    event::Kind,
    play::MAX_INVITATION,
    rooms::{ENDED, MAX_CAPACITY},
};
use rusqlite::{Transaction, params};

use super::{
    PENDING_SECS, announce, details::Details, load, supervisor::Reported, supervisor::Supervisor,
};
use crate::{AppState, Keys, config::Config, error::Result};

/// How often the supervisor is asked which rooms are alive.
const POLL_SECS: u64 = 5;
/// Closed rooms and their bans are removed after this.
const RETENTION_SECS: u64 = 24 * 60 * 60;

/// Runs `poll` every few seconds, first at once so rooms left open in the
/// database are reconciled as soon as the bridge starts.
pub async fn poll_forever(state: AppState) {
    loop {
        poll(&state).await;
        tokio::time::sleep(Duration::from_secs(POLL_SECS)).await;
    }
}

/// Asks the supervisor which rooms are alive and brings the database in line.
/// A failed poll changes nothing.
pub async fn poll(state: &AppState) {
    let Some(supervisor) = Supervisor::of(state) else {
        return;
    };
    let _one_at_a_time = state.rooms.poll.lock().await;
    // Only a room that was already published (its creation finished) before
    // the supervisor is asked can be judged by its answer: one published
    // since may not be in it. Rooms still being created are left to their
    // creators' requests.
    let published = published_rooms(state).await;
    let reported = match supervisor.list().await {
        Ok(reported) => reported,
        Err(_) => {
            if !state.rooms.down.swap(true, Ordering::Relaxed) {
                eprintln!("ember-bridge: the room supervisor did not answer");
            }
            return;
        }
    };
    if state.rooms.down.swap(false, Ordering::Relaxed) {
        eprintln!("ember-bridge: the room supervisor answers again");
    }
    let now = state.now();
    let (keys, config) = (state.keys.clone(), state.config.clone());
    let applied = state
        .db
        .write(move |tx| apply(tx, &keys, &config, &reported, &published, now))
        .await;
    if applied.is_ok() {
        state.committed();
    }
}

/// The ids of the open rooms whose creation has finished.
async fn published_rooms(state: &AppState) -> BTreeSet<String> {
    state
        .db
        .read(|tx| {
            let mut statement = tx.prepare(
                "SELECT room_id FROM rooms WHERE closed_at IS NULL AND invitation_sealed IS NOT NULL",
            )?;
            let ids = statement
                .query_map([], |row| row.get(0))?
                .collect::<rusqlite::Result<BTreeSet<String>>>()?;
            Ok(ids)
        })
        .await
        // Nothing is judged without it: an empty set closes no room.
        .unwrap_or_default()
}

/// Applies one report. `published` is what `published_rooms` returned before
/// the report was requested; only those rooms are closed when it omits them.
fn apply(
    tx: &Transaction<'_>,
    keys: &Keys,
    config: &Config,
    reported: &[Reported],
    published: &BTreeSet<String>,
    now: u64,
) -> Result<()> {
    let mut alive = BTreeSet::new();
    for room in reported {
        alive.insert(room.room_id.as_str());
        let before = load(tx, &room.room_id)?;
        // A room still being created (no invitation stored yet) is left to
        // its creator's request to finish.
        let sealed = (room.invitation.len() <= MAX_INVITATION
            && room.invitation.is_ascii()
            && !room.invitation.is_empty())
        .then(|| keys.seal(room.invitation.as_bytes()));
        // The host's own report, each field only if it holds. Its name and
        // capacity replace the ones the room was created with; a capacity
        // that would not hold the members it reports is not taken. A report
        // that is not an object leaves everything as it was. The member
        // count stored is worked out once, here, from the capacity the room
        // ends up with, and the fighters were judged against that same count.
        let reported = u8::try_from(room.members).unwrap_or(u8::MAX);
        let held = before
            .as_ref()
            .map_or(MAX_CAPACITY, |before| before.capacity);
        let details = room.details.as_ref().filter(|details| details.is_object());
        let has_details = details.is_some();
        let details = match details {
            Some(details) => Details::from_value(details, reported, held),
            None => Details::absent(reported, held),
        };
        tx.execute(
            "UPDATE rooms SET name = COALESCE(?8, name), capacity = COALESCE(?9, capacity),
                 members = ?2, tables_playing = ?3,
                 locked = CASE WHEN ?7 THEN COALESCE(?10, locked) ELSE locked END,
                 host_name = CASE WHEN ?7 THEN ?11 ELSE host_name END,
                 fighters = CASE WHEN ?7 THEN ?12 ELSE fighters END,
                 set_format = COALESCE(?13, set_format), rotation = COALESCE(?14, rotation),
                 invitation_sealed = COALESCE(?4, invitation_sealed),
                 opened_at = COALESCE(opened_at, CASE WHEN ?6 THEN ?5 END)
             WHERE room_id = ?1 AND closed_at IS NULL AND invitation_sealed IS NOT NULL",
            params![
                room.room_id,
                details.members,
                room.tables_playing.min(u32::from(u8::MAX)),
                sealed,
                now,
                // The supervisor's latch, not the count this poll saw: a member who
                // came and went between polls has already ended creator-only entry.
                room.opened.unwrap_or(room.members >= 1),
                has_details,
                details.name,
                details.capacity,
                details.locked,
                details.host_name,
                details.fighters,
                details.set_format,
                details.rotation,
            ],
        )?;
        // Bans last for the room's life and are never evicted: the supervisor
        // reports at most `MAX_ROOM_BANS` per room, and the table keeps all.
        for banned in &room.banned {
            if EmberId::parse(banned).is_ok() {
                tx.execute(
                    "INSERT OR IGNORE INTO room_bans (room_id, ember_id, created_at)
                     SELECT room_id, ?2, ?3 FROM rooms WHERE room_id = ?1",
                    params![room.room_id, banned, now],
                )?;
            }
        }
        if let (Some(before), Some(after)) = (before, load(tx, &room.room_id)?)
            && after.connection_id.is_some()
            && after.closed_at.is_none()
            && after.region.is_some()
        {
            // At most one event per poll: opening says the counts too. What a
            // connection's view carries is the counts, the name and the
            // capacity; the listing details are not in it.
            let kind = if before.opened_at.is_none() && after.opened_at.is_some() {
                Some(Kind::RoomOpened)
            } else if (
                &before.name,
                before.capacity,
                before.members,
                before.tables_playing,
            ) != (
                &after.name,
                after.capacity,
                after.members,
                after.tables_playing,
            ) {
                Some(Kind::RoomChanged)
            } else {
                None
            };
            if let Some(kind) = kind {
                announce(tx, config, now, kind, &after, None)?;
            }
        }
    }
    // A room that was published before the supervisor was asked and is not
    // in its answer has ended. A room published since is not judged by this
    // report, whatever its reservation time.
    for room_id in published.iter().filter(|id| !alive.contains(id.as_str())) {
        let closed = tx.execute(
            "UPDATE rooms SET closed_at = ?2, members = 0, tables_playing = 0, creator_address = NULL,
                 closed_reason = ?3
             WHERE room_id = ?1 AND closed_at IS NULL",
            params![room_id, now, ENDED],
        )? == 1;
        if closed && let Some(room) = load(tx, room_id)? {
            announce(tx, config, now, Kind::RoomClosed, &room, Some(ENDED))?;
        }
    }
    // Creations that never finished.
    tx.execute(
        "UPDATE rooms SET closed_at = ?1, creator_address = NULL
         WHERE closed_at IS NULL AND invitation_sealed IS NULL AND created_at + ?2 <= ?1",
        params![now, PENDING_SECS],
    )?;
    let cutoff = now.saturating_sub(RETENTION_SECS);
    tx.execute(
        "DELETE FROM room_bans WHERE room_id IN (SELECT room_id FROM rooms WHERE closed_at <= ?1)",
        [cutoff],
    )?;
    tx.execute("DELETE FROM rooms WHERE closed_at <= ?1", [cutoff])?;
    Ok(())
}
