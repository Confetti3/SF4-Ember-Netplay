//! Connection checks between two helpers.

use super::*;

#[tokio::test]
async fn completed_probe_records_the_recommendation_and_closes_the_connection() {
    phase(
        "completed_probe_records_the_recommendation_and_closes_the_connection",
        Duration::from_secs(20),
        async {
            let host = endpoint().await;
            let guest = endpoint().await;
            let room = [47; 16];
            let (request, pair_revision) = (9, 23);
            // One exchange: the responder answers, then either closes at once, as
            // the actor does once it has recorded the probe, or keeps its side
            // open and hands the connection back.
            let exchange = |close_after_answer: bool| {
                let (host, guest) = (host.clone(), guest.clone());
                async move {
                    tokio::join!(
                        run_probe(
                            host.clone(),
                            address(&guest),
                            room,
                            guest.id(),
                            request,
                            pair_revision,
                            false,
                        ),
                        async {
                            let connection = guest.accept().await.unwrap().await.unwrap();
                            match transport::accept_game_stream(&connection).await.unwrap() {
                                transport::GameStream::Probe(send, recv) => {
                                    let connection = serve_probe(
                                        connection,
                                        send,
                                        recv,
                                        room,
                                        request,
                                        pair_revision,
                                    )
                                    .await?;
                                    if close_after_answer {
                                        connection.close(0u32.into(), b"probe complete");
                                        Ok(None)
                                    } else {
                                        Ok(Some(connection))
                                    }
                                }
                                transport::GameStream::Gameplay(_, _) => {
                                    Err(failed("expected probe stream"))
                                }
                            }
                        }
                    )
                }
            };

            // The prober still receives the whole answer when the responder
            // closes straight after it; the close may already be visible.
            let (closed_probe, closed_guest) = exchange(true).await;
            let closed_probe = closed_probe.unwrap();
            closed_guest.unwrap();
            assert!(
                !closed_probe.route_changed,
                "the responder's close after the stream end is not a route change"
            );
            assert!(!closed_probe.samples_us.is_empty());
            if let Some(connection) = closed_probe.connection {
                connection.close(0u32.into(), b"test done");
            }

            // With the responder's side held open, only completed_probe can close
            // the connection, so the check below is about the actor.
            let (host_probe, guest_side) = exchange(false).await;
            let host_probe = host_probe.unwrap();
            let _guest_side = guest_side.unwrap();
            let connection = host_probe.connection.clone().unwrap();
            assert!(connection.close_reason().is_none());

            let (events, mut receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let mut actor = test_actor(host.clone(), events);
            actor.epoch = 5;
            actor.room = Some(room);
            let permit = |request, pair_revision| ProbePermission {
                request,
                pair_revision,
                expires: tokio::time::Instant::now() + Duration::from_secs(5),
            };
            actor.own_probes.insert(
                guest.id(),
                OwnProbe {
                    request,
                    pair_revision,
                },
            );
            actor
                .completed(Completion::Probe(5, Ok(host_probe)))
                .await
                .unwrap();
            assert!(
                connection.close_reason().is_some(),
                "nothing reuses a probe connection, so it must not be kept open"
            );
            assert!(actor.own_probes.is_empty());
            assert!(matches!(
                receiver.recv().await,
                Some(Event::ProbeResult { request: 9, status, .. }) if status == "ready"
            ));

            // A check the peer runs against us reports nothing and leaves the
            // recommendation alone.
            actor.probe_peers.insert(guest.id());
            actor.probe_permissions.insert(guest.id(), permit(10, 23));
            actor
                .completed(Completion::Probe(
                    5,
                    Ok(ProbeCompletion {
                        peer: guest.id(),
                        request: 10,
                        pair_revision: 23,
                        samples_us: Vec::new(),
                        connection: None,
                        report: false,
                        route_changed: false,
                        route: None,
                        metrics: crate::probe::Metrics::default(),
                    }),
                ))
                .await
                .unwrap();
            assert!(actor.probe_permissions.is_empty() && actor.probe_peers.is_empty());
            assert!(actor.pending_probe_invalidations.is_empty());
            assert!(receiver.try_recv().is_err(), "the peer's check reported");
            host.close().await;
            guest.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn probe_invalidation_retries_after_bulk_backpressure() {
    phase(
        "probe_invalidation_retries_after_bulk_backpressure",
        Duration::from_secs(10),
        async {
            let local = endpoint().await;
            let remote = endpoint().await;
            let (events, mut receiver) = mpsc::channel(LIFECYCLE_EVENT_RESERVE + 1);
            let mut actor = test_actor(local.clone(), events);
            actor.epoch = 7;
            actor.room = Some([59; 16]);
            assert!(actor.emit_bulk(Event::CheckpointAck {
                epoch: 7,
                room: [59; 16],
                transfer: 1,
                offset: 0,
            }));

            actor.queue_probe_invalidation(remote.id(), 23, 37, "unavailable".into());
            assert!(actor.pending_probe_invalidations.contains_key(&remote.id()));
            assert!(matches!(
                receiver.recv().await,
                Some(Event::CheckpointAck { .. })
            ));

            actor.pump_probe_invalidations();
            assert!(!actor.pending_probe_invalidations.contains_key(&remote.id()));
            assert!(matches!(
                receiver.recv().await,
                Some(Event::ProbeResult {
                    peer,
                    request: 23,
                    pair_revision: 37,
                    route,
                    status,
                    ..
                }) if peer == remote.id() && route == "unavailable" && status == "invalidated"
            ));
            local.close().await;
            remote.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn obsolete_probe_completion_cannot_replace_newer_peer_state() {
    phase(
        "obsolete_probe_completion_cannot_replace_newer_peer_state",
        Duration::from_secs(10),
        async {
            let local = endpoint().await;
            let remote = endpoint().await;
            let (old_connection, old_remote) =
                tokio::join!(local.connect(address(&remote), GAME_ALPN), async {
                    remote.accept().await.unwrap().await
                });
            let old_connection = old_connection.unwrap();
            let old_observer = old_connection.clone();
            let (events, _receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let mut actor = test_actor(local.clone(), events);
            actor.epoch = 5;
            actor.room = Some([61; 16]);
            actor.probe_peers.insert(remote.id());
            actor.probe_permissions.insert(
                remote.id(),
                ProbePermission {
                    request: 29,
                    pair_revision: 41,
                    expires: tokio::time::Instant::now() + Duration::from_secs(5),
                },
            );
            actor.pending_probe_invalidations.insert(
                remote.id(),
                PendingProbeInvalidation {
                    request: 29,
                    pair_revision: 41,
                    route: "newer route".into(),
                },
            );

            actor
                .completed(Completion::Probe(
                    5,
                    Ok(ProbeCompletion {
                        peer: remote.id(),
                        request: 11,
                        pair_revision: 17,
                        samples_us: Vec::new(),
                        connection: Some(old_connection),
                        report: false,
                        route_changed: false,
                        route: None,
                        metrics: crate::probe::Metrics::default(),
                    }),
                ))
                .await
                .unwrap();

            assert!(actor.probe_peers.contains(&remote.id()));
            assert!(
                actor
                    .probe_permissions
                    .get(&remote.id())
                    .is_some_and(
                        |permission| permission.request == 29 && permission.pair_revision == 41
                    )
            );
            assert!(
                actor
                    .pending_probe_invalidations
                    .get(&remote.id())
                    .is_some_and(|invalidation| invalidation.request == 29
                        && invalidation.pair_revision == 41)
            );
            old_observer.closed().await;
            assert!(old_observer.close_reason().is_some());
            drop(old_remote);
            local.close().await;
            remote.close().await;
        },
    )
    .await;
}

/// A completed probe of `request` and `pair_revision`, as a worker reports it:
/// a clean hundred-sample measurement for the prober, nothing for the responder.
fn probe_completion(
    peer: EndpointId,
    request: u64,
    pair_revision: u64,
    report: bool,
) -> Completion {
    let samples: Vec<u64> = if report { vec![300; 100] } else { Vec::new() };
    Completion::Probe(
        5,
        Ok(ProbeCompletion {
            peer,
            request,
            pair_revision,
            metrics: crate::probe::Metrics {
                expected: if report { 100 } else { 0 },
                sent: if report { 100 } else { 0 },
                replies: samples.len() as u32,
                ..crate::probe::Metrics::default()
            },
            samples_us: samples,
            connection: None,
            report,
            route_changed: false,
            route: Some("direct".into()),
        }),
    )
}

/// The peer checks us while we check it, and both checks carry the same request
/// id and pair revision. Each completion settles only the probe of its own role,
/// in either order.
#[tokio::test]
async fn a_probe_completion_settles_only_the_probe_of_its_own_role() {
    phase("a_probe_completion_settles_only_the_probe_of_its_own_role", Duration::from_secs(10), async {
        let local = endpoint().await;
        let remote = endpoint().await;
        let peer = remote.id();
        let (events, mut receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(local.clone(), events);
        actor.epoch = 5;
        actor.room = Some([62; 16]);
        let stage = |actor: &mut Actor| {
            actor.own_probes.insert(
                peer,
                OwnProbe {
                    request: 1,
                    pair_revision: 8,
                },
            );
            actor.probe_peers.insert(peer);
            actor.probe_permissions.insert(
                peer,
                ProbePermission {
                    request: 1,
                    pair_revision: 8,
                    expires: tokio::time::Instant::now() + Duration::from_secs(5),
                },
            );
        };

        // The answered check finishes first. It reports nothing and leaves
        // our own check running.
        stage(&mut actor);
        actor
            .completed(probe_completion(peer, 1, 8, false))
            .await
            .unwrap();
        assert!(actor.own_probes.contains_key(&peer));
        assert!(actor.probe_permissions.is_empty() && actor.probe_peers.is_empty());
        assert!(receiver.try_recv().is_err(), "the answered check reported");
        // Our own check then completes with its own samples.
        actor
            .completed(probe_completion(peer, 1, 8, true))
            .await
            .unwrap();
        assert!(actor.own_probes.is_empty());
        assert!(matches!(
            receiver.recv().await,
            Some(Event::ProbeResult { request: 1, pair_revision: 8, status, sample_count: 100, .. })
                if status == "ready"
        ));

        // Our own check finishes first. The answered check still being
        // served is untouched by it, and finishing later reports nothing.
        stage(&mut actor);
        actor
            .completed(probe_completion(peer, 1, 8, true))
            .await
            .unwrap();
        assert!(actor.own_probes.is_empty());
        assert!(actor.probe_permissions.contains_key(&peer));
        assert!(actor.probe_peers.contains(&peer));
        assert!(matches!(
            receiver.recv().await,
            Some(Event::ProbeResult { status, sample_count: 100, .. }) if status == "ready"
        ));
        actor
            .completed(probe_completion(peer, 1, 8, false))
            .await
            .unwrap();
        assert!(actor.probe_permissions.is_empty() && actor.probe_peers.is_empty());
        assert!(receiver.try_recv().is_err(), "the answered check reported");
        assert!(actor.pending_probe_invalidations.is_empty());
        local.close().await;
        remote.close().await;
    })
    .await;
}

/// A helper actor staged for one probe exchange and started, as the real
/// helper runs it.
struct ProbeSide {
    events: mpsc::Receiver<Event>,
    _commands: mpsc::Sender<Request>,
    _fault: watch::Sender<bool>,
    _service: TaskScope,
}

impl ProbeSide {
    const REVISION: u64 = 4;

    fn actor(endpoint: &Endpoint) -> (Actor, mpsc::Receiver<Event>) {
        let (events_tx, events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(endpoint.clone(), events_tx);
        actor.epoch = 1;
        actor.room = Some([94; 16]);
        (actor, events)
    }

    /// The worker the probe command spawns to check `peer`, released once
    /// `gate` opens.
    fn stage_check(
        actor: &mut Actor,
        peer: &Endpoint,
        gate: Option<tokio::sync::oneshot::Receiver<()>>,
    ) {
        let (endpoint, address, peer_id) = (actor.endpoint.clone(), address(peer), peer.id());
        actor.own_probes.insert(
            peer_id,
            OwnProbe {
                request: 1,
                pair_revision: Self::REVISION,
            },
        );
        actor.tasks.spawn(async move {
            if let Some(gate) = gate {
                let _ = gate.await;
            }
            let completion = run_probe(
                endpoint,
                address,
                [94; 16],
                peer_id,
                1,
                Self::REVISION,
                false,
            )
            .await
            .expect("check of the peer");
            Completion::Probe(1, Ok(completion))
        });
    }

    /// The reservation the peer's check needs, as its control frame installs it.
    fn permit(actor: &mut Actor, peer: &Endpoint) {
        actor.install_probe_permission(peer.id(), 1, Self::REVISION);
    }

    fn start(mut actor: Actor, events: mpsc::Receiver<Event>) -> Self {
        let (commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (fault, failure) = watch::channel(false);
        let service = tokio::spawn(async move {
            let result = actor.run(command_rx, failure).await;
            actor.tasks.shutdown().await;
            result
        });
        Self {
            events,
            _commands: commands,
            _fault: fault,
            _service: TaskScope(vec![service.abort_handle()]),
        }
    }

    async fn probe_result(&mut self) -> Event {
        phase("a probe result", Duration::from_secs(40), async {
            loop {
                match self.events.recv().await.expect("helper events") {
                    event @ Event::ProbeResult { .. } => return event,
                    Event::Error { code, .. } => panic!("helper reported {code}"),
                    _ => (),
                }
            }
        })
        .await
    }
}

/// A checks B and B checks A the moment A has its result, with the same request
/// id and pair revision both ways, while A is still serving B's check. Both
/// checks must finish with their own samples and neither helper may retire the
/// other's recommendation, however the two helpers' closes and completions
/// interleave.
async fn reverse_check_after_check() {
    let (a, b) = (endpoint().await, endpoint().await);
    let ((mut a_actor, a_events), (mut b_actor, b_events)) =
        (ProbeSide::actor(&a), ProbeSide::actor(&b));
    let (release, gate) = tokio::sync::oneshot::channel();
    ProbeSide::stage_check(&mut a_actor, &b, None);
    ProbeSide::permit(&mut b_actor, &a);
    ProbeSide::stage_check(&mut b_actor, &a, Some(gate));
    ProbeSide::permit(&mut a_actor, &b);
    let mut a_side = ProbeSide::start(a_actor, a_events);
    let mut b_side = ProbeSide::start(b_actor, b_events);

    let first = a_side.probe_result().await;
    assert_no_address(&first);
    match first {
        Event::ProbeResult {
            peer,
            request: 1,
            status,
            sample_count,
            ..
        } => assert!(peer == b.id() && status == "ready" && sample_count >= 80),
        other => panic!("A's check: {}", serde_json::to_string(&other).unwrap()),
    }
    release.send(()).unwrap();
    let second = b_side.probe_result().await;
    assert_no_address(&second);
    match second {
        Event::ProbeResult {
            peer,
            request: 1,
            pair_revision: ProbeSide::REVISION,
            route,
            status,
            sample_count,
            ..
        } => {
            assert_eq!(peer, a.id());
            assert_eq!(status, "ready", "B's check must not be retired: {route}");
            assert!(sample_count >= 80, "{sample_count} samples");
            assert_eq!(route, "direct");
        }
        other => panic!("B's check: {}", serde_json::to_string(&other).unwrap()),
    }
    // Serving B's check retires nothing of A's, and nothing of A's check
    // reaches B afterwards.
    assert!(
        timeout(Duration::from_millis(500), a_side.probe_result())
            .await
            .is_err(),
        "A's recommendation was retired or replaced"
    );
    assert!(
        timeout(Duration::from_millis(300), b_side.probe_result())
            .await
            .is_err()
    );
    drop((a_side, b_side));
    a.close().await;
    b.close().await;
}

/// A and B check each other at the same moment, so each helper serves the
/// other's check while its own is in flight and the two finish in any order.
/// Each side ends with exactly one result of its own, with samples, and no
/// retirement.
async fn simultaneous_checks_both_ways() {
    let (a, b) = (endpoint().await, endpoint().await);
    let ((mut a_actor, a_events), (mut b_actor, b_events)) =
        (ProbeSide::actor(&a), ProbeSide::actor(&b));
    ProbeSide::permit(&mut a_actor, &b);
    ProbeSide::permit(&mut b_actor, &a);
    ProbeSide::stage_check(&mut a_actor, &b, None);
    ProbeSide::stage_check(&mut b_actor, &a, None);
    let mut a_side = ProbeSide::start(a_actor, a_events);
    let mut b_side = ProbeSide::start(b_actor, b_events);

    let (a_result, b_result) = tokio::join!(a_side.probe_result(), b_side.probe_result());
    for (result, peer) in [(a_result, b.id()), (b_result, a.id())] {
        match result {
            Event::ProbeResult {
                peer: reported,
                request: 1,
                pair_revision: ProbeSide::REVISION,
                status,
                sample_count,
                ..
            } => assert!(reported == peer && status == "ready" && sample_count >= 80),
            other => panic!("check: {}", serde_json::to_string(&other).unwrap()),
        }
    }
    for side in [&mut a_side, &mut b_side] {
        assert!(
            timeout(Duration::from_millis(500), side.probe_result())
                .await
                .is_err(),
            "a recommendation was retired or replaced"
        );
    }
    drop((a_side, b_side));
    a.close().await;
    b.close().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_peers_new_check_always_completes_with_its_own_samples() {
    // Each exchange already overlaps two checks between the same pair; a few
    // rounds cover both completion orders. Running them one after another
    // keeps the sample counts about the checks, not about machine load.
    for _ in 0..2 {
        reverse_check_after_check().await;
        simultaneous_checks_both_ways().await;
    }
}

#[tokio::test]
async fn expired_probe_authorization_completion_never_installs_or_dials() {
    let host_primary = endpoint().await;
    let peer_primary = endpoint().await;
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
    let (events, mut events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(host_primary.clone(), events);
    let _invite = actor.setup_host_recovery(seed).await.unwrap();
    actor.epoch = 1;
    actor.room = Some(room);
    let recovery = actor.recovery.clone().unwrap();
    let target_incarnation = u64::MAX - 9;
    actor.remember_admission(Admission {
        room,
        incarnation: target_incarnation,
        authority_term: recovery.coordinator.current_term(),
        coordination_endpoint: peer_primary.id(),
        coordination_address: address(&peer_primary),
        primary_endpoint: peer_primary.id(),
    });
    let term = recovery.coordinator.current_term();
    let checkpoint = serde_json::to_vec(&serde_json::json!({
        "version": 1,
        "request": 50,
        "term": term,
        "base_revision": 0,
        "checkpoint": {
            "members": [
                {"member": 1, "incarnation": recovery.incarnation, "data": {"authenticatedEndpoint": host_primary.id().to_string()}},
                {"member": 2, "incarnation": target_incarnation, "data": {"authenticatedEndpoint": peer_primary.id().to_string()}}
            ],
            "room": {
                "version": 2,
                "snapshot": {
                    "members": [{"id": 1, "fighter": 3}, {"id": 2, "fighter": 7}],
                    "tables": [{"revision": 7, "p1": 1, "p2": 2}]
                }
            }
        },
        "effects": [],
        "effects_digest": "4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945"
    }))
    .unwrap();
    let transfer = CheckpointTransfer::new(room, 50, term, 0, 1, checkpoint).unwrap();
    let receipt = recovery
        .coordinator
        .propose(recovery.propose(&transfer).unwrap())
        .await
        .unwrap();
    assert!(receipt.accepted);
    assert!(
        recovery
            .probe_pair_bound(
                recovery.incarnation,
                target_incarnation,
                host_primary.id(),
                peer_primary.id(),
                7,
            )
            .await,
        "every completion fence except wall-clock expiry must be valid"
    );
    let key = ProbeAuthorizationKey {
        epoch: 1,
        room,
        incarnation: recovery.incarnation,
        peer: peer_primary.id(),
        target_incarnation,
        request: 51,
        pair_revision: 7,
        benchmark: false,
    };
    actor
        .pending_probe_authorizations
        .insert(peer_primary.id(), key.clone());
    actor.own_probes.insert(
        peer_primary.id(),
        OwnProbe {
            request: 51,
            pair_revision: 7,
        },
    );
    let task_count = actor.tasks.len();
    actor
        .completed(Completion::ProbeAuthorization(
            key,
            Ok(ProbeAuthorization {
                term: recovery.coordinator.current_term(),
                leader: recovery.coordinator.current_leader(),
                revision: recovery.coordinator.store().committed().await.revision(),
                expires: now().unwrap() + 60,
                // Its window has run out on the monotonic clock, whatever the wall
                // clock says.
                deadline: tokio::time::Instant::now(),
            }),
        ))
        .await
        .unwrap();
    assert!(!actor.probe_permissions.contains_key(&peer_primary.id()));
    assert!(!actor.own_probes.contains_key(&peer_primary.id()));
    assert_eq!(actor.tasks.len(), task_count);
    assert!(matches!(
        events_rx.recv().await,
        Some(Event::Error { code, .. }) if code == "probe_unavailable"
    ));
    recovery.stop().await;
    host_primary.close().await;
    peer_primary.close().await;
}

#[tokio::test]
async fn a_probe_reservation_wait_does_not_block_the_actor() {
    phase(
        "a_probe_reservation_wait_does_not_block_the_actor",
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
            let (events, _events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let mut actor = test_actor(host.clone(), events);
            let (_, recovery) = led_host(&mut actor, seed).await;
            let source = crate::recovery::RecoverySession::join(
                room,
                remote.id(),
                recovery.incarnation,
                recovery.coordination_address.clone(),
                crate::transport::Transport::Online,
            )
            .await
            .unwrap();
            let source_admission = source.advertise().await;
            recovery.add_learner(&source_admission).await.unwrap();
            actor.remember_admission(source_admission);
            actor.remember_admission(recovery.advertise().await);
            let state = recovery.state().await;
            // The native roster seats both members at table revision 5.
            let roster = serde_json::json!({
                "version": 1,
                "request": 1,
                "term": state.term,
                "base_revision": 0,
                "checkpoint": {
                    "members": [
                        {"member": 1, "incarnation": source.incarnation,
                         "data": {"authenticatedEndpoint": remote.id().to_string()}},
                        {"member": 2, "incarnation": recovery.incarnation,
                         "data": {"authenticatedEndpoint": host.id().to_string()}}
                    ],
                    "room": {"version": 2, "snapshot": {
                        "members": [{"id": 1}, {"id": 2}],
                        "tables": [{"revision": 5, "p1": 1, "p2": 2}]
                    }}
                }
            });
            let receipt = recovery
                .coordinator
                .propose(crate::coordination::Proposal {
                    request: "roster".into(),
                    dedup_id: "roster".into(),
                    term: state.term,
                    base: recovery.coordinator.store().committed().await.revision(),
                    checkpoint: roster.to_string().into(),
                    admin: None,
                })
                .await
                .unwrap();
            assert!(receipt.accepted);

            let expires = now().unwrap() + 60;
            let payload = serde_json::to_string(&CoordinationControl::ProbeReservation {
                room,
                source: remote.id(),
                source_incarnation: source.incarnation,
                target_incarnation: recovery.incarnation,
                request: 9,
                pair_revision: 5,
                term: state.term,
                expires,
            })
            .unwrap();
            // The reservation's Raft entry does not exist yet. The frame is
            // accepted at once instead of holding the actor tick for the wait.
            let started = std::time::Instant::now();
            assert!(
                actor
                    .accept_coordination_control(remote.id(), &payload)
                    .await
                    .unwrap()
            );
            assert!(started.elapsed() < Duration::from_secs(2));
            assert!(actor.probe_permissions.is_empty());
            assert!(actor.pending_probe_reservations.contains_key(&remote.id()));

            recovery
                .reserve_probe(
                    source.incarnation,
                    recovery.incarnation,
                    9,
                    5,
                    state.term,
                    expires,
                )
                .await
                .unwrap();
            complete_next(&mut actor).await;
            assert!(actor.pending_probe_reservations.is_empty());
            assert!(actor.probe_permissions.contains_key(&remote.id()));
            assert!(actor.probe_peers.contains(&remote.id()));
            source.stop().await;
            recovery.stop().await;
            host.close().await;
            remote.close().await;
        },
    )
    .await;
}
