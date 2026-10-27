//! Fixtures that more than one scenario family uses.

use super::*;

pub(in crate::service) async fn endpoint() -> Endpoint {
    Endpoint::builder(presets::Minimal)
        .clear_ip_transports()
        .bind_addr((Ipv4Addr::LOCALHOST, 0))
        .unwrap()
        .portmapper_config(PortmapperConfig::Disabled)
        .alpns(vec![CONTROL_ALPN.to_vec(), GAME_ALPN.to_vec()])
        .bind()
        .await
        .unwrap()
}

pub(super) fn address(endpoint: &Endpoint) -> EndpointAddr {
    EndpointAddr::new(endpoint.id()).with_ip_addr(endpoint.bound_sockets()[0])
}

/// The only route strings an event may carry: a class and a relay region.
pub(super) fn assert_public_route(route: &str) {
    let region = route.strip_prefix("relay:");
    assert!(
        route == "direct"
            || route == "unavailable"
            || region.is_some_and(|code| ["use1", "usw1", "euc1", "aps1", "other"].contains(&code)),
        "route {route:?} is not a public route"
    );
}

/// A serialized event names no address, port or relay URL, whatever route it
/// carries.
pub(super) fn assert_no_address(event: &Event) {
    let json = serde_json::to_string(event).unwrap();
    for address in ["ip:", "http", "iroh.link", "127.0.0.1", "[::"] {
        assert!(!json.contains(address), "{address} in {json}");
    }
    let value = serde_json::to_value(event).unwrap();
    if let Some(route) = value["route"].as_str() {
        assert_public_route(route);
    }
}

/// `work` within `limit`, or a failure that names the unfinished `step`.
pub(super) async fn phase<T>(step: &str, limit: Duration, work: impl Future<Output = T>) -> T {
    timeout(limit, work)
        .await
        .unwrap_or_else(|_| panic!("{step} did not finish within {limit:?}"))
}

pub(super) async fn next(events: &mut mpsc::Receiver<Event>, kind: &str) -> Event {
    loop {
        let event = events.recv().await.unwrap();
        let value = serde_json::to_value(&event).unwrap();
        if let Some(route) = value["route"].as_str() {
            assert_public_route(route);
        }
        if value["type"] == kind {
            return event;
        }
        assert_ne!(
            value["type"], "error",
            "unexpected helper error: {}",
            value["code"]
        );
    }
}

pub(in crate::service) fn test_actor(endpoint: Endpoint, events: mpsc::Sender<Event>) -> Actor {
    Actor {
        endpoint,
        relay_only: false,
        epoch: 0,
        opening: false,
        room: None,
        hosted: None,
        room_invite: None,
        host_address: None,
        controls: BTreeMap::new(),
        parked_controls: BTreeMap::new(),
        games: BTreeMap::new(),
        closed_generation: 0,
        tasks: JoinSet::new(),
        events: EventOutbox::new(events),
        recovery: None,
        admissions: BTreeMap::new(),
        admission_order: Vec::new(),
        pending_admissions: BTreeMap::new(),
        applied_admission_members: BTreeSet::new(),
        incoming_transfer: None,
        pending_checkpoint_proposal: None,
        pending_checkpoint_retry: None,
        outgoing_transfer: None,
        committed_native_members: None,
        pending_retired_incarnations: BTreeSet::new(),
        retired_incarnations: BTreeSet::new(),
        last_exported_revision: 0,
        pending_checkpoint_ack: None,
        pending_checkpoint_committed: None,
        next_transport_message: TRANSPORT_MESSAGE_ID_BASE,
        next_coordination_operation: 1,
        pending_coordination_refresh: None,
        pending_membership_operation: None,
        pending_admission_operation: None,
        pending_admission_bindings: Vec::new(),
        deferred_admissions: VecDeque::new(),
        pending_membership_publications: BTreeSet::new(),
        last_coordination_state: None,
        coordination_writable: false,
        unwritable_leader_since: None,
        reconnect_target: None,
        pending_game_admissions: BTreeMap::new(),
        pending_probe_invalidations: BTreeMap::new(),
        probe_peers: BTreeSet::new(),
        probe_permissions: BTreeMap::new(),
        own_probes: BTreeMap::new(),
        pending_probe_authorizations: BTreeMap::new(),
        pending_probe_reservations: BTreeMap::new(),
        join_settled: false,
        join_control_losses: 0,
        join_first_loss: None,
        retirement_started: None,
        departure_failed: false,
        short: ShortLinks::default(),
        public: None,
        coordination_port: None,
        refresh_roster: None,
        seats: Default::default(),
        wake: Arc::new(Notify::new()),
        latency: super::latency::Latency::offline(),
    }
}

