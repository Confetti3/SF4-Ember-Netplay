//! Admission of members, voter selection and membership history.

use super::*;

/// Drain events until the control closes; neither a session nor a native
/// message may have been published for it.
async fn expect_closed_without_session(events: &mut mpsc::Receiver<Event>) {
    loop {
        match events.recv().await.unwrap() {
            Event::PeerSession { .. } => panic!("session published for a rejected binding"),
            Event::Message { .. } => panic!("native message delivered before its session"),
            Event::Error { code, .. } => panic!("unexpected helper error: {code}"),
            Event::ControlClosed { .. } => break,
            _ => (),
        }
    }
}

#[tokio::test]
async fn a_refused_admission_queue_closes_the_control_without_a_session() {
    phase(
        "a_refused_admission_queue_closes_the_control_without_a_session",
        Duration::from_secs(30),
        async {
            let mut fixture = bound_host(|actor| {
                // An operation that never completes keeps the queue from draining.
                actor.pending_admission_operation = Some(AdmissionOperationKey {
                    sequence: 0,
                    epoch: 1,
                    room: actor.room.unwrap(),
                    incarnation: 0,
                    term: 0,
                    leader: None,
                    peer: actor.endpoint.id(),
                    admissions: Arc::from([]),
                    add_member_if_leader: false,
                });
                for _ in 0..MAX_CONTROL_PEERS {
                    actor.deferred_admissions.push_back(DeferredAdmission {
                        peer: actor.endpoint.id(),
                        admissions: Vec::new(),
                        add_member_if_leader: false,
                        bindings: Vec::new(),
                    });
                }
            })
            .await;
            let connection = fixture
                .remote
                .connect(address(&fixture.host), CONTROL_ALPN)
                .await
                .unwrap();
            let mut control = transport::connect_control_on(connection, &fixture.invite)
                .await
                .unwrap();
            let _ = next(&mut fixture.events, "connected").await;
            let admission = Admission {
                room: fixture.room,
                incarnation: 0x4242,
                authority_term: 1,
                coordination_endpoint: fixture.remote.id(),
                coordination_address: address(&fixture.remote),
                primary_endpoint: fixture.remote.id(),
            };
            send_admission(&mut control, 2, admission).await;
            expect_closed_without_session(&mut fixture.events).await;
            fixture.recovery.stop().await;
            fixture.host.close().await;
            fixture.remote.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn a_binding_the_operation_rejects_closes_the_control_without_a_session() {
    phase(
        "a_binding_the_operation_rejects_closes_the_control_without_a_session",
        Duration::from_secs(60),
        async {
            let mut fixture = bound_host(|_| {}).await;
            let connection = fixture
                .remote
                .connect(address(&fixture.host), CONTROL_ALPN)
                .await
                .unwrap();
            let mut control = transport::connect_control_on(connection, &fixture.invite)
                .await
                .unwrap();
            let _ = next(&mut fixture.events, "connected").await;
            // The host's coordination route refuses this incarnation, so the
            // learner can never be added; the bounded operation fails and the
            // control closes with nothing native read from it. (An unreachable
            // learner is not enough: OpenRaft counts a fresh learner as caught up
            // while the log is short, and a room of two promotes nobody.)
            let nobody = endpoint().await;
            let admission = Admission {
                room: fixture.room,
                incarnation: 0x4343,
                authority_term: 1,
                coordination_endpoint: nobody.id(),
                coordination_address: address(&nobody),
                primary_endpoint: fixture.remote.id(),
            };
            nobody.close().await;
            fixture.recovery.rpc.retire(admission.incarnation).await;
            send_admission(&mut control, 2, admission).await;
            control
                .sender
                .send(&ControlFrame {
                    message_id: 3,
                    payload: b"native message before any session".to_vec(),
                })
                .await
                .unwrap();
            expect_closed_without_session(&mut fixture.events).await;
            fixture.recovery.stop().await;
            fixture.host.close().await;
            fixture.remote.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn an_accepted_admission_publishes_its_session_even_when_roster_known() {
    phase(
        "an_accepted_admission_publishes_its_session_even_when_roster_known",
        Duration::from_secs(60),
        async {
            let mut fixture = bound_host(|_| {}).await;
            let member = crate::recovery::RecoverySession::join(
                fixture.room,
                fixture.remote.id(),
                fixture.recovery.incarnation,
                fixture.recovery.coordination_address.clone(),
                crate::transport::Transport::Online,
            )
            .await
            .unwrap();
            let admission = member.advertise().await;
            let connection = fixture
                .remote
                .connect(address(&fixture.host), CONTROL_ALPN)
                .await
                .unwrap();
            let mut control = transport::connect_control_on(connection, &fixture.invite)
                .await
                .unwrap();
            let _ = next(&mut fixture.events, "connected").await;
            // The session is published when the binding completes, and the
            // message sent right behind the Admission is delivered only after it.
            send_admission(&mut control, 2, admission.clone()).await;
            control
                .sender
                .send(&ControlFrame {
                    message_id: 3,
                    payload: b"first native message".to_vec(),
                })
                .await
                .unwrap();
            let mut session_seen = false;
            loop {
                let event = fixture.events.recv().await.unwrap();
                match event {
                    Event::PeerSession { incarnation, .. } => {
                        assert_eq!(incarnation, member.incarnation);
                        session_seen = true;
                    }
                    Event::Message { payload, .. } => {
                        assert_eq!(payload, "first native message");
                        assert!(session_seen, "native message delivered before its session");
                        break;
                    }
                    Event::Error { code, .. } => panic!("unexpected helper error: {code}"),
                    _ => (),
                }
            }
            // A reconnect re-presenting the same, now roster-known, incarnation
            // is published again; the native record treats the repeat as a no-op.
            let again = fixture
                .remote
                .connect(address(&fixture.host), CONTROL_ALPN)
                .await
                .unwrap();
            let mut control = transport::connect_control_on(again, &fixture.invite)
                .await
                .unwrap();
            let _ = next(&mut fixture.events, "connected").await;
            send_admission(&mut control, 2, admission).await;
            assert!(matches!(
                next(&mut fixture.events, "peer_session").await,
                Event::PeerSession { incarnation, .. } if incarnation == member.incarnation
            ));
            member.stop().await;
            fixture.recovery.stop().await;
            fixture.host.close().await;
            fixture.remote.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn a_completion_settles_only_the_control_pending_for_its_incarnation() {
    phase(
        "a_completion_settles_only_the_control_pending_for_its_incarnation",
        Duration::from_secs(30),
        async {
            let host = endpoint().await;
            let remote = endpoint().await;
            let relay = test_relay();
            let seed = Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600)
                .unwrap();
            let room = seed.room();
            let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let mut actor = test_actor(host.clone(), events_tx);
            let (invite, recovery) = led_host(&mut actor, seed).await;
            // The remote's current control; its predecessor's operation is still
            // in flight when it presents its own Admission.
            let accept = |invite: Invite| {
                let host = host.clone();
                async move {
                    let connection = host.accept().await.unwrap().await.unwrap();
                    transport::accept_control(connection, &invite)
                        .await
                        .unwrap()
                }
            };
            let connect = |invite: Invite| {
                let remote = remote.clone();
                let host = host.clone();
                async move {
                    let connection = remote.connect(address(&host), CONTROL_ALPN).await.unwrap();
                    transport::connect_control_on(connection, &invite)
                        .await
                        .unwrap()
                }
            };
            let (_old_side, old_control) =
                tokio::join!(connect(invite.clone()), accept(invite.clone()));
            let old_control = ControlWorker::start(old_control);
            let old_id = old_control.id();
            drop(old_control); // replaced before its operation completes
            let (_remote_side, current) =
                tokio::join!(connect(invite.clone()), accept(invite.clone()));
            let mut current = ControlWorker::start(current);
            // Every process incarnation binds its own coordination key.
            let old_coordination = endpoint().await;
            let new_coordination = endpoint().await;
            let (old, new) = (0x1111, 0x2222);
            let admission = |incarnation| {
                let coordination = if incarnation == old {
                    &old_coordination
                } else {
                    &new_coordination
                };
                Admission {
                    room,
                    incarnation,
                    authority_term: 1,
                    coordination_endpoint: coordination.id(),
                    coordination_address: address(coordination),
                    primary_endpoint: remote.id(),
                }
            };
            current.set_session(Session::Pending(new));
            let control_id = current.id();
            actor.controls.insert(remote.id(), current);
            let key = |sequence| AdmissionOperationKey {
                sequence,
                epoch: 1,
                room,
                incarnation: recovery.incarnation,
                term: recovery.coordinator.current_term(),
                leader: recovery.coordinator.current_leader(),
                peer: remote.id(),
                admissions: Arc::from([]),
                add_member_if_leader: false,
            };
            let binding = |control, incarnation| ControlBinding {
                peer: remote.id(),
                control,
                incarnation,
            };
            let untouched = |actor: &Actor, events: &mut mpsc::Receiver<Event>| {
                assert_eq!(
                    actor
                        .controls
                        .get(&remote.id())
                        .map(|control| control.session()),
                    Some(Session::Pending(new))
                );
                while let Ok(event) = events.try_recv() {
                    assert!(
                        !matches!(
                            event,
                            Event::PeerSession { .. } | Event::ControlClosed { .. }
                        ),
                        "a replaced control's completion touched its replacement"
                    );
                }
            };
            // The old control's operation succeeds, then one fails: neither
            // binds, closes or releases the replacement, even though it is the
            // same endpoint and the replacement is pending.
            actor.pending_admission_operation = Some(key(1));
            actor.pending_admission_bindings = vec![binding(old_id, old)];
            actor
                .completed_admission(
                    key(1),
                    Ok(AdmissionOperationResult {
                        admissions: vec![admission(old)],
                    }),
                )
                .await
                .unwrap();
            untouched(&actor, &mut events);
            actor.pending_admission_operation = Some(key(2));
            actor.pending_admission_bindings = vec![binding(old_id, old)];
            actor
                .completed_admission(key(2), Err(failed("binding rejected")))
                .await
                .unwrap();
            untouched(&actor, &mut events);
            // Roster work for the replacement's own incarnation owns no control.
            actor.pending_admission_operation = Some(key(3));
            actor.pending_admission_bindings = Vec::new();
            actor
                .completed_admission(key(3), Err(failed("roster write lost")))
                .await
                .unwrap();
            untouched(&actor, &mut events);
            // Its own operation binds it and publishes exactly its incarnation.
            actor.pending_admission_operation = Some(key(4));
            actor.pending_admission_bindings = vec![binding(control_id, new)];
            actor
                .completed_admission(
                    key(4),
                    Ok(AdmissionOperationResult {
                        admissions: vec![admission(new)],
                    }),
                )
                .await
                .unwrap();
            assert_eq!(
                actor
                    .controls
                    .get(&remote.id())
                    .map(|control| control.session()),
                Some(Session::Bound(new))
            );
            assert!(matches!(
                next(&mut events, "peer_session").await,
                Event::PeerSession { incarnation, .. } if incarnation == new
            ));
            // A later control whose own operation fails is closed, by its id.
            let (_remote_side_again, later) =
                tokio::join!(connect(invite.clone()), accept(invite.clone()));
            let mut later = ControlWorker::start(later);
            later.set_session(Session::Pending(0x3333));
            let later_id = later.id();
            assert_ne!(later_id, control_id);
            actor.controls.insert(remote.id(), later);
            actor.pending_admission_operation = Some(key(5));
            actor.pending_admission_bindings = vec![binding(later_id, 0x3333)];
            actor
                .completed_admission(key(5), Err(failed("binding rejected")))
                .await
                .unwrap();
            assert!(!actor.controls.contains_key(&remote.id()));
            assert!(matches!(
                next(&mut events, "control_closed").await,
                Event::ControlClosed { control, .. } if control == later_id
            ));
            actor.tasks.abort_all();
            recovery.stop().await;
            host.close().await;
            remote.close().await;
            old_coordination.close().await;
            new_coordination.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn a_rejected_admission_publishes_no_session() {
    phase(
        "a_rejected_admission_publishes_no_session",
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
            let retired = 0x7777;
            actor.retired_incarnations.insert(retired);
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
            // A replayed Admission for a retired incarnation is rejected and its
            // control closed. The native room must not have been told about a
            // new session first: that instruction could not be retracted.
            let payload = serde_json::to_vec(&CoordinationControl::Admission {
                admission: Admission {
                    room,
                    incarnation: retired,
                    authority_term: 1,
                    coordination_endpoint: remote.id(),
                    coordination_address: address(&remote),
                    primary_endpoint: remote.id(),
                },
            })
            .unwrap();
            control
                .sender
                .send(&ControlFrame {
                    message_id: 2,
                    payload,
                })
                .await
                .unwrap();
            expect_closed_without_session(&mut events).await;
            recovery.stop().await;
            host.close().await;
            remote.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn rejoin_expiry_exception_requires_current_native_membership() {
    let local = endpoint().await;
    let remote = endpoint().await;
    let (events, _receiver) = mpsc::channel(128);
    let mut actor = test_actor(local.clone(), events);
    actor.room = Some([7; 16]);
    actor.remember_admission(Admission {
        room: [7; 16],
        incarnation: 9,
        authority_term: 1,
        coordination_endpoint: remote.id(),
        coordination_address: address(&remote),
        primary_endpoint: remote.id(),
    });
    assert_eq!(actor.control_rejoin_member(remote.id()), None);
    actor.committed_native_members = Some(BTreeMap::from([(remote.id(), 8)]));
    assert_eq!(actor.control_rejoin_member(remote.id()), None);
    actor.committed_native_members = Some(BTreeMap::from([(remote.id(), 9)]));
    assert_eq!(actor.control_rejoin_member(remote.id()), Some(9));
    assert_eq!(actor.control_rejoin_member(local.id()), None);
    actor.pending_retired_incarnations.insert(9);
    assert_eq!(actor.control_rejoin_member(remote.id()), None);
    actor.pending_retired_incarnations.clear();
    actor.retired_incarnations.insert(9);
    assert_eq!(actor.control_rejoin_member(remote.id()), None);
    actor.retired_incarnations.clear();
    actor.room = Some([8; 16]);
    assert_eq!(actor.control_rejoin_member(remote.id()), None);
    actor.room = Some([7; 16]);
    actor.committed_native_members.as_mut().unwrap().clear();
    assert_eq!(actor.control_rejoin_member(remote.id()), None);
    local.close().await;
    remote.close().await;
}

#[tokio::test]
async fn native_rebound_uses_only_the_applied_unambiguous_incarnation() {
    let primary = endpoint().await;
    let old_coordination = endpoint().await;
    let fresh_coordination = endpoint().await;
    let room = [18; 16];
    let admission = |incarnation, coordination: &Endpoint| Admission {
        room,
        incarnation,
        authority_term: 1,
        coordination_endpoint: coordination.id(),
        coordination_address: address(coordination),
        primary_endpoint: primary.id(),
    };
    // Incarnation identifiers are random. The retired value is
    // deliberately numerically larger than the fresh value so collection
    // order would select the wrong process without applied-membership
    // filtering.
    let admissions = BTreeMap::from([
        (900, admission(900, &old_coordination)),
        (100, admission(100, &fresh_coordination)),
    ]);
    assert_eq!(
        applied_member_incarnations(&admissions, &BTreeSet::from([100]), &BTreeSet::from([900]),),
        BTreeMap::from([(primary.id().to_string(), 100)])
    );
    assert!(
        applied_member_incarnations(&admissions, &BTreeSet::from([100, 900]), &BTreeSet::new(),)
            .is_empty()
    );
    primary.close().await;
    old_coordination.close().await;
    fresh_coordination.close().await;
}

#[tokio::test]
async fn fresh_learner_accepts_membership_only_from_invitation_authority() {
    phase(
        "fresh_learner_accepts_membership_only_from_invitation_authority",
        Duration::from_secs(20),
        async {
            let host_primary = endpoint().await;
            let guest_primary = endpoint().await;
            let unrelated_primary = endpoint().await;
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
            let host = crate::recovery::RecoverySession::host(room, host_primary.id(), crate::transport::Transport::Online)
                .await
                .unwrap();
            let guest = crate::recovery::RecoverySession::join(
                room,
                guest_primary.id(),
                host.incarnation,
                host.coordination_address.clone(),
                crate::transport::Transport::Online,
            )
            .await
            .unwrap();
            assert_eq!(guest.coordinator.current_leader(), None);
            assert_eq!(
                guest
                    .coordinator
                    .store()
                    .applied_membership_provenance()
                    .await,
                (BTreeSet::new(), BTreeSet::new())
            );

            let host_admission = host.advertise().await;
            let guest_admission = guest.advertise().await;
            let (events, _events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let mut actor = test_actor(guest_primary.clone(), events);
            actor.epoch = 1;
            actor.room = Some(room);
            // The compact sf4e2/emd2 invitation intentionally carries no
            // coordination incarnation. Its authenticated Admission bootstrap
            // has already bound the host's process to invite.endpoint().
            let compact = Invite::parse_for_build(
                &seed.encode_discord().unwrap(),
                now().unwrap(),
                "test-build",
            )
            .unwrap();
            assert_eq!(compact.authority_incarnation(), 0);
            actor.room_invite = Some(compact);
            actor.recovery = Some(guest.clone());
            actor.remember_admission(host_admission.clone());
            actor.remember_admission(guest_admission.clone());
            let payload = serde_json::to_string(&CoordinationControl::Membership {
                admissions: vec![host_admission, guest_admission],
                retired: Vec::new(),
            })
            .unwrap();

            assert!(
                actor
                    .accept_coordination_control(unrelated_primary.id(), &payload)
                    .await
                    .is_err()
            );
            assert!(
                actor
                    .accept_coordination_control(host_primary.id(), &payload)
                    .await
                    .unwrap()
            );

            guest.stop().await;
            host.stop().await;
            host_primary.close().await;
            guest_primary.close().await;
            unrelated_primary.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn delayed_admission_cannot_readd_authenticated_removed_voter() {
    phase(
        "delayed_admission_cannot_readd_authenticated_removed_voter",
        Duration::from_secs(25),
        async {
            let host_primary = endpoint().await;
            let departing_primary = endpoint().await;
            let room = [29; 16];
            let host = crate::recovery::RecoverySession::host(room, host_primary.id(), crate::transport::Transport::Online)
                .await
                .unwrap();
            let departing = crate::recovery::RecoverySession::join(
                room,
                departing_primary.id(),
                host.incarnation,
                host.coordination_address.clone(),
                crate::transport::Transport::Online,
            )
            .await
            .unwrap();
            let admission = departing.advertise().await;
            host.add_learner(&admission).await.unwrap();
            host.promote_voters(BTreeSet::from([host.incarnation, admission.incarnation]))
                .await
                .unwrap();

            // This is the service Admission precheck before its await. The
            // authenticated method-7 removal then commits in that window.
            let (precheck_members, precheck_history) = host
                .coordinator
                .store()
                .applied_membership_provenance()
                .await;
            assert!(precheck_members.contains(&admission.incarnation));
            assert!(precheck_history.contains(&admission.incarnation));
            assert_eq!(
                host.coordinator
                    .dispatch(
                        admission.incarnation,
                        crate::coordination::Method::Remove,
                        &[0]
                    )
                    .await
                    .unwrap(),
                vec![1]
            );
            let (members, history) = host
                .coordinator
                .store()
                .applied_membership_provenance()
                .await;
            assert_eq!(members, BTreeSet::from([host.incarnation]));
            assert!(history.contains(&admission.incarnation));

            assert!(host.add_learner(&admission).await.is_err());
            assert!(host.rpc.is_retired(admission.incarnation).await);
            assert_eq!(
                host.coordinator.store().applied_members().await,
                BTreeSet::from([host.incarnation])
            );
            assert_eq!(
                host.coordinator.store().applied_voters().await,
                BTreeSet::from([host.incarnation])
            );

            departing.stop().await;
            host.stop().await;
            host_primary.close().await;
            departing_primary.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn successor_rejects_replayed_admission_from_applied_membership_history() {
    phase(
        "successor_rejects_replayed_admission_from_applied_membership_history",
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
            actor.room_invite = Some(invite.clone());
            let recovery = actor.recovery.clone().unwrap();
            let departed = crate::recovery::RecoverySession::join(
                room,
                departed_primary.id(),
                recovery.incarnation,
                recovery.coordination_address.clone(),
                crate::transport::Transport::Online,
            )
            .await
            .unwrap();
            let admission = departed.advertise().await;
            recovery.add_learner(&admission).await.unwrap();
            recovery
                .remove_nodes(BTreeSet::from([admission.incarnation]))
                .await
                .unwrap();
            assert!(
                !recovery
                    .coordinator
                    .store()
                    .applied_members()
                    .await
                    .contains(&admission.incarnation)
            );

            // Model a successor actor that receives both membership entries
            // before its one-second state tick. Its actor-local observation is
            // empty, while the replicated state machine has seen the member.
            actor.remember_admission(admission.clone());
            assert!(actor.applied_admission_members.is_empty());
            let payload = serde_json::to_string(&CoordinationControl::Admission {
                admission: admission.clone(),
            })
            .unwrap();
            assert!(
                actor
                    .accept_coordination_control(departed_primary.id(), &payload)
                    .await
                    .is_err()
            );
            assert!(
                actor
                    .pending_retired_incarnations
                    .contains(&admission.incarnation)
            );
            assert!(
                !recovery
                    .coordinator
                    .store()
                    .applied_members()
                    .await
                    .contains(&admission.incarnation)
            );

            departed.stop().await;
            recovery.stop().await;
            host.close().await;
            departed_primary.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn an_accepted_admission_keeps_its_identity() {
    phase(
        "an_accepted_admission_keeps_its_identity",
        Duration::from_secs(20),
        async {
            let host = endpoint().await;
            let peer = endpoint().await;
            let member_primary = endpoint().await;
            let member_coordination = endpoint().await;
            let peer_coordination = endpoint().await;
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
            let recovery = actor.recovery.clone().unwrap();
            let own = actor.admissions[&recovery.incarnation].clone();
            let member = Admission {
                room,
                incarnation: 0x5150,
                authority_term: 1,
                coordination_endpoint: member_coordination.id(),
                coordination_address: address(&member_coordination),
                primary_endpoint: member_primary.id(),
            };
            actor.remember_admission(member.clone());
            let present = |admission: Admission| {
                serde_json::to_string(&CoordinationControl::Admission { admission }).unwrap()
            };

            // The peer's authenticated primary endpoint paired with an
            // identity another admission already holds: the host's own
            // incarnation, a member's incarnation, or the host's coordination key
            // under a fresh incarnation. Also a coordination key that disagrees
            // with the address that would route it.
            let conflicting = [
                Admission {
                    primary_endpoint: peer.id(),
                    ..own.clone()
                },
                Admission {
                    primary_endpoint: peer.id(),
                    ..member.clone()
                },
                Admission {
                    incarnation: 0x5151,
                    primary_endpoint: peer.id(),
                    ..own.clone()
                },
                Admission {
                    incarnation: 0x5152,
                    primary_endpoint: peer.id(),
                    ..member.clone()
                },
                Admission {
                    room,
                    incarnation: 0x5152,
                    authority_term: 1,
                    coordination_endpoint: member_coordination.id(),
                    coordination_address: address(&peer_coordination),
                    primary_endpoint: peer.id(),
                },
            ];
            for admission in conflicting {
                assert!(
                    actor
                        .accept_coordination_control(peer.id(), &present(admission))
                        .await
                        .is_err()
                );
            }
            assert_eq!(
                actor.admissions[&recovery.incarnation].primary_endpoint,
                host.id()
            );
            assert_eq!(
                actor.admissions[&0x5150].primary_endpoint,
                member_primary.id()
            );
            assert!(actor.pending_admissions.is_empty());
            assert!(actor.pending_admission_operation.is_none());

            // An admission with its own identities is held apart until its
            // operation accepts it; nothing resolves a route through it before.
            let fresh = Admission {
                room,
                incarnation: 0x5153,
                authority_term: 1,
                coordination_endpoint: peer_coordination.id(),
                coordination_address: address(&peer_coordination),
                primary_endpoint: peer.id(),
            };
            assert!(
                actor
                    .accept_coordination_control(peer.id(), &present(fresh.clone()))
                    .await
                    .unwrap()
            );
            assert!(!actor.admissions.contains_key(&fresh.incarnation));
            assert_eq!(
                actor.pending_admissions[&peer.id()].incarnation,
                fresh.incarnation
            );

            // Even a completion that reports a conflicting binding as validated
            // leaves the accepted record in place.
            let key = actor.pending_admission_operation.clone().unwrap();
            actor
                .completed_admission(
                    key,
                    Ok(AdmissionOperationResult {
                        admissions: vec![Admission {
                            primary_endpoint: peer.id(),
                            ..member.clone()
                        }],
                    }),
                )
                .await
                .unwrap();
            assert_eq!(
                actor.admissions[&0x5150].primary_endpoint,
                member_primary.id()
            );
            assert!(!actor.admissions.contains_key(&fresh.incarnation));

            actor.tasks.abort_all();
            recovery.stop().await;
            for endpoint in [
                host,
                peer,
                member_primary,
                member_coordination,
                peer_coordination,
            ] {
                endpoint.close().await;
            }
        },
    )
    .await;
}

#[tokio::test]
async fn overlapping_admissions_are_serialized_and_reach_current_voter_default() {
    phase(
        "overlapping_admissions_are_serialized_and_reach_current_voter_default",
        Duration::from_secs(25),
        async {
            let host_primary = endpoint().await;
            let first_primary = endpoint().await;
            let second_primary = endpoint().await;
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
            let (events, _events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let mut actor = test_actor(host_primary.clone(), events);
            let invite = actor.setup_host_recovery(seed).await.unwrap();
            actor.epoch = 1;
            actor.room = Some(room);
            actor.hosted = Some(invite.clone());
            actor.room_invite = Some(invite);
            let recovery = actor.recovery.clone().unwrap();
            let first = crate::recovery::RecoverySession::join(
                room,
                first_primary.id(),
                recovery.incarnation,
                recovery.coordination_address.clone(),
                crate::transport::Transport::Online,
            )
            .await
            .unwrap();
            let second = crate::recovery::RecoverySession::join(
                room,
                second_primary.id(),
                recovery.incarnation,
                recovery.coordination_address.clone(),
                crate::transport::Transport::Online,
            )
            .await
            .unwrap();
            let first_admission = first.advertise().await;
            let second_admission = second.advertise().await;

            actor
                .queue_admission_operation(
                    first_primary.id(),
                    vec![first_admission.clone()],
                    true,
                    None,
                )
                .unwrap();
            actor
                .queue_admission_operation(first_primary.id(), vec![first_admission], true, None)
                .unwrap();
            assert!(actor.deferred_admissions.is_empty());
            actor
                .queue_admission_operation(
                    second_primary.id(),
                    vec![second_admission.clone()],
                    true,
                    None,
                )
                .unwrap();
            assert_eq!(actor.deferred_admissions.len(), 1);

            while !actor.admissions.contains_key(&first.incarnation)
                || !actor.admissions.contains_key(&second.incarnation)
                || recovery.coordinator.store().applied_voters().await.len() != 3
            {
                let completion = timeout(Duration::from_secs(10), actor.tasks.join_next())
                    .await
                    .expect("serialized admission completion")
                    .expect("admission worker exists")
                    .unwrap();
                actor.completed(completion).await.unwrap();
            }
            assert_eq!(
                recovery.coordinator.store().applied_voters().await,
                BTreeSet::from([recovery.incarnation, first.incarnation, second.incarnation])
            );
            actor.tasks.abort_all();
            first.stop().await;
            second.stop().await;
            recovery.stop().await;
            host_primary.close().await;
            first_primary.close().await;
            second_primary.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn async_admission_completion_publishes_third_member_to_existing_follower() {
    phase(
        "async_admission_completion_publishes_third_member_to_existing_follower",
        Duration::from_secs(25),
        async {
            let host_primary = endpoint().await;
            let follower_primary = endpoint().await;
            let newcomer_primary = endpoint().await;
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
            let (events, _events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let mut actor = test_actor(host_primary.clone(), events);
            let invite = actor.setup_host_recovery(seed).await.unwrap();
            actor.epoch = 1;
            actor.room = Some(room);
            actor.hosted = Some(invite.clone());
            actor.room_invite = Some(invite.clone());
            let recovery = actor.recovery.clone().unwrap();

            let follower = crate::recovery::RecoverySession::join(
                room,
                follower_primary.id(),
                recovery.incarnation,
                recovery.coordination_address.clone(),
                crate::transport::Transport::Online,
            )
            .await
            .unwrap();
            let follower_admission = follower.advertise().await;
            recovery.add_learner(&follower_admission).await.unwrap();
            recovery
                .promote_voters(BTreeSet::from([recovery.incarnation, follower.incarnation]))
                .await
                .unwrap();
            actor.remember_admission(follower_admission);

            // This established follower route predates the third admission.
            // It therefore receives the new binding only if the asynchronous
            // Admission completion publishes the post-commit roster.
            let connecting = async {
                let connection = follower_primary
                    .connect(address(&host_primary), CONTROL_ALPN)
                    .await
                    .unwrap();
                transport::connect_control_on(connection, &invite)
                    .await
                    .unwrap()
            };
            let accepting = async {
                let connection = host_primary.accept().await.unwrap().await.unwrap();
                transport::accept_control(connection, &invite)
                    .await
                    .unwrap()
            };
            let (mut follower_control, host_control) = tokio::join!(connecting, accepting);
            actor
                .controls
                .insert(follower_primary.id(), ControlWorker::start(host_control));

            let newcomer = crate::recovery::RecoverySession::join(
                room,
                newcomer_primary.id(),
                recovery.incarnation,
                recovery.coordination_address.clone(),
                crate::transport::Transport::Online,
            )
            .await
            .unwrap();
            let newcomer_admission = newcomer.advertise().await;
            actor
                .queue_admission_operation(
                    newcomer_primary.id(),
                    vec![newcomer_admission.clone()],
                    true,
                    None,
                )
                .unwrap();

            while !actor.admissions.contains_key(&newcomer.incarnation) {
                let completion = timeout(Duration::from_secs(10), actor.tasks.join_next())
                    .await
                    .expect("third admission completion")
                    .expect("admission worker exists")
                    .unwrap();
                actor.completed(completion).await.unwrap();
            }

            let frame = timeout(Duration::from_secs(5), follower_control.receiver.receive())
                .await
                .expect("post-admission membership publication")
                .unwrap();
            let published: CoordinationControl = serde_json::from_slice(&frame.payload).unwrap();
            let CoordinationControl::Membership {
                admissions,
                retired,
            } = published
            else {
                panic!("expected membership publication");
            };
            assert!(retired.is_empty());
            assert!(admissions.iter().any(|admission| {
                admission.incarnation == newcomer.incarnation
                    && admission.primary_endpoint == newcomer_primary.id()
            }));
            assert!(admissions.iter().any(|admission| {
                admission.incarnation == follower.incarnation
                    && admission.primary_endpoint == follower_primary.id()
            }));
            assert!(actor.pending_membership_publications.is_empty());

            actor.controls.clear();
            actor.tasks.abort_all();
            newcomer.stop().await;
            follower.stop().await;
            recovery.stop().await;
            newcomer_primary.close().await;
            follower_primary.close().await;
            host_primary.close().await;
        },
    )
    .await;
}
