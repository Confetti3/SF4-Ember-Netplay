//! Public rooms for the native side (docs/design/PUBLIC_ROOMS.md): listing
//! the rooms a bridge hosts, creating one, and asking for the signed ticket
//! that admits this helper's endpoint to a room.
//!
//! These calls use the player session alone, like `assignment_list`; the
//! bridge's room routes take no proof of the command. What the bridge sends
//! back is checked before the game sees it: every summary against its own
//! rules, and every ticket against the approved bridge's published key and
//! against this helper's endpoint and Ember ID.
use std::sync::Arc;

use ember_protocol::{
    Error as ProtocolError,
    api::ApiError,
    rooms::{
        BANNED, CreateRoom, INVALID_NAME, ROOM_FULL, ROOM_LIMIT, ROOM_LOCKED, ROOM_NOT_FOUND,
        ROOM_NOT_OPEN, RoomAdmission, RoomList, RoomSummary, TicketRequest, UNSUPPORTED_BUILD,
    },
};
use serde_json::Value;

use super::{
    Failure, Outcome, Shared,
    bridges::Approved,
    client::{self, answer},
    play::bridge_key,
};

/// Open rooms one listing holds.
const MAX_LISTED: usize = 100;
/// The longest build ID the bridge accepts.
const MAX_BUILD: usize = 128;
/// Refusals of the room routes that the game acts on.
const REASONS: [&str; 8] = [
    ROOM_LIMIT,
    UNSUPPORTED_BUILD,
    INVALID_NAME,
    ROOM_NOT_FOUND,
    ROOM_NOT_OPEN,
    ROOM_FULL,
    BANNED,
    ROOM_LOCKED,
];

fn invalid_request() -> Failure {
    Failure::new("invalid_request")
}

fn invalid_response() -> Failure {
    Failure::new("bridge_invalid_response")
}

/// The bridge's answer as JSON when it is `success`. A refusal is the
/// bridge's own reason when it names one of the room reasons. A 404 without
/// one is a bridge that does not offer rooms. Anything else is mapped as for
/// every other request.
fn reply(status: u16, body: &[u8], success: u16) -> Result<Value, Failure> {
    if status == success {
        return answer(status, body, &[success]);
    }
    let reason = serde_json::from_slice::<ApiError>(body)
        .ok()
        .and_then(|error| error.error.details.get("reason").cloned())
        .and_then(|reason| {
            let reason = reason.as_str()?.to_owned();
            REASONS.contains(&reason.as_str()).then_some(reason)
        });
    if let Some(reason) = reason {
        return Err(Failure(reason));
    }
    if status == 404 {
        return Err(Failure::new("rooms_unavailable"));
    }
    Err(answer(status, body, &[]).unwrap_err())
}

/// A build ID as a query value: the bridge's rules, with everything outside
/// the unreserved characters percent-encoded.
fn build_query(build: &str) -> Result<String, Failure> {
    if build.is_empty() || build.len() > MAX_BUILD || !build.is_ascii() {
        return Err(invalid_request());
    }
    let mut query = String::with_capacity(build.len());
    for byte in build.bytes() {
        if byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'.' | b'_' | b'~') {
            query.push(char::from(byte));
        } else {
            query.push_str(&format!("%{byte:02X}"));
        }
    }
    Ok(query)
}

/// The listing's path and query: the build, and `detail=1` for what the room
/// hosts reported. An older bridge ignores the parameter it does not know.
fn list_path(build: &str) -> Result<String, Failure> {
    Ok(format!(
        "/v1/rooms?build_id={}&detail=1",
        build_query(build)?
    ))
}

fn check_room_id(room_id: &str) -> Result<(), Failure> {
    let hex = room_id.len() == 32
        && room_id
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b));
    if hex { Ok(()) } else { Err(invalid_request()) }
}

fn to_json<T: serde::Serialize>(value: &T) -> Result<Value, Failure> {
    serde_json::to_value(value).map_err(|_| Failure::new("internal"))
}

fn parse<T: serde::de::DeserializeOwned>(value: Value) -> Result<T, Failure> {
    serde_json::from_value(value).map_err(|_| invalid_response())
}

/// The open rooms of `build`, each checked, with the details their room hosts
/// reported (`detail=1`; a bridge that does not know it leaves them out). One
/// invalid summary fails the whole listing.
pub async fn list(shared: &Arc<Shared>, bridge_id: &str, build: &str) -> Outcome {
    let url_tail = list_path(build)?;
    client::with_session(shared, bridge_id, |bridge, token| {
        let url = format!("{}{url_tail}", bridge.origin);
        async move {
            let (status, body) = shared
                .client
                .call(reqwest::Method::GET, &url, Some(&token), None)
                .await?;
            let list: RoomList = parse(reply(status, &body, 200)?)?;
            if list.rooms.len() > MAX_LISTED || list.rooms.iter().any(|room| room.check().is_err())
            {
                return Err(invalid_response());
            }
            Ok(Some(to_json(&list)?))
        }
    })
    .await
}

