//! Rooms a connection opens for its linked players (docs/design/
//! INTEGRATION_PATHS.md, "Rooms for a connection"), against a stand-in room
//! supervisor: who may use the routes, the limits, the room events a
//! connection's stream carries, listing, closing, and tickets.
mod common;

use common::{
    Bridge, Player, code,
    supervisor::{
        BUILD, ENDPOINT, Supervisor, enable, fake_supervisor, report, report_details, secrets,
    },
};
use ember_bridge::config;
use ember_protocol::rooms::{
    ConnectionRoom, ConnectionRoomList, RoomAdmission, RoomList, RoomState, RoomSummary,
};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};

/// A player with a session and the participant they are linked as.
struct Member {
    player: Player,
    participant: String,
}

impl Member {
    fn id(&self) -> &str {
        self.player.id().as_str()
    }

    fn creator(&self) -> Json {
        json!({ "participant_id": self.participant, "ember_id": self.id() })
    }
}

struct Fixture {
    bridge: Bridge,
    fake: Supervisor,
    /// The provider credential of `mock-a`, which may open rooms.
    provider: String,
    /// The one of `mock-b`, which may too.
    neighbour: String,
    /// Four players linked on `mock-a`.
    members: Vec<Member>,
    /// A player linked on `mock-b` only.
    zed: Member,
    /// A player with a session, linked nowhere.
    ned: Player,
}

async fn member(bridge: &Bridge, provider: &str, connection: &str, byte: u8) -> Member {
    let mut player = bridge.player(byte);
    bridge.open_session(&mut player).await;
    let link = bridge
        .link(provider, &player, connection, &format!("subject-{byte}"))
        .await;
    Member {
        participant: link["participant_id"].as_str().unwrap().to_owned(),
        player,
    }
}

async fn fixture() -> Fixture {
    let (fake, url) = fake_supervisor().await;
    let bridge = Bridge::start_with(
        |config| {
            enable(config, url);
            for tenant in &mut config.tenants {
                for connection in &mut tenant.connections {
                    if matches!(connection.id.as_str(), "mock-a" | "mock-b") {
                        connection.rooms = Some(config::ConnectionRooms { max_open: 3 });
                    }
                }
            }
        },
        secrets(),
    )
    .await;
    let provider = bridge.provider("mock-a").await;
    let neighbour = bridge.provider("mock-b").await;
    let mut members = Vec::new();
    for byte in 1..=4 {
        members.push(member(&bridge, &provider, "mock-a", byte).await);
    }
    let zed = member(&bridge, &neighbour, "mock-b", 5).await;
    let mut ned = bridge.player(6);
    bridge.open_session(&mut ned).await;
    Fixture {
        bridge,
        fake,
        provider,
        neighbour,
        members,
        zed,
        ned,
    }
}

impl Fixture {
    async fn create(&self, token: &str, creator: &Member, name: &str) -> (StatusCode, Json) {
        self.bridge
            .post(
                token,
                "/v1/rooms",
                json!({ "name": name, "capacity": 4, "build_id": BUILD, "creator": creator.creator() }),
            )
            .await
    }

    /// A room the connection of `token` opens for `creator`.
    async fn open(&self, token: &str, creator: &Member) -> ConnectionRoom {
        let (status, body) = self.create(token, creator, "Fight Night").await;
        assert_eq!(status, StatusCode::CREATED, "{body}");
        let room: ConnectionRoom = serde_json::from_value(body).unwrap();
        room.room.check().unwrap();
        room
    }

    /// A room `player` opens from Ember, with their own session.
    async fn ember_room(&self, player: &Player) -> RoomSummary {
        let (status, body) = self
            .bridge
            .post(
                player.token(),
                "/v1/rooms",
                json!({ "name": "Friendly matches", "capacity": 4, "build_id": BUILD }),
            )
            .await;
        assert_eq!(status, StatusCode::CREATED, "{body}");
        serde_json::from_value(body).unwrap()
    }

    async fn close(&self, token: &str, room_id: &str, reason: &str) -> (StatusCode, Json) {
        self.bridge
            .post(
                token,
                &format!("/v1/rooms/{room_id}/close"),
                json!({ "reason": reason }),
            )
            .await
    }

    async fn poll(&self) {
        ember_bridge::poll_rooms(self.bridge.state()).await;
    }