/// Commits a native checkpoint replayable as transfer `transfer` on the
/// leader's coordination.
pub(super) async fn commit_native(
    recovery: &crate::recovery::RecoverySession,
    transfer: u64,
    checkpoint: &str,
) {
    let base = recovery.coordinator.store().committed().await.revision();
    let receipt = recovery
        .coordinator
        .propose(crate::coordination::Proposal {
            request: transfer.to_string(),
            dedup_id: format!("test:{transfer}"),
            term: recovery.coordinator.current_term(),
            base,
            checkpoint: checkpoint.into(),
            admin: None,
        })
        .await
        .unwrap();
    assert!(receipt.accepted);
}

/// Hosts `seed`'s room on `actor` and opens it at epoch 1 once `actor` leads
/// its coordination.
pub(super) async fn led_host(
    actor: &mut Actor,
    seed: Invite,
) -> (Invite, crate::recovery::RecoverySession) {
    let room = seed.room();
    let invite = actor.setup_host_recovery(seed).await.unwrap();
    let recovery = actor.recovery.clone().unwrap();
    while recovery.coordinator.current_leader() != Some(recovery.incarnation) {
        tokio::time::sleep(Duration::from_millis(25)).await;
    }
    actor.epoch = 1;
    actor.room = Some(room);
    actor.hosted = Some(invite.clone());
    actor.room_invite = Some(invite.clone());
    (invite, recovery)
}

/// One host actor running its loop, one remote endpoint with a control to it.
pub(super) struct BoundHost {
    pub(super) host: Endpoint,
    pub(super) remote: Endpoint,
    pub(super) room: [u8; 16],
    pub(super) invite: Invite,
    pub(super) recovery: crate::recovery::RecoverySession,
    pub(super) events: mpsc::Receiver<Event>,
    // Dropping either sender ends the actor's loop as an IPC loss.
    _commands: mpsc::Sender<Request>,
    _fault: watch::Sender<bool>,
    _scope: TaskScope,
}

pub(super) async fn bound_host(prepare: impl FnOnce(&mut Actor)) -> BoundHost {
    let host = endpoint().await;
    let remote = endpoint().await;
    let relay = test_relay();
    let seed = Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600).unwrap();
    let room = seed.room();
    let (events_tx, events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (fault, failure) = watch::channel(false);
    let mut actor = test_actor(host.clone(), events_tx);
    let (invite, recovery) = led_host(&mut actor, seed).await;
    prepare(&mut actor);
    let service = tokio::spawn(async move {
        let result = actor.run(command_rx, failure).await;
        actor.clear_room();
        actor.tasks.shutdown().await;
        result
    });
    BoundHost {
        host,
        remote,
        room,
        invite,
        recovery,
        events,
        _commands: commands,
        _fault: fault,
        _scope: TaskScope(vec![service.abort_handle()]),
    }
}

pub(super) async fn send_admission(
    control: &mut transport::ControlChannel,
    id: u64,
    admission: Admission,
) {
    let payload = serde_json::to_vec(&CoordinationControl::Admission { admission }).unwrap();
    control
        .sender
        .send(&ControlFrame {
            message_id: id,
            payload,
        })
        .await
        .unwrap();
}

/// Runs the actor's next finished worker through its completion handler.
pub(super) async fn complete_next(actor: &mut Actor) {
    let completion = timeout(Duration::from_secs(10), actor.tasks.join_next())
        .await
        .expect("worker completion")
        .expect("worker exists")
        .unwrap();
    actor.completed(completion).await.unwrap();
}

pub(super) fn test_relay() -> iroh::RelayUrl {
    iroh::defaults::prod::default_relay_map()
        .urls::<Vec<_>>()
        .remove(0)
}

pub(super) fn joined_invite(authority: &Endpoint) -> Invite {
    Invite::create(
        authority.id(),
        test_relay(),
        "test-build".into(),
        now().unwrap(),
        3600,
    )
    .unwrap()
}

pub(super) async fn next_error(events: &mut mpsc::Receiver<Event>) -> Event {
    loop {
        let event = events.recv().await.unwrap();
        if matches!(event, Event::Error { .. }) {
            return event;
        }
    }
}

/// One helper actor of a two-player room, driven through its command queue.
pub(super) struct GameSide {
    pub(super) commands: mpsc::Sender<Request>,
    pub(super) events: mpsc::Receiver<Event>,
    pub(super) socket: UdpSocket,
    pub(super) generation: u64,
    _fault: watch::Sender<bool>,
    _service: TaskScope,
}

