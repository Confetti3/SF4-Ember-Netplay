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
    rooms::{
        BANNED, CreateRoom, INVALID_NAME, ROOM_FULL, ROOM_LIMIT, ROOM_LOCKED, ROOM_NOT_FOUND,
        ROOM_NOT_OPEN, RoomAdmission, RoomList, RoomSummary, TicketRequest, UNSUPPORTED_BUILD,
        is_default_game, is_game,
    },
};
use serde_json::Value;

use super::{
    Code, Failure, Outcome, Shared,
    bridges::Approved,
    client::{self, Body, BridgeCall, Refusals, Verb},
    play::{self, bridge_key},
};

/// Open rooms one listing holds.
const MAX_LISTED: usize = 100;
/// The longest build ID the bridge accepts.
const MAX_BUILD: usize = 128;
/// A create refused for the player's own limit rather than the room
/// server's: an open room of their own, or their network's rooms.
const ROOM_LIMIT_OWN_ROOM: &str = "room_limit_own_room";
const ROOM_LIMIT_NETWORK: &str = "room_limit_network";
/// Refusals of the room routes that the game acts on.
const REASONS: [&str; 10] = [
    ROOM_LIMIT,
    ROOM_LIMIT_OWN_ROOM,
    ROOM_LIMIT_NETWORK,
    UNSUPPORTED_BUILD,
    INVALID_NAME,
    ROOM_NOT_FOUND,
    ROOM_NOT_OPEN,
    ROOM_FULL,
    BANNED,
    ROOM_LOCKED,
];

/// A 404 naming none of the room reasons is a bridge that does not offer
/// rooms.
const REFUSALS: Refusals<'static> = Refusals {
    reasons: &REASONS,
    missing: Some(Code::RoomsUnavailable),
};

fn invalid_request() -> Failure {
    Failure::new(Code::InvalidRequest)
}

/// A failed create, worded for the option it asked for. A bridge that predates
/// `own_delay` refuses the body it does not know as an invalid request, which
/// for such a create can be nothing else: the helper checked every other field
/// before sending. A create that asked for each player's own input delay and
/// was refused or answered without it is told to the player as
/// `own_delay_unsupported` and never retried as a room with the shared delay.
fn create_failure(failure: Failure, own_delay: bool) -> Failure {
    match failure {
        Failure::Bridge(code) if own_delay && code == "invalid_request" => {
            Failure::new(Code::OwnDelayUnsupported)
        }
        other => other,
    }
}

fn invalid_response() -> Failure {
    Failure::new(Code::BridgeInvalidResponse)
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

/// A game the request names, which must be one the protocol knows.
fn check_game(game: &str) -> Result<(), Failure> {
    if is_game(game) {
        Ok(())
    } else {
        Err(invalid_request())
    }
}

/// The listing's path and query: the build, `detail=1` for what the room
/// hosts reported, and the game when it is not SF4 Ember's, so an SF4
/// listing asks what it always did. An older bridge ignores the parameters it
/// does not know.
fn list_path(build: &str, game: &str) -> Result<String, Failure> {
    check_game(game)?;
    let build = build_query(build)?;
    Ok(if is_default_game(game) {
        format!("/v1/rooms?build_id={build}&detail=1")
    } else {
        format!("/v1/rooms?build_id={build}&detail=1&game={game}")
    })
}

fn check_room_id(room_id: &str) -> Result<(), Failure> {
    let hex = room_id.len() == 32
        && room_id
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b));
    if hex { Ok(()) } else { Err(invalid_request()) }
}

fn to_json<T: serde::Serialize>(value: &T) -> Result<Value, Failure> {
    serde_json::to_value(value).map_err(|_| Failure::new(Code::Internal))
}

/// The open rooms of `build` and `game`, each checked, with the details their
/// room hosts reported (`detail=1`; a bridge that does not know it leaves them
/// out). One invalid summary, or one of another game, fails the whole listing.
pub async fn list(shared: &Arc<Shared>, bridge_id: &str, build: &str, game: &str) -> Outcome {
    let path = list_path(build, game)?;
    let call = BridgeCall::new(Verb::Get, &path, &[200]).refusals(REFUSALS);
    let list: RoomList = client::request_as(shared, bridge_id, &call).await?;
    if list.rooms.len() > MAX_LISTED
        || list
            .rooms
            .iter()
            .any(|room| room.check().is_err() || room.game != game)
    {
        return Err(invalid_response());
    }
    Ok(Some(to_json(&list)?))
}

