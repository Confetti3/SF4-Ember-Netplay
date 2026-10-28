//! Prints the wire contract the TypeScript SDK mirrors by hand, as one JSON
//! object, so the SDK's test can compare its own lists with the protocol's:
//!
//! ```text
//! event_types         every event family name, in `Kind::ALL` order
//! event_fields        the keys of the event envelope
//! room_states         the values of a connection room's `state`
//! room_close_reasons  the values of `reason` on `room.closed`
//! shapes              the sorted keys of each wire struct the SDK types
//! rotations           the lobby rotations
//! tournament_formats  the tournament formats
//! max_games_to_win    the largest first-to-N a set may have
//! ```
//!
//! A struct's keys come from a sample written as a struct literal with every
//! field and every optional field set, so adding a field to the protocol
//! stops this example compiling until the sample names it. A new room state
//! stops it the same way (the match in state_name names every variant).
//!
//! ```text
//! cargo run -p ember-protocol --example contract
//! ```
use ember_protocol::{
    SigningIdentity,
    encoding::Counter,
    event::{Event, Kind},
    lobby::Rotation,
    matches::MAX_GAMES_TO_WIN,
    rooms::{CLOSED_BY_CONNECTION, ConnectionRoom, ENDED, RoomState, RoomSummary},
    tournament::Format,
};
use serde::Serialize;
use serde_json::{Value, json};
use zeroize::Zeroizing;

/// The keys of the JSON a value serializes to, sorted.
fn keys<T: Serialize>(value: &T) -> Vec<String> {
    let Value::Object(map) = serde_json::to_value(value).expect("a wire struct serializes") else {
        panic!("a wire struct serializes to an object");
    };
    let mut names: Vec<String> = map.keys().cloned().collect();
    names.sort();
    names
}

/// The wire spelling of a state; the match names every variant.
fn state_name(state: RoomState) -> String {
    match state {
        RoomState::Waiting | RoomState::Open | RoomState::Closed => {}
    }
    serde_json::to_value(state)
        .expect("a state serializes")
        .as_str()
        .expect("a state is a string")
        .to_owned()
}

fn main() {
    let creator = SigningIdentity::from_seed(&Zeroizing::new([7u8; 32]))
        .expect("a fixed seed is a valid identity")
        .ember_id()
        .clone();
    let summary = RoomSummary {
        room_id: "0123456789abcdef0123456789abcdef".into(),
        name: "Room".into(),
        build_id: "build".into(),
        members: 1,
        capacity: 2,
        tables_playing: 0,
        region: "use1".into(),
        created_at: 1,
        own_delay: true,
        host_name: Some("Host".into()),
        fighters: Some(vec![0]),
        locked: Some(false),
        set_format: Some(0),
        rotation: Some(0),
    };
    let room = ConnectionRoom {
        room: summary.clone(),
        state: RoomState::Open,
        creator_ember_id: creator,
        join_url: "https://embernetplay.link/r#brg/room".into(),
    };
    let event = Event {
        specversion: "1.0".into(),
        id: "evt_00000000-0000-4000-8000-000000000000".into(),
        source: "https://bridge.example".into(),
        kind: Kind::RoomOpened.full(),
        subject: "subject".into(),
        time: "2026-01-01T00:00:00Z".into(),
        datacontenttype: "application/json".into(),
        dataschema: "urn:ember:identity-tournament:v1:room".into(),
        emberseq: Counter(1),
        data: serde_json::Map::new(),
    };
    let states = [RoomState::Waiting, RoomState::Open, RoomState::Closed].map(state_name);
    let contract = json!({
        "event_types": Kind::ALL.iter().map(|kind| kind.name()).collect::<Vec<_>>(),
        "event_fields": keys(&event),
        "room_states": states,
        "room_close_reasons": [CLOSED_BY_CONNECTION, ENDED],
        "rotations": Rotation::ALL.map(Rotation::as_str),
        "tournament_formats": Format::ALL.map(Format::as_str),
        "max_games_to_win": MAX_GAMES_TO_WIN,
        "shapes": {
            "RoomSummary": keys(&summary),
            "ConnectionRoom": keys(&room),
        },
    });
    println!(
        "{}",
        serde_json::to_string_pretty(&contract).expect("the contract serializes")
    );
}
