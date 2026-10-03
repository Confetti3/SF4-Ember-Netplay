//! Public (server-owned) rooms in the actor: admission by ticket, the account
//! on `Connected`, bans, the room marker and the IPC shapes.
use ember_protocol::{
    EmberId, SigningIdentity,
    play::VERSION as TICKET_VERSION,
    rooms::{RoomTicket, TICKET_SECS},
};
use zeroize::Zeroizing;

use super::public_support::{BRIDGE, BUILD, hex, host_state, identity, public_host, ticket_for};
use super::*;
use crate::public_room::{MAX_BANS, PublicRoom};

#[tokio::test]
async fn a_member_with_a_ticket_connects_and_the_event_names_its_account() {
    timeout(Duration::from_secs(30), async {
        let mut fixture = public_host().await;
        let (remote, player) = (endpoint().await, identity(1));
        let _control = fixture
            .dial(&remote, &ticket_for(&fixture.invite, &player, &remote))
            .await
            .unwrap();
        let Event::Connected {
            peer,
            room,
            account,
            ..
        } = next(&mut fixture.events, "connected").await
        else {
            unreachable!()
        };
        assert_eq!(peer, remote.id());
        assert_eq!(room, fixture.invite.room());
        assert_eq!(account.as_deref(), Some(player.ember_id().as_str()));
        fixture.host.close().await;
        remote.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn a_public_host_refuses_the_plain_room_proof() {
    timeout(Duration::from_secs(30), async {
        let mut fixture = public_host().await;
        let remote = endpoint().await;
        assert!(fixture.dial_plain(&remote).await.is_err());
        fixture.no_event("connected").await;
        fixture.host.close().await;
        remote.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn a_second_endpoint_under_one_account_is_refused_and_the_first_is_kept() {
    timeout(Duration::from_secs(30), async {
        let mut fixture = public_host().await;
        let (first, second, player) = (endpoint().await, endpoint().await, identity(1));
        let _held = fixture
            .dial(&first, &ticket_for(&fixture.invite, &player, &first))
            .await
            .unwrap();
        let _ = next(&mut fixture.events, "connected").await;
        let newcomer = ticket_for(&fixture.invite, &player, &second);
        assert!(fixture.dial(&second, &newcomer).await.is_err());
        // Nothing was installed or closed for it.
        loop {
            let Ok(event) = timeout(Duration::from_millis(600), fixture.events.recv()).await else {
                break;
            };
            match event.unwrap() {
                Event::Connected { .. } | Event::ControlClosed { .. } => {
                    panic!("the refused endpoint left a trace")
                }
                _ => (),
            }
        }
        // Another account from that endpoint is welcome.
        let other = identity(2);
        let _other = fixture
            .dial(&second, &ticket_for(&fixture.invite, &other, &second))
            .await
            .unwrap();
        let Event::Connected { account, .. } = next(&mut fixture.events, "connected").await else {
            unreachable!()
        };
        assert_eq!(account.as_deref(), Some(other.ember_id().as_str()));
        fixture.host.close().await;
        first.close().await;
        second.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn the_same_endpoint_may_present_its_ticket_again() {
    timeout(Duration::from_secs(30), async {
        let mut fixture = public_host().await;
        let (remote, player) = (endpoint().await, identity(1));
        let ticket = ticket_for(&fixture.invite, &player, &remote);
        let _first = fixture.dial(&remote, &ticket).await.unwrap();
        let _ = next(&mut fixture.events, "connected").await;
        let _again = fixture.dial(&remote, &ticket).await.unwrap();
        let Event::Connected { account, .. } = next(&mut fixture.events, "connected").await else {
            unreachable!()
        };
        assert_eq!(account.as_deref(), Some(player.ember_id().as_str()));
        fixture.host.close().await;
        remote.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn the_creator_comes_first_and_the_rule_does_not_return_when_the_room_empties() {
    timeout(Duration::from_secs(30), async {
        let mut fixture = public_host().await;
        let (first, second, third) = (endpoint().await, endpoint().await, endpoint().await);
        let (creator, guest, late) = (identity(1), identity(2), identity(3));
        // The room has no member yet: only its creator may be the first.
        assert!(
            fixture
                .dial(&second, &ticket_for(&fixture.invite, &guest, &second))
                .await
                .is_err()
        );
        fixture.no_event("connected").await;
        let held = fixture
            .dial(&first, &ticket_for(&fixture.invite, &creator, &first))
            .await
            .unwrap();
        let Event::Connected { account, .. } = next(&mut fixture.events, "connected").await else {
            unreachable!()
        };
        assert_eq!(account.as_deref(), Some(creator.ember_id().as_str()));
        // Now anyone with a ticket is welcome.
        let other = fixture
            .dial(&second, &ticket_for(&fixture.invite, &guest, &second))
            .await
            .unwrap();
        let _ = next(&mut fixture.events, "connected").await;
        // Everyone leaves; another account may be first now.
        held.connection.close(0u32.into(), b"left");
        other.connection.close(0u32.into(), b"left");
        let _ = next(&mut fixture.events, "control_closed").await;
        let _ = next(&mut fixture.events, "control_closed").await;
        let _again = fixture
            .dial(&third, &ticket_for(&fixture.invite, &late, &third))
            .await
            .unwrap();
        let Event::Connected { account, .. } = next(&mut fixture.events, "connected").await else {
            unreachable!()
        };
        assert_eq!(account.as_deref(), Some(late.ember_id().as_str()));
        fixture.host.close().await;
        first.close().await;
        second.close().await;
        third.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn banning_an_account_closes_its_control_and_keeps_it_out() {
    timeout(Duration::from_secs(30), async {
        let mut fixture = public_host().await;
        let (remote, bystander, player, other) =
            (endpoint().await, endpoint().await, identity(1), identity(2));
        let mut control = fixture
            .dial(&remote, &ticket_for(&fixture.invite, &player, &remote))
            .await
            .unwrap();
        let _bystander = fixture
            .dial(&bystander, &ticket_for(&fixture.invite, &other, &bystander))
            .await
            .unwrap();
        let _ = next(&mut fixture.events, "connected").await;
        let _ = next(&mut fixture.events, "connected").await;

        fixture
            .commands
            .send(Request {
                id: 5,
                command: Command::BanAccount {
                    epoch: 1,
                    account: player.ember_id().to_string(),
                },
            })
            .await
            .unwrap();
        let Event::ControlClosed { peer, .. } = next(&mut fixture.events, "control_closed").await
        else {
            unreachable!()
        };
        assert_eq!(peer, remote.id());
        // The banned member's end sees the connection go.
        assert!(control.receiver.receive().await.is_err());
        // It cannot come back with a fresh ticket, from this endpoint or any
        // other; the account that was not banned is unaffected.
        assert!(
            fixture
                .dial(&remote, &ticket_for(&fixture.invite, &player, &remote))
                .await
                .is_err()
        );
        let third = endpoint().await;
        assert!(
            fixture
                .dial(&third, &ticket_for(&fixture.invite, &player, &third))
                .await
                .is_err()
        );
        fixture.no_event("connected").await;
        fixture.no_event("control_closed").await;
        fixture.host.close().await;
        remote.close().await;
        bystander.close().await;
        third.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn banning_on_a_private_room_or_with_bad_input_is_an_error() {
    let own = endpoint().await;
    let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(own.clone(), events_tx);
    actor.epoch = 1;
    let account = identity(1).ember_id().to_string();
    let ban = |epoch, account: &str| Request {
        id: 8,
        command: Command::BanAccount {
            epoch,
            account: account.into(),
        },
    };
    // A private room.
    assert!(actor.command(ban(1, &account)).unwrap());
    assert!(matches!(
        events.recv().await.unwrap(),
        Event::Error { request_id: 8, ref code, .. } if code == "invalid_room_state"
    ));
    // A public host, with a stale epoch and with something that is no ID.
    actor.public = Some(PublicRoom::Host(host_state()));
    assert!(actor.command(ban(2, &account)).unwrap());
    assert!(matches!(
        events.recv().await.unwrap(),
        Event::Error { ref code, .. } if code == "stale_epoch"
    ));
    assert!(actor.command(ban(1, "not an id")).unwrap());
    assert!(matches!(
        events.recv().await.unwrap(),
        Event::Error { ref code, .. } if code == "invalid_request"
    ));
    assert!(events.try_recv().is_err());
    own.close().await;
}

#[tokio::test]
async fn a_ban_past_the_limit_is_an_error_and_keeps_every_ban() {
    let own = endpoint().await;
    let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(own.clone(), events_tx);
    actor.epoch = 1;
    let numbered = |index: usize| {
        let mut seed = [0u8; 32];
        seed[..8].copy_from_slice(&(index as u64 + 1).to_le_bytes());
        SigningIdentity::from_seed(&Zeroizing::new(seed)).unwrap()
    };
    let mut host = host_state();
    for index in 0..MAX_BANS {
        host.ban(numbered(index).ember_id().clone()).unwrap();
    }
    actor.public = Some(PublicRoom::Host(host));
    let (first, extra) = (numbered(0), numbered(MAX_BANS));
    let ban = |account: &EmberId| Request {
        id: 9,
        command: Command::BanAccount {
            epoch: 1,
            account: account.to_string(),
        },
    };
    assert!(actor.command(ban(extra.ember_id())).unwrap());
    assert!(matches!(
        events.recv().await.unwrap(),
        Event::Error { request_id: 9, ref code, .. } if code == "ban_limit"
    ));
    let Some(PublicRoom::Host(host)) = &actor.public else {
        unreachable!()
    };
    assert!(!host.is_banned(extra.ember_id()));
    assert!(host.is_banned(first.ember_id()));
    // A repeat of a held ban is not an error.
    assert!(actor.command(ban(first.ember_id())).unwrap());
    assert!(events.try_recv().is_err());
    own.close().await;
}

#[tokio::test]
async fn host_public_marks_the_room_server_owned_and_refuses_a_bad_key() {
    let own = endpoint().await;
    let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(own.clone(), events_tx);
    let host_public_in = |epoch, key: &str, room| Request {
        id: 3,
        command: Command::HostPublic {
            epoch,
            build: BUILD.into(),
            room,
            ticket_key: key.into(),
            ticket_kid: "k1".into(),
            bridge_id: BRIDGE.into(),
            creator: identity(1).ember_id().to_string(),
        },
    };
    let host_public = |epoch, key: &str| host_public_in(epoch, key, [5; 16]);
    assert!(actor.command(host_public(1, "not a key")).unwrap());
    assert!(matches!(
        events.recv().await.unwrap(),
        Event::Error { epoch: 1, request_id: 3, ref code, .. } if code == "invalid_request"
    ));
    assert!(!actor.server_owned() && !actor.opening);

    // An all-zero room id is refused like a bad key, and the epoch is free.
    let key = identity(9).public_key().to_b64u();
    assert!(actor.command(host_public_in(2, &key, [0; 16])).unwrap());
    assert!(matches!(
        events.recv().await.unwrap(),
        Event::Error { epoch: 2, request_id: 3, ref code, .. } if code == "invalid_request"
    ));
    assert!(!actor.server_owned() && !actor.opening);

    assert!(actor.command(host_public(3, &key)).unwrap());
    assert!(actor.server_owned() && actor.opening);
    assert!(actor.is_public_host());
    assert!(events.try_recv().is_err());
    // Leaving clears the marker with the rest of the room.
    actor.clear_room();
    assert!(!actor.server_owned());

    // A private host is not server-owned.
    assert!(
        actor
            .command(Request {
                id: 4,
                command: Command::Host {
                    epoch: 4,
                    build: BUILD.into(),
                },
            })
            .unwrap()
    );
    assert!(!actor.server_owned() && actor.opening);
    own.close().await;
}

#[tokio::test]
async fn join_public_marks_the_room_server_owned_and_keeps_the_ticket() {
    let own = endpoint().await;
    let authority = endpoint().await;
    let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(own.clone(), events_tx);
    let invite = joined_invite(&authority);
    let ticket = ticket_for(&invite, &identity(1), &own);
    let join_public = |epoch, invitation: String| Request {
        id: 6,
        command: Command::JoinPublic {
            epoch,
            invitation,
            ticket: ticket.clone(),
            build: BUILD.into(),
        },
    };
    assert!(
        actor
            .command(join_public(1, invite.encode().unwrap()))
            .unwrap()
    );
    assert!(actor.server_owned() && !actor.is_public_host());
    assert_eq!(actor.member_ticket().as_ref(), Some(&ticket));
    assert!(events.try_recv().is_err());
    actor.clear_room();
    assert!(!actor.server_owned() && actor.member_ticket().is_none());

    // A refused invitation clears the marker and names the epoch asked for.
    assert!(actor.command(join_public(2, "sf4e3:bad".into())).unwrap());
    assert!(matches!(
        events.recv().await.unwrap(),
        Event::Error { epoch: 2, request_id: 6, ref code, .. }
            if code == "invalid_or_incompatible_invitation"
    ));
    assert!(!actor.server_owned());
    own.close().await;
    authority.close().await;
}

#[tokio::test]
async fn a_member_of_a_public_room_is_never_dialed() {
    timeout(Duration::from_secs(30), async {
        let member = endpoint().await;
        let remote = endpoint().await;
        let invite = Invite::create(
            member.id(),
            test_relay(),
            BUILD.into(),
            now().unwrap(),
            3600,
        )
        .unwrap();
        let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_fault, failure) = watch::channel(false);
        let mut actor = test_actor(member.clone(), events_tx);
        actor.epoch = 1;
        actor.room = Some(invite.room());
        actor.room_invite = Some(invite.clone());
        actor.public = Some(PublicRoom::Member {
            ticket: ticket_for(&invite, &identity(1), &member),
        });
        let service = tokio::spawn(async move {
            let result = actor.run(command_rx, failure).await;
            actor.clear_room();
            actor.tasks.shutdown().await;
            result
        });
        let _scope = TaskScope(vec![service.abort_handle()]);
        let connection = remote
            .connect(address(&member), CONTROL_ALPN)
            .await
            .unwrap();
        assert!(
            transport::connect_control_on(connection, &invite)
                .await
                .is_err()
        );
        let waited = timeout(Duration::from_millis(600), next(&mut events, "connected")).await;
        assert!(waited.is_err());
        member.close().await;
        remote.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn the_coordination_endpoint_binds_the_given_port_or_hosting_fails() {
    let own = endpoint().await;
    let free = std::net::UdpSocket::bind((Ipv4Addr::UNSPECIFIED, 0))
        .unwrap()
        .local_addr()
        .unwrap()
        .port();
    let session = crate::recovery::RecoverySession::host_on([5; 16], own.id(), false, Some(free))
        .await
        .unwrap();
    // The port is held by the coordination endpoint now.
    assert!(std::net::UdpSocket::bind((Ipv4Addr::UNSPECIFIED, free)).is_err());
    session.stop().await;

    let held = std::net::UdpSocket::bind((Ipv4Addr::UNSPECIFIED, 0)).unwrap();
    let held_port = held.local_addr().unwrap().port();
    assert!(
        crate::recovery::RecoverySession::host_on([6; 16], own.id(), false, Some(held_port))
            .await
            .is_err()
    );

    // Hosting reports the same failure as any other coordination failure.
    let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(own.clone(), events_tx);
    actor.coordination_port = Some(held_port);
    actor.epoch = 1;
    actor.opening = true;
    let invite =
        Invite::create(own.id(), test_relay(), BUILD.into(), now().unwrap(), 3600).unwrap();
    actor.completed_hosted(1, Ok(invite)).await.unwrap();
    assert!(matches!(
        events.recv().await.unwrap(),
        Event::Error { ref code, .. } if code == "coordination_unavailable"
    ));
    assert_eq!(actor.room, None);
    drop(held);
    own.close().await;
}

#[test]
fn connected_serializes_as_before_without_an_account() {
    let peer = iroh::SecretKey::generate().public();
    let event = Event::Connected {
        epoch: 4,
        peer,
        room: [7; 16],
        control: 9,
        account: None,
    };
    let peer_text = serde_json::to_string(&peer).unwrap();
    let room = serde_json::to_string(&[7u8; 16]).unwrap();
    assert_eq!(
        serde_json::to_string(&event).unwrap(),
        format!(r#"{{"type":"connected","epoch":4,"peer":{peer_text},"room":{room},"control":9}}"#)
    );
    let account = identity(1).ember_id().to_string();
    let event = Event::Connected {
        epoch: 4,
        peer,
        room: [7; 16],
        control: 9,
        account: Some(account.clone()),
    };
    assert_eq!(
        serde_json::to_string(&event).unwrap(),
        format!(
            r#"{{"type":"connected","epoch":4,"peer":{peer_text},"room":{room},"control":9,"account":"{account}"}}"#
        )
    );
}

#[test]
fn the_public_commands_round_trip_and_keep_their_names() {
    let account = identity(1).ember_id().to_string();
    let key = identity(9).public_key().to_b64u();
    let host = r#"{"type":"host_public","epoch":3,"build":"b","room":[9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9],"ticket_key":"KEY","ticket_kid":"k1","bridge_id":"BRIDGE","creator":"CREATOR"}"#
        .replace("KEY", &key)
        .replace("BRIDGE", BRIDGE)
        .replace("CREATOR", &account);
    let Ok(Command::HostPublic {
        epoch,
        build,
        room,
        ticket_key,
        ticket_kid,
        bridge_id,
        creator,
    }) = serde_json::from_str(&host)
    else {
        panic!("host_public did not parse");
    };
    assert_eq!(
        (
            epoch,
            build.as_str(),
            room,
            ticket_key.as_str(),
            ticket_kid.as_str(),
            bridge_id.as_str(),
            creator.as_str()
        ),
        (3, "b", [9; 16], key.as_str(), "k1", BRIDGE, account.as_str())
    );
    let again = serde_json::to_string(&Command::HostPublic {
        epoch,
        build,
        room,
        ticket_key,
        ticket_kid,
        bridge_id,
        creator,
    })
    .unwrap();
    assert_eq!(again, host);
    // `creator` is required, and a creator that is no Ember ID is refused.
    assert!(serde_json::from_str::<Command>(&host.replace(&format!(r#","creator":"{account}""#), "")).is_err());

    let ban = format!(r#"{{"type":"ban_account","epoch":3,"account":"{account}"}}"#);
    let Ok(Command::BanAccount {
        epoch: 3,
        account: parsed,
    }) = serde_json::from_str(&ban)
    else {
        panic!("ban_account did not parse");
    };
    assert_eq!(parsed, account);
    assert_eq!(
        serde_json::to_string(&Command::BanAccount {
            epoch: 3,
            account: parsed
        })
        .unwrap(),
        ban
    );

    let endpoint_id = iroh::SecretKey::generate().public();
    let invite =
        Invite::create(endpoint_id, test_relay(), "b".into(), now().unwrap(), 3600).unwrap();
    let signed = RoomTicket {
        version: TICKET_VERSION,
        bridge_id: BRIDGE.into(),
        room_id: hex(&invite.room()),
        ember_id: identity(1).ember_id().clone(),
        endpoint_id: "c".repeat(64),
        issued_at: 1000,
        expires_at: 1000 + TICKET_SECS,
    }
    .sign(&identity(9), "k1")
    .unwrap();
    let join = serde_json::to_string(&Command::JoinPublic {
        epoch: 4,
        invitation: invite.encode().unwrap(),
        ticket: signed.clone(),
        build: "b".into(),
    })
    .unwrap();
    assert!(
        join.starts_with(r#"{"type":"join_public","epoch":4,"invitation":"sf4e3:"#)
            || join.starts_with(r#"{"type":"join_public","epoch":4,"invitation":"sf4e2:"#)
    );
    let Ok(Command::JoinPublic { ticket, build, .. }) = serde_json::from_str(&join) else {
        panic!("join_public did not parse");
    };
    assert_eq!((ticket, build.as_str()), (signed, "b"));

    // Unknown fields stay refused, as for every command.
    assert!(
        serde_json::from_str::<Command>(
            r#"{"type":"ban_account","epoch":1,"account":"x","extra":true}"#
        )
        .is_err()
    );
}

#[tokio::test]
async fn a_public_host_opens_its_room_under_the_id_it_was_asked_for() {
    let own = endpoint().await;
    let requested = [0x42; 16];
    let invite = crate::service::public::host_invite(
        own.id(),
        test_relay(),
        BUILD.into(),
        Some(requested),
    )
    .unwrap();
    assert_eq!(invite.room(), requested);
    // A private host keeps generating its own, and the capability is random
    // either way.
    let private =
        crate::service::public::host_invite(own.id(), test_relay(), BUILD.into(), None).unwrap();
    assert_ne!(private.room(), requested);
    assert_ne!(invite.proof().capability, [0; 32]);
    assert!(
        crate::service::public::host_invite(own.id(), test_relay(), BUILD.into(), Some([0; 16]))
            .is_err()
    );

    // The `hosted` event and the invitation it carries name that room.
    let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(own.clone(), events_tx);
    actor.epoch = 1;
    actor.opening = true;
    actor.public = Some(PublicRoom::Host(host_state()));
    actor.completed_hosted(1, Ok(invite)).await.unwrap();
    let Event::Hosted {
        room, invitation, ..
    } = next(&mut events, "hosted").await
    else {
        unreachable!()
    };
    assert_eq!(room, requested);
    let parsed = Invite::parse_for_build(&invitation, now().unwrap(), BUILD).unwrap();
    assert_eq!(parsed.room(), requested);
    assert_eq!(actor.room, Some(requested));
    actor.clear_room();
    own.close().await;
}
