//! Public rooms (docs/design/PUBLIC_ROOMS.md): a listing of the rooms a room
//! supervisor hosts on this machine, creating one, and a signed ticket per
//! join. The supervisor is the source of truth for which rooms are alive; the
//! bridge keeps its last report (`poll`) and serves the listing from it. The
//! report also carries what the room host says about its room (`details`): its
//! name and capacity as the moderator last set them, and the extras a player's
//! listing shows when it asks for `detail=1`.
//!
//! A player creates a room for themselves from Ember. A connection whose
//! configuration has `rooms` creates one for a player linked on it, and sees
//! and closes the rooms it created, with events on its stream
//! (docs/design/INTEGRATION_PATHS.md). Each route resolves its `Caller` once.
//!
//! On when the configuration has `rooms` and the integration secrets hold the
//! supervisor's secret; otherwise the routes answer `not_found` and the
//! `rooms` capability is not offered.
mod create;
mod details;
mod poll;
mod supervisor;
mod ticket;

use std::{
    convert::Infallible,
    fmt::Write as _,
    net::IpAddr,
    sync::atomic::{AtomicBool, AtomicU64},
};

use axum::{
    extract::{FromRequestParts, Path, Query, State},
    http::{HeaderMap, StatusCode, request::Parts},
    response::Response,
};
use ember_protocol::{
    EmberId,
    api::ErrorCode,
    event::Kind,
    rooms::{
        BANNED, CLOSED_BY_CONNECTION, CloseRoom, ConnectionCreateRoom, ConnectionRoom,
        ConnectionRoomList, CreateRoom, INVALID_NAME, NOT_LINKED, ROOM_FULL, ROOM_LIMIT,
        ROOM_LOCKED, RoomList, RoomState, RoomSummary, UNSUPPORTED_BUILD, join_url,
    },
};
use rusqlite::{OptionalExtension, Row, Transaction, params};
use serde_json::Value;

use self::{
    create::{Owner, open},
    supervisor::Supervisor,
};
pub use self::{
    poll::{poll, poll_forever},
    ticket::ticket,
};
use crate::{
    AppState,
    auth::{self, Actor, Player},
    config::Config,
    error::{ApiFailure, Result},
    events::{NewEvent, emit},
    http::{Body, GENERAL_BODY, json, ok},
    routes::play::require_play,
};

/// The capability this feature is advertised as.
pub const FEATURE: &str = "rooms";
/// Connections whose configuration allows it may open rooms for players.
pub const CONNECTION_FEATURE: &str = "rooms.connections";
/// Rooms in one listing.
const LIST_LIMIT: i64 = 100;
/// A creation the supervisor never answered is given up on after this.
const PENDING_SECS: u64 = 60;
const MAX_BUILD: usize = 128;

/// Per-process state of the room routes.
pub struct Shared {
    /// One poll at a time, from the supervisor's answer to its commit, so a
    /// slow poll never writes over a newer one.
    poll: tokio::sync::Mutex<()>,
    /// The last poll failed; it is logged on the way down and on the way up.
    down: AtomicBool,
    /// Polls started, counted so a create refused for the room limit can
    /// reuse a poll that began after its refusal instead of queueing another.
    polls: AtomicU64,
}

impl Default for Shared {
    fn default() -> Self {
        Self {
            poll: tokio::sync::Mutex::new(()),
            down: AtomicBool::new(false),
            polls: AtomicU64::new(0),
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
        Ok(Self(hex(&digest[..16])))
    }
}

/// Who is calling a room route: a player from Ember, or a connection allowed
/// to open rooms for its players.
enum Caller {
    Player(Player),
    Connection { connection_id: String, max_open: u8 },
}

impl Caller {
    async fn of(state: &AppState, headers: &HeaderMap) -> Result<Self> {
        match auth::any(state, headers).await? {
            Actor::Player(player) => Ok(Self::Player(require_play(player)?)),
            Actor::Service(service) => {
                let connection_id = service.provider_connection()?.to_owned();
                let max_open = state
                    .config
                    .connection(&connection_id)
                    .and_then(|(_, connection)| connection.rooms.as_ref())
                    .map(|rooms| rooms.max_open)
                    .ok_or_else(ApiFailure::forbidden)?;
                Ok(Self::Connection {
                    connection_id,
                    max_open,
                })
            }
            Actor::Browser(_) => Err(ApiFailure::forbidden()),
        }
    }

