//! A room bound to a tournament match (docs/design/MATCH_WATCH.md 5.3, 5.4):
//! the binding read from the committed checkpoint, who may open a control,
//! and a watcher's join. Voters and retirement are in `retirement.rs`.
use iroh::endpoint::ConnectionError;
use serde_json::{Value, json};

use super::*;
use crate::{
    invite::RoomProof,
    recovery::RecoverySession,
    service::bound::{Binding, VoterPolicy},
};

/// The tournament object the room model writes into its snapshot
/// (`RoomModelTournament.cxx`), naming `fighters` and `watchers`.
fn tournament(fighters: &[EndpointId], watchers: &[EndpointId]) -> Value {
    let named = |endpoints: &[EndpointId], first: usize| {
        endpoints
            .iter()
            .enumerate()
            .map(|(at, endpoint)| {
                json!({ "endpoint": endpoint.to_string(), "ember_id": format!("emb_{}", first + at) })
            })
            .collect::<Vec<_>>()
    };
    let mut binding = json!({
        "match_id": "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12",
        "assignment_generation": 1,
        "binding_revision": 2,
        "games_to_win": 2,
        "fighters": named(fighters, 1),
    });
    if !watchers.is_empty() {
        binding["watchers"] = json!(named(watchers, 3));
    }
    binding
}

/// A committed envelope: the native roster, and the room snapshot's
/// tournament object.
fn checkpoint(members: &[EndpointId], tournament: Value) -> String {
    let members = members
        .iter()
        .map(|endpoint| json!({ "data": { "authenticatedEndpoint": endpoint.to_string() } }))
        .collect::<Vec<_>>();
    json!({ "checkpoint": {
        "members": members,
        "room": { "version": 2, "snapshot": { "revision": 4, "tournament": tournament } },
    }})
    .to_string()
}

fn bound(fighters: &[EndpointId], watchers: &[EndpointId]) -> BoundRoster {
    BoundRoster {
        fighters: fighters.iter().copied().collect(),
        watchers: watchers.iter().copied().collect(),
    }
}

fn read(text: &str) -> Binding {
    BoundRoster::read(&serde_json::from_str(text).unwrap())
}

fn key() -> EndpointId {
    iroh::SecretKey::generate().public()
}

#[test]
fn the_binding_is_read_from_the_committed_room_snapshot() {
    let (a, b, w) = (key(), key(), key());
    assert_eq!(
        read(&checkpoint(&[a], tournament(&[a, b], &[w]))),
        Binding::Bound(bound(&[a, b], &[w]))
    );
    assert_eq!(
        read(&checkpoint(&[a], tournament(&[a, b], &[]))),
        Binding::Bound(bound(&[a, b], &[]))
    );
    // An unbound room writes an empty object, and an older one none at all.
    assert_eq!(read(&checkpoint(&[a], json!({}))), Binding::Unbound);
    assert_eq!(read(&checkpoint(&[a], Value::Null)), Binding::Unbound);
    let mut bare: Value = serde_json::from_str(&checkpoint(&[a], json!({}))).unwrap();
    bare["checkpoint"]["room"]["snapshot"]
        .as_object_mut()
        .unwrap()
        .remove("tournament");
    assert_eq!(BoundRoster::read(&bare), Binding::Unbound);
    // Not the shape the room model writes: a binding, but not one to read,
    // which is not the same as no binding.
    let unreadable = [
        tournament(&[a], &[]),
        tournament(&[a, b, w], &[]),
        tournament(&[a, b], &(0..9).map(|_| key()).collect::<Vec<_>>()),
        json!({ "fighters": [{ "ember_id": "emb_1" }, { "endpoint": b.to_string() }] }),
        json!({ "fighters": "both" }),
        json!("bound"),
    ];
    for binding in unreadable {
        assert_eq!(
            read(&checkpoint(&[a], binding.clone())),
            Binding::Unreadable,
            "{binding}"
        );
    }
    // Hex that is no key names no one, and the binding holds for the rest.
    let not_a_key = "d".repeat(64);
    assert!(not_a_key.parse::<EndpointId>().is_err());
    let mut binding = tournament(&[a, b], &[w, w]);
    binding["watchers"][1]["endpoint"] = json!(not_a_key);
    assert_eq!(
        read(&checkpoint(&[a], binding)),
        Binding::Bound(bound(&[a, b], &[w]))
    );
}