/// The ticket request this helper makes for `build`, checked.
fn ticket_request(shared: &Shared, build: &str) -> Result<TicketRequest, Failure> {
    let request = TicketRequest {
        endpoint_id: shared.endpoint_id.clone(),
        build_id: build.to_owned(),
    };
    request.check().map_err(|_| invalid_request())?;
    Ok(request)
}

/// Opens a room and takes its creator's ticket in one step. A room opened
/// but not entered is closed by the supervisor, so a failed ticket needs no
/// cleanup here.
pub async fn create(
    shared: &Arc<Shared>,
    bridge_id: &str,
    name: &str,
    capacity: u8,
    build: &str,
) -> Outcome {
    let command = CreateRoom {
        name: name.to_owned(),
        capacity,
        build_id: build.to_owned(),
    };
    command.check().map_err(|error| match error {
        ProtocolError::InvalidField("name") => Failure::new(INVALID_NAME),
        _ => invalid_request(),
    })?;
    // Nothing is opened that this helper could not then take a ticket for.
    ticket_request(shared, build)?;
    let body = serde_json::to_vec(&command).map_err(|_| Failure::new("internal"))?;
    let created = client::with_session(shared, bridge_id, |bridge, token| {
        let (url, body) = (format!("{}/v1/rooms", bridge.origin), body.clone());
        let command = &command;
        async move {
            let (status, response) = shared
                .client
                .call_within(
                    reqwest::Method::POST,
                    &url,
                    Some(&token),
                    Some(body),
                    Some(client::CREATE_TIMEOUT),
                )
                .await?;
            let room: RoomSummary = parse(reply(status, &response, 201)?)?;
            let asked = room.name == command.name
                && room.capacity == command.capacity
                && room.build_id == command.build_id;
            if room.check().is_err() || !asked {
                return Err(invalid_response());
            }
            Ok(Some(to_json(&room)?))
        }
    })
    .await?;
    let room: RoomSummary = parse(created.ok_or_else(invalid_response)?)?;
    // A second session call, so a session that lapses between the two steps
    // renews without asking the bridge for another room.
    admit(shared, bridge_id, &room.room_id, build).await
}

/// The ticket and invitation for an open room.
pub async fn ticket(shared: &Arc<Shared>, bridge_id: &str, room_id: &str, build: &str) -> Outcome {
    check_room_id(room_id)?;
    build_query(build)?;
    admit(shared, bridge_id, room_id, build).await
}

/// Asks for the ticket and checks the admission before it is answered.
async fn admit(shared: &Arc<Shared>, bridge_id: &str, room_id: &str, build: &str) -> Outcome {
    let body = serde_json::to_vec(&ticket_request(shared, build)?)
        .map_err(|_| Failure::new("internal"))?;
    let path = format!("/v1/rooms/{room_id}/tickets");
    client::with_session(shared, bridge_id, |bridge, token| {
        let (url, body) = (format!("{}{path}", bridge.origin), body.clone());
        async move {
            let (status, response) = shared
                .client
                .call(reqwest::Method::POST, &url, Some(&token), Some(body))
                .await?;
            let admission: RoomAdmission = parse(reply(status, &response, 201)?)?;
            verify(shared, &bridge, &admission, room_id, build).await?;
            Ok(Some(to_json(&admission)?))
        }
    })
    .await
}