    /// The supervisor reports `members` and `tables`, and the bridge polls.
    async fn reported(&self, room_id: &str, members: u32, tables: u32) {
        report(&self.fake, room_id, members, tables, &[]);
        self.poll().await;
    }

    /// The room events on the stream of `token`, as (short type, event).
    async fn room_events(&self, token: &str) -> Vec<(String, Json)> {
        self.bridge
            .events(token, "0")
            .await
            .into_iter()
            .filter_map(|event| {
                let kind = event["type"]
                    .as_str()?
                    .strip_prefix("io.ember.tournament.")?
                    .strip_suffix(".v1")?
                    .to_owned();
                kind.starts_with("room.").then_some((kind, event))
            })
            .collect()
    }

    async fn kinds(&self, token: &str) -> Vec<String> {
        self.room_events(token)
            .await
            .into_iter()
            .map(|(kind, _)| kind)
            .collect()
    }

    async fn listing(&self, token: &str) -> Vec<ConnectionRoom> {
        let (status, body) = self.bridge.get(token, "/v1/rooms").await;
        assert_eq!(status, StatusCode::OK, "{body}");
        serde_json::from_value::<ConnectionRoomList>(body)
            .unwrap()
            .rooms
    }
}

fn reason(body: &Json) -> &str {
    body["error"]["details"]["reason"].as_str().unwrap_or("")
}

#[tokio::test]
async fn only_a_connection_with_rooms_may_use_the_routes_and_players_are_unchanged() {
    let f = fixture().await;
    let kate = &f.members[0];
    let room = f.open(&f.provider, kate).await;

    // `mock-c` has no `rooms`; an organizer is no provider either.
    let without = f.bridge.provider("mock-c").await;
    let organizer = f.bridge.organizer("t1").await;
    for token in [&without, &organizer] {
        let (status, body) = f.create(token, kate, "Room").await;
        assert_eq!(
            (status, code(&body)),
            (StatusCode::FORBIDDEN, "forbidden"),
            "{body}"
        );
        let (status, body) = f.bridge.get(token, "/v1/rooms").await;
        assert_eq!(
            (status, code(&body)),
            (StatusCode::FORBIDDEN, "forbidden"),
            "{body}"
        );
    }
    let (status, _) = f
        .bridge
        .get(&without, &format!("/v1/rooms/{}", room.room.room_id))
        .await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    let (status, _) = f.close(&without, &room.room.room_id, "No").await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    assert_eq!(f.fake.lock().unwrap().created.len(), 1);

    // A player session works as it always has: their own room, answered as a
    // listing entry and listed once a member is in.
    let ember = f.ember_room(&f.ned).await;
    assert_eq!((ember.members, ember.capacity), (0, 4));
    f.reported(&ember.room_id, 1, 0).await;
    let (status, body) = f.bridge.get(f.ned.token(), "/v1/rooms").await;
    assert_eq!(status, StatusCode::OK, "{body}");
    let listed = serde_json::from_value::<RoomList>(body).unwrap().rooms;
    let ids: Vec<_> = listed.iter().map(|room| room.room_id.as_str()).collect();
    assert_eq!(ids, [ember.room_id.as_str()]);
    // The connection-only routes are not a player's.
    let (status, _) = f
        .bridge
        .get(f.ned.token(), &format!("/v1/rooms/{}", room.room.room_id))
        .await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    let (status, _) = f.close(f.ned.token(), &room.room.room_id, "No").await;
    assert_eq!(status, StatusCode::FORBIDDEN);
}

#[tokio::test]
async fn a_room_for_a_linked_player_is_hosted_and_announced_to_its_connection_only() {
    let f = fixture().await;
    let kate = &f.members[0];
    let room = f.open(&f.provider, kate).await;
    assert_eq!(room.state, RoomState::Waiting);
    assert_eq!(room.creator_ember_id.as_str(), kate.id());
    assert_eq!(
        room.join_url,
        format!(
            "https://embernetplay.link/r#{}/{}",
            f.bridge.bridge_id, room.room.room_id
        )
    );
    assert_eq!(
        (
            room.room.name.as_str(),
            room.room.capacity,
            room.room.members,
            room.room.region.as_str()
        ),
        ("Fight Night", 4, 0, "use1")
    );
    {
        let fake = f.fake.lock().unwrap();
        let sent = &fake.created[0];
        assert_eq!(sent["room_id"], room.room.room_id.as_str());
        assert_eq!(sent["creator"], kate.id());
        assert_eq!(sent["build_id"], BUILD);
        assert_eq!(sent["bridge_id"], f.bridge.bridge_id.as_str());
    }

    // Another connection opens one of its own while this one is listening.
    let other = f.open(&f.neighbour, &f.zed).await;
    let events = f.room_events(&f.provider).await;
    assert_eq!(events.len(), 1, "{events:?}");
    let (kind, event) = &events[0];
    assert_eq!(kind, "room.created");
    assert_eq!(event["subject"], format!("rooms/{}", room.room.room_id));
    assert_eq!(event["data"], serde_json::to_value(&room).unwrap());
    let theirs = f.room_events(&f.neighbour).await;
    assert_eq!(theirs.len(), 1, "{theirs:?}");
    assert_eq!(
        theirs[0].1["subject"],
        format!("rooms/{}", other.room.room_id)
    );
    // Nor does a connection without rooms hear of either.
    let without = f.bridge.provider("mock-c").await;
    assert!(f.room_events(&without).await.is_empty());
}

