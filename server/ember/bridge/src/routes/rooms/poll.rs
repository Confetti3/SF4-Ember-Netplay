//! Keeping the bridge's rooms in line with the supervisor: member counts,
//! invitations, kicks and what the room host says about its room (name,
//! capacity, lock, listing details) come in, and an open room it no longer
//! reports is closed. A connection's rooms announce what changed on its
//! stream.
use std::{collections::BTreeSet, sync::atomic::Ordering, time::Duration};

use ember_protocol::{EmberId, event::Kind, play::MAX_INVITATION, rooms::ENDED};
use rusqlite::{Transaction, params};

use super::{
    PENDING_SECS, Room, announce, details::Details, load, supervisor::Reported,
    supervisor::Supervisor,
};
use crate::{AppState, Keys, config::Config, error::Result, skill};

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
    let one_at_a_time = state.rooms.poll.lock().await;
    poll_locked(state, &supervisor).await;
    drop(one_at_a_time);
}

/// The number of polls started so far; pass it to `poll_since`.
pub(super) fn polls_started(state: &AppState) -> u64 {
    state.rooms.polls.load(Ordering::Acquire)
}

/// A poll that started after `seen`. Polls run one at a time, so once this
/// caller holds the lock, any poll started since `seen` has finished and its
/// answer is reused: concurrent callers share one supervisor listing instead
/// of queueing one each, and none reuses a listing taken before it asked.
pub(super) async fn poll_since(state: &AppState, seen: u64) {
    let Some(supervisor) = Supervisor::of(state) else {
        return;
    };
    let one_at_a_time = state.rooms.poll.lock().await;
    if polls_started(state) == seen {
        poll_locked(state, &supervisor).await;
    }
    drop(one_at_a_time);
}

/// The poll itself; the caller holds `state.rooms.poll`.
async fn poll_locked(state: &AppState, supervisor: &Supervisor<'_>) {
    state.rooms.polls.fetch_add(1, Ordering::AcqRel);
    poll_once(state, supervisor).await;
}