    /// The connection, for the routes only a connection may use.
    fn connection(self) -> Result<String> {
        match self {
            Self::Connection { connection_id, .. } => Ok(connection_id),
            Self::Player(_) => Err(ApiFailure::forbidden()),
        }
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
        ROOM_LOCKED => (ErrorCode::StaleRevision, "The room is locked."),
        BANNED => (ErrorCode::Forbidden, "You were removed from this room."),
        NOT_LINKED => (
            ErrorCode::Forbidden,
            "That player is not linked on this connection.",
        ),
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
    creator_ember_id: String,
    opened_at: Option<u64>,
    closed_at: Option<u64>,
    connection_id: Option<String>,
    /// What the room host last reported for the listing; NULL until it has,
    /// and for any one the bridge found invalid (`details`).
    host_name: Option<String>,
    fighters: Option<Vec<u8>>,
    locked: Option<bool>,
    set_format: Option<u8>,
    rotation: Option<u8>,
}

const COLUMNS: &str = "room_id, name, build_id, capacity, members, tables_playing, created_at, region, invitation_sealed,
    creator_ember_id, opened_at, closed_at, connection_id, host_name, fighters, locked, set_format, rotation";

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
        creator_ember_id: row.get(9)?,
        opened_at: row.get(10)?,
        closed_at: row.get(11)?,
        connection_id: row.get(12)?,
        host_name: row.get(13)?,
        fighters: row.get(14)?,
        locked: row.get(15)?,
        set_format: row.get(16)?,
        rotation: row.get(17)?,
    })
}

fn load(tx: &Transaction<'_>, room_id: &str) -> Result<Option<Room>> {
    Ok(tx
        .query_row(
            &format!("SELECT {COLUMNS} FROM rooms WHERE room_id = ?1"),
            [room_id],
            room_of,
        )
        .optional()?)
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
            host_name: None,
            fighters: None,
            locked: None,
            set_format: None,
            rotation: None,
        };
        summary.check().map_err(|_| ApiFailure::unavailable())?;
        Ok(summary)
    }

    /// The listing form a player asks for with `detail=1`: the summary and
    /// whatever the room host reported. A report that does not hold together
    /// is left out whole, and the room is listed as it would be without one.
    fn listing(&self) -> Result<RoomSummary> {
        let mut summary = self.summary()?;
        let detailed = RoomSummary {
            host_name: self.host_name.clone(),
            fighters: self.fighters.clone(),
            locked: self.locked,
            set_format: self.set_format,
            rotation: self.rotation,
            ..summary.clone()
        };
        if detailed.details_valid() {
            summary = detailed;
        }
        Ok(summary)
    }

    /// The form the connection that created it sees.
    fn view(&self, config: &Config) -> Result<ConnectionRoom> {
        let state = match (self.closed_at, self.opened_at) {
            (Some(_), _) => RoomState::Closed,
            (None, Some(_)) => RoomState::Open,
            (None, None) => RoomState::Waiting,
        };
        Ok(ConnectionRoom {
            room: self.summary()?,
            state,
            creator_ember_id: EmberId::parse(&self.creator_ember_id)
                .map_err(|_| ApiFailure::unavailable())?,
            join_url: join_url(&config.bridge_id, &self.room_id),
        })
    }
}

