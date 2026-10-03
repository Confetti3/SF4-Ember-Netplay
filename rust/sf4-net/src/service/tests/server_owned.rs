//! What a server-owned room changes in the actor: one voter, a host that is
//! not a member, bare member records, a room that ends with its host, and the
//! sixteenth control.
use super::public_support::{BUILD, host_state, identity, member_admission, public_host, ticket_for};
use super::*;
use crate::{
    public_room::{self, PublicRoom},
    recovery::RecoverySession,
};

/// A host actor with a running coordination session, flagged as a public host
/// when `public`.
async fn host_actor(own: &Endpoint, public: bool) -> (Actor, mpsc::Receiver<Event>) {
    let seed = Invite::create(own.id(), test_relay(), BUILD.into(), now().unwrap(), 3600).unwrap();
    let room = seed.room();
    let (events_tx, events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(own.clone(), events_tx);
    if public {
        actor.public = Some(PublicRoom::Host(host_state()));
    }
    let invite = actor.setup_host_recovery(seed).await.unwrap();
    let recovery = actor.recovery.clone().unwrap();
    while recovery.coordinator.current_leader() != Some(recovery.incarnation) {
        tokio::time::sleep(Duration::from_millis(25)).await;
    }
    actor.epoch = 1;
    actor.room = Some(room);
    actor.hosted = Some(invite.clone());
    actor.room_invite = Some(invite);
    (actor, events)
}

fn roster(endpoints: &[EndpointId]) -> String {
    let members: Vec<_> = endpoints
        .iter()
        .map(
            |endpoint| serde_json::json!({"data": {"authenticatedEndpoint": endpoint.to_string()}}),
        )
        .collect();
    serde_json::json!({"checkpoint": {"members": members}}).to_string()
}

async fn drive_until(actor: &mut Actor, mut done: impl FnMut(&Actor) -> bool) {
    while !done(actor) {
        let completion = timeout(Duration::from_secs(15), actor.tasks.join_next())
            .await
            .expect("a completion")
            .expect("a worker")
            .unwrap();
        actor.completed(completion).await.unwrap();
    }
}

#[tokio::test]
async fn the_host_stays_the_only_voter_as_sixteen_members_are_admitted() {
    timeout(Duration::from_secs(120), async {
        let own = endpoint().await;
        let (mut actor, _events) = host_actor(&own, true).await;
        let recovery = actor.recovery.clone().unwrap();
        assert_eq!(
            recovery.applied_voter_ids().await,
            BTreeSet::from([recovery.incarnation])
        );
        let mut learners = Vec::new();
        for count in 1..=16usize {
            let primary = iroh::SecretKey::generate().public();
            let learner = RecoverySession::join_in(
                recovery.room,
                primary,
                recovery.incarnation,
                recovery.coordination_address.clone(),
                false,
                true,
            )
            .await
            .unwrap();
            let admission = learner.advertise().await;
            actor
                .queue_admission_operation(primary, vec![admission], true, None)
                .unwrap();
            let incarnation = learner.incarnation;
            drive_until(&mut actor, |actor| {
                actor.admissions.contains_key(&incarnation)
            })
            .await;
            // The same admission of a third member makes a private room three
            // voters; here the set never changes.
            assert_eq!(
                recovery.applied_voter_ids().await,
                BTreeSet::from([recovery.incarnation]),
                "{count} members"
            );
            assert_eq!(recovery.applied_member_ids().await.len(), count + 1);
            assert_eq!(actor.desired_voters(count), 1);
            learners.push(learner);
        }
        assert_eq!(recovery.state().await.voter_count, 1);
        actor.tasks.abort_all();
        for learner in learners {
            learner.stop().await;
        }
        recovery.stop().await;
        own.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn no_other_voter_set_is_committed_in_a_server_owned_room() {
    timeout(Duration::from_secs(60), async {
        let primary = endpoint().await;
        let room = [31; 16];
        let mut outcomes = Vec::new();
        for server_owned in [true, false] {
            let host = RecoverySession::host_in(room, primary.id(), false, None, server_owned)
                .await
                .unwrap();
            let learner = RecoverySession::join_in(
                room,
                iroh::SecretKey::generate().public(),
                host.incarnation,
                host.coordination_address.clone(),
                false,
                server_owned,
            )
            .await
            .unwrap();
            host.add_learner(&learner.advertise().await).await.unwrap();
            let both = BTreeSet::from([host.incarnation, learner.incarnation]);
            outcomes.push(host.promote_voters(both).await.is_ok());
            learner.stop().await;
            host.stop().await;
        }
        // The same promotion that makes a private learner a voter is refused.
        assert_eq!(outcomes, [false, true]);
        primary.close().await;
    })
    .await
    .unwrap();
}

/// Members are told another member's endpoint id and nothing to dial it by.
#[tokio::test]
async fn a_member_is_told_only_the_ids_of_the_other_members() {
    use crate::control::ControlWorker;
    timeout(Duration::from_secs(60), async {
        let mut records = Vec::new();
        for public in [true, false] {
            let own = endpoint().await;
            let remote = endpoint().await;
            let other = endpoint().await;
            let (mut actor, _events) = host_actor(&own, public).await;
            let invite = actor.hosted.clone().unwrap();
            let ticket = ticket_for(&invite, &identity(1), &remote);
            let (client, server) = tokio::join!(
                async {
                    let connection = remote.connect(address(&own), CONTROL_ALPN).await.unwrap();
                    if public {
                        public_room::connect_public_on(connection, &invite, &ticket).await
                    } else {
                        transport::connect_control_on(connection, &invite).await
                    }
                },
                async {
                    let connection = own.accept().await.unwrap().await.unwrap();
                    if public {
                        public_room::accept_public_control_at(
                            connection,
                            &invite,
                            host_state().policy(),
                            BTreeMap::new(),
                            now,
                        )
                        .await
                    } else {
                        transport::accept_control(connection, &invite).await
                    }
                }
            );
            let mut client = client.unwrap();
            actor
                .controls
                .insert(remote.id(), ControlWorker::start(server.unwrap()));
            let room = actor.room.unwrap();
            actor.remember_admission(member_admission(room, 0x77, remote.id(), &other));
            assert!(actor.send_membership_control(remote.id(), &BTreeSet::new()));
            let frame = timeout(Duration::from_secs(10), client.receiver.receive())
                .await
                .unwrap()
                .unwrap();
            let CoordinationControl::Membership { admissions, .. } =
                serde_json::from_slice(&frame.payload).unwrap()
            else {
                panic!("not a membership message");
            };
            let host_incarnation = actor.recovery.as_ref().unwrap().incarnation;
            let host_own = actor.admissions[&host_incarnation]
                .coordination_address
                .clone();
            let host_record = admissions
                .iter()
                .find(|admission| admission.incarnation == host_incarnation)
                .unwrap();
            // The host's own record keeps its route in every room.
            assert_eq!(host_record.coordination_address, host_own);
            let record = admissions
                .iter()
                .find(|admission| admission.incarnation == 0x77)
                .unwrap()
                .clone();
            assert_eq!(record.coordination_endpoint, other.id());
            assert!(record.identity_consistent());
            records.push(record.coordination_address);
            actor.controls.clear();
            actor.clear_room();
            own.close().await;
            remote.close().await;
            other.close().await;
        }
        let (public, private) = (&records[0], &records[1]);
        assert_eq!(public.addrs.len(), 0, "a server-owned room sends no route");
        assert!(!private.addrs.is_empty(), "a private room sends the route");
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn a_public_host_absent_from_the_native_roster_is_never_retired() {
    timeout(Duration::from_secs(60), async {
        let own = endpoint().await;
        let route = endpoint().await;
        let (mut actor, _events) = host_actor(&own, true).await;
        let recovery = actor.recovery.clone().unwrap();
        let room = actor.room.unwrap();
        let member = iroh::SecretKey::generate().public();
        actor.remember_admission(member_admission(room, 0x77, member, &route));
        let term = recovery.coordinator.current_term();

        // The host leads but is not in the checkpoint's member list.
        let with_member = roster(&[member]);
        let retained = actor.committed_roster(with_member.as_bytes()).unwrap();
        assert_eq!(retained, BTreeSet::from([own.id(), member]));
        actor.schedule_membership_reconciliation(retained.clone(), term, 1, false);
        assert_eq!(actor.committed_native_members, Some(retained));
        assert!(actor.pending_retired_incarnations.is_empty());
        actor.tasks.abort_all();
        actor.pending_membership_operation = None;

        // The member leaves and the room is empty: the member is retired, the
        // host is not.
        let empty = roster(&[]);
        let retained = actor.committed_roster(empty.as_bytes()).unwrap();
        assert_eq!(retained, BTreeSet::from([own.id()]));
        actor.schedule_membership_reconciliation(retained, term, 2, false);
        assert_eq!(actor.pending_retired_incarnations, BTreeSet::from([0x77]));
        assert!(
            !actor
                .pending_retired_incarnations
                .contains(&recovery.incarnation)
        );
        assert!(actor.admissions.contains_key(&recovery.incarnation));
        actor.tasks.abort_all();

        // A private host absent from the same list is not reconciled at all,
        // and an empty roster is not one.
        let (events, _rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut private = test_actor(own.clone(), events);
        let retained = private.committed_roster(with_member.as_bytes()).unwrap();
        assert_eq!(retained, BTreeSet::from([member]));
        private.schedule_membership_reconciliation(retained, term, 1, false);
        assert_eq!(private.committed_native_members, None);
        assert!(private.committed_roster(empty.as_bytes()).is_none());

        recovery.stop().await;
        route.close().await;
        own.close().await;
    })
    .await
    .unwrap();
}

/// Every control a public host holds counts toward sixteen, not fifteen.
#[tokio::test]
async fn a_public_host_takes_sixteen_controls_and_refuses_the_seventeenth() {
    timeout(Duration::from_secs(90), async {
        let mut fixture = public_host().await;
        let mut held = Vec::new();
        for seed in 1..=16u8 {
            let remote = endpoint().await;
            let control = fixture
                .dial(
                    &remote,
                    &ticket_for(&fixture.invite, &identity(seed), &remote),
                )
                .await
                .unwrap();
            let _ = next(&mut fixture.events, "connected").await;
            held.push((remote, control));
        }
        let overflow = endpoint().await;
        assert!(
            fixture
                .dial(
                    &overflow,
                    &ticket_for(&fixture.invite, &identity(17), &overflow)
                )
                .await
                .is_err()
        );
        fixture.no_event("connected").await;
        // The same endpoint redialing is a replacement, not a seventeenth.
        let (remote, _) = &held[0];
        assert!(
            fixture
                .dial(remote, &ticket_for(&fixture.invite, &identity(1), remote))
                .await
                .is_ok()
        );
        fixture.host.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn the_limits_differ_by_room_kind() {
    let own = endpoint().await;
    let (events, _rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(own.clone(), events);
    assert_eq!(actor.max_control_peers(), 15);
    actor.public = Some(PublicRoom::Member {
        ticket: ticket_for(&joined_invite(&own), &identity(1), &own),
    });
    assert_eq!(actor.max_control_peers(), 15);
    actor.public = Some(PublicRoom::Host(host_state()));
    assert_eq!(actor.max_control_peers(), 16);
    assert!(MAX_TASKS >= actor.max_control_peers() + MAX_GAME_LINKS);
    assert_eq!(actor.leader_loss_grace(), Duration::from_secs(20));
    actor.public = None;
    assert_eq!(actor.leader_loss_grace(), RECOVERY_ELECTION_GRACE);
    own.close().await;
}

/// A member whose host stays unwritable past the grace: whether the room ends.
async fn host_lost(server_owned: bool) -> (bool, bool) {
    let host_primary = endpoint().await;
    let member_primary = endpoint().await;
    let room = [61; 16];
    let host = RecoverySession::host_in(room, host_primary.id(), false, None, server_owned)
        .await
        .unwrap();
    let learner = RecoverySession::join_in(
        room,
        member_primary.id(),
        host.incarnation,
        host.coordination_address.clone(),
        false,
        server_owned,
    )
    .await
    .unwrap();
    host.add_learner(&learner.advertise().await).await.unwrap();
    while learner.coordinator.current_leader() != Some(host.incarnation) {
        tokio::time::sleep(Duration::from_millis(25)).await;
    }
    let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(member_primary.clone(), events_tx);
    actor.epoch = 1;
    actor.room = Some(room);
    actor.recovery = Some(learner.clone());
    actor.remember_admission(host.advertise().await);
    actor.remember_admission(learner.advertise().await);
    if server_owned {
        let invite = joined_invite(&host_primary);
        actor.public = Some(PublicRoom::Member {
            ticket: ticket_for(&invite, &identity(1), &member_primary),
        });
    }
    host.stop().await;
    // The host has been unwritable for the whole grace already.
    let term = learner.coordinator.current_term();
    actor.unwritable_leader_since = Some((
        term,
        host.incarnation,
        Instant::now() - Duration::from_secs(30),
    ));
    for _ in 0..3 {
        actor.emit_coordination_state().await.unwrap();
        loop {
            let completion = timeout(Duration::from_secs(15), actor.tasks.join_next())
                .await
                .expect("a refresh")
                .expect("a worker")
                .unwrap();
            let refresh = matches!(completion, Completion::CoordinationRefresh(..));
            actor.completed(completion).await.unwrap();
            if refresh {
                break;
            }
        }
        if actor.room.is_none() {
            break;
        }
    }
    let mut closed = false;
    while let Ok(event) = events.try_recv() {
        closed |= matches!(event, Event::RoomClosed { epoch: 1 });
    }
    let cleared = actor.room.is_none();
    actor.clear_room();
    host_primary.close().await;
    member_primary.close().await;
    (cleared, closed)
}

/// With no election to wait for, a member that has lost its host for good sees
/// the room close, as after a Leave; a private member only keeps waiting.
#[tokio::test]
async fn a_member_whose_host_is_gone_for_the_grace_sees_the_room_close() {
    timeout(Duration::from_secs(120), async {
        let (public, private) = tokio::join!(host_lost(true), host_lost(false));
        assert_eq!(public, (true, true), "room cleared and room_closed emitted");
        assert_eq!(private, (false, false), "a private member keeps its room");
    })
    .await
    .unwrap();
}

/// Leaving a public room hands authority to nobody and keeps no grace.
#[tokio::test]
async fn a_public_host_leaving_performs_no_handoff() {
    timeout(Duration::from_secs(60), async {
        let primary = endpoint().await;
        let (mut actor, mut events) = host_actor(&primary, true).await;
        let recovery = actor.recovery.clone().unwrap();
        // Force two voters underneath the host, as a handoff would need.
        let learner = RecoverySession::join_in(
            recovery.room,
            iroh::SecretKey::generate().public(),
            recovery.incarnation,
            recovery.coordination_address.clone(),
            false,
            true,
        )
        .await
        .unwrap();
        recovery
            .add_learner(&learner.advertise().await)
            .await
            .unwrap();
        recovery
            .coordinator
            .raft()
            .change_membership(
                BTreeSet::from([recovery.incarnation, learner.incarnation]),
                true,
            )
            .await
            .unwrap();
        let both = BTreeSet::from([recovery.incarnation, learner.incarnation]);
        while learner.applied_voter_ids().await != both {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
        let (_commands, mut command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_fault, mut failure) = watch::channel(false);
        assert!(
            actor
                .leave_command(1, false, &mut command_rx, &mut failure)
                .await
                .unwrap()
        );
        assert!(matches!(
            next(&mut events, "room_closed").await,
            Event::RoomClosed { epoch: 1 }
        ));
        // The room is gone at once: no grace, no coordination route kept.
        assert!(actor.retirement_started.is_none() && actor.recovery.is_none());
        assert!(actor.room.is_none() && !actor.departure_failed);
        // Nobody was promoted or asked to campaign.
        assert_eq!(learner.applied_voter_ids().await, both);
        assert_eq!(
            learner.coordinator.current_leader(),
            Some(recovery.incarnation)
        );
        learner.stop().await;
        primary.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn a_private_host_leaving_still_keeps_its_grace() {
    timeout(Duration::from_secs(60), async {
        let primary = endpoint().await;
        let (mut actor, mut events) = host_actor(&primary, false).await;
        let (_commands, mut command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_fault, mut failure) = watch::channel(false);
        assert!(
            actor
                .leave_command(1, false, &mut command_rx, &mut failure)
                .await
                .unwrap()
        );
        let _ = next(&mut events, "room_closed").await;
        assert!(actor.retirement_started.is_some() && actor.recovery.is_some());
        actor.clear_room();
        primary.close().await;
    })
    .await
    .unwrap();
}
