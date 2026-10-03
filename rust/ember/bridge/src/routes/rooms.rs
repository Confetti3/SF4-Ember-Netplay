//! Public rooms (docs/design/PUBLIC_ROOMS.md): a listing of the rooms a room
//! supervisor hosts on this machine, creating one, and a signed ticket per
//! join. The supervisor is the source of truth for which rooms are alive; the
//! bridge keeps its last report (`poll`) and serves the listing from it.
//!
//! On when the configuration has `rooms` and the integration secrets hold the
//! supervisor's secret; otherwise the routes answer `not_found` and the
//! `rooms` capability is not offered.
mod supervisor;

use std::{
    collections::BTreeSet,
    convert::Infallible,
    fmt::Write as _,
    net::IpAddr,
    sync::atomic::{AtomicBool, Ordering},
    time::Duration,
};

use axum::{
    extract::{FromRequestParts, Path, Query, State},
    http::{HeaderMap, StatusCode, request::Parts},
    response::Response,
};
use ember_protocol::{
    EmberId, Error as ProtocolError,
    api::ErrorCode,
    play::{self, MAX_INVITATION},
    rooms::{
        BANNED, CreateRoom, INVALID_NAME, ROOM_FULL, ROOM_LIMIT, ROOM_NOT_FOUND, ROOM_NOT_OPEN,
        RoomAdmission, RoomList, RoomSummary, RoomTicket, TICKET_SECS, TicketRequest,
        UNSUPPORTED_BUILD,
    },
};
use rusqlite::{OptionalExtension, Row, Transaction, params};

use self::supervisor::{Create, Failure, Reported, Supervisor};
use crate::{
    AppState, Keys,
    error::{ApiFailure, Result},
    http::{Body, GENERAL_BODY, json, ok},
    routes::play::playing,
    util::random,
};

/// The capability this feature is advertised as.
pub const FEATURE: &str = "rooms";
/// How often the supervisor is asked which rooms are alive.
const POLL_SECS: u64 = 5;
/// Open rooms one address may create.
const ROOMS_PER_ADDRESS: i64 = 2;
/// Ticket requests one address may make per window.
const TICKETS_PER_WINDOW: usize = 30;
const TICKET_WINDOW_SECS: u64 = 10 * 60;
/// Rooms in one listing.
const LIST_LIMIT: i64 = 100;
/// A creation the supervisor never answered is given up on after this.
const PENDING_SECS: u64 = 60;
/// Closed rooms and their bans are removed after this.
const RETENTION_SECS: u64 = 24 * 60 * 60;
const MAX_BUILD: usize = 128;

/// Per-process state of the room routes.
pub struct Shared {
    /// One poll at a time, from the supervisor's answer to its commit, so a
    /// slow poll never writes over a newer one.
    poll: tokio::sync::Mutex<()>,
    /// The last poll failed; it is logged on the way down and on the way up.
    down: AtomicBool,
}

impl Default for Shared {
    fn default() -> Self {
        Self {
            poll: tokio::sync::Mutex::new(()),
            down: AtomicBool::new(false),
        }
    }
}

/// Whether public rooms are on: a supervisor and its secret are configured.
pub fn enabled(state: &AppState) -> bool {
    Supervisor::of(state).is_some()
}

fn supervisor(state: &AppState) -> Result<Supervisor<'_>> {
    Supervisor::of(state).ok_or_else(ApiFailure::not_found)
}

/// The caller's network address as a keyed hash under the bridge's persistent
/// HMAC key, so a stored hash still matches after a restart but cannot be
/// turned back into an address: an IPv4 address, or an IPv6 /64. The service listens on loopback behind nginx, which sets `X-Real-IP`;
/// only a local process could forge it. Without the header (tests, a direct
/// loopback connection) every caller shares the key `local`.
pub struct Address(String);

impl FromRequestParts<AppState> for Address {
    type Rejection = Infallible;

    async fn from_request_parts(
        parts: &mut Parts,
        state: &AppState,
    ) -> std::result::Result<Self, Infallible> {
        let forwarded = parts
            .headers
            .get("x-real-ip")
            .and_then(|value| value.to_str().ok())
            .and_then(|value| value.trim().parse::<IpAddr>().ok());
        let bytes = match forwarded {
            Some(IpAddr::V4(v4)) => v4.octets().to_vec(),
            Some(IpAddr::V6(v6)) => match v6.to_ipv4_mapped() {
                Some(v4) => v4.octets().to_vec(),
                None => v6.octets()[..8].to_vec(),
            },
            None => b"local".to_vec(),
        };
        let digest = state.keys.keyed_hash("room-address", &bytes);
        let mut key = String::with_capacity(32);
        for byte in &digest[..16] {
            let _ = write!(key, "{byte:02x}");
        }
        Ok(Self(key))
    }
}