/// Records a room event for the connection that created `room`; a room a
/// player created from Ember has none.
fn announce(
    tx: &Transaction<'_>,
    config: &Config,
    now: u64,
    kind: Kind,
    room: &Room,
    reason: Option<&str>,
) -> Result<()> {
    let Some(connection_id) = &room.connection_id else {
        return Ok(());
    };
    let tenant_id: String = tx.query_row(
        "SELECT tenant_id FROM provider_connections WHERE id = ?1",
        [connection_id],
        |row| row.get(0),
    )?;
    let mut data =
        serde_json::to_value(room.view(config)?).map_err(|_| ApiFailure::unavailable())?;
    if let (Some(reason), Value::Object(fields)) = (reason, &mut data) {
        fields.insert("reason".into(), reason.into());
    }
    emit(
        tx,
        config,
        now,
        NewEvent {
            kind,
            tenant_id: &tenant_id,
            connection_id: Some(connection_id),
            subject: format!("rooms/{}", room.room_id),
            match_id: None,
            ember_id: None,
            lobby_id: None,
            tournament_id: None,
            data,
        },
    )?;
    Ok(())
}

/// A build filter or request build: what the protocol accepts as a build ID.
fn build_ok(build_id: &str) -> bool {
    !build_id.is_empty() && build_id.len() <= MAX_BUILD && build_id.is_ascii()
}

#[derive(serde::Deserialize)]
pub struct ListQuery {
    #[serde(default)]
    build_id: Option<String>,
    /// 1 adds what the room hosts reported, and when the listing was made.
    #[serde(default)]
    detail: Option<u8>,
}

/// `GET /v1/rooms?build_id=&detail=`: for a player, open rooms with at least
/// one member, unlocked ones first and those with the most free seats first
/// (with `detail=1` also what the room hosts reported and `listed_at`); for a
/// connection, the rooms it created that are not closed, newest first.
pub async fn list(
    State(state): State<AppState>,
    headers: HeaderMap,
    Query(query): Query<ListQuery>,
) -> Result<Response> {
    supervisor(&state)?;
    let caller = Caller::of(&state, &headers).await?;
    if query
        .build_id
        .as_deref()
        .is_some_and(|build| !build_ok(build))
    {
        return Err(ApiFailure::invalid("build_id is not a build ID."));
    }
    let config = state.config.clone();
    let detailed = query.detail == Some(1);
    let now = state.now();
    let body = state
        .db
        .read(move |tx| match caller {
            Caller::Player(_) => {
                let mut statement = tx.prepare(&format!(
                    "SELECT {COLUMNS} FROM rooms
                     WHERE closed_at IS NULL AND invitation_sealed IS NOT NULL AND members >= 1
                       AND (?1 IS NULL OR build_id = ?1)
                     ORDER BY COALESCE(locked, 0), capacity - members DESC, created_at, room_id LIMIT ?2"
                ))?;
                let rows = statement.query_map(params![query.build_id, LIST_LIMIT], room_of)?;
                let mut rooms = Vec::new();
                for room in rows {
                    let room = room?;
                    rooms.push(if detailed {
                        room.listing()?
                    } else {
                        room.summary()?
                    });
                }
                Ok(serde_json::to_value(RoomList {
                    rooms,
                    listed_at: detailed.then_some(now),
                }))
            }
            Caller::Connection { connection_id, .. } => {
                let mut statement = tx.prepare(&format!(
                    "SELECT {COLUMNS} FROM rooms
                     WHERE connection_id = ?1 AND closed_at IS NULL AND invitation_sealed IS NOT NULL
                       AND (?2 IS NULL OR build_id = ?2)
                     ORDER BY created_at DESC, room_id LIMIT ?3"
                ))?;
                let rows =
                    statement.query_map(params![connection_id, query.build_id, LIST_LIMIT], room_of)?;
                let mut rooms = Vec::new();
                for room in rows {
                    rooms.push(room?.view(&config)?);
                }
                Ok(serde_json::to_value(ConnectionRoomList { rooms }))
            }
        })
        .await?
        .map_err(|_| ApiFailure::unavailable())?;
    Ok(ok(&body))
}