async fn poll_once(state: &AppState, supervisor: &Supervisor<'_>) {
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
    // A failed write changes nothing; the next poll tries again.
    let _ = state
        .db
        .write(move |tx| apply(tx, &keys, &config, &reported, &published, now))
        .await;
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
/// A room the report leaves as it was is not written at all, so a poll that
/// changes nothing dirties no page and, writing no event, wakes nobody.
fn apply(
    tx: &Transaction<'_>,
    keys: &Keys,
    config: &Config,
    rooms: &[Reported],
    published: &BTreeSet<String>,
    now: u64,
) -> Result<()> {
    let mut alive = BTreeSet::new();
    for room in rooms {
        alive.insert(room.room_id.as_str());
        // A room still being created (no invitation stored yet) is left to
        // its creator's request to finish, and a closed one stays closed.
        let before = load(tx, &room.room_id)?
            .filter(|before| before.closed_at.is_none() && before.invitation_sealed.is_some());
        let after = match &before {
            Some(before) => {
                let after = reported(before, room, keys, now);
                let written = after != *before && store(tx, &after)?;
                written.then_some(after)
            }
            None => None,
        };
        skill::record_games(tx, &room.room_id, room.games.as_ref(), now)?;
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
        if let (Some(before), Some(after)) = (before, after)
            && after.connection_id.is_some()
            && after.hosted()
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

/// The room as one report leaves it: what the room's record would hold after
/// the report, by the rules each field follows. The invitation is sealed again
/// only when it is not the one stored, so an unchanged room comes out equal to
/// `before`.
fn reported(before: &Room, room: &Reported, keys: &Keys, now: u64) -> Room {
    let valid = room.invitation.len() <= MAX_INVITATION
        && room.invitation.is_ascii()
        && !room.invitation.is_empty();
    let unchanged = before
        .invitation_sealed
        .as_deref()
        .and_then(|sealed| keys.open(sealed))
        .is_some_and(|plain| plain.as_slice() == room.invitation.as_bytes());
    let sealed = (valid && !unchanged).then(|| keys.seal(room.invitation.as_bytes()));
    // The host's own report, each field only if it holds. Its name and
    // capacity replace the ones the room was created with; a capacity that
    // would not hold the members it reports is not taken. A report that is
    // not an object leaves everything as it was. The member count stored is
    // worked out once, here, from the capacity the room ends up with, and the
    // fighters were judged against that same count.
    let members = u8::try_from(room.members).unwrap_or(u8::MAX);
    let details = room.details.as_ref().filter(|details| details.is_object());
    let has_details = details.is_some();
    let details = match details {
        Some(details) => Details::from_value(details, members, before.capacity),
        None => Details::absent(members, before.capacity),
    };
    // The supervisor's latch, not the count this poll saw: a member who came
    // and went between polls has already ended creator-only entry.
    let opened = room.opened.unwrap_or(room.members >= 1);
    let report = Report {
        details,
        has_details,
        sealed,
        tables: room.tables_playing,
        opened,
    };
    with_report(before, report, now)
}

/// What one report says about a room, judged.
struct Report {
    details: Details,
    /// The report's `details` was an object.
    has_details: bool,
    /// The invitation sealed again, or `None` to keep the stored one.
    sealed: Option<Vec<u8>>,
    /// The tables it says are playing.
    tables: u32,
    /// Whether the room has ever had a member.
    opened: bool,
}

/// `before` with `report` applied. A field the report does not say keeps its
/// value, except the moderator's name, the fighters and the input delay mode,
/// which a report with an object replaces even when it leaves them out; the
/// lock changes only with such a report. The room opens once, at the first
/// report that says it has had a member.
fn with_report(before: &Room, report: Report, now: u64) -> Room {
    let Report {
        details,
        has_details,
        sealed,
        tables,
        opened,
    } = report;
    let mut after = before.clone();
    after.members = details.members;
    after.tables_playing = u8::try_from(tables.min(u32::from(u8::MAX))).unwrap_or(u8::MAX);
    if let Some(name) = details.name {
        after.name = name;
    }
    if let Some(capacity) = details.capacity {
        after.capacity = capacity;
    }
    if has_details {
        after.locked = details.locked.or(before.locked);
        after.host_name = details.host_name;
        after.fighters = details.fighters;
        after.own_delay = details.own_delay;
    }
    after.set_format = details.set_format.or(before.set_format);
    after.rotation = details.rotation.or(before.rotation);
    if sealed.is_some() {
        after.invitation_sealed = sealed;
    }
    after.opened_at = before.opened_at.or(opened.then_some(now));
    after
}

/// Writes what a report changed in `room`'s record. False when the room was
/// closed or is still being created, which a report never changes.
fn store(tx: &Transaction<'_>, room: &Room) -> Result<bool> {
    Ok(tx.execute(
        "UPDATE rooms SET name = ?2, capacity = ?3, members = ?4, tables_playing = ?5, locked = ?6,
             host_name = ?7, fighters = ?8, set_format = ?9, rotation = ?10, own_delay = ?11,
             invitation_sealed = ?12, opened_at = ?13
         WHERE room_id = ?1 AND closed_at IS NULL AND invitation_sealed IS NOT NULL",
        params![
            room.room_id,
            room.name,
            room.capacity,
            room.members,
            room.tables_playing,
            room.locked,
            room.host_name,
            room.fighters,
            room.set_format,
            room.rotation,
            room.own_delay,
            room.invitation_sealed,
            room.opened_at,
        ],
    )? == 1)
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::*;

    fn room() -> Room {
        Room {
            room_id: "room".into(),
            name: "Fight Night".into(),
            build_id: "build".into(),
            capacity: 4,
            members: 2,
            tables_playing: 1,
            created_at: 10,
            host_region: Some("use1".into()),
            creator_region: None,
            invitation_sealed: Some(vec![1, 2, 3]),
            creator_ember_id: "creator".into(),
            opened_at: Some(20),
            closed_at: None,
            connection_id: None,
            host_name: Some("Player 1".into()),
            fighters: Some(vec![3, 4]),
            locked: Some(true),
            set_format: Some(3),
            rotation: Some(1),
            own_delay: true,
        }
    }

    fn report(details: Option<Details>, tables: u32, opened: bool) -> Report {
        let has_details = details.is_some();
        Report {
            details: details.unwrap_or_else(|| Details::absent(2, 4)),
            has_details,
            sealed: None,
            tables,
            opened,
        }
    }

    #[test]
    fn a_report_that_says_what_the_room_holds_changes_nothing() {
        let before = room();
        let details = Details::from_value(
            &json!({ "name": "Fight Night", "capacity": 4, "locked": true, "host_name": "Player 1",
                "fighters": [3, 4], "set_format": 3, "rotation": 1, "own_delay": true }),
            2,
            4,
        );
        assert_eq!(
            with_report(&before, report(Some(details), 1, true), 99),
            before
        );
        // Not an object: the counts are all it says.
        assert_eq!(with_report(&before, report(None, 1, true), 99), before);
    }

    #[test]
    fn an_object_replaces_the_moderator_fighters_and_delay_and_keeps_the_rest() {
        let before = room();
        let after = with_report(
            &before,
            report(Some(Details::from_value(&json!({}), 2, 4)), 1, true),
            99,
        );
        assert_eq!(
            (after.host_name, after.fighters, after.own_delay),
            (None, None, false)
        );
        assert_eq!(
            (
                after.name,
                after.capacity,
                after.locked,
                after.set_format,
                after.rotation
            ),
            (
                before.name.clone(),
                before.capacity,
                before.locked,
                before.set_format,
                before.rotation
            )
        );
        // Without an object those stay as they were.
        let after = with_report(&before, report(None, 3, true), 99);
        assert_eq!(
            (after.host_name.as_deref(), after.own_delay),
            (Some("Player 1"), true)
        );
        assert_eq!(after.tables_playing, 3);
    }

    #[test]
    fn the_room_opens_once_and_a_new_invitation_replaces_the_stored_one() {
        let mut before = room();
        before.opened_at = None;
        assert_eq!(
            with_report(&before, report(None, 1, false), 99).opened_at,
            None
        );
        assert_eq!(
            with_report(&before, report(None, 1, true), 99).opened_at,
            Some(99)
        );
        assert_eq!(
            with_report(&room(), report(None, 1, true), 99).opened_at,
            Some(20)
        );
        let mut sealed = report(None, 1, true);
        sealed.sealed = Some(vec![9]);
        assert_eq!(
            with_report(&room(), sealed, 99).invitation_sealed,
            Some(vec![9])
        );
        // More tables than a byte holds are stored as the most it holds.
        assert_eq!(
            with_report(&room(), report(None, 300, true), 99).tables_playing,
            u8::MAX
        );
    }

    #[test]
    fn the_same_invitation_is_not_sealed_again() {
        let keys = Keys::generate();
        let mut before = room();
        before.invitation_sealed = Some(keys.seal(b"sf4e3:room"));
        let same = Reported {
            room_id: "room".into(),
            members: 2,
            tables_playing: 1,
            invitation: "sf4e3:room".into(),
            banned: Vec::new(),
            opened: Some(true),
            details: None,
            games: None,
        };
        assert_eq!(reported(&before, &same, &keys, 99), before);
        let new = Reported {
            invitation: "sf4e3:other".into(),
            ..same
        };
        let after = reported(&before, &new, &keys, 99);
        assert_ne!(after.invitation_sealed, before.invitation_sealed);
        assert_eq!(
            keys.open(after.invitation_sealed.as_deref().unwrap())
                .unwrap()
                .as_slice(),
            b"sf4e3:other"
        );
        // A report without a usable invitation keeps the stored one.
        let empty = Reported {
            invitation: String::new(),
            ..new
        };
        assert_eq!(reported(&before, &empty, &keys, 99), before);
    }
}