/// A refusal with its reason, which clients act on.
fn refuse(reason: &'static str) -> ApiFailure {
    let (code, message) = match reason {
        INVALID_NAME => (
            ErrorCode::InvalidRequest,
            "A room name is 1 to 64 characters on one line.",
        ),
        UNSUPPORTED_BUILD => (
            ErrorCode::IncompatibleBuild,
            "No room host runs this build.",
        ),
        ROOM_LIMIT => (
            ErrorCode::StaleRevision,
            "You or your network already have as many open rooms as are allowed.",
        ),
        ROOM_FULL => (ErrorCode::StaleRevision, "The room is full."),
        BANNED => (ErrorCode::Forbidden, "You were removed from this room."),
        _ => (ErrorCode::NotFound, "That room is not open."),
    };
    ApiFailure::new(code, message).detail("reason", reason)
}

/// A room as the bridge last knew it.
struct Room {
    room_id: String,
    name: String,
    build_id: String,
    capacity: u8,
    members: u8,
    tables_playing: u8,
    created_at: u64,
    region: Option<String>,
    invitation_sealed: Option<Vec<u8>>,
}

const COLUMNS: &str = "room_id, name, build_id, capacity, members, tables_playing, created_at, region, invitation_sealed";

fn room_of(row: &Row<'_>) -> rusqlite::Result<Room> {
    Ok(Room {
        room_id: row.get(0)?,
        name: row.get(1)?,
        build_id: row.get(2)?,
        capacity: row.get(3)?,
        members: row.get(4)?,
        tables_playing: row.get(5)?,
        created_at: row.get(6)?,
        region: row.get(7)?,
        invitation_sealed: row.get(8)?,
    })
}

impl Room {
    /// The listing form, once the supervisor has reported the room hosted.
    fn summary(&self) -> Result<RoomSummary> {
        let summary = RoomSummary {
            room_id: self.room_id.clone(),
            name: self.name.clone(),
            build_id: self.build_id.clone(),
            members: self.members,
            capacity: self.capacity,
            tables_playing: self.tables_playing,
            region: self.region.clone().ok_or_else(ApiFailure::unavailable)?,
            created_at: self.created_at,
        };
        summary.check().map_err(|_| ApiFailure::unavailable())?;
        Ok(summary)
    }
}

/// A build filter or request build: what the protocol accepts as a build ID.
fn build_ok(build_id: &str) -> bool {
    !build_id.is_empty() && build_id.len() <= MAX_BUILD && build_id.is_ascii()
}

#[derive(serde::Deserialize)]
pub struct ListQuery {
    #[serde(default)]
    build_id: Option<String>,
}

/// `GET /v1/rooms?build_id=`: open rooms with at least one member, the ones
/// with the most free seats first.
pub async fn list(
    State(state): State<AppState>,
    headers: HeaderMap,
    Query(query): Query<ListQuery>,
) -> Result<Response> {
    supervisor(&state)?;
    playing(&state, &headers).await?;
    if query
        .build_id
        .as_deref()
        .is_some_and(|build| !build_ok(build))
    {
        return Err(ApiFailure::invalid("build_id is not a build ID."));
    }
    let rooms = state
        .db
        .read(move |tx| {
            let mut statement = tx.prepare(&format!(
                "SELECT {COLUMNS} FROM rooms
                 WHERE closed_at IS NULL AND invitation_sealed IS NOT NULL AND members >= 1
                   AND (?1 IS NULL OR build_id = ?1)
                 ORDER BY capacity - members DESC, created_at, room_id LIMIT ?2"
            ))?;
            let rows = statement.query_map(params![query.build_id, LIST_LIMIT], room_of)?;
            let mut rooms = Vec::new();
            for room in rows {
                rooms.push(room?.summary()?);
            }
            Ok(rooms)
        })
        .await?;
    Ok(ok(&RoomList { rooms }))
}

