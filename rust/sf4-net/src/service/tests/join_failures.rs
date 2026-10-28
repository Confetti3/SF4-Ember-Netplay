//! Joins that fail, and what each failure says.

use super::*;

#[tokio::test]
async fn a_rejoining_incarnation_supersedes_the_older_one_of_its_endpoint() {
    phase(
        "a_rejoining_incarnation_supersedes_the_older_one_of_its_endpoint",
        Duration::from_secs(60),
        async {
            let host = endpoint().await;
            let remote = endpoint().await;
            let seed = Invite::create(
                host.id(),
                test_relay(),
                "test-build".into(),
                now().unwrap(),
                3600,
            )
            .unwrap();
            let room = seed.room();
            let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (_commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (_fault, failure) = watch::channel(false);
            let mut actor = test_actor(host.clone(), events_tx);
            let invite = actor.setup_host_recovery(seed).await.unwrap();
            let recovery = actor.recovery.clone().unwrap();
            while recovery.coordinator.current_leader() != Some(recovery.incarnation) {
                tokio::time::sleep(Duration::from_millis(25)).await;
            }
            // The endpoint's previous process is an admitted member whose exit
            // was abandoned, so the leader still holds its route binding.
            let old = crate::recovery::RecoverySession::join(
                room,
                remote.id(),
                recovery.incarnation,
                recovery.coordination_address.clone(),
                false,
            )
            .await
            .unwrap();
            let old_admission = old.advertise().await;
            recovery.add_learner(&old_admission).await.unwrap();
            actor.epoch = 1;
            actor.room = Some(room);
            actor.hosted = Some(invite.clone());
            actor.room_invite = Some(invite.clone());
            actor.remember_admission(old_admission);
            let new = crate::recovery::RecoverySession::join(
                room,
                remote.id(),
                recovery.incarnation,
                recovery.coordination_address.clone(),
                false,
            )
            .await
            .unwrap();
            let new_admission = new.advertise().await;
            let service = tokio::spawn(async move {
                let result = actor.run(command_rx, failure).await;
                actor.clear_room();
                actor.tasks.shutdown().await;
                result
            });
            let _scope = TaskScope(vec![service.abort_handle()]);

            let connection = remote.connect(address(&host), CONTROL_ALPN).await.unwrap();
            let mut control = transport::connect_control_on(connection, &invite)
                .await
                .unwrap();
            let _ = next(&mut events, "connected").await;
            send_admission(&mut control, 2, new_admission).await;
            // The new process is bound although the old one still held the
            // endpoint, and the old one leaves the coordination membership.
            let session = timeout(Duration::from_secs(20), next(&mut events, "peer_session"))
                .await
                .expect("the rejoining process was never bound");
            assert!(matches!(
                session,
                Event::PeerSession { incarnation, .. } if incarnation == new.incarnation
            ));
            phase(
                "the superseded incarnation is removed",
                Duration::from_secs(30),
                async {
                    loop {
                        let members = recovery.coordinator.store().applied_members().await;
                        if members.contains(&new.incarnation) && !members.contains(&old.incarnation)
                        {
                            break;
                        }
                        tokio::time::sleep(Duration::from_millis(50)).await;
                    }
                },
            )
            .await;
            assert!(recovery.rpc.is_retired(old.incarnation).await);
            old.stop().await;
            new.stop().await;
            recovery.stop().await;
            host.close().await;
            remote.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn failed_attempts_say_at_which_stage_they_failed() {
    let local = endpoint().await;
    let (events, mut receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(local.clone(), events);
    let unreachable = transport::host_unreachable;
    // A join whose connection never opened, with no home relay connected here
    // (the test endpoint has none): the relay is what is missing.
    actor.epoch = 1;
    actor.opening = true;
    actor
        .completed_control(1, Err(unreachable()), None)
        .await
        .unwrap();
    assert!(matches!(
        next_error(&mut receiver).await,
        Event::Error { epoch: 1, ref code, ref reason, .. }
            if code == "join_failed" && reason.as_deref() == Some("relay_unreachable")
    ));
    // A connection that opened and was refused carries no stage.
    actor.epoch = 2;
    actor.opening = true;
    actor
        .completed_control(2, Err(io::Error::other("refused")), None)
        .await
        .unwrap();
    assert!(matches!(
        next_error(&mut receiver).await,
        Event::Error { epoch: 2, ref code, reason: None, .. } if code == "join_failed"
    ));
    // A public host's refusal, and a handshake that ran out of time, each have
    // their own reason, apart from the generic close above and from each other.
    let invite = Invite::create(
        local.id(),
        test_relay(),
        "stage".into(),
        now().unwrap(),
        3600,
    )
    .unwrap();
    let ticket = public_support::ticket_for(&invite, &public_support::identity(1), &local);
    for (epoch, error, expected) in [
        (4, transport::admission_refused(), "refused"),
        (5, transport::timed_out(), "timeout"),
    ] {
        actor.epoch = epoch;
        actor.opening = true;
        // A failed join clears the room, and its public marker with it.
        actor.public = Some(crate::public_room::PublicRoom::Member {
            ticket: ticket.clone(),
        });
        actor
            .completed_control(epoch, Err(error), None)
            .await
            .unwrap();
        assert!(matches!(
            next_error(&mut receiver).await,
            Event::Error { epoch: seen, ref code, ref reason, .. }
                if seen == epoch && code == "join_failed" && reason.as_deref() == Some(expected)
        ));
    }
    // A private join never reports either: the same errors are the generic one.
    actor.public = None;
    for (epoch, error) in [
        (6, transport::admission_refused()),
        (7, transport::timed_out()),
    ] {
        actor.epoch = epoch;
        actor.opening = true;
        actor
            .completed_control(epoch, Err(error), None)
            .await
            .unwrap();
        assert!(matches!(
            next_error(&mut receiver).await,
            Event::Error { epoch: seen, ref code, reason: None, .. }
                if seen == epoch && code == "join_failed"
        ));
    }
    // A host attempt whose relay never came online.
    actor.epoch = 3;
    actor.opening = true;
    actor
        .completed_hosted(3, Err(network::relay_unavailable()), None)
        .await
        .unwrap();
    assert!(matches!(
        next_error(&mut receiver).await,
        Event::Error { epoch: 3, ref code, ref reason, .. }
            if code == "host_unavailable" && reason.as_deref() == Some("relay_unreachable")
    ));
    local.close().await;
}

#[tokio::test]
async fn the_actor_reports_its_network_without_a_room() {
    phase(
        "the_actor_reports_its_network_without_a_room",
        Duration::from_secs(15),
        async {
            let local = endpoint().await;
            let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (_commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (_fault, failure) = watch::channel(false);
            let mut actor = test_actor(local.clone(), events_tx);
            let service = tokio::spawn(async move { actor.run(command_rx, failure).await });
            let _service_scope = TaskScope(vec![service.abort_handle()]);
            // The first tick reports before any net report exists.
            let Event::NetworkReport { relay, nat, .. } = next(&mut events, "network_report").await
            else {
                unreachable!()
            };
            assert!(
                relay.is_empty()
                    || ["use1", "usw1", "euc1", "aps1", "other"].contains(&relay.as_str())
            );
            assert!(["checking", "open", "strict", "no_udp"].contains(&nat.as_str()));
            local.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn a_join_that_is_never_admitted_ends_in_join_failed() {
    let joiner = endpoint().await;
    let authority = endpoint().await;
    let other = endpoint().await;
    let invite = joined_invite(&authority);
    let (events, mut receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(joiner.clone(), events);
    actor.epoch = 1;
    actor.room = Some(invite.room());
    actor.room_invite = Some(invite);
    // Losing a control to some other member says nothing about the join.
    for _ in 0..=MAX_JOIN_CONTROL_LOSSES {
        assert!(!actor.join_control_lost(other.id()).await.unwrap());
    }
    for _ in 1..MAX_JOIN_CONTROL_LOSSES {
        assert!(!actor.join_control_lost(authority.id()).await.unwrap());
    }
    // Quick refusals alone do not end the join, as on a slow relay; the same
    // count spread over the give-up window does.
    assert!(!actor.join_control_lost(authority.id()).await.unwrap());
    assert!(actor.room.is_some());
    actor.join_first_loss = actor
        .join_first_loss
        .map(|first| first - JOIN_GIVE_UP_AFTER);
    assert!(actor.join_control_lost(authority.id()).await.unwrap());
    assert!(actor.room.is_none());
    assert!(matches!(
        next_error(&mut receiver).await,
        Event::Error { epoch: 1, ref code, ref reason, .. }
            if code == "join_failed" && reason.as_deref() == Some("control_lost")
    ));

    // A join whose member was admitted redials without limit.
    let mut settled = test_actor(joiner.clone(), mpsc::channel(IPC_QUEUE_CAPACITY).0);
    settled.join_settled = true;
    settled.room_invite = Some(joined_invite(&authority));
    for _ in 0..=MAX_JOIN_CONTROL_LOSSES {
        assert!(!settled.join_control_lost(authority.id()).await.unwrap());
    }
    joiner.close().await;
    authority.close().await;
    other.close().await;
}
