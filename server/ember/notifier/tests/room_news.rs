//! Room events announced like match news.
mod support;

use ember_protocol::event::Event;
use serde_json::{Value, json};
use support::{PLAYER, base_config, notifier, room};

fn event(kind: &str, data: Value) -> Event {
    serde_json::from_value(json!({
        "specversion": "1.0",
        "id": "evt_eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee",
        "source": "https://bridge.ember.example",
        "type": format!("io.ember.tournament.{kind}.v1"),
        "subject": "rooms/0123456789abcdef0123456789abcdef",
        "time": "2026-10-03T00:00:00Z",
        "datacontenttype": "application/json",
        "dataschema": format!("urn:ember:identity-tournament:v1:{kind}"),
        "emberseq": "1",
        "data": data,
    }))
    .unwrap()
}

#[test]
fn announces_a_room_from_creation_to_closing() {
    let mut config = base_config("room-news");
    config.names.insert(PLAYER.into(), "Player A".into());
    let announcer = notifier(config);
    let link = format!(
        "https://embernetplay.link/r#{}/{}",
        support::BRIDGE_ID,
        support::ROOM_ID
    );

    let created = announcer
        .render(&event("room.created", room("waiting", 0)))
        .unwrap();
    assert_eq!(created.title, "Fight Night");
    assert_eq!(
        created.text,
        format!("Player A opened a room, 0/8 players. Join: {link}")
    );

    let opened = announcer
        .render(&event("room.opened", room("open", 1)))
        .unwrap();
    assert_eq!(
        opened.text,
        format!("The room is open, 1/8 players. Join: {link}")
    );

    // The closing event carries a `reason` beside the room's own fields.
    for (reason, text) in [
        ("ended", "The room ended."),
        (
            "closed_by_connection",
            "The room was closed by its organizer.",
        ),
    ] {
        let mut data = room("closed", 0);
        data["reason"] = json!(reason);
        let closed = announcer.render(&event("room.closed", data)).unwrap();
        assert_eq!(closed.title, "Fight Night");
        assert_eq!(closed.text, text);
    }

    // Changes are not announced, and neither is data that is not a room.
    assert!(
        announcer
            .render(&event("room.changed", room("open", 2)))
            .is_none()
    );
    assert!(
        announcer
            .render(&event("room.created", json!({ "room_id": "x" })))
            .is_none()
    );

    // An unknown player shows by fingerprint, as in the match news.
    let text = notifier(base_config("room-news-anonymous"))
        .render(&event("room.created", room("waiting", 0)))
        .unwrap()
        .text;
    assert!(!text.contains("Player A"), "{text}");
}