/// `POST /v1/rooms`: opens a room on the supervisor and answers its listing
/// form. The creator then asks for a ticket like any joiner (the contract's
/// `CreateRoom` has no endpoint to sign a ticket for).
pub async fn create(
    State(state): State<AppState>,
    Address(address): Address,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let supervisor = supervisor(&state)?;
    let player = playing(&state, &headers).await?;
    let command: CreateRoom = body.parse()?;
    command.check().map_err(|error| match error {
        ProtocolError::InvalidField("name") => refuse(INVALID_NAME),
        other => other.into(),
    })?;
    let room_id = hex(&random::<16>());
    let now = state.now();
    let creator = player.ember_id.to_string();
    // Reserved before the supervisor is asked, so two requests cannot both
    // pass the limits while the room host starts.
    {
        let (room_id, creator, command) = (room_id.clone(), creator.clone(), command.clone());
        state
            .db
            .write(move |tx| reserve(tx, &room_id, &creator, &address, &command, now))
            .await?;
    }
    let created = supervisor
        .create(&Create {
            room_id: &room_id,
            name: &command.name,
            capacity: command.capacity,
            build_id: &command.build_id,
            creator: &creator,
            bridge_id: &state.config.bridge_id,
            ticket_key: &state.keys.signing_key().to_b64u(),
            ticket_kid: &state.keys.signing_kid(),
        })
        .await;
    let created = match created {
        Ok(created) => created,
        Err(Failure::Refused(reason)) => {
            forget(&state, &room_id).await;
            return Err(refuse(reason));
        }
        Err(Failure::Unavailable) => {
            // It may have started the room before the answer was lost.
            supervisor.delete(&room_id).await;
            forget(&state, &room_id).await;
            return Err(ApiFailure::unavailable());
        }
    };
    let summary = RoomSummary {
        room_id: room_id.clone(),
        name: command.name,
        build_id: command.build_id,
        members: 0,
        capacity: command.capacity,
        tables_playing: 0,
        region: created.region,
        created_at: now,
    };
    let invitation_valid = !created.invitation.is_empty()
        && created.invitation.len() <= MAX_INVITATION
        && created.invitation.is_ascii();
    let stored = if summary.check().is_ok() && invitation_valid {
        let sealed = state.keys.seal(created.invitation.as_bytes());
        let (room_id, region) = (room_id.clone(), summary.region.clone());
        state
            .db
            .write(move |tx| {
                Ok(tx.execute(
                    "UPDATE rooms SET invitation_sealed = ?1, region = ?2
                     WHERE room_id = ?3 AND closed_at IS NULL",
                    params![sealed, region, room_id],
                )? == 1)
            })
            .await
            .unwrap_or(false)
    } else {
        false
    };
    if !stored {
        supervisor.delete(&room_id).await;
        forget(&state, &room_id).await;
        return Err(ApiFailure::unavailable());
    }
    Ok(json(StatusCode::CREATED, &summary))
}