/// `POST /v1/rooms`: opens a room on the supervisor. A player's room is
/// their own, answered in its listing form; the creator then asks for a
/// ticket like any joiner (the contract's `CreateRoom` has no endpoint to
/// sign a ticket for). A connection's is for the linked player it names,
/// answered as the connection sees it.
pub async fn create(
    State(state): State<AppState>,
    Address(address): Address,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let supervisor = supervisor(&state)?;
    match Caller::of(&state, &headers).await? {
        Caller::Player(player) => {
            let command: CreateRoom = body.parse()?;
            let room = open(
                &state,
                &supervisor,
                command,
                player.ember_id,
                Owner::Address(address),
            )
            .await?;
            Ok(json(StatusCode::CREATED, &room.summary()?))
        }
        Caller::Connection {
            connection_id,
            max_open,
        } => {
            let command: ConnectionCreateRoom = body.parse()?;
            command.check().map_err(create::refusal)?;
            let creator = command.creator.ember_id.clone();
            let owner = Owner::Connection {
                connection_id,
                participant_id: command.creator.participant_id.clone(),
                max_open,
            };
            let room = open(&state, &supervisor, command.room(), creator, owner).await?;
            Ok(json(StatusCode::CREATED, &room.view(&state.config)?))
        }
    }
}

/// `GET /v1/rooms/{room_id}`: one of the calling connection's rooms, closed
/// ones included while the bridge keeps them.
pub async fn get(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(room_id): Path<String>,
) -> Result<Response> {
    supervisor(&state)?;
    let connection_id = Caller::of(&state, &headers).await?.connection()?;
    let config = state.config.clone();
    let view = state
        .db
        .read(move |tx| owned(tx, &room_id, &connection_id)?.view(&config))
        .await?;
    Ok(ok(&view))
}

/// A hosted room the connection created, or `room_not_found`.
fn owned(tx: &Transaction<'_>, room_id: &str, connection_id: &str) -> Result<Room> {
    load(tx, room_id)?
        .filter(|room| {
            room.connection_id.as_deref() == Some(connection_id) && room.region.is_some()
        })
        .ok_or_else(|| refuse(ember_protocol::rooms::ROOM_NOT_FOUND))
}

/// `POST /v1/rooms/{room_id}/close`: the supervisor closes the room, then it
/// is recorded closed. Closing a closed room answers it again.
pub async fn close(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(room_id): Path<String>,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let supervisor = supervisor(&state)?;
    let connection_id = Caller::of(&state, &headers).await?.connection()?;
    let command: CloseRoom = body.parse()?;
    command.check()?;
    let (looked_up, owner) = (room_id.clone(), connection_id.clone());
    let room = state
        .db
        .read(move |tx| owned(tx, &looked_up, &owner))
        .await?;
    // No poll judges the room between its host going and its record closing,
    // so the close is this connection's, with its reason.
    let _one_at_a_time = state.rooms.poll.lock().await;
    if room.closed_at.is_none() {
        // The room is recorded closed only once the supervisor has told its
        // host to close, so a room recorded closed is on its way out.
        supervisor
            .delete(&room_id)
            .await
            .map_err(|_| ApiFailure::unavailable())?;
    }
    let config = state.config.clone();
    let now = state.now();
    let view = state
        .db
        .write(move |tx| {
            let mut room = owned(tx, &room_id, &connection_id)?;
            if room.closed_at.is_none() {
                tx.execute(
                    "UPDATE rooms SET closed_at = ?2, members = 0, tables_playing = 0, closed_reason = ?3,
                         creator_address = NULL
                     WHERE room_id = ?1",
                    params![room_id, now, CLOSED_BY_CONNECTION],
                )?;
                (room.closed_at, room.members, room.tables_playing) = (Some(now), 0, 0);
                announce(tx, &config, now, Kind::RoomClosed, &room, Some(CLOSED_BY_CONNECTION))?;
            }
            room.view(&config)
        })
        .await?;
    state.committed();
    Ok(ok(&view))
}

fn hex(bytes: &[u8]) -> String {
    let mut text = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        let _ = write!(text, "{byte:02x}");
    }
    text
}
