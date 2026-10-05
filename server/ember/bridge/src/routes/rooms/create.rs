//! Opening a room: one path for a player's own room and a connection's room
//! for a linked player. The limits are checked and the room reserved in one
//! write before the supervisor is asked, so two requests cannot both pass them
//! while the room host starts.
use ember_protocol::{
    EmberId, Error as ProtocolError,
    event::Kind,
    play::MAX_INVITATION,
    rooms::{CreateRoom, INVALID_NAME, NOT_LINKED, ROOM_LIMIT},
};
use std::time::Duration;

use rusqlite::{OptionalExtension, Transaction, params};

use super::{
    PENDING_SECS, Room, announce, hex, load, refuse,
    supervisor::{Create, Failure, Supervisor},
};
use crate::{
    AppState,
    error::{ApiFailure, Result},
    routes::matches,
    util::random,
};

/// How long a create refused for the room limit waits for a fresh poll.
const RECONCILE_SECS: u64 = 5;

/// Open rooms one address may create.
const ROOMS_PER_ADDRESS: i64 = 2;

/// Whose limits a new room counts against, besides its creator's one open
/// room.
#[derive(Clone)]
pub enum Owner {
    /// A player's own room: their network address (`super::Address`).
    Address(String),
    /// A connection's room for a player linked on it as `participant_id`.
    Connection {
        connection_id: String,
        participant_id: String,
        max_open: u8,
    },
}

/// A request's own rules as a refusal clients act on.
pub fn refusal(error: ProtocolError) -> ApiFailure {
    match error {
        ProtocolError::InvalidField("name") => refuse(INVALID_NAME),
        other => other.into(),
    }
}

/// Opens a room for `creator` and answers it as recorded, hosted and
/// waiting for its creator.
pub async fn open(
    state: &AppState,
    supervisor: &Supervisor<'_>,
    command: CreateRoom,
    creator: EmberId,
    owner: Owner,
) -> Result<Room> {
    command.check().map_err(refusal)?;
    let room_id = hex(&random::<16>());
    let now = state.now();
    let attempt = || {
        let (room_id, creator, owner, command) = (
            room_id.clone(),
            creator.clone(),
            owner.clone(),
            command.clone(),
        );
        state
            .db
            .write(move |tx| reserve(tx, &room_id, &creator, &owner, &command, now))
    };
    if let Err(refused) = attempt().await {
        // The rooms in the way may have ended since the last poll: a room
        // whose last member just left is gone from the supervisor at once,
        // but up to a poll interval later here. Ask it once before refusing.
        let reason = refused
            .details
            .get("reason")
            .and_then(|reason| reason.as_str());
        if reason != Some(ROOM_LIMIT) {
            return Err(refused);
        }
        // Creates refused together share one poll, and a slow supervisor
        // costs this request a bounded wait, after which the refusal stands.
        let seen = super::poll::polls_started(state);
        let reconciled = tokio::time::timeout(
            Duration::from_secs(RECONCILE_SECS),
            super::poll::poll_since(state, seen),
        )
        .await;
        if reconciled.is_err() {
            return Err(refused);
        }
        attempt().await?;
    }
    let created = supervisor
        .create(&Create {
            room_id: &room_id,
            name: &command.name,
            capacity: command.capacity,
            build_id: &command.build_id,
            creator: creator.as_str(),
            bridge_id: &state.config.bridge_id,
            ticket_key: &state.keys.signing_key().to_b64u(),
            ticket_kid: &state.keys.signing_kid(),
        })
        .await;
    let created = match created {
        Ok(created) => created,
        Err(Failure::Refused(reason)) => {
            forget(state, &room_id).await;
            return Err(refuse(reason));
        }
        Err(Failure::Unavailable) => {
            // It may have started the room before the answer was lost.
            let _ = supervisor.delete(&room_id).await;
            forget(state, &room_id).await;
            return Err(ApiFailure::unavailable());
        }
    };
    let invitation_valid = !created.invitation.is_empty()
        && created.invitation.len() <= MAX_INVITATION
        && created.invitation.is_ascii();
    let stored = if invitation_valid {
        let sealed = state.keys.seal(created.invitation.as_bytes());
        let (room_id, region, config) = (room_id.clone(), created.region, state.config.clone());
        state
            .db
            .write(move |tx| {
                let updated = tx.execute(
                    "UPDATE rooms SET invitation_sealed = ?1, region = ?2
                     WHERE room_id = ?3 AND closed_at IS NULL",
                    params![sealed, region, room_id],
                )? == 1;
                let room = load(tx, &room_id)?.filter(|_| updated);
                match room {
                    // The answer's own rules, before anyone is told of it.
                    Some(room) if room.summary().is_ok() => {
                        announce(tx, &config, now, Kind::RoomCreated, &room, None)?;
                        Ok(Some(room))
                    }
                    _ => Err(ApiFailure::unavailable()),
                }
            })
            .await
            .ok()
            .flatten()
    } else {
        None
    };
    match stored {
        Some(room) => {
            state.committed();
            Ok(room)
        }
        None => {
            let _ = supervisor.delete(&room_id).await;
            forget(state, &room_id).await;
            Err(ApiFailure::unavailable())
        }
    }
}