#[tokio::test]
async fn creation_is_refused_for_players_not_linked_or_already_in_a_room() {
    let f = fixture().await;
    let (kate, kim) = (&f.members[0], &f.members[2]);

    // Not linked on this connection: nobody, a player with a session, or one
    // linked on another connection, even under a participant that is real here.
    let stranger = Member {
        player: f.bridge.player(7),
        participant: kate.participant.clone(),
    };
    for creator in [&f.zed, &stranger] {
        let (status, body) = f.create(&f.provider, creator, "Room").await;
        assert_eq!(
            (status, reason(&body)),
            (StatusCode::FORBIDDEN, "not_linked"),
            "{body}"
        );
    }
    let unlinked = Member {
        player: f.bridge.player(6),
        participant: f.zed.participant.clone(),
    };
    let (status, body) = f.create(&f.provider, &unlinked, "Room").await;
    assert_eq!(
        (status, reason(&body)),
        (StatusCode::FORBIDDEN, "not_linked"),
        "{body}"
    );
    assert!(f.fake.lock().unwrap().created.is_empty());

    // An open room this connection made for the player: its ID comes back so
    // a bot can hand out the existing link.
    let first = f.open(&f.provider, kate).await;
    let (status, body) = f.create(&f.provider, kate, "Again").await;
    assert_eq!(
        (status, reason(&body)),
        (StatusCode::CONFLICT, "room_limit"),
        "{body}"
    );
    assert_eq!(
        body["error"]["details"]["room_id"],
        first.room.room_id.as_str()
    );

    // One the player made in Ember is theirs to keep: the limit, no room ID.
    f.ember_room(&kim.player).await;
    let (status, body) = f.create(&f.provider, kim, "Mine").await;
    assert_eq!(
        (status, reason(&body)),
        (StatusCode::CONFLICT, "room_limit"),
        "{body}"
    );
    assert!(body["error"]["details"].get("room_id").is_none(), "{body}");
    // None of the refusals reached the supervisor: only the two rooms made.
    assert_eq!(f.fake.lock().unwrap().created.len(), 2);
}

#[tokio::test]
async fn max_open_limits_the_connection_and_the_address_limit_does_not_apply() {
    let f = fixture().await;
    // Every request here comes from one address, which would stop a player at
    // two rooms; the connection may have its three.
    let mut rooms = Vec::new();
    for member in &f.members[..3] {
        rooms.push(f.open(&f.provider, member).await);
    }
    let lee = &f.members[3];
    let (status, body) = f.create(&f.provider, lee, "Fourth").await;
    assert_eq!(
        (status, reason(&body)),
        (StatusCode::CONFLICT, "room_limit"),
        "{body}"
    );
    assert!(body["error"]["details"].get("room_id").is_none(), "{body}");
    // A full connection still hands back a creator's own room, so a repeated
    // command gets its link.
    let (status, body) = f.create(&f.provider, &f.members[0], "Again").await;
    assert_eq!(
        (status, reason(&body)),
        (StatusCode::CONFLICT, "room_limit"),
        "{body}"
    );
    assert_eq!(
        body["error"]["details"]["room_id"],
        rooms[0].room.room_id.as_str()
    );
    // Another connection counts its own.
    f.open(&f.neighbour, &f.zed).await;
    // Closing one frees a place.
    let (status, body) = f.close(&f.provider, &rooms[0].room.room_id, "Done").await;
    assert_eq!(status, StatusCode::OK, "{body}");
    f.open(&f.provider, lee).await;
    // The same address still stops players making their own at two.
    f.ember_room(&f.ned).await;
    let mut second = f.bridge.player(8);
    f.bridge.open_session(&mut second).await;
    f.ember_room(&second).await;
    let mut third = f.bridge.player(9);
    f.bridge.open_session(&mut third).await;
    let (status, body) = f
        .bridge
        .post(
            third.token(),
            "/v1/rooms",
            json!({ "name": "Third", "capacity": 4, "build_id": BUILD }),
        )
        .await;
    assert_eq!(
        (status, reason(&body)),
        (StatusCode::CONFLICT, "room_limit"),
        "{body}"
    );
}