// The contract with the room model, whose to_json test writes this file.
#[test]
fn the_room_models_bound_checkpoint_reads_as_it_was_written() {
    let fixture = include_str!(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../src/tests/fixtures/tournament_watch_checkpoint.json"
    ));
    let endpoint = |hex: char| hex.to_string().repeat(64).parse::<EndpointId>().unwrap();
    let watchers = serde_json::from_str::<Value>(fixture).unwrap()["checkpoint"]["room"]
        ["snapshot"]["tournament"]["watchers"]
        .as_array()
        .unwrap()
        .iter()
        .map(|watcher| watcher["endpoint"].as_str().unwrap().parse::<EndpointId>().unwrap())
        .collect::<Vec<_>>();
    assert_eq!(watchers.len(), 2);
    assert_eq!(
        read(fixture),
        Binding::Bound(bound(&[endpoint('a'), endpoint('b')], &watchers))
    );
}

#[test]
fn only_a_bound_rooms_fighters_vote() {
    let (a, b, w) = (key(), key(), key());
    let everyone = BTreeSet::from([a, b, w]);
    let unbound = VoterPolicy(None);
    assert!(everyone.iter().all(|endpoint| unbound.allows(endpoint)));
    assert!(unbound.allows_member(&BTreeMap::new(), 7));
    let fighters = VoterPolicy(Some(BTreeSet::from([a, b])));
    assert!(fighters.allows(&a) && fighters.allows(&b) && !fighters.allows(&w));
    // An incarnation with no admission on record never votes in a bound room.
    assert!(!fighters.allows_member(&BTreeMap::new(), 7));
    let route = iroh::SecretKey::generate().public();
    let admission = |incarnation, primary_endpoint| Admission {
        room: [1; 16],
        incarnation,
        authority_term: 1,
        coordination_endpoint: route,
        coordination_address: EndpointAddr::new(route),
        primary_endpoint,
    };
    let admissions = BTreeMap::from([(1, admission(1, a)), (2, admission(2, w))]);
    assert!(fighters.allows_member(&admissions, 1));
    assert!(!fighters.allows_member(&admissions, 2));
}

/// Binds `actor`'s room to `fighters` the way a refresh leaves it: in the
/// roster cache the gate and the voter policy read.
fn bind(actor: &mut Actor, fighters: &[EndpointId], watchers: &[EndpointId]) {
    let room = [9; 16];
    actor.room = Some(room);
    actor.refresh_roster = Some((
        room,
        Arc::new(crate::coordination::Committed::default()),
        Some(CommittedRoom {
            members: BTreeSet::new(),
            bound: Some(bound(fighters, watchers)),
            unreadable: false,
        }),
    ));
}