/// The ticket request this helper makes for `build` and `game`, checked. The
/// class and the developer mark are asked for only from a bridge that offers
/// ranking: an older one refuses either field.
fn ticket_request(
    endpoint_id: &str,
    build: &str,
    game: &str,
    ranking: bool,
) -> Result<TicketRequest, Failure> {
    let request = TicketRequest {
        endpoint_id: endpoint_id.to_owned(),
        build_id: build.to_owned(),
        skill_badge: ranking,
        developer_badge: ranking,
        game: game.to_owned(),
    };
    request.check().map_err(|_| invalid_request())?;
    Ok(request)
}

/// Opens a room and takes its creator's ticket in one step. The room is
/// placed in this helper's relay region when it has a known one. A room
/// opened but not entered is closed by the supervisor, so a failed ticket
/// needs no cleanup here.
pub async fn create(
    shared: &Arc<Shared>,
    bridge_id: &str,
    name: &str,
    capacity: u8,
    build: &str,
    own_delay: bool,
    game: &str,
) -> Outcome {
    let command = CreateRoom {
        name: name.to_owned(),
        capacity,
        build_id: build.to_owned(),
        region: (shared.home_region)().map(str::to_owned),
        own_delay,
        game: game.to_owned(),
    };
    command.check().map_err(|error| match error {
        ProtocolError::InvalidField("name") => Failure::new(Code::InvalidName),
        _ => invalid_request(),
    })?;
    // Nothing is opened that this helper could not then take a ticket for.
    ticket_request(&shared.endpoint_id, build, game, false)?;
    let body = serde_json::to_vec(&command).map_err(|_| Failure::new(Code::Internal))?;
    let call = BridgeCall::new(Verb::Post, "/v1/rooms", &[201])
        .body(Body::Json(&body))
        .refusals(REFUSALS)
        .timeout(client::CREATE_TIMEOUT);
    let room: RoomSummary = client::request_as(shared, bridge_id, &call)
        .await
        .and_then(|room: RoomSummary| {
            let asked = room.name == command.name
                && room.capacity == command.capacity
                && room.build_id == command.build_id
                && room.game == command.game;
            if room.check().is_err() || !asked {
                return Err(invalid_response());
            }
            // A room made without what was asked for is not entered.
            if room.own_delay != command.own_delay {
                return Err(if command.own_delay {
                    Failure::new(Code::OwnDelayUnsupported)
                } else {
                    invalid_response()
                });
            }
            Ok(room)
        })
        .map_err(|failure| create_failure(failure, own_delay))?;
    // A second session call, so a session that lapses between the two steps
    // renews without asking the bridge for another room.
    admit(shared, bridge_id, &room.room_id, build, game).await
}

/// The ticket and invitation for an open room.
pub async fn ticket(
    shared: &Arc<Shared>,
    bridge_id: &str,
    room_id: &str,
    build: &str,
    game: &str,
) -> Outcome {
    check_room_id(room_id)?;
    build_query(build)?;
    check_game(game)?;
    admit(shared, bridge_id, room_id, build, game).await
}

/// Asks for the ticket and checks the admission before it is answered.
async fn admit(
    shared: &Arc<Shared>,
    bridge_id: &str,
    room_id: &str,
    build: &str,
    game: &str,
) -> Outcome {
    ticket_request(&shared.endpoint_id, build, game, false)?;
    let path = format!("/v1/rooms/{room_id}/tickets");
    let path = &path;
    client::with_session(shared, bridge_id, |bridge, token| async move {
        // What the bridge offers, read once a run; a failed read asks as
        // an older build would, which every bridge answers.
        let ranking = play::features(shared, &bridge, play::Need::Nothing)
            .await
            .is_ok_and(|features| features.ranking);
        let body = serde_json::to_vec(&ticket_request(&shared.endpoint_id, build, game, ranking)?)
            .map_err(|_| Failure::new(Code::Internal))?;
        let call = BridgeCall::new(Verb::Post, path, &[201])
            .body(Body::Json(&body))
            .refusals(REFUSALS);
        let admission: RoomAdmission =
            client::fetch_as(shared, &bridge, Some(&token), &call).await?;
        verify(shared, &bridge, &admission, room_id, build, game).await?;
        Ok(Some(to_json(&admission)?))
    })
    .await
}