#[tokio::test]
async fn the_supervisors_reports_become_at_most_one_event_per_room_per_poll() {
    let f = fixture().await;
    let room = f.open(&f.provider, &f.members[0]).await;
    let id = room.room.room_id.clone();
    f.poll().await;
    assert_eq!(f.kinds(&f.provider).await, ["room.created"]);

    // The first member opens it; the count changed too, but that is one event.
    f.reported(&id, 1, 0).await;
    assert_eq!(f.kinds(&f.provider).await, ["room.created", "room.opened"]);
    let events = f.room_events(&f.provider).await;
    let opened = &events[1].1["data"];
    assert_eq!(
        (opened["state"].as_str(), opened["room"]["members"].as_u64()),
        (Some("open"), Some(1))
    );

    // Members and tables both change in one poll: one event.
    f.reported(&id, 3, 1).await;
    let events = f.room_events(&f.provider).await;
    assert_eq!(events.len(), 3, "{events:?}");
    assert_eq!(events[2].0, "room.changed");
    let changed = &events[2].1["data"];
    assert_eq!(
        (
            changed["room"]["members"].as_u64(),
            changed["room"]["tables_playing"].as_u64()
        ),
        (Some(3), Some(1))
    );
    // Nothing changed: nothing is said.
    f.poll().await;
    assert_eq!(f.room_events(&f.provider).await.len(), 3);
    // Members leaving is a change as well.
    f.reported(&id, 0, 0).await;
    assert_eq!(f.kinds(&f.provider).await.last().unwrap(), "room.changed");

    // The supervisor no longer has the room: it ended.
    f.fake.lock().unwrap().rooms.clear();
    f.poll().await;
    let events = f.room_events(&f.provider).await;
    assert_eq!(
        events
            .iter()
            .map(|(kind, _)| kind.as_str())
            .collect::<Vec<_>>(),
        [
            "room.created",
            "room.opened",
            "room.changed",
            "room.changed",
            "room.closed"
        ]
    );
    let closed = &events[4].1["data"];
    assert_eq!(closed["reason"], "ended");
    assert_eq!(closed["state"], "closed");
    assert_eq!(events[4].1["subject"], format!("rooms/{id}"));
    f.poll().await;
    assert_eq!(f.room_events(&f.provider).await.len(), 5);
}

