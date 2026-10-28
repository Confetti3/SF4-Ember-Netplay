//! Invitations, control replacement and refused joins.

use super::*;

#[tokio::test]
async fn actor_replaces_authenticated_control_from_same_endpoint() {
    phase(
        "actor_replaces_authenticated_control_from_same_endpoint",
        Duration::from_secs(15),
        async {
            let host = endpoint().await;
            let remote = endpoint().await;
            let relay = test_relay();
            let invite =
                Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600)
                    .unwrap();
            let room = invite.room();
            let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (_commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (_fault, failure) = watch::channel(false);
            let mut actor = test_actor(host.clone(), events_tx);
            actor.epoch = 1;
            actor.room = Some(room);
            actor.hosted = Some(invite.clone());
            actor.room_invite = Some(invite.clone());
            let service = tokio::spawn(async move {
                let result = actor.run(command_rx, failure).await;
                actor.clear_room();
                actor.tasks.shutdown().await;
                result
            });
            let _service_scope = TaskScope(vec![service.abort_handle()]);

            let first_connection = remote.connect(address(&host), CONTROL_ALPN).await.unwrap();
            let _first = transport::connect_control_on(first_connection, &invite)
                .await
                .unwrap();
            let _ = next(&mut events, "connected").await;

            // A clean helper Leave closes the old control at the remote end,
            // but QUIC close delivery can race the next Join. The replacement
            // is authenticated by the same endpoint identity and room proof,
            // so it must supersede the stale worker without waiting for a
            // close notification tick.
            let second_connection = remote.connect(address(&host), CONTROL_ALPN).await.unwrap();
            let mut second = transport::connect_control_on(second_connection, &invite)
                .await
                .unwrap();
            let _ = next(&mut events, "connected").await;
            second
                .sender
                .send(&ControlFrame {
                    message_id: 2,
                    payload: b"replacement control".to_vec(),
                })
                .await
                .unwrap();
            assert!(matches!(
                next(&mut events, "message").await,
                Event::Message { payload, .. } if payload == "replacement control"
            ));
            host.close().await;
            remote.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn a_join_through_an_invitation_to_this_helper_says_so() {
    let own = endpoint().await;
    let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(own.clone(), events_tx);
    // A former leader holding a copy taken while it still led the room.
    let invitation = Invite::create(
        own.id(),
        test_relay(),
        "test-build".into(),
        now().unwrap(),
        3600,
    )
    .unwrap()
    .encode()
    .unwrap();
    assert!(
        actor
            .command_now(Request {
                id: 6,
                command: Command::Join {
                    epoch: 1,
                    invitation,
                    build: "test-build".into(),
                    observer: false,
                },
            })
            .await
            .unwrap()
    );
    assert!(matches!(
        events.recv().await.unwrap(),
        Event::Error { epoch: 1, request_id: 6, ref code, ref reason, .. }
            if code == "invalid_or_incompatible_invitation" && reason.as_deref() == Some("own_room")
    ));
    assert_eq!(actor.room, None);
    own.close().await;
}

#[test]
fn a_copied_invitation_lasts_a_week() {
    let id = iroh::SecretKey::generate().public();
    let start = now().unwrap();
    let invite = Invite::create(
        id,
        test_relay(),
        "test-build".into(),
        start,
        INVITE_LIFETIME,
    )
    .unwrap();
    let text = invite.encode().unwrap();
    // A link copied today still opens the room six days from now, and not
    // after its week.
    let day = 24 * 60 * 60;
    assert!(Invite::parse_for_build(&text, start + 6 * day, "test-build").is_ok());
    assert!(Invite::parse_for_build(&text, start + 7 * day + 1, "test-build").is_err());
}

#[tokio::test]
async fn an_open_room_renews_its_invitation_before_it_expires() {
    let own = endpoint().await;
    let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(own.clone(), events_tx);
    actor.epoch = 4;
    let start = now().unwrap();
    let invite = Invite::create(
        own.id(),
        test_relay(),
        "test-build".into(),
        start,
        INVITE_LIFETIME,
    )
    .unwrap();
    actor.hosted = Some(invite.clone());
    actor.room_invite = Some(invite.clone());
    // Less than a tenth of its lifetime gone: nothing to do.
    actor.renew_invitation(start + INVITE_LIFETIME / 20);
    assert!(events.try_recv().is_err());
    assert_eq!(
        actor.room_invite.as_ref().unwrap().expires(),
        invite.expires()
    );
    // Past a tenth: both copies move on and the native side hears of it.
    let later = start + INVITE_LIFETIME / 4;
    actor.renew_invitation(later);
    let Event::DiscordInvite {
        epoch, invitation, ..
    } = events.recv().await.unwrap()
    else {
        panic!("expected a refreshed invitation");
    };
    assert_eq!(epoch, 4);
    assert_eq!(
        actor.room_invite.as_ref().unwrap().expires(),
        later + INVITE_LIFETIME
    );
    assert_eq!(
        actor.hosted.as_ref().unwrap().expires(),
        later + INVITE_LIFETIME
    );
    let parsed = Invite::parse_for_build(&invitation, invite.expires() + 1, "test-build").unwrap();
    assert_eq!(parsed.room(), invite.room());
    // Out of a room there is nothing to renew.
    actor.hosted = None;
    actor.room_invite = None;
    actor.renew_invitation(later + INVITE_LIFETIME);
    assert!(events.try_recv().is_err());
    own.close().await;
}

#[tokio::test]
async fn a_refused_join_names_the_epoch_it_asked_for() {
    let host = endpoint().await;
    let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (_commands, mut command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (_fault, mut failure) = watch::channel(false);
    let mut actor = test_actor(host.clone(), events_tx);
    actor.epoch = 1;
    actor.room = Some([7; 16]);
    // The native client filters events by its current epoch, so a refusal
    // tagged with the helper's epoch would leave that attempt joining forever.
    let join = |epoch| Command::Join {
        epoch,
        invitation: "not-an-invitation".into(),
        build: "test-build".into(),
        observer: false,
    };
    assert!(
        actor
            .command_now(Request {
                id: 5,
                command: join(2),
            })
            .await
            .unwrap()
    );
    assert!(matches!(
        next(&mut events, "error").await,
        Event::Error { epoch: 2, request_id: 5, ref code, .. } if code == "invalid_room_state"
    ));
    assert_eq!(actor.room, Some([7; 16]));
    // A stale Leave is a rejection, not a failed departure: it must not let
    // a later epoch release a healthy room.
    assert!(
        actor
            .leave_command(9, false, &mut command_rx, &mut failure)
            .await
            .unwrap()
    );
    assert!(matches!(
        next(&mut events, "error").await,
        Event::Error { epoch: 1, ref code, .. } if code == "stale_epoch"
    ));
    assert!(!actor.departure_failed);
    assert!(!actor.begin(3, "test-build"));
    assert_eq!(actor.room, Some([7; 16]));
    // An invalid request never releases retained state either.
    actor.departure_failed = true;
    assert!(!actor.begin(1, "test-build"));
    assert_eq!(actor.room, Some([7; 16]));
    host.close().await;
}