/// The admission is well formed, its ticket is signed by the approved
/// bridge's key, and it admits this endpoint, as this identity, to the room
/// asked for.
async fn verify(
    shared: &Arc<Shared>,
    bridge: &Approved,
    admission: &RoomAdmission,
    room_id: &str,
    build: &str,
) -> Result<(), Failure> {
    admission.check().map_err(|_| invalid_response())?;
    let signed = &admission.ticket;
    let key = bridge_key(shared, bridge, &signed.kid).await?;
    let ticket = signed
        .verify(&key, &signed.kid)
        .map_err(|_| Failure::new("untrusted_signature"))?;
    let me = client::current_id(shared)?;
    let room = &admission.room;
    if ticket.bridge_id != bridge.bridge_id
        || ticket.endpoint_id != shared.endpoint_id
        || ticket.ember_id != me
        || ticket.room_id != room.room_id
        || room.room_id != room_id
        || room.build_id != build
    {
        return Err(Failure::new("binding_mismatch"));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::*;
    use crate::tournament::Request;

    fn request(value: Value) -> Request {
        serde_json::from_value(value).expect("a request")
    }

    #[test]
    fn requests_have_the_shapes_the_game_sends() {
        let list = json!({ "op": "room_list", "bridge_id": "brg_x", "build": "b1" });
        let create = json!({ "op": "room_create", "bridge_id": "brg_x", "name": "Friendly matches", "capacity": 8, "build": "b1" });
        let ticket = json!({ "op": "room_ticket", "bridge_id": "brg_x", "room_id": "0123456789abcdef0123456789abcdef", "build": "b1" });
        for (value, op) in [
            (list, "room_list"),
            (create, "room_create"),
            (ticket, "room_ticket"),
        ] {
            let parsed = request(value.clone());
            assert_eq!(parsed.op(), op);
            assert!(!parsed.is_identity());
            assert_eq!(serde_json::to_value(&parsed).unwrap(), value);
        }
    }

    #[test]
    fn requests_refuse_missing_extra_and_mistyped_fields() {
        let bad = [
            json!({ "op": "room_list", "bridge_id": "brg_x" }),
            json!({ "op": "room_list", "bridge_id": "brg_x", "build": "b", "extra": 1 }),
            json!({ "op": "room_create", "bridge_id": "brg_x", "name": "n", "capacity": 300, "build": "b" }),
            json!({ "op": "room_create", "bridge_id": "brg_x", "name": "n", "capacity": "8", "build": "b" }),
            json!({ "op": "room_create", "bridge_id": "brg_x", "name": "n", "build": "b" }),
            json!({ "op": "room_ticket", "bridge_id": "brg_x", "build": "b" }),
        ];
        for value in bad {
            assert!(
                serde_json::from_value::<Request>(value.clone()).is_err(),
                "{value}"
            );
        }
    }

    #[test]
    fn a_refusal_keeps_the_bridges_reason() {
        let refusal = |code: &str, reason: Option<&str>| {
            let details = reason.map_or(json!({}), |reason| json!({ "reason": reason }));
            serde_json::to_vec(&json!({ "error": {
                "code": code, "message": "m", "retryable": false, "request_id": "req_1", "details": details,
            }}))
            .unwrap()
        };
        assert_eq!(REASONS.len(), 8);
        assert!(REASONS.contains(&"room_locked"));
        for reason in REASONS {
            let body = refusal("stale_revision", Some(reason));
            assert_eq!(reply(409, &body, 201).unwrap_err().0, reason);
        }
        // The reason wins over the status, so a missing room is not "no rooms".
        let body = refusal("not_found", Some(ROOM_NOT_FOUND));
        assert_eq!(reply(404, &body, 201).unwrap_err().0, "room_not_found");
        // A reason the game does not know falls back to the error code.
        let body = refusal("forbidden", Some("something_new"));
        assert_eq!(reply(403, &body, 201).unwrap_err().0, "forbidden");
        let body = refusal("rate_limited", None);
        assert_eq!(reply(429, &body, 201).unwrap_err().0, "rate_limited");
        let body = refusal("unauthenticated", None);
        assert_eq!(reply(401, &body, 201).unwrap_err().0, "unauthenticated");
        // A bridge without the feature answers 404 with no room reason, or
        // with no JSON at all.
        let body = refusal("not_found", None);
        assert_eq!(reply(404, &body, 200).unwrap_err().0, "rooms_unavailable");
        assert_eq!(reply(404, b"", 200).unwrap_err().0, "rooms_unavailable");
        assert_eq!(reply(502, b"", 200).unwrap_err().0, "http_502");
        assert_eq!(
            reply(200, b"{\"rooms\":[]}", 200).unwrap(),
            json!({ "rooms": [] })
        );
        // Only the success status counts as success.
        assert_eq!(reply(200, b"{}", 201).unwrap_err().0, "http_200");
    }

    #[test]
    fn the_listing_asks_for_details() {
        assert_eq!(
            list_path("build-1").unwrap(),
            "/v1/rooms?build_id=build-1&detail=1"
        );
        assert_eq!(
            list_path("a b").unwrap(),
            "/v1/rooms?build_id=a%20b&detail=1"
        );
        assert!(list_path("").is_err());
    }

    #[test]
    fn builds_and_room_ids_are_checked_before_any_request() {
        assert_eq!(build_query("build-1.2_x~").unwrap(), "build-1.2_x~");
        assert_eq!(build_query("a b&c=d/e").unwrap(), "a%20b%26c%3Dd%2Fe");
        for bad in ["", "caf\u{e9}", &"b".repeat(MAX_BUILD + 1)] {
            assert_eq!(build_query(bad).unwrap_err().0, "invalid_request");
        }
        assert!(build_query(&"b".repeat(MAX_BUILD)).is_ok());
        assert!(check_room_id("0123456789abcdef0123456789abcdef").is_ok());
        for bad in ["", "../v1/links", "0123456789ABCDEF0123456789ABCDEF", "abc"] {
            assert!(check_room_id(bad).is_err(), "{bad}");
        }
    }
}