#[tokio::test]
async fn a_connection_lists_and_reads_only_its_own_rooms() {
    let f = fixture().await;
    let (kate, sam, kim) = (&f.members[0], &f.members[1], &f.members[2]);
    let first = f.open(&f.provider, kate).await;
    f.bridge.clock.advance(2);
    let second = f.open(&f.provider, sam).await;
    f.bridge.clock.advance(2);
    let third = f.open(&f.provider, kim).await;
    let theirs = f.open(&f.neighbour, &f.zed).await;
    let ember = f.ember_room(&f.ned).await;

    let ids = |rooms: &[ConnectionRoom]| -> Vec<String> {
        rooms.iter().map(|room| room.room.room_id.clone()).collect()
    };
    // Newest first, whether anyone is in them or not; no one else's.
    assert_eq!(
        ids(&f.listing(&f.provider).await),
        [&third, &second, &first].map(|room| room.room.room_id.clone())
    );
    assert_eq!(
        ids(&f.listing(&f.neighbour).await),
        std::slice::from_ref(&theirs.room.room_id)
    );

    // A closed room leaves the listing but can still be read.
    let (status, closed) = f.close(&f.provider, &second.room.room_id, "Done").await;
    assert_eq!(status, StatusCode::OK, "{closed}");
    assert_eq!(
        ids(&f.listing(&f.provider).await),
        [&third, &first].map(|room| room.room.room_id.clone())
    );
    let (status, read) = f
        .bridge
        .get(&f.provider, &format!("/v1/rooms/{}", second.room.room_id))
        .await;
    assert_eq!(status, StatusCode::OK, "{read}");
    assert_eq!(
        serde_json::from_value::<ConnectionRoom>(read)
            .unwrap()
            .state,
        RoomState::Closed
    );
    let (status, read) = f
        .bridge
        .get(&f.provider, &format!("/v1/rooms/{}", first.room.room_id))
        .await;
    assert_eq!(status, StatusCode::OK, "{read}");
    assert_eq!(
        serde_json::from_value::<ConnectionRoom>(read).unwrap(),
        first
    );

    // Another connection's room, a player's own room and one that never
    // existed are all the same answer.
    for id in [
        theirs.room.room_id.as_str(),
        ember.room_id.as_str(),
        &"e".repeat(32),
    ] {
        let (status, body) = f.bridge.get(&f.provider, &format!("/v1/rooms/{id}")).await;
        assert_eq!(
            (status, reason(&body)),
            (StatusCode::NOT_FOUND, "room_not_found"),
            "{id}: {body}"
        );
        let (status, body) = f.close(&f.provider, id, "Not mine").await;
        assert_eq!(
            (status, reason(&body)),
            (StatusCode::NOT_FOUND, "room_not_found"),
            "{id}: {body}"
        );
    }
    assert_eq!(
        f.fake.lock().unwrap().deleted,
        std::slice::from_ref(&second.room.room_id)
    );
}

#[tokio::test]
async fn closing_a_room_closes_it_on_the_supervisor_and_says_so_once() {
    let f = fixture().await;
    let kate = &f.members[0];
    let room = f.open(&f.provider, kate).await;
    let id = room.room.room_id.clone();
    f.reported(&id, 2, 0).await;

    let (status, body) = f.close(&f.provider, &id, "").await;
    assert_eq!(status, StatusCode::BAD_REQUEST, "{body}");
    assert!(f.fake.lock().unwrap().deleted.is_empty());

    let (status, body) = f.close(&f.provider, &id, "Night is over").await;
    assert_eq!(status, StatusCode::OK, "{body}");
    let closed: ConnectionRoom = serde_json::from_value(body.clone()).unwrap();
    assert_eq!(
        (
            closed.state,
            closed.room.members,
            closed.room.tables_playing
        ),
        (RoomState::Closed, 0, 0)
    );
    assert_eq!(closed.room.room_id, id);
    assert_eq!(f.fake.lock().unwrap().deleted, std::slice::from_ref(&id));

    // Closing it again answers the same, asks nothing and says nothing more.
    let (status, again) = f.close(&f.provider, &id, "Night is over").await;
    assert_eq!((status, again), (StatusCode::OK, body));
    f.poll().await;
    assert_eq!(f.fake.lock().unwrap().deleted, std::slice::from_ref(&id));
    let events = f.room_events(&f.provider).await;
    assert_eq!(
        events
            .iter()
            .map(|(kind, _)| kind.as_str())
            .collect::<Vec<_>>(),
        ["room.created", "room.opened", "room.closed"]
    );
    assert_eq!(events[2].1["data"]["reason"], "closed_by_connection");
    assert_eq!(events[2].1["data"]["state"], "closed");
    // The creator is free to have another room.
    f.open(&f.provider, kate).await;
}

#[tokio::test]
async fn a_supervisor_that_fails_the_close_leaves_the_room_open() {
    let f = fixture().await;
    let room = f.open(&f.provider, &f.members[0]).await;
    let id = room.room.room_id.clone();
    f.reported(&id, 1, 0).await;

    f.fake.lock().unwrap().fail_delete = true;
    let (status, body) = f.close(&f.provider, &id, "Night is over").await;
    assert_eq!(
        (status, code(&body)),
        (StatusCode::SERVICE_UNAVAILABLE, "service_unavailable"),
        "{body}"
    );
    let listed = f.listing(&f.provider).await;
    assert_eq!(listed.len(), 1);
    assert_eq!(listed[0].state, RoomState::Open);
    assert_eq!(f.kinds(&f.provider).await, ["room.created", "room.opened"]);

    // Once the supervisor answers it closes, as if nothing had failed.
    f.fake.lock().unwrap().fail_delete = false;
    let (status, body) = f.close(&f.provider, &id, "Night is over").await;
    assert_eq!(status, StatusCode::OK, "{body}");
    assert_eq!(f.kinds(&f.provider).await.last().unwrap(), "room.closed");
    assert!(f.listing(&f.provider).await.is_empty());
}