impl GameSide {
    /// `peer` is dialed through its bound address.
    pub(super) async fn start(endpoint: &Endpoint, room: [u8; 16], peer: &Endpoint) -> Self {
        let (events_tx, events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (fault, failure) = watch::channel(false);
        let mut actor = test_actor(endpoint.clone(), events_tx);
        actor.epoch = 1;
        actor.room = Some(room);
        actor.host_address = Some(address(peer));
        let service = tokio::spawn(async move {
            let result = actor.run(command_rx, failure).await;
            actor.tasks.shutdown().await;
            result
        });
        Self {
            commands,
            events,
            socket: UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap(),
            generation: 1,
            _fault: fault,
            _service: TaskScope(vec![service.abort_handle()]),
        }
    }

    pub(super) async fn prepare(
        &mut self,
        room: [u8; 16],
        peer: &Endpoint,
        capability: u8,
        dial: bool,
    ) {
        self.commands
            .send(Request {
                id: 1,
                command: Command::PrepareGame {
                    epoch: 1,
                    peer: peer.id(),
                    room,
                    generation: self.generation,
                    capability: [capability; 32],
                    local_port: self.socket.local_addr().unwrap().port(),
                    max_packet: 1024,
                    dial,
                },
            })
            .await
            .unwrap();
        let _ = next(&mut self.events, "game_waiting").await;
    }

    pub(super) async fn ready(&mut self) -> u16 {
        match next(&mut self.events, "game_ready").await {
            Event::GameReady { virtual_port, .. } => virtual_port,
            _ => unreachable!(),
        }
    }

    /// End the current match. The next `prepare` is a rematch.
    pub(super) async fn end_match(&mut self) {
        self.commands
            .send(Request {
                id: 2,
                command: Command::EndMatch {
                    epoch: 1,
                    generation: self.generation,
                },
            })
            .await
            .unwrap();
        self.generation += 1;
    }

    /// The `gameplay_prepare_failed` error, and the reason on the `game_closed` after it.
    pub(super) async fn failure(&mut self) -> (Option<EndpointId>, String, Option<String>) {
        let mut error = None;
        loop {
            match self.events.recv().await.unwrap() {
                Event::Error {
                    code, peer, reason, ..
                } => {
                    assert_eq!(code, "gameplay_prepare_failed");
                    error = Some((peer, reason.unwrap_or_default()));
                }
                Event::GameClosed { reason, .. } if error.is_some() => {
                    let (peer, text) = error.unwrap();
                    return (peer, text, reason);
                }
                _ => (),
            }
        }
    }

    /// Nothing that ends or completes the slot arrives within `wait`.
    pub(super) async fn stays_waiting(&mut self, wait: Duration, why: &str) {
        let _ = timeout(wait, async {
            while let Some(event) = self.events.recv().await {
                assert!(
                    !matches!(
                        event,
                        Event::GameClosed { .. } | Event::Error { .. } | Event::GameReady { .. }
                    ),
                    "{why}: {}",
                    serde_json::to_string(&event).unwrap()
                );
            }
        })
        .await;
    }
}

pub(super) async fn game_connection_pair(
    from: &Endpoint,
    to: &Endpoint,
) -> (Connection, Connection) {
    let (dialed, accepted) = tokio::join!(from.connect(address(to), GAME_ALPN), async {
        to.accept().await.unwrap().await
    });
    (dialed.unwrap(), accepted.unwrap())
}

/// A hosting actor with one remote member seated at table revision 5, the
/// setup a probe reservation from that member needs. The remote's controls are
/// real workers so their ids differ.
pub(super) struct ReservationFixture {
    pub(super) host: Endpoint,
    pub(super) remote: Endpoint,
    pub(super) actor: Actor,
    pub(super) events: mpsc::Receiver<Event>,
    pub(super) recovery: crate::recovery::RecoverySession,
    pub(super) source: crate::recovery::RecoverySession,
    pub(super) invite: Invite,
    pub(super) room: [u8; 16],
    pub(super) term: u64,
    pub(super) expires: u64,
}

impl ReservationFixture {
    pub(super) async fn start() -> Self {
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
        let (events_tx, events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(host.clone(), events_tx);
        let (invite, recovery) = led_host(&mut actor, seed).await;
        let source = crate::recovery::RecoverySession::join(
            room,
            remote.id(),
            recovery.incarnation,
            recovery.coordination_address.clone(),
            false,
        )
        .await
        .unwrap();
        let source_admission = source.advertise().await;
        recovery.add_learner(&source_admission).await.unwrap();
        actor.remember_admission(source_admission);
        actor.remember_admission(recovery.advertise().await);
        let state = recovery.state().await;
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
        Self {
            host,
            remote,
            actor,
            events,
            recovery,
            source,
            invite,
            room,
            term: state.term,
            expires: now().unwrap() + 60,
        }
    }

    /// Install a fresh control from the remote, replacing any current one.
    /// Returns its id.
    pub(super) async fn connect_control(&mut self) -> u64 {
        let (_remote_side, accepted) = tokio::join!(
            async {
                let connection = self
                    .remote
                    .connect(address(&self.host), CONTROL_ALPN)
                    .await
                    .unwrap();
                transport::connect_control_on(connection, &self.invite)
                    .await
                    .unwrap()
            },
            async {
                let connection = self.host.accept().await.unwrap().await.unwrap();
                transport::accept_control(connection, &self.invite)
                    .await
                    .unwrap()
            }
        );
        let worker = ControlWorker::start(accepted);
        let id = worker.id();
        self.actor.remove_control(self.remote.id());
        self.actor.controls.insert(self.remote.id(), worker);
        id
    }

    pub(super) fn frame(&self, request: u64) -> String {
        serde_json::to_string(&CoordinationControl::ProbeReservation {
            room: self.room,
            source: self.remote.id(),
            source_incarnation: self.source.incarnation,
            target_incarnation: self.recovery.incarnation,
            request,
            pair_revision: 5,
            term: self.term,
            expires: self.expires,
        })
        .unwrap()
    }

    pub(super) async fn reserve(&self, request: u64) {
        self.recovery
            .reserve_probe(
                self.source.incarnation,
                self.recovery.incarnation,
                request,
                5,
                self.term,
                self.expires,
            )
            .await
            .unwrap();
    }

    pub(super) fn no_control_closed(&mut self) {
        while let Ok(event) = self.events.try_recv() {
            assert!(
                !matches!(event, Event::ControlClosed { .. }),
                "an expired reservation closed a control it was not presented on"
            );
        }
    }

    pub(super) async fn stop(mut self) {
        self.actor.tasks.abort_all();
        self.source.stop().await;
        self.recovery.stop().await;
        self.host.close().await;
        self.remote.close().await;
    }
}

impl ReservationFixture {
    /// Commits the native roster with both fighters seated at table
    /// `revision`, as the room's next checkpoint.
    pub(super) async fn seat_pair(&self, revision: u64) {
        let roster = serde_json::json!({
            "version": 1,
            "request": revision,
            "term": self.term,
            "base_revision": 0,
            "checkpoint": {
                "members": [
                    {"member": 1, "incarnation": self.source.incarnation,
                     "data": {"authenticatedEndpoint": self.remote.id().to_string()}},
                    {"member": 2, "incarnation": self.recovery.incarnation,
                     "data": {"authenticatedEndpoint": self.host.id().to_string()}}
                ],
                "room": {"version": 2, "snapshot": {
                    "members": [{"id": 1}, {"id": 2}],
                    "tables": [{"revision": revision, "p1": 1, "p2": 2}]
                }}
            }
        });
        let receipt = self
            .recovery
            .coordinator
            .propose(crate::coordination::Proposal {
                request: format!("roster:{revision}"),
                dedup_id: format!("roster:{revision}"),
                term: self.term,
                base: self
                    .recovery
                    .coordinator
                    .store()
                    .committed()
                    .await
                    .revision(),
                checkpoint: roster.to_string().into(),
                admin: None,
            })
            .await
            .unwrap();
        assert!(receipt.accepted);
    }