/// Checks the creator's and the address's limits and records the room as
/// pending, in one write.
fn reserve(
    tx: &Transaction<'_>,
    room_id: &str,
    creator: &str,
    address: &str,
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
    if open("creator_ember_id", creator)? >= 1
        || open("creator_address", address)? >= ROOMS_PER_ADDRESS
    {
        return Err(refuse(ROOM_LIMIT));
    }
    tx.execute(
        "INSERT INTO rooms (room_id, name, capacity, build_id, creator_ember_id, creator_address, created_at)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
        params![
            room_id,
            command.name,
            command.capacity,
            command.build_id,
            creator,
            address,
            now
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

/// `POST /v1/rooms/{room_id}/tickets`: a signed ticket for the caller's
/// helper endpoint, and the room's current invitation. A room that has never
/// had a member admits only its creator.
pub async fn ticket(
    State(state): State<AppState>,
    Address(address): Address,
    headers: HeaderMap,
    Path(room_id): Path<String>,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    supervisor(&state)?;
    let player = playing(&state, &headers).await?;
    let request: TicketRequest = body.parse()?;
    request.check()?;
    let now = state.now();
    state
        .rate
        .check(
            &format!("rooms-ticket:{address}"),
            TICKETS_PER_WINDOW,
            TICKET_WINDOW_SECS,
            now,
        )
        .map_err(ApiFailure::rate_limited)?;
    let ember_id = player.ember_id.clone();
    let looked_up = room_id.clone();
    let TicketRequest {
        endpoint_id,
        build_id,
    } = request;
    let (room, sealed) = state
        .db
        .read(move |tx| {
            let room = tx
                .query_row(
                    &format!(
                        "SELECT {COLUMNS} FROM rooms
                         WHERE room_id = ?1 AND closed_at IS NULL AND invitation_sealed IS NOT NULL"
                    ),
                    [&looked_up],
                    room_of,
                )
                .optional()?
                .ok_or_else(|| refuse(ROOM_NOT_FOUND))?;
            // Until the supervisor has reported a member the room is not
            // public, and only its creator may go in first. Once it has had
            // one, that does not come back when the room empties.
            let (creator, opened): (String, Option<u64>) = tx.query_row(
                "SELECT creator_ember_id, opened_at FROM rooms WHERE room_id = ?1",
                [&looked_up],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )?;
            if opened.is_none() && creator != ember_id.as_str() {
                return Err(refuse(ROOM_NOT_OPEN));
            }
            if room.build_id != build_id {
                return Err(refuse(UNSUPPORTED_BUILD));
            }
            let banned: bool = tx.query_row(
                "SELECT EXISTS (SELECT 1 FROM room_bans WHERE room_id = ?1 AND ember_id = ?2)",
                params![looked_up, ember_id.as_str()],
                |row| row.get(0),
            )?;
            if banned {
                return Err(refuse(BANNED));
            }
            if room.members >= room.capacity {
                return Err(refuse(ROOM_FULL));
            }
            let sealed = room.invitation_sealed.clone().unwrap_or_default();
            Ok((room, sealed))
        })
        .await?;
    let invitation = state
        .keys
        .open(&sealed)
        .and_then(|plain| String::from_utf8(plain.to_vec()).ok())
        .ok_or_else(ApiFailure::unavailable)?;
    let signed = state
        .keys
        .sign_ticket(&RoomTicket {
            version: play::VERSION,
            bridge_id: state.config.bridge_id.clone(),
            room_id,
            ember_id: player.ember_id,
            endpoint_id,
            issued_at: now,
            expires_at: now + TICKET_SECS,
        })
        .map_err(|_| ApiFailure::unavailable())?;
    let admission = RoomAdmission {
        room: room.summary()?,
        invitation,
        ticket: signed,
    };
    admission.check().map_err(|_| ApiFailure::unavailable())?;
    Ok(json(StatusCode::CREATED, &admission))
}

/// Runs `poll` every few seconds, first at once so rooms left open in the
/// database are reconciled as soon as the bridge starts.
pub async fn poll_forever(state: AppState) {
    loop {
        poll(&state).await;
        tokio::time::sleep(Duration::from_secs(POLL_SECS)).await;
    }
}

/// Asks the supervisor which rooms are alive and brings the database in line:
/// member counts, invitations and kicks come in, and an open room it no
/// longer reports is closed. A failed poll changes nothing.
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
    let keys = state.keys.clone();
    let _ = state
        .db
        .write(move |tx| apply(tx, &keys, &reported, &published, now))
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
fn apply(
    tx: &Transaction<'_>,
    keys: &Keys,
    reported: &[Reported],
    published: &BTreeSet<String>,
    now: u64,
) -> Result<()> {
    let mut alive = BTreeSet::new();
    for room in reported {
        alive.insert(room.room_id.as_str());
        // A room still being created (no invitation stored yet) is left to
        // its creator's request to finish.
        let sealed = (room.invitation.len() <= MAX_INVITATION
            && room.invitation.is_ascii()
            && !room.invitation.is_empty())
        .then(|| keys.seal(room.invitation.as_bytes()));
        tx.execute(
            "UPDATE rooms SET members = MIN(?2, capacity), tables_playing = ?3,
                 invitation_sealed = COALESCE(?4, invitation_sealed),
                 opened_at = COALESCE(opened_at, CASE WHEN ?6 THEN ?5 END)
             WHERE room_id = ?1 AND closed_at IS NULL AND invitation_sealed IS NOT NULL",
            params![
                room.room_id,
                room.members.min(u32::from(u8::MAX)),
                room.tables_playing.min(u32::from(u8::MAX)),
                sealed,
                now,
                // The supervisor's latch, not the count this poll saw: a member who
                // came and went between polls has already ended creator-only entry.
                room.opened.unwrap_or(room.members >= 1),
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
    }
    // A room that was published before the supervisor was asked and is not
    // in its answer has ended. A room published since is not judged by this
    // report, whatever its reservation time.
    for room_id in published.iter().filter(|id| !alive.contains(id.as_str())) {
        tx.execute(
            "UPDATE rooms SET closed_at = ?2, members = 0, creator_address = NULL
             WHERE room_id = ?1 AND closed_at IS NULL",
            params![room_id, now],
        )?;
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

fn hex(bytes: &[u8]) -> String {
    let mut text = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        let _ = write!(text, "{byte:02x}");
    }
    text
}