#[tokio::test]
async fn tickets_for_a_connections_room_go_to_its_creator_first_then_anyone() {
    let f = fixture().await;
    let (kate, sam) = (&f.members[0], &f.members[1]);
    let room = f.open(&f.provider, kate).await;
    let id = room.room.room_id.clone();
    let ticket = |token: String| {
        let (bridge, id) = (&f.bridge, id.clone());
        async move {
            bridge
                .post(
                    &token,
                    &format!("/v1/rooms/{id}/tickets"),
                    json!({ "endpoint_id": ENDPOINT, "build_id": BUILD }),
                )
                .await
        }
    };

    // Until a member is in, the room is not public.
    let (status, body) = ticket(sam.player.token().to_owned()).await;
    assert_eq!(
        (status, reason(&body)),
        (StatusCode::NOT_FOUND, "room_not_open"),
        "{body}"
    );
    let (status, body) = ticket(kate.player.token().to_owned()).await;
    assert_eq!(status, StatusCode::CREATED, "{body}");
    let admission: RoomAdmission = serde_json::from_value(body).unwrap();
    admission.check().unwrap();
    assert_eq!(admission.invitation, format!("sf4e3:{id}"));
    assert_eq!(admission.ticket.ticket.ember_id.as_str(), kate.id());

    f.reported(&id, 1, 0).await;
    let (status, body) = ticket(sam.player.token().to_owned()).await;
    assert_eq!(status, StatusCode::CREATED, "{body}");
    assert_eq!(
        serde_json::from_value::<RoomAdmission>(body)
            .unwrap()
            .ticket
            .ticket
            .ember_id
            .as_str(),
        sam.id()
    );
    // Not for a provider credential: tickets are a player's.
    let (status, _) = ticket(f.provider.clone()).await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    // Nor once the room is closed.
    let (status, _) = f.close(&f.provider, &id, "Done").await;
    assert_eq!(status, StatusCode::OK);
    let (status, body) = ticket(sam.player.token().to_owned()).await;
    assert_eq!(
        (status, reason(&body)),
        (StatusCode::NOT_FOUND, "room_not_found"),
        "{body}"
    );
}

/// The host's name and capacity are in a connection's view, so changing them
/// is a `room.changed`; the listing details are not, so changing only those
/// says nothing, and the view never carries them.
#[tokio::test]
async fn a_rename_or_new_capacity_is_a_change_but_listing_details_are_not() {
    let f = fixture().await;
    let room = f.open(&f.provider, &f.members[0]).await;
    let id = room.room.room_id.clone();
    f.reported(&id, 1, 0).await;
    assert_eq!(f.kinds(&f.provider).await, ["room.created", "room.opened"]);

    report_details(&f.fake, &id, json!({ "name": "Renamed", "capacity": 4 }));
    f.poll().await;
    let events = f.room_events(&f.provider).await;
    assert_eq!(events.len(), 3, "{events:?}");
    assert_eq!(events[2].0, "room.changed");
    assert_eq!(events[2].1["data"]["room"]["name"], "Renamed");

    report_details(&f.fake, &id, json!({ "name": "Renamed", "capacity": 6 }));
    f.poll().await;
    let events = f.room_events(&f.provider).await;
    assert_eq!(events.len(), 4, "{events:?}");
    assert_eq!(events[3].1["data"]["room"]["capacity"], 6);

    // Only the extras change: nothing is said, and they are not in the view.
    report_details(
        &f.fake,
        &id,
        json!({ "name": "Renamed", "capacity": 6, "locked": true, "host_name": "Kate",
            "fighters": [3], "set_format": 2, "rotation": 1 }),
    );
    f.poll().await;
    let events = f.room_events(&f.provider).await;
    assert_eq!(events.len(), 4, "{events:?}");
    let (status, body) = f.bridge.get(&f.provider, &format!("/v1/rooms/{id}")).await;
    assert_eq!(status, StatusCode::OK, "{body}");
    assert_eq!(body["room"]["name"], "Renamed");
    let listed = f.listing(&f.provider).await;
    let view = serde_json::to_value(&listed[0]).unwrap();
    for key in ["host_name", "fighters", "locked", "set_format", "rotation"] {
        assert!(view["room"].get(key).is_none(), "{key}");
    }
}