// The one count every site uses: a room that is not bound counts everyone, as
// it always did, and a bound one counts only its fighters, however many
// watch.
#[tokio::test]
async fn the_voter_count_is_made_in_one_place_and_counts_only_voters() {
    let own = endpoint().await;
    let (events, _events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(own.clone(), events);
    let (a, b) = (key(), key());
    let watchers = (0..14).map(|_| key()).collect::<Vec<_>>();
    let everyone = [a, b]
        .into_iter()
        .chain(watchers.iter().copied())
        .collect::<BTreeSet<_>>();
    for count in 0..=everyone.len() {
        let roster = everyone
            .iter()
            .copied()
            .take(count)
            .collect::<BTreeSet<_>>();
        assert_eq!(
            actor.desired_voters(&roster),
            crate::recovery::stable_voter_count(count),
            "{count} members"
        );
    }
    bind(&mut actor, &[a, b], &watchers);
    for count in 0..=everyone.len() {
        let roster = everyone
            .iter()
            .copied()
            .take(count)
            .collect::<BTreeSet<_>>();
        let fighters = roster.iter().filter(|id| [a, b].contains(id)).count();
        assert_eq!(
            actor.desired_voters(&roster),
            crate::recovery::stable_voter_count(fighters),
            "{count} members"
        );
        assert_eq!(actor.desired_voters(&roster), 1, "{count} members");
    }
    own.close().await;
}
#[tokio::test]
async fn a_server_owned_room_is_never_bound() {
    let own = endpoint().await;
    let (events, _events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(own.clone(), events);
    let (a, b) = (key(), key());
    let text = checkpoint(&[a, b], tournament(&[a, b], &[]));
    assert!(
        actor
            .committed_room(text.as_bytes())
            .unwrap()
            .bound
            .is_some()
    );
    let invite = Invite::create(
        own.id(),
        test_relay(),
        "test-build".into(),
        now().unwrap(),
        60,
    )
    .unwrap();
    actor.public = Some(crate::public_room::PublicRoom::Member {
        ticket: super::public_support::ticket_for(
            &invite,
            &super::public_support::identity(1),
            &own,
        ),
    });
    assert!(
        actor
            .committed_room(text.as_bytes())
            .unwrap()
            .bound
            .is_none()
    );
    own.close().await;
}

#[tokio::test]
async fn a_proof_says_observer_only_when_it_is_one() {
    let host = key();
    let invite =
        Invite::create(host, test_relay(), "test-build".into(), now().unwrap(), 60).unwrap();
    let plain = serde_json::to_value(invite.proof()).unwrap();
    assert!(plain.get("observer").is_none(), "{plain}");
    let watching = RoomProof {
        observer: true,
        ..invite.proof()
    };
    assert_eq!(
        serde_json::to_value(&watching).unwrap()["observer"],
        json!(true)
    );
    let read: RoomProof = serde_json::from_value(plain).unwrap();
    assert!(!read.observer);
    // The IPC join likewise.
    let join = |observer| Command::Join {
        epoch: 1,
        invitation: "x".into(),
        build: "b".into(),
        observer,
    };
    assert!(
        serde_json::to_value(join(false))
            .unwrap()
            .get("observer")
            .is_none()
    );
    let sent =
        json!({ "type": "join", "epoch": 1, "invitation": "x", "build": "b", "observer": true });
    assert!(matches!(
        serde_json::from_value::<Command>(sent).unwrap(),
        Command::Join { observer: true, .. }
    ));
}

#[tokio::test]
async fn a_watcher_joins_with_the_full_invitation_only() {
    let own = endpoint().await;
    let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(own.clone(), events_tx);
    let invite =
        Invite::create(key(), test_relay(), "test-build".into(), now().unwrap(), 60).unwrap();
    let join = Command::Join {
        epoch: 1,
        invitation: invite.short_code(),
        build: "test-build".into(),
        observer: true,
    };
    assert!(
        actor
            .command(Request {
                id: 9,
                command: join
            })
            .unwrap()
    );
    let refused = timeout(Duration::from_secs(5), events.recv()).await;
    match refused.expect("an answer").unwrap() {
        Event::Error { code, reason, .. } => {
            assert_eq!(code, "invalid_or_incompatible_invitation");
            assert_eq!(reason.as_deref(), Some("observer_short_link"));
        }
        _ => panic!("expected the join to be refused"),
    }
    assert!(actor.room.is_none() && actor.tasks.is_empty());
    own.close().await;
}

/// Dials the host with `proof`.
async fn dial(
    from: &Endpoint,
    host: &Endpoint,
    proof: &RoomProof,
) -> io::Result<transport::ControlChannel> {
    let connection = from.connect(address(host), CONTROL_ALPN).await.unwrap();
    transport::connect_control_on_proof(connection, proof, Some(host.id())).await
}

/// Connects to the host and waits for it to close the connection, before
/// any handshake, answering the reason it gave.
async fn refusal(from: &Endpoint, host: &Endpoint) -> Vec<u8> {
    let connection = from.connect(address(host), CONTROL_ALPN).await.unwrap();
    match timeout(Duration::from_secs(10), connection.closed()).await {
        Ok(ConnectionError::ApplicationClosed(close)) => close.reason.to_vec(),
        other => panic!("not refused by the host: {other:?}"),
    }
}

/// Commits `checkpoint` on the leader's coordination.
async fn commit(recovery: &RecoverySession, checkpoint: String) {
    let term = recovery.state().await.term;
    let base = recovery.committed().await.revision();
    let receipt = recovery
        .coordinator
        .propose(crate::coordination::Proposal {
            request: format!("watch-{base}"),
            dedup_id: format!("watch-{base}"),
            term,
            base,
            checkpoint,
            admin: None,
        })
        .await
        .unwrap();
    assert!(receipt.accepted);
}

#[tokio::test]
async fn a_bound_room_takes_a_control_only_from_an_endpoint_it_names() {
    timeout(Duration::from_secs(60), async {
        let mut fixture = bound_host(|_| {}).await;
        let (host, fighter) = (fixture.host.clone(), fixture.remote.clone());
        let (watcher, stranger) = (endpoint().await, endpoint().await);
        commit(
            &fixture.recovery,
            checkpoint(
                &[host.id()],
                tournament(&[host.id(), fighter.id()], &[watcher.id()]),
            ),
        )
        .await;
        let revision = fixture.recovery.committed().await.revision();
        // The gate reads the roster the next refresh caches.
        loop {
            if let Event::CoordinationState { revision: read, .. } =
                next(&mut fixture.events, "coordination_state").await
                && read >= revision
            {
                break;
            }
        }
        let plain = fixture.invite.proof();
        let observing = RoomProof {
            observer: true,
            ..fixture.invite.proof()
        };
        assert_eq!(refusal(&stranger, &host).await, b"not admitted");
        assert!(dial(&stranger, &host, &plain).await.is_err());
        // A named watcher may say it only watches; a fighter joins as ever.
        assert!(dial(&watcher, &host, &observing).await.is_ok());
        let _ = next(&mut fixture.events, "connected").await;
        assert!(dial(&fighter, &host, &plain).await.is_ok());
        let _ = next(&mut fixture.events, "connected").await;
        fixture.recovery.stop().await;
        for endpoint in [host, fighter, watcher, stranger] {
            endpoint.close().await;
        }
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn a_room_without_a_binding_refuses_a_joiner_that_only_watches() {
    timeout(Duration::from_secs(60), async {
        let mut fixture = bound_host(|_| {}).await;
        let observing = RoomProof {
            observer: true,
            ..fixture.invite.proof()
        };
        assert!(
            dial(&fixture.remote, &fixture.host, &observing)
                .await
                .is_err()
        );
        assert!(
            dial(&fixture.remote, &fixture.host, &fixture.invite.proof())
                .await
                .is_ok()
        );
        let _ = next(&mut fixture.events, "connected").await;
        fixture.recovery.stop().await;
        fixture.host.close().await;
        fixture.remote.close().await;
    })
    .await
    .unwrap();
}

// The game's join says it only watches, and the room's helper hears it: a
// room with no binding refuses that join and takes a plain one.
#[tokio::test]
async fn a_watchers_join_tells_the_room_it_only_watches() {
    use iroh::{
        address_lookup::MemoryLookup,
        endpoint::{PortmapperConfig, presets},
    };
    timeout(Duration::from_secs(60), async {
        let fixture = bound_host(|_| {}).await;
        let lookup = MemoryLookup::new();
        lookup.add_endpoint_info(address(&fixture.host));
        let own = Endpoint::builder(presets::Minimal)
            .address_lookup(lookup)
            .clear_ip_transports()
            .bind_addr((Ipv4Addr::LOCALHOST, 0))
            .unwrap()
            .portmapper_config(PortmapperConfig::Disabled)
            .alpns(vec![CONTROL_ALPN.to_vec(), GAME_ALPN.to_vec()])
            .bind()
            .await
            .unwrap();
        let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut guest = test_actor(own.clone(), events_tx);
        let join = |epoch, observer| Request {
            id: epoch,
            command: Command::Join {
                epoch,
                invitation: fixture.invite.encode().unwrap(),
                build: "test-build".into(),
                observer,
            },
        };
        assert!(guest.command(join(1, true)).unwrap());
        complete_next(&mut guest).await;
        let refused = loop {
            match events.recv().await.unwrap() {
                Event::Error { code, .. } => break code,
                Event::Connected { .. } => panic!("an unbound room took a watcher"),
                _ => (),
            }
        };
        assert_eq!(refused, "join_failed");
        assert!(guest.command(join(2, false)).unwrap());
        complete_next(&mut guest).await;
        let _ = next(&mut events, "connected").await;
        guest.clear_room();
        fixture.recovery.stop().await;
        fixture.host.close().await;
        own.close().await;
    })
    .await
    .unwrap();
}

#[path = "watch/retirement.rs"]
mod retirement;