/// Checks the creator's and the owner's limits and records the room as
/// pending, in one write.
fn reserve(
    tx: &Transaction<'_>,
    room_id: &str,
    creator: &EmberId,
    owner: &Owner,
    command: &CreateRoom,
    now: u64,
) -> Result<()> {
    tx.execute(
        "UPDATE rooms SET closed_at = ?1, creator_address = NULL
         WHERE closed_at IS NULL AND invitation_sealed IS NULL AND created_at + ?2 <= ?1",
        params![now, PENDING_SECS],
    )?;
    let open = |column: &str, value: &str| -> Result<i64> {
        Ok(tx.query_row(
            &format!("SELECT COUNT(*) FROM rooms WHERE {column} = ?1 AND closed_at IS NULL"),
            [value],
            |row| row.get(0),
        )?)
    };
    let (address, connection_id) = match owner {
        Owner::Address(address) => (Some(address.as_str()), None),
        Owner::Connection {
            connection_id,
            participant_id,
            ..
        } => {
            if !matches::linked(tx, connection_id, participant_id, creator)? {
                return Err(refuse(NOT_LINKED));
            }
            (None, Some(connection_id.as_str()))
        }
    };
    // The creator's open room comes first, so a connection that opened it can
    // hand out its link instead, whatever its own limit says.
    let existing: Option<(String, Option<String>)> = tx
        .query_row(
            "SELECT room_id, connection_id FROM rooms WHERE creator_ember_id = ?1 AND closed_at IS NULL",
            [creator.as_str()],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .optional()?;
    if let Some((existing, created_by)) = existing {
        let refusal = refuse(ROOM_LIMIT);
        return Err(match connection_id {
            Some(connection_id) if created_by.as_deref() == Some(connection_id) => {
                refusal.detail("room_id", existing)
            }
            _ => refusal,
        });
    }
    // Only then the owner's limit, which a new room would count against.
    let full = match owner {
        Owner::Address(address) => open("creator_address", address)? >= ROOMS_PER_ADDRESS,
        Owner::Connection {
            connection_id,
            max_open,
            ..
        } => open("connection_id", connection_id)? >= i64::from(*max_open),
    };
    if full {
        return Err(refuse(ROOM_LIMIT));
    }
    tx.execute(
        "INSERT INTO rooms (room_id, name, capacity, build_id, creator_ember_id, creator_address, created_at,
             connection_id)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
        params![
            room_id,
            command.name,
            command.capacity,
            command.build_id,
            creator.as_str(),
            address,
            now,
            connection_id
        ],
    )?;
    Ok(())
}

/// Drops a room that never became hosted.
async fn forget(state: &AppState, room_id: &str) {
    let room_id = room_id.to_owned();
    let _ = state
        .db
        .write(move |tx| {
            tx.execute(
                "DELETE FROM rooms WHERE room_id = ?1 AND invitation_sealed IS NULL",
                [room_id],
            )?;
            Ok(())
        })
        .await;
}