/// The admission is well formed, its ticket is signed by the approved
/// bridge's key, and it admits this endpoint, as this identity, to the room
/// asked for, of the build and game asked for.
async fn verify(
    shared: &Arc<Shared>,
    bridge: &Approved,
    admission: &RoomAdmission,
    room_id: &str,
    build: &str,
    game: &str,
) -> Result<(), Failure> {
    admission.check().map_err(|_| invalid_response())?;
    let signed = &admission.ticket;
    let key = bridge_key(shared, bridge, &signed.kid).await?;
    let ticket = signed
        .verify(&key, &signed.kid)
        .map_err(|_| Failure::new(Code::UntrustedSignature))?;
    let me = client::current_id(shared)?;
    let room = &admission.room;
    if ticket.bridge_id != bridge.bridge_id
        || ticket.endpoint_id != shared.endpoint_id
        || ticket.ember_id != me
        || ticket.room_id != room.room_id
        || room.room_id != room_id
        || room.build_id != build
        || room.game != game
    {
        return Err(Failure::new(Code::BindingMismatch));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use ember_protocol::rooms::{LIMIT_NETWORK, LIMIT_OWN_ROOM};
    use serde_json::json;

    use super::*;
    use crate::tournament::Request;

    fn request(value: Value) -> Request {
        serde_json::from_value(value).expect("a request")
    }

    fn reply(status: u16, body: &[u8], success: u16) -> Result<Value, Failure> {
        client::answer(status, body, &[success], &REFUSALS)
    }

    #[test]
    fn requests_have_the_shapes_the_game_sends() {
        let list = json!({ "op": "room_list", "bridge_id": "brg_x", "build": "b1" });
        let create = json!({ "op": "room_create", "bridge_id": "brg_x", "name": "Friendly matches", "capacity": 8, "build": "b1" });
        let own = json!({ "op": "room_create", "bridge_id": "brg_x", "name": "Friendly matches", "capacity": 8, "build": "b1", "own_delay": true });
        let ticket = json!({ "op": "room_ticket", "bridge_id": "brg_x", "room_id": "0123456789abcdef0123456789abcdef", "build": "b1" });
        for (value, op) in [
            (list, "room_list"),
            (create, "room_create"),
            (own, "room_create"),
            (ticket, "room_ticket"),
        ] {
            let parsed = request(value.clone());
            assert_eq!(parsed.op(), op);
            assert_eq!(parsed.lane(), crate::tournament::Lane::Bridge);
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
            json!({ "op": "room_create", "bridge_id": "brg_x", "name": "n", "capacity": 8, "build": "b", "own_delay": 1 }),
            json!({ "op": "room_create", "bridge_id": "brg_x", "name": "n", "capacity": 8, "build": "b", "own_delay": "yes" }),
            json!({ "op": "room_ticket", "bridge_id": "brg_x", "build": "b" }),
            json!({ "op": "room_list", "bridge_id": "brg_x", "build": "b", "game": null }),
            json!({ "op": "room_ticket", "bridge_id": "brg_x", "room_id": "0123456789abcdef0123456789abcdef", "build": "b", "game": true }),
        ];
        for value in bad {
            assert!(
                serde_json::from_value::<Request>(value.clone()).is_err(),
                "{value}"
            );
        }
    }

    /// What an older bridge answers to a create with a field it does not know:
    /// its own refusal of the body, as an invalid request.
    fn old_bridge_refusal() -> Vec<u8> {
        serde_json::to_vec(&json!({ "error": {
            "code": "invalid_request", "message": "The request body is not valid.",
            "retryable": false, "request_id": "req_1", "details": {},
        }}))
        .unwrap()
    }

    #[test]
    fn a_create_for_each_players_own_delay_fails_visibly_on_a_bridge_that_does_not_know_it() {
        let refused = reply(400, &old_bridge_refusal(), 201).unwrap_err();
        assert_eq!(refused.as_str(), "invalid_request");
        // Asked for the option, it is the option that is missing; it is never
        // retried without it, so the player is told.
        assert_eq!(
            create_failure(refused, true).as_str(),
            "own_delay_unsupported"
        );
        // The shared delay asked for nothing, so the refusal stays what it was.
        let refused = reply(400, &old_bridge_refusal(), 201).unwrap_err();
        assert_eq!(create_failure(refused, false).as_str(), "invalid_request");
        // Every other failure keeps its own word, with or without the option.
        for code in [
            "room_limit",
            "unsupported_build",
            "invalid_name",
            "unavailable",
            "http_502",
        ] {
            assert_eq!(
                create_failure(Failure::Bridge(code.into()), true).as_str(),
                code
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
        assert_eq!(REASONS.len(), 10);
        assert!(REASONS.contains(&"room_locked"));
        for reason in REASONS {
            let body = refusal("stale_revision", Some(reason));
            assert_eq!(reply(409, &body, 201).unwrap_err().as_str(), reason);
        }
        // A room limit that names whose limit it is: the player's own room or
        // their network's. A cause the game does not know leaves `room_limit`.
        let caused = |cause: &str| {
            serde_json::to_vec(&json!({ "error": {
                "code": "stale_revision", "message": "m", "retryable": false, "request_id": "req_1",
                "details": { "reason": ROOM_LIMIT, "cause": cause },
            }}))
            .unwrap()
        };
        for (cause, code) in [
            (LIMIT_OWN_ROOM, "room_limit_own_room"),
            (LIMIT_NETWORK, "room_limit_network"),
            ("something_new", "room_limit"),
        ] {
            assert_eq!(reply(409, &caused(cause), 201).unwrap_err().as_str(), code);
        }
        // A cause on another reason is not read.
        let body = serde_json::to_vec(&json!({ "error": {
            "code": "stale_revision", "message": "m", "retryable": false, "request_id": "req_1",
            "details": { "reason": ROOM_FULL, "cause": LIMIT_OWN_ROOM },
        }}))
        .unwrap();
        assert_eq!(reply(409, &body, 201).unwrap_err().as_str(), "room_full");
        // A reason the game does not know takes no cause, even when the two
        // spell a known one: `room` and `full` are not `room_full`.
        for (reason, cause) in [("room", "full"), ("room", "limit_network")] {
            let body = serde_json::to_vec(&json!({ "error": {
                "code": "stale_revision", "message": "m", "retryable": false, "request_id": "req_1",
                "details": { "reason": reason, "cause": cause },
            }}))
            .unwrap();
            assert_eq!(reply(409, &body, 201).unwrap_err().as_str(), "stale_revision");
        }
        // The reason wins over the status, so a missing room is not "no rooms".
        let body = refusal("not_found", Some(ROOM_NOT_FOUND));
        assert_eq!(
            reply(404, &body, 201).unwrap_err().as_str(),
            "room_not_found"
        );
        // A reason the game does not know falls back to the error code.
        let body = refusal("forbidden", Some("something_new"));
        assert_eq!(reply(403, &body, 201).unwrap_err().as_str(), "forbidden");
        let body = refusal("rate_limited", None);
        assert_eq!(reply(429, &body, 201).unwrap_err().as_str(), "rate_limited");
        let body = refusal("unauthenticated", None);
        assert_eq!(
            reply(401, &body, 201).unwrap_err().as_str(),
            "unauthenticated"
        );
        // A bridge without the feature answers 404 with no room reason, or
        // with no JSON at all.
        let body = refusal("not_found", None);
        assert_eq!(
            reply(404, &body, 200).unwrap_err().as_str(),
            "rooms_unavailable"
        );
        assert_eq!(
            reply(404, b"", 200).unwrap_err().as_str(),
            "rooms_unavailable"
        );
        assert_eq!(reply(502, b"", 200).unwrap_err().as_str(), "http_502");
        assert_eq!(
            reply(200, b"{\"rooms\":[]}", 200).unwrap(),
            json!({ "rooms": [] })
        );
        // Only the success status counts as success.
        assert_eq!(reply(200, b"{}", 201).unwrap_err().as_str(), "http_200");
    }

    #[test]
    fn a_ticket_asks_for_badges_only_from_a_bridge_that_offers_ranking() {
        let endpoint = "ab".repeat(32);
        let older =
            serde_json::to_value(ticket_request(&endpoint, "b1", "usf4", false).unwrap()).unwrap();
        assert_eq!(older, json!({ "endpoint_id": endpoint, "build_id": "b1" }));
        let ranking =
            serde_json::to_value(ticket_request(&endpoint, "b1", "usf4", true).unwrap()).unwrap();
        assert_eq!(ranking["skill_badge"], json!(true));
        assert_eq!(ranking["developer_badge"], json!(true));
    }

    #[test]
    fn the_listing_asks_for_details() {
        assert_eq!(
            list_path("build-1", "usf4").unwrap(),
            "/v1/rooms?build_id=build-1&detail=1"
        );
        assert_eq!(
            list_path("a b", "usf4").unwrap(),
            "/v1/rooms?build_id=a%20b&detail=1"
        );
        assert!(list_path("", "usf4").is_err());
    }

    /// An SFV Ember build ID, which holds slashes.
    const SFV_BUILD: &str = "sfv-ember/13173604/0.2/8dafe602f7615d90";

    #[test]
    fn an_sfv_listing_names_its_game_and_an_sf4_one_does_not() {
        // SF4 Ember's own build ID, a SHA-256, asks exactly what it did.
        let sf4 = "0f".repeat(32);
        assert_eq!(
            list_path(&sf4, "usf4").unwrap(),
            format!("/v1/rooms?build_id={sf4}&detail=1")
        );
        assert_eq!(
            list_path(SFV_BUILD, "sfv").unwrap(),
            "/v1/rooms?build_id=sfv-ember%2F13173604%2F0.2%2F8dafe602f7615d90&detail=1&game=sfv"
        );
        for game in ["", "sf6", "USF4", "sfv&x=1"] {
            assert_eq!(
                list_path(SFV_BUILD, game).unwrap_err().as_str(),
                "invalid_request",
                "{game}"
            );
        }
    }

    #[test]
    fn requests_name_their_game_only_when_it_is_not_sf4() {
        let sf4 = request(json!({ "op": "room_list", "bridge_id": "brg_x", "build": "b1" }));
        let Request::RoomList { game, .. } = &sf4 else {
            panic!("a listing");
        };
        assert_eq!(game, "usf4");
        // An explicit default is read and not written back.
        let explicit = request(
            json!({ "op": "room_list", "bridge_id": "brg_x", "build": "b1", "game": "usf4" }),
        );
        assert_eq!(
            serde_json::to_value(&explicit).unwrap(),
            json!({ "op": "room_list", "bridge_id": "brg_x", "build": "b1" })
        );
        for value in [
            json!({ "op": "room_list", "bridge_id": "brg_x", "build": SFV_BUILD, "game": "sfv" }),
            json!({ "op": "room_create", "bridge_id": "brg_x", "name": "Friendly matches", "capacity": 8, "build": SFV_BUILD, "game": "sfv" }),
            json!({ "op": "room_ticket", "bridge_id": "brg_x", "room_id": "0123456789abcdef0123456789abcdef", "build": SFV_BUILD, "game": "sfv" }),
        ] {
            assert_eq!(serde_json::to_value(request(value.clone())).unwrap(), value);
        }
        // Only a string is a game; which strings are games is checked before
        // any request is made.
        let bad = json!({ "op": "room_list", "bridge_id": "brg_x", "build": "b1", "game": 1 });
        assert!(serde_json::from_value::<Request>(bad).is_err());
        assert_eq!(
            ticket_request(&"ab".repeat(32), "b1", "sf6", false)
                .unwrap_err()
                .as_str(),
            "invalid_request"
        );
    }

    #[test]
    fn an_sfv_ticket_and_create_carry_the_game_and_sf4_ones_do_not() {
        let endpoint = "ab".repeat(32);
        let sfv = serde_json::to_value(ticket_request(&endpoint, SFV_BUILD, "sfv", false).unwrap())
            .unwrap();
        assert_eq!(
            sfv,
            json!({ "endpoint_id": endpoint, "build_id": SFV_BUILD, "game": "sfv" })
        );
        let ranking =
            serde_json::to_value(ticket_request(&endpoint, SFV_BUILD, "sfv", true).unwrap())
                .unwrap();
        assert_eq!(ranking["game"], "sfv");
        let create = |game: &str| CreateRoom {
            name: "Friendly matches".into(),
            capacity: 8,
            build_id: SFV_BUILD.into(),
            region: Some("euc1".into()),
            own_delay: false,
            game: game.into(),
        };
        assert_eq!(
            serde_json::to_string(&create("usf4")).unwrap(),
            r#"{"name":"Friendly matches","capacity":8,"build_id":"sfv-ember/13173604/0.2/8dafe602f7615d90","region":"euc1"}"#
        );
        assert_eq!(
            serde_json::to_string(&create("sfv")).unwrap(),
            r#"{"name":"Friendly matches","capacity":8,"build_id":"sfv-ember/13173604/0.2/8dafe602f7615d90","region":"euc1","game":"sfv"}"#
        );
    }

    #[test]
    fn builds_and_room_ids_are_checked_before_any_request() {
        assert_eq!(build_query("build-1.2_x~").unwrap(), "build-1.2_x~");
        assert_eq!(build_query("a b&c=d/e").unwrap(), "a%20b%26c%3Dd%2Fe");
        for bad in ["", "caf\u{e9}", &"b".repeat(MAX_BUILD + 1)] {
            assert_eq!(build_query(bad).unwrap_err().as_str(), "invalid_request");
        }
        assert!(build_query(&"b".repeat(MAX_BUILD)).is_ok());
        assert!(check_room_id("0123456789abcdef0123456789abcdef").is_ok());
        for bad in ["", "../v1/links", "0123456789ABCDEF0123456789ABCDEF", "abc"] {
            assert!(check_room_id(bad).is_err(), "{bad}");
        }
    }
}
