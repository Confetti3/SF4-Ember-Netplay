//! `POST /v1/rooms/{room_id}/tickets`: a signed ticket for the caller's
//! helper endpoint, and the room's current invitation. Players only.
use axum::{
    extract::{Path, State},
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::{
    play,
    rooms::{
        BANNED, ROOM_FULL, ROOM_NOT_FOUND, ROOM_NOT_OPEN, RoomAdmission, RoomTicket, TICKET_SECS,
        TicketRequest, UNSUPPORTED_BUILD,
    },
};
use rusqlite::params;

use super::{Address, load, refuse, supervisor};
use crate::{
    AppState,
    error::{ApiFailure, Result},
    http::{Body, GENERAL_BODY, json},
    routes::play::playing,
};

/// Ticket requests one address may make per window.
const TICKETS_PER_WINDOW: usize = 30;
const TICKET_WINDOW_SECS: u64 = 10 * 60;

/// A room that has never had a member admits only its creator.
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
            let room = load(tx, &looked_up)?
                .filter(|room| room.closed_at.is_none() && room.invitation_sealed.is_some())
                .ok_or_else(|| refuse(ROOM_NOT_FOUND))?;
            // Until the supervisor has reported a member the room is not
            // public, and only its creator may go in first. Once it has had
            // one, that does not come back when the room empties.
            if room.opened_at.is_none() && room.creator_ember_id != ember_id.as_str() {
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