    /// Commits the reservation for the pair as seated at table `revision`.
    pub(super) async fn commit_reservation(&self, request: u64, revision: u64) {
        self.recovery
            .reserve_probe(
                self.source.incarnation,
                self.recovery.incarnation,
                request,
                revision,
                self.term,
                self.expires,
            )
            .await
            .unwrap();
    }

    /// The source's frame for that reservation.
    pub(super) fn frame_at(&self, request: u64, revision: u64) -> String {
        serde_json::to_string(&CoordinationControl::ProbeReservation {
            room: self.room,
            source: self.remote.id(),
            source_incarnation: self.source.incarnation,
            target_incarnation: self.recovery.incarnation,
            request,
            pair_revision: revision,
            term: self.term,
            expires: self.expires,
        })
        .unwrap()
    }

    pub(super) async fn present(&mut self, via: EndpointId, frame: &str) {
        assert!(
            self.actor
                .accept_coordination_control(via, frame)
                .await
                .unwrap()
        );
    }

    /// Nothing installed or pending, and the source's control still open.
    pub(super) fn dropped_quietly(&mut self, control: u64) {
        let remote = self.remote.id();
        assert!(self.actor.pending_probe_reservations.is_empty());
        assert!(self.actor.probe_permissions.is_empty());
        assert_eq!(
            self.actor.controls.get(&remote).map(ControlWorker::id),
            Some(control)
        );
        self.no_control_closed();
    }
}

/// Marks `actor` as the room's coordination leader in `term`, as a refresh
/// that saw it lead would.
pub(super) fn leads(actor: &mut Actor, term: u64) {
    actor.last_coordination_state = Some(CoordinationMarker {
        term,
        revision: 1,
        writable: true,
        leader_local: true,
        voter_count: 1,
        learner_count: 0,
    });
}
