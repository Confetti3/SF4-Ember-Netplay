//! A lobby as stored, its queue and seats, and its snapshot.
use std::collections::BTreeMap;

use ember_protocol::{EmberId, event::Kind, lobby::Rotation};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::Serialize;
use serde_json::json;

use crate::{
    ctx::Ctx,
    db,
    error::{ApiFailure, Result},
    events::{self, NewEvent, Viewer, emit},
    routes::records,
};

pub(super) struct Lobby {
    pub(super) id: String,
    pub(super) tenant_id: String,
    pub(super) connection_id: String,
    pub(super) external_lobby_id: String,
    pub(super) revision: u64,
    pub(super) open: bool,
    pub(super) games_to_win: u8,
    pub(super) rotation: Rotation,
    pub(super) required_build_id: String,
    pub(super) metadata: BTreeMap<String, String>,
    pub(super) current_match_id: Option<String>,
    pub(super) sets_started: u64,
    pub(super) sets_completed: u64,
    pub(super) streak_holder: Option<String>,
    pub(super) streak: u64,
}

/// The lobby `id`. A rotation this bridge does not know (a newer bridge wrote
/// it before a rollback) fails the read: the lobby answers as unavailable
/// rather than rotating some other way.
pub(super) fn load(tx: &Transaction<'_>, id: &str) -> Result<Option<Lobby>> {
    Ok(read(tx, id)?)
}

/// `load` for a pass over many lobbies, such as the ones a freed or unlinked
/// player waits in: one whose stored rotation this bridge does not know is
/// logged and passed over (`None`), so it never stops the others or the
/// write that freed the player.
pub(super) fn load_listed(tx: &Transaction<'_>, id: &str) -> Result<Option<Lobby>> {
    match read(tx, id) {
        Err(error) if db::unknown(&error) => {
            eprintln!(
                "ember-bridge: lobby {id} has a stored rotation this bridge does not know; skipped"
            );
            Ok(None)
        }
        read => Ok(read?),
    }
}

fn read(tx: &Transaction<'_>, id: &str) -> rusqlite::Result<Option<Lobby>> {
    tx.query_row(
        "SELECT id, tenant_id, connection_id, external_lobby_id, revision, state, games_to_win, rotation,
                required_build_id, metadata, current_match_id, sets_started, sets_completed, streak_holder, streak
         FROM lobbies WHERE id = ?1",
        [id],
        |row| {
            Ok(Lobby {
                id: row.get(0)?,
                tenant_id: row.get(1)?,
                connection_id: row.get(2)?,
                external_lobby_id: row.get(3)?,
                revision: row.get(4)?,
                open: row.get::<_, String>(5)? == "open",
                games_to_win: row.get(6)?,
                rotation: db::known(row, 7, Rotation::parse)?,
                required_build_id: row.get(8)?,
                metadata: serde_json::from_str(&row.get::<_, String>(9)?).unwrap_or_default(),
                current_match_id: row.get(10)?,
                sets_started: row.get(11)?,
                sets_completed: row.get(12)?,
                streak_holder: row.get(13)?,
                streak: row.get(14)?,
            })
        },
    )
    .optional()
}

#[derive(Clone, Debug, Serialize)]
pub(super) struct Entry {
    pub(super) participant_id: String,
    pub(super) ember_id: EmberId,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(super) slot: Option<u8>,
}

fn entries(tx: &Transaction<'_>, lobby_id: &str, state: &str) -> Result<Vec<Entry>> {
    tx.prepare(
        "SELECT participant_id, ember_id, slot FROM lobby_entries WHERE lobby_id = ?1 AND state = ?2
         ORDER BY position, slot",
    )?
    .query_map(params![lobby_id, state], |row| {
        Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?, row.get::<_, Option<u8>>(2)?))
    })?
    .map(|row| {
        let (participant_id, ember_id, slot) = row?;
        Ok(Entry {
            participant_id,
            ember_id: EmberId::parse(&ember_id).map_err(|_| ApiFailure::unavailable())?,
            slot,
        })
    })
    .collect()
}

pub(super) fn queue(tx: &Transaction<'_>, lobby_id: &str) -> Result<Vec<Entry>> {
    entries(tx, lobby_id, "queued")
}

/// Seated players by slot.
pub(super) fn seats(tx: &Transaction<'_>, lobby_id: &str) -> Result<[Option<Entry>; 2]> {
    let mut seats = [None, None];
    for entry in entries(tx, lobby_id, "seated")? {
        if let Some(slot) = entry.slot.filter(|slot| *slot < 2) {
            seats[usize::from(slot)] = Some(entry);
        }
    }
    Ok(seats)
}

pub(super) fn seated_ids(tx: &Transaction<'_>, lobby_id: &str) -> Result<[Option<EmberId>; 2]> {
    Ok(seats(tx, lobby_id)?.map(|entry| entry.map(|entry| entry.ember_id)))
}

pub(super) fn ids(entries: &[Entry]) -> Vec<&EmberId> {
    entries.iter().map(|entry| &entry.ember_id).collect()
}

pub(super) fn visible(tx: &Transaction<'_>, viewer: &Viewer, lobby: &Lobby) -> Result<bool> {
    Ok(match viewer {
        Viewer::Provider { connection_id, .. } => *connection_id == lobby.connection_id,
        Viewer::Organizer { tenant_id } => *tenant_id == lobby.tenant_id,
        Viewer::Player { ember_id } => tx.query_row(
            "SELECT EXISTS (SELECT 1 FROM lobby_entries WHERE lobby_id = ?1 AND ember_id = ?2)",
            params![lobby.id, ember_id.as_str()],
            |row| row.get(0),
        )?,
    })
}

pub(super) fn touch(tx: &Transaction<'_>, lobby_id: &str, now: u64) -> Result<u64> {
    Ok(tx.query_row(
        "UPDATE lobbies SET revision = revision + 1, updated_at = ?2 WHERE id = ?1 RETURNING revision",
        params![lobby_id, now],
        |row| row.get(0),
    )?)
}

pub(super) fn lobby_event(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    lobby: &Lobby,
    kind: Kind,
    data: serde_json::Value,
) -> Result<i64> {
    emit(
        tx,
        &ctx.config,
        ctx.now,
        NewEvent {
            kind,
            tenant_id: &lobby.tenant_id,
            connection_id: Some(&lobby.connection_id),
            subject: format!("lobbies/{}", lobby.id),
            match_id: None,
            ember_id: None,
            lobby_id: Some(&lobby.id),
            tournament_id: None,
            data,
        },
    )
}

pub(super) fn snapshot(tx: &Transaction<'_>, lobby: &Lobby) -> Result<serde_json::Value> {
    Ok(json!({
        "lobby_id": lobby.id,
        "external_lobby_id": lobby.external_lobby_id,
        "state": if lobby.open { "open" } else { "closed" },
        "revision": lobby.revision.to_string(),
        "games_to_win": lobby.games_to_win,
        "rotation": lobby.rotation,
        "required_build_id": lobby.required_build_id,
        "current_match_id": lobby.current_match_id,
        "sets_started": lobby.sets_started,
        "sets_completed": lobby.sets_completed,
        "seated": seats(tx, &lobby.id)?.iter().flatten().collect::<Vec<_>>(),
        "queue": queue(tx, &lobby.id)?,
        "streak": lobby.streak_holder.as_ref().map(|holder| json!({ "ember_id": holder, "sets": lobby.streak })),
        "standings": records::lobby_standings(tx, &lobby.id)?,
        "metadata": lobby.metadata,
        "event_cursor": events::head(tx)?.to_string(),
    }))
}
