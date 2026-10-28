//! Departures, hand-off and retirement of members.

use super::*;

#[test]
fn leaving_leader_hands_off_to_a_reachable_voter() {
    use super::departure::handoff_successor;
    let voters = BTreeSet::from([10, 20, 30]);
    // Voter 20 comes first by ID but its game is gone; handing it authority
    // could never commit and would strand the room without a quorum.
    assert_eq!(handoff_successor(&voters, 10, |id| id == 30), Some(30));
    // With every other voter reachable, voter order decides as before.
    assert_eq!(handoff_successor(&voters, 10, |_| true), Some(20));
    // With none reachable, still name one so the leave can try and report.
    assert_eq!(handoff_successor(&voters, 10, |_| false), Some(20));
    assert_eq!(handoff_successor(&BTreeSet::from([10]), 10, |_| true), None);
}

#[tokio::test]
async fn an_unconfirmed_departure_is_released_by_the_next_epoch() {
    phase(
        "an_unconfirmed_departure_is_released_by_the_next_epoch",
        Duration::from_secs(60),
        async {
            let host_primary = endpoint().await;
            let follower_primary = endpoint().await;
            let relay = test_relay();
            let seed = Invite::create(
                host_primary.id(),
                relay,
                "test-build".into(),
                now().unwrap(),
                3600,
            )
            .unwrap();
            let room = seed.room();
            let (host_events, _host_events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let mut host = test_actor(host_primary.clone(), host_events);
            let invite = host.setup_host_recovery(seed).await.unwrap();
            let host_recovery = host.recovery.clone().unwrap();
            while host_recovery.coordinator.current_leader() != Some(host_recovery.incarnation) {
                tokio::time::sleep(Duration::from_millis(25)).await;
            }
            let voter = crate::recovery::RecoverySession::join(
                room,
                follower_primary.id(),
                host_recovery.incarnation,
                host_recovery.coordination_address.clone(),
                false,
            )
            .await
            .unwrap();
            let voter_admission = voter.advertise().await;
            host_recovery.add_learner(&voter_admission).await.unwrap();
            host_recovery
                .promote_voters(BTreeSet::from([
                    host_recovery.incarnation,
                    voter.incarnation,
                ]))
                .await
                .unwrap();
            host.remember_admission(voter_admission.clone());

            // The follower is a committed native member and voter, but its host
            // never applies the departure: no native Leave commits and no
            // membership proof arrives, exactly the case the native client gives
            // up on after its own bound.
            let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (_commands, mut command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (_fault, mut failure) = watch::channel(false);
            let mut follower = test_actor(follower_primary.clone(), events_tx);
            follower.epoch = 1;
            follower.room = Some(room);
            follower.room_invite = Some(invite.clone());
            follower.remember_admission(voter_admission);
            follower.remember_admission(host_recovery.advertise().await);
            follower.committed_native_members = Some(BTreeMap::from([
                (host_primary.id(), host_recovery.incarnation),
                (follower_primary.id(), voter.incarnation),
            ]));
            follower.recovery = Some(voter.clone());
            while voter.coordinator.current_leader() != Some(host_recovery.incarnation) {
                tokio::time::sleep(Duration::from_millis(25)).await;
            }
            assert!(
                follower
                    .leave_command(1, false, &mut command_rx, &mut failure)
                    .await
                    .unwrap()
            );
            assert!(matches!(
                next(&mut events, "error").await,
                Event::Error { epoch: 1, ref code, .. } if code == "leave_successor_unconfirmed"
            ));
            assert!(follower.departure_failed);
            assert_eq!(follower.room, Some(room));
            // The native client has left locally; its next room releases this one,
            // but the departed vote lingers for the grace: the host can still
            // commit the removal that the departure never got.
            assert!(follower.begin(2, "test-build"));
            assert_eq!(follower.room, None);
            assert!(!follower.departure_failed);
            assert!(follower.recovery.is_none());
            host_recovery
                .remove_members(BTreeSet::from([host_recovery.incarnation]))
                .await
                .unwrap();
            assert_eq!(
                host_recovery.coordinator.store().applied_voters().await,
                BTreeSet::from([host_recovery.incarnation])
            );
            host_recovery.stop().await;
            host_primary.close().await;
            follower_primary.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn an_abandoned_coordinated_room_keeps_its_vote_for_the_grace() {
    phase(
        "an_abandoned_coordinated_room_keeps_its_vote_for_the_grace",
        Duration::from_secs(30),
        async {
            let host = endpoint().await;
            let relay = test_relay();
            let seed = Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600)
                .unwrap();
            let room = seed.room();
            let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (_commands, mut command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (_fault, mut failure) = watch::channel(false);
            let mut actor = test_actor(host.clone(), events_tx);
            let invite = actor.setup_host_recovery(seed).await.unwrap();
            let recovery = actor.recovery.clone().unwrap();
            actor.epoch = 1;
            actor.room = Some(room);
            actor.hosted = Some(invite.clone());
            actor.room_invite = Some(invite);
            // An abandon releases the native room at once but keeps answering
            // the old room's authority RPCs, so the leader can still commit the
            // departure with this vote.
            assert!(
                actor
                    .leave_command(1, true, &mut command_rx, &mut failure)
                    .await
                    .unwrap()
            );
            assert!(matches!(
                next(&mut events, "room_closed").await,
                Event::RoomClosed { epoch: 1 }
            ));
            assert!(actor.retirement_started.is_some());
            assert!(actor.recovery.is_some());
            assert!(actor.controls.is_empty() && actor.hosted.is_none());
            // The next room begins at once; the old route lingers on its own.
            assert!(actor.begin(2, "test-build"));
            assert!(actor.recovery.is_none() && actor.room.is_none());
            assert!(actor.retirement_started.is_none());
            assert!(recovery.coordinator.current_leader().is_some());
            recovery.stop().await;
            host.close().await;
        },
    )
    .await;
}

/// A helper in departure grace answers authority RPCs only: a commit applied
/// then is not exported to a native side that has closed the room.
#[tokio::test]
async fn departure_grace_exports_no_commit() {
    phase(
        "departure_grace_exports_no_commit",
        Duration::from_secs(30),
        async {
            let host = endpoint().await;
            let relay = test_relay();
            let seed = Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600)
                .unwrap();
            let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let mut actor = test_actor(host.clone(), events_tx);
            let (_invite, recovery) = led_host(&mut actor, seed).await;
            commit_native(&recovery, 17, "{\"checkpoint\":{}}").await;
            actor.enter_departure_grace();
            actor.pump_committed_checkpoint().await.unwrap();
            assert!(actor.outgoing_transfer.is_none());
            assert_eq!(actor.last_exported_revision, 0);
            while let Ok(event) = events.try_recv() {
                assert!(!matches!(event, Event::CheckpointBegin { .. }));
            }
            // The gate is the grace alone: the same commit exports without it.
            actor.retirement_started = None;
            actor.pump_committed_checkpoint().await.unwrap();
            assert!(actor.outgoing_transfer.is_some());
            assert!(matches!(
                next(&mut events, "checkpoint_begin").await,
                Event::CheckpointBegin {
                    transfer: 17,
                    revision: 1,
                    ..
                }
            ));
            recovery.stop().await;
            host.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn departure_notice_retires_the_seat_at_the_leader() {
    phase(
        "departure_notice_retires_the_seat_at_the_leader",
        Duration::from_secs(30),
        async {
            let host = endpoint().await;
            let remote = endpoint().await;
            let relay = test_relay();
            let seed = Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600)
                .unwrap();
            let room = seed.room();
            let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (_commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (_fault, failure) = watch::channel(false);
            let mut actor = test_actor(host.clone(), events_tx);
            let (invite, recovery) = led_host(&mut actor, seed).await;
            let incarnation = 0x5151;
            actor.remember_admission(Admission {
                room,
                incarnation,
                authority_term: 1,
                coordination_endpoint: remote.id(),
                coordination_address: address(&remote),
                primary_endpoint: remote.id(),
            });
            let service = tokio::spawn(async move {
                let result = actor.run(command_rx, failure).await;
                actor.clear_room();
                actor.tasks.shutdown().await;
                result
            });
            let _service_scope = TaskScope(vec![service.abort_handle()]);

            let connection = remote.connect(address(&host), CONTROL_ALPN).await.unwrap();
            let mut control = transport::connect_control_on(connection, &invite)
                .await
                .unwrap();
            let _ = next(&mut events, "connected").await;
            // A notice for an incarnation this leader never admitted is ignored;
            // the admitted member's notice hands its seat to the native room
            // while its control, and its vote, stay open.
            for (id, notice) in [(2, 0x9999), (3, incarnation)] {
                let payload = serde_json::to_vec(&CoordinationControl::Departure {
                    room,
                    incarnation: notice,
                })
                .unwrap();
                control
                    .sender
                    .send(&ControlFrame {
                        message_id: id,
                        payload,
                    })
                    .await
                    .unwrap();
            }
            assert!(matches!(
                next(&mut events, "peer_departed").await,
                Event::PeerDeparted { epoch: 1, peer } if peer == remote.id()
            ));
            recovery.stop().await;
            host.close().await;
            remote.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn a_confirmed_departure_is_not_reconciled_on_every_refresh() {
    phase(
        "a_confirmed_departure_is_not_reconciled_on_every_refresh",
        Duration::from_secs(20),
        async {
            let host = endpoint().await;
            let departed_primary = endpoint().await;
            let relay = test_relay();
            let seed = Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600)
                .unwrap();
            let room = seed.room();
            let (events, _events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let mut actor = test_actor(host.clone(), events);
            let invite = actor.setup_host_recovery(seed).await.unwrap();
            actor.epoch = 1;
            actor.room = Some(room);
            actor.hosted = Some(invite.clone());
            actor.room_invite = Some(invite);
            // The committed native roster no longer lists the member that left.
            actor.committed_native_members = Some(BTreeMap::from([(
                host.id(),
                actor.recovery.as_ref().unwrap().incarnation,
            )]));
            let recovery = actor.recovery.clone().unwrap();
            let departed = crate::recovery::RecoverySession::join(
                room,
                departed_primary.id(),
                recovery.incarnation,
                recovery.coordination_address.clone(),
                false,
            )
            .await
            .unwrap();
            let admission = departed.advertise().await;
            let incarnation = admission.incarnation;
            recovery.add_learner(&admission).await.unwrap();
            actor.remember_admission(admission);
            recovery
                .remove_nodes(BTreeSet::from([incarnation]))
                .await
                .unwrap();

            // The first refresh sees the departure in applied history and fences
            // it; the reconciliation it starts confirms the retirement.
            actor.emit_coordination_state().await.unwrap();
            complete_next(&mut actor).await;
            assert!(actor.pending_retired_incarnations.contains(&incarnation));
            assert!(actor.pending_membership_operation.is_some());
            complete_next(&mut actor).await;
            assert!(actor.retired_incarnations.contains(&incarnation));
            assert!(actor.pending_retired_incarnations.is_empty());
            assert!(!actor.admissions.contains_key(&incarnation));

            // That completion refreshes once more. The departure stays in the
            // Raft history for good, but a confirmed retirement must not be
            // fenced and reconciled again: each round refreshes, which used to
            // start the next round, and every leader round published a
            // Membership frame to every member.
            complete_next(&mut actor).await;
            assert!(
                actor.pending_retired_incarnations.is_empty(),
                "a refresh fenced a confirmed retirement again"
            );
            assert!(
                actor.pending_membership_operation.is_none(),
                "a confirmed retirement started another reconciliation"
            );
            assert!(actor.tasks.is_empty());

            departed.stop().await;
            recovery.stop().await;
            host.close().await;
            departed_primary.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn a_full_retirement_set_makes_room_by_dropping_an_aged_out_tombstone() {
    let host = endpoint().await;
    let departed_primary = endpoint().await;
    let relay = test_relay();
    let seed = Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600).unwrap();
    let room = seed.room();
    let (events, _events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(host.clone(), events);
    let _invite = actor.setup_host_recovery(seed).await.unwrap();
    let recovery = actor.recovery.clone().unwrap();
    let incarnation = u64::MAX - 1;
    let admission = Admission {
        room,
        incarnation,
        authority_term: 1,
        coordination_endpoint: departed_primary.id(),
        coordination_address: address(&departed_primary),
        primary_endpoint: departed_primary.id(),
    };
    recovery
        .rpc
        .admit_bound(
            incarnation,
            admission.coordination_address.clone(),
            admission.primary_endpoint,
        )
        .await
        .unwrap();
    actor.remember_admission(admission);
    actor
        .retired_incarnations
        .extend(1..=MAX_RETIRED_INCARNATIONS as u64);
    actor.pending_retired_incarnations.insert(incarnation);

    let (applied, history) = recovery
        .coordinator
        .store()
        .applied_membership_provenance()
        .await;
    assert!(!applied.contains(&incarnation));
    recovery.rpc.retire(incarnation).await;
    actor.apply_confirmed_retirements(BTreeSet::from([incarnation]), &history);
    assert!(recovery.rpc.is_retired(incarnation).await);
    assert!(!actor.admissions.contains_key(&incarnation));
    assert!(!actor.admission_order.contains(&incarnation));
    // The 128 tombstones are unknown to the replicated history, so they have
    // aged out and the newest departure takes the place of the lowest one.
    assert!(actor.pending_retired_incarnations.is_empty());
    assert!(actor.retired_incarnations.contains(&incarnation));
    assert!(!actor.retired_incarnations.contains(&1));
    assert_eq!(actor.retired_incarnations.len(), MAX_RETIRED_INCARNATIONS);

    recovery.stop().await;
    host.close().await;
    departed_primary.close().await;
}

#[tokio::test]
async fn a_retirement_set_holding_only_recent_departures_fails_closed() {
    let local = endpoint().await;
    let (events, _receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(local.clone(), events);
    let mut history: BTreeSet<u64> = (1..=MAX_RETIRED_INCARNATIONS as u64).collect();
    actor
        .retired_incarnations
        .extend(1..=MAX_RETIRED_INCARNATIONS as u64);
    // Every tombstone is still inside the replicated window: none may go.
    assert!(!actor.record_retirement(u64::MAX, &history));
    assert_eq!(actor.retired_incarnations.len(), MAX_RETIRED_INCARNATIONS);
    assert!(!actor.retired_incarnations.contains(&u64::MAX));
    // Once one has aged out of the window it makes room.
    history.remove(&5);
    assert!(actor.record_retirement(u64::MAX, &history));
    assert!(actor.retired_incarnations.contains(&u64::MAX));
    assert!(!actor.retired_incarnations.contains(&5));
    assert_eq!(actor.retired_incarnations.len(), MAX_RETIRED_INCARNATIONS);
    local.close().await;
}

/// A learner follower with a control to a running leader that has admitted it.
struct LearnerFollower {
    fixture: BoundHost,
    learner: crate::recovery::RecoverySession,
    follower: Actor,
    events: mpsc::Receiver<Event>,
    commands: mpsc::Receiver<Request>,
    failure: watch::Receiver<bool>,
    _command_sender: mpsc::Sender<Request>,
    _fault: watch::Sender<bool>,
}

async fn learner_follower() -> LearnerFollower {
    let mut fixture = bound_host(|_| {}).await;
    let learner = crate::recovery::RecoverySession::join(
        fixture.room,
        fixture.remote.id(),
        fixture.recovery.incarnation,
        fixture.recovery.coordination_address.clone(),
        false,
    )
    .await
    .unwrap();
    let admission = learner.advertise().await;
    let connection = fixture
        .remote
        .connect(address(&fixture.host), CONTROL_ALPN)
        .await
        .unwrap();
    let channel = transport::connect_control_on(connection, &fixture.invite)
        .await
        .unwrap();
    let _ = next(&mut fixture.events, "connected").await;
    let worker = ControlWorker::start(channel);
    worker
        .try_send(ControlFrame {
            message_id: TRANSPORT_MESSAGE_ID_BASE,
            payload: serde_json::to_vec(&CoordinationControl::Admission {
                admission: admission.clone(),
            })
            .unwrap(),
        })
        .unwrap();
    // The leader binds the session once the learner is a Raft member. It
    // also promotes the member, so hand the vote back: a learner is what a
    // room of four or more members holds.
    let _ = next(&mut fixture.events, "peer_session").await;
    fixture
        .recovery
        .promote_voters(BTreeSet::from([fixture.recovery.incarnation]))
        .await
        .unwrap();
    let (events_tx, events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (command_sender, commands) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (fault, failure) = watch::channel(false);
    let mut follower = test_actor(fixture.remote.clone(), events_tx);
    follower.epoch = 1;
    follower.room = Some(fixture.room);
    follower.room_invite = Some(fixture.invite.clone());
    follower.remember_admission(admission);
    follower.committed_native_members = Some(BTreeMap::from([
        (fixture.host.id(), fixture.recovery.incarnation),
        (fixture.remote.id(), learner.incarnation),
    ]));
    follower.recovery = Some(learner.clone());
    follower.next_transport_message = TRANSPORT_MESSAGE_ID_BASE + 1;
    follower.controls.insert(fixture.host.id(), worker);
    while learner.coordinator.current_leader() != Some(fixture.recovery.incarnation)
        || learner.coordinator.store().applied_voters().await
            != BTreeSet::from([fixture.recovery.incarnation])
    {
        tokio::time::sleep(Duration::from_millis(25)).await;
    }
    LearnerFollower {
        fixture,
        learner,
        follower,
        events,
        commands,
        failure,
        _command_sender: command_sender,
        _fault: fault,
    }
}

/// The leader must hear the departure before it sees the control close.
async fn expect_departure_before_close(events: &mut mpsc::Receiver<Event>, peer: EndpointId) {
    loop {
        match events.recv().await.unwrap() {
            Event::PeerDeparted { peer: departed, .. } if departed == peer => return,
            Event::ControlClosed { .. } => panic!("control closed before the departure notice"),
            _ => (),
        }
    }
}

#[tokio::test]
async fn a_learner_departure_notice_reaches_the_leader() {
    phase(
        "a_learner_departure_notice_reaches_the_leader",
        Duration::from_secs(60),
        async {
            let mut fixture = learner_follower().await;
            assert!(
                fixture
                    .follower
                    .leave_command(1, false, &mut fixture.commands, &mut fixture.failure)
                    .await
                    .unwrap()
            );
            assert!(matches!(
                next(&mut fixture.events, "room_closed").await,
                Event::RoomClosed { epoch: 1 }
            ));
            expect_departure_before_close(&mut fixture.fixture.events, fixture.fixture.remote.id())
                .await;
            fixture.learner.stop().await;
            fixture.fixture.recovery.stop().await;
            fixture.fixture.host.close().await;
            fixture.fixture.remote.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn an_abandoning_follower_announces_its_departure() {
    phase(
        "an_abandoning_follower_announces_its_departure",
        Duration::from_secs(60),
        async {
            let mut fixture = learner_follower().await;
            assert!(
                fixture
                    .follower
                    .leave_command(1, true, &mut fixture.commands, &mut fixture.failure)
                    .await
                    .unwrap()
            );
            assert!(matches!(
                next(&mut fixture.events, "room_closed").await,
                Event::RoomClosed { epoch: 1 }
            ));
            assert!(fixture.follower.controls.is_empty());
            expect_departure_before_close(&mut fixture.fixture.events, fixture.fixture.remote.id())
                .await;
            fixture.learner.stop().await;
            fixture.fixture.recovery.stop().await;
            fixture.fixture.host.close().await;
            fixture.fixture.remote.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn removing_a_voter_does_not_wait_for_an_unreachable_voter() {
    phase(
        "removing_a_voter_does_not_wait_for_an_unreachable_voter",
        Duration::from_secs(60),
        async {
            let host = endpoint().await;
            let leaving_primary = endpoint().await;
            let dead_primary = endpoint().await;
            let seed = Invite::create(
                host.id(),
                test_relay(),
                "test-build".into(),
                now().unwrap(),
                3600,
            )
            .unwrap();
            let room = seed.room();
            let (events, _events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let mut actor = test_actor(host.clone(), events);
            let invite = actor.setup_host_recovery(seed).await.unwrap();
            actor.epoch = 1;
            actor.room = Some(room);
            actor.hosted = Some(invite.clone());
            actor.room_invite = Some(invite);
            let recovery = actor.recovery.clone().unwrap();
            while recovery.coordinator.current_leader() != Some(recovery.incarnation) {
                tokio::time::sleep(Duration::from_millis(25)).await;
            }
            let mut sessions = Vec::new();
            for primary in [&leaving_primary, &dead_primary] {
                let session = crate::recovery::RecoverySession::join(
                    room,
                    primary.id(),
                    recovery.incarnation,
                    recovery.coordination_address.clone(),
                    false,
                )
                .await
                .unwrap();
                let admission = session.advertise().await;
                recovery.add_learner(&admission).await.unwrap();
                actor.remember_admission(admission);
                sessions.push(session);
            }
            let (leaving, dead) = (sessions.remove(0), sessions.remove(0));
            recovery
                .promote_voters(BTreeSet::from([
                    recovery.incarnation,
                    leaving.incarnation,
                    dead.incarnation,
                ]))
                .await
                .unwrap();
            // One voter has gone for good, and the roster has dropped the other.
            dead.stop().await;
            actor
                .pending_retired_incarnations
                .insert(leaving.incarnation);
            let retained = BTreeSet::from([host.id(), dead_primary.id()]);
            actor.committed_native_members = Some(BTreeMap::from([
                (host.id(), recovery.incarnation),
                (dead_primary.id(), dead.incarnation),
            ]));
            let term = recovery.state().await.term;
            let revision = recovery.coordinator.store().committed().await.revision();
            actor.spawn_membership_operation(retained, term, revision);
            // The replacement voter set leaves out the voter that cannot answer,
            // so the change commits instead of waiting on it.
            complete_next(&mut actor).await;
            assert_eq!(
                recovery.coordinator.store().applied_voters().await,
                BTreeSet::from([recovery.incarnation])
            );
            assert!(actor.retired_incarnations.contains(&leaving.incarnation));
            // The unreachable voter keeps its membership as a learner.
            assert!(
                recovery
                    .coordinator
                    .store()
                    .applied_members()
                    .await
                    .contains(&dead.incarnation)
            );
            leaving.stop().await;
            recovery.stop().await;
            host.close().await;
            leaving_primary.close().await;
            dead_primary.close().await;
        },
    )
    .await;
}
