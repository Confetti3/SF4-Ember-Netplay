//! The latency service against a local relay and a local DNS and pkarr
//! server, with no public infrastructure and no region hint
//! (MEMBER_LATENCY.md 5, Rust integration 1 to 6). The members run the real
//! actor loop and are driven over its command queue.
use std::any::Any;

use iroh::{
    RelayMap, RelayUrl,
    endpoint::{ConnectOptions, Connection},
    test_utils::{DnsPkarrServer, run_relay_server},
    tls::CaTlsConfig,
};
use tokio::{
    sync::{mpsc::UnboundedReceiver, watch},
    task::JoinHandle,
};

use super::{
    openings::{open, refused},
    *,
};
use crate::{
    service::latency::endpoint::{LATENCY_ALPN, latency_transport},
    wire::{CONTROL_ALPN, GAME_ALPN},
};

/// A local relay and a local DNS and pkarr server.
struct Infra {
    relay_map: RelayMap,
    relay_url: RelayUrl,
    dns: Arc<DnsPkarrServer>,
    _relay: Box<dyn Any + Send>,
}

impl Infra {
    async fn start() -> Self {
        let (relay_map, relay_url, relay) = run_relay_server().await.expect("relay");
        let dns = Arc::new(DnsPkarrServer::run().await.expect("dns"));
        Self {
            relay_map,
            relay_url,
            dns,
            _relay: Box::new(relay),
        }
    }

    fn net(&self) -> LatencyNet {
        let (map, dns) = (self.relay_map.clone(), self.dns.clone());
        LatencyNet::new(move || {
            Endpoint::builder(presets::Minimal)
                .preset(dns.preset())
                .relay_mode(RelayMode::Custom(map.clone()))
                .ca_tls_config(CaTlsConfig::insecure_skip_verify())
        })
    }

    /// Waits until each endpoint's record is published.
    async fn published(&self, endpoints: &[EndpointId]) {
        for id in endpoints {
            self.dns
                .on_endpoint(id, Duration::from_secs(15))
                .await
                .expect("published");
        }
    }

    /// `members` bound on this infrastructure, with their records published.
    async fn members(&self, count: u64, relay_only: bool) -> Vec<Member> {
        let mut members = Vec::new();
        for id in 1..=count {
            members.push(member_on(self.net(), None, id, relay_only).await);
        }
        let ids: Vec<EndpointId> = members.iter().map(Member::latency).collect();
        self.published(&ids).await;
        members
    }
}

/// A member running the actor loop.
struct Node {
    commands: mpsc::Sender<Request>,
    tables: UnboundedReceiver<(u64, Vec<MemberLatencyRow>)>,
    recovery: RecoverySession,
    task: JoinHandle<Actor>,
    next_request: u64,
    _ipc: watch::Sender<bool>,
    latency: Endpoint,
    seat: Seat,
}

impl Node {
    fn start(member: Member) -> Self {
        let seat = member.seat();
        let latency = member.latency_endpoint().clone();
        let Member {
            mut actor,
            mut events,
            recovery,
            ..
        } = member;
        let (commands, receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (ipc, failure) = watch::channel(false);
        let (tables_tx, tables) = tokio::sync::mpsc::unbounded_channel();
        tokio::spawn(async move {
            while let Some(event) = events.recv().await {
                if let Event::MemberLatency { view, members, .. } = event {
                    let _ = tables_tx.send((view, members));
                }
            }
        });
        let task = tokio::spawn(async move {
            let _ = actor.run(receiver, failure).await;
            actor
        });
        Self {
            commands,
            tables,
            recovery,
            task,
            next_request: 10,
            _ipc: ipc,
            latency,
            seat,
        }
    }

    async fn command(&mut self, command: Command) {
        self.next_request += 1;
        self.commands
            .send(Request {
                id: self.next_request,
                command,
            })
            .await
            .expect("actor running");
    }

    async fn watch(&mut self, view: u64, on: bool) {
        self.command(Command::LatencyWatch {
            epoch: 1,
            room: ROOM,
            view,
            on,
        })
        .await;
    }

    async fn commit(&self, checkpoint: &str) {
        let accepted = self
            .recovery
            .coordinator
            .store()
            .apply_checkpoints(&[checkpoint.to_owned()])
            .await;
        assert_eq!(accepted, 1);
    }

    /// The tables of view `view` until one satisfies `done`.
    async fn table(
        &mut self,
        view: u64,
        limit: Duration,
        done: impl Fn(&[MemberLatencyRow]) -> bool,
    ) -> Vec<MemberLatencyRow> {
        let mut last = Vec::new();
        let found = timeout(limit, async {
            while let Some((at, rows)) = self.tables.recv().await {
                if at == view {
                    if done(&rows) {
                        return rows;
                    }
                    last = rows;
                }
            }
            panic!("the actor stopped")
        })
        .await;
        found.unwrap_or_else(|_| panic!("no such table for view {view}; the last was {last:?}"))
    }

    /// Stops the actor loop and hands the actor back for inspection.
    async fn stop(mut self) -> (Actor, RecoverySession) {
        self.command(Command::Shutdown).await;
        let actor = timeout(Duration::from_secs(10), self.task)
            .await
            .expect("stops")
            .expect("ran");
        (actor, self.recovery)
    }
}

async fn finish(nodes: Vec<Node>) -> Vec<Actor> {
    let mut actors = Vec::new();
    for node in nodes {
        let (mut actor, recovery) = node.stop().await;
        actor.clear_room();
        recovery.stop().await;
        actor.endpoint.close().await;
        actors.push(actor);
    }
    actors
}

fn values(rows: &[MemberLatencyRow], count: usize) -> bool {
    rows.len() == count
        && rows
            .iter()
            .all(|row| matches!(row.state, LatencyState::Value { rtt_ms, .. } if (1..=999).contains(&rtt_ms)))
}

fn is_value(state: LatencyState) -> bool {
    matches!(state, LatencyState::Value { .. })
}

fn state_of(rows: &[MemberLatencyRow], member: u64) -> Option<LatencyState> {
    rows.iter()
        .find(|row| row.member == member)
        .map(|row| row.state)
}

#[tokio::test]
async fn a_private_room_measures_an_idle_member_a_spectator_and_another_table() {
    let infra = Infra::start().await;
    let members = infra.members(4, false).await;
    let seats = vec![
        members[0].seat().at(STATUS_SEATED, 0),
        members[1].seat(),
        members[2].seat().at(STATUS_WATCHING, 0),
        members[3].seat().at(STATUS_SEATED, 1),
    ];
    let checkpoint = checkpoint(&seats, &[(0, PHASE_WAITING), (1, PHASE_WAITING)]);
    for member in &members {
        member.commit(&checkpoint).await;
    }
    let mut nodes: Vec<Node> = members.into_iter().map(Node::start).collect();
    nodes[0].watch(1, true).await;
    let rows = nodes[0]
        .table(1, Duration::from_secs(40), |rows| values(rows, 3))
        .await;
    let mut measured: Vec<u64> = rows.iter().map(|row| row.member).collect();
    measured.sort_unstable();
    assert_eq!(measured, [2, 3, 4]);
    for row in &rows {
        assert_eq!(row.incarnation, seats[row.member as usize - 1].incarnation);
    }
    finish(nodes).await;
}

#[tokio::test]
async fn strangers_wrong_incarnations_old_incarnations_and_rowless_members_are_refused() {
    let infra = Infra::start().await;
    let members = infra.members(3, false).await;
    let stranger = bind_latency(&infra.net(), false).await.unwrap();
    let old = bind_latency(&infra.net(), false).await.unwrap();
    infra.published(&[stranger.id(), old.id()]).await;
    let (first, second, third) = (
        members[0].incarnation(),
        members[1].incarnation(),
        members[2].incarnation(),
    );
    let seats: Vec<Seat> = members.iter().map(Member::seat).collect();
    let second_endpoint = members[1].latency_endpoint().clone();
    // Member 3 is not in its own copy of the roster.
    let without_third = checkpoint(&seats[..2], &[]);
    members[0].commit(&checkpoint(&seats, &[])).await;
    members[1].commit(&checkpoint(&seats, &[])).await;
    members[2].commit(&without_third).await;
    let mut nodes: Vec<Node> = members.into_iter().map(Node::start).collect();
    let target = EndpointAddr::new(nodes[0].latency.id()).with_relay_url(infra.relay_url.clone());
    let dial = |from: &Endpoint| {
        let (from, target) = (from.clone(), target.clone());
        async move {
            let options = ConnectOptions::new().with_transport_config(latency_transport(0));
            from.connect_with_opts(target, LATENCY_ALPN, options)
                .await
                .unwrap()
                .await
                .expect("TLS completes")
        }
    };
    // A non-member.
    let connection = dial(&stranger).await;
    assert!(refused(&connection).await);
    // A member naming the wrong incarnations.
    let connection = dial(&second_endpoint).await;
    assert!(open(&connection, second + 1, first).await.is_none());
    assert!(closed(&connection).await);
    let connection = dial(&second_endpoint).await;
    assert!(open(&connection, second, first + 1).await.is_none());
    assert!(closed(&connection).await);
    // Member 2 rejoins under a new incarnation and endpoint; the old ones are
    // refused.
    let mut rejoined = seats.clone();
    rejoined[1] = Seat::new(2, second + 1000, seats[1].primary, Some(old.id()));
    nodes[0].commit(&checkpoint(&rejoined, &[])).await;
    sleep(Duration::from_millis(200)).await;
    let connection = dial(&second_endpoint).await;
    assert!(refused(&connection).await);
    let connection = dial(&old).await;
    assert!(
        open(&connection, second, first).await.is_none(),
        "the old incarnation"
    );
    // The member without a row of its own is unavailable everywhere it looks,
    // and refuses the others.
    nodes[2].watch(1, true).await;
    let rows = nodes[2]
        .table(1, Duration::from_secs(10), |rows| !rows.is_empty())
        .await;
    assert!(
        rows.iter()
            .all(|row| row.state == LatencyState::Unavailable { reason: LatencyUnavailable::RoomUnavailable }),
        "{rows:?}"
    );
    nodes[0].watch(1, true).await;
    let rows = nodes[0]
        .table(1, Duration::from_secs(20), |rows| {
            state_of(rows, 3) == Some(LatencyState::Unavailable { reason: LatencyUnavailable::Refused })
        })
        .await;
    assert_eq!(
        rows.iter().find(|row| row.member == 3).unwrap().incarnation,
        third
    );
    stranger.close().await;
    old.close().await;
    finish(nodes).await;
}

#[tokio::test]
async fn latency_endpoints_take_no_control_or_game_connection_and_touch_nothing_else() {
    let infra = Infra::start().await;
    let members = infra.members(2, false).await;
    let checkpoint = checkpoint(&members.iter().map(Member::seat).collect::<Vec<_>>(), &[]);
    for member in &members {
        member.commit(&checkpoint).await;
    }
    let mut nodes: Vec<Node> = members.into_iter().map(Node::start).collect();
    nodes[0].watch(1, true).await;
    nodes[0]
        .table(1, Duration::from_secs(40), |rows| values(rows, 1))
        .await;
    // A primary-style endpoint on the same relay reaches the latency
    // endpoint on its ALPN, and on no other.
    let caller = bind_latency(&infra.net(), false).await.unwrap();
    let target = EndpointAddr::new(nodes[0].latency.id()).with_relay_url(infra.relay_url.clone());
    let reached = caller
        .connect(target.clone(), LATENCY_ALPN)
        .await
        .expect("reachable");
    assert!(refused(&reached).await);
    for alpn in [CONTROL_ALPN, GAME_ALPN] {
        let attempt = timeout(
            Duration::from_secs(10),
            caller.connect(target.clone(), alpn),
        )
        .await
        .expect("answered");
        assert!(attempt.is_err(), "{}", String::from_utf8_lossy(alpn));
    }
    caller.close().await;
    for actor in finish(nodes).await {
        assert!(actor.probe_permissions.is_empty());
        assert!(actor.probe_peers.is_empty());
        assert!(actor.games.is_empty());
        assert!(actor.own_probes.is_empty());
    }
}

#[tokio::test]
async fn an_opened_connection_takes_no_more_streams_or_datagrams() {
    let infra = Infra::start().await;
    let members = infra.members(3, false).await;
    let checkpoint = checkpoint(&members.iter().map(Member::seat).collect::<Vec<_>>(), &[]);
    for member in &members {
        member.commit(&checkpoint).await;
    }
    let (first, second) = (members[0].incarnation(), members[1].incarnation());
    let second_endpoint = members[1].latency_endpoint().clone();
    let mut nodes: Vec<Node> = members.into_iter().map(Node::start).collect();
    // Member 2 opens to member 1 by hand.
    let target = EndpointAddr::new(nodes[0].latency.id()).with_relay_url(infra.relay_url.clone());
    let options = ConnectOptions::new().with_transport_config(latency_transport(0));
    let connection: Connection = second_endpoint
        .connect_with_opts(target, LATENCY_ALPN, options)
        .await
        .unwrap()
        .await
        .unwrap();
    let _opened = open(&connection, second, first).await.expect("opened");
    assert!(
        timeout(Duration::from_secs(2), connection.open_bi())
            .await
            .is_err(),
        "a second stream never opens"
    );
    assert!(
        timeout(Duration::from_secs(2), connection.open_uni())
            .await
            .is_err()
    );
    assert!(connection.send_datagram(vec![1u8].into()).is_err());
    // Member 1's board keeps its values meanwhile.
    nodes[0].watch(1, true).await;
    nodes[0]
        .table(1, Duration::from_secs(40), |rows| values(rows, 2))
        .await;
    sleep(Duration::from_secs(2)).await;
    nodes[0].watch(1, true).await;
    assert!(
        connection.close_reason().is_none(),
        "the opening still stands"
    );
    finish(nodes).await;
}

#[tokio::test]
async fn departure_rejoin_a_match_and_leaving_the_room() {
    let infra = Infra::start().await;
    let members = infra.members(3, false).await;
    let seats: Vec<Seat> = members.iter().map(Member::seat).collect();
    let all = checkpoint(&seats, &[]);
    for member in &members {
        member.commit(&all).await;
    }
    let peer = members[1].primary();
    let mut nodes: Vec<Node> = members.into_iter().map(Node::start).collect();
    nodes[0].watch(1, true).await;
    nodes[0]
        .table(1, Duration::from_secs(40), |rows| values(rows, 2))
        .await;
    // Member 3 leaves, then comes back.
    let without_third = checkpoint(&seats[..2], &[]);
    for node in &nodes {
        node.commit(&without_third).await;
    }
    nodes[0]
        .table(1, Duration::from_secs(10), |rows| {
            rows.len() == 1 && rows[0].member == 2
        })
        .await;
    for node in &nodes {
        node.commit(&all).await;
    }
    nodes[0].watch(1, true).await;
    nodes[0]
        .table(1, Duration::from_secs(40), |rows| values(rows, 2))
        .await;

    // Member 2 prepares a game: it refuses everyone until it is in the
    // room's committed match, and then its last value is kept.
    let opponent = nodes[0].seat.primary;
    nodes[1]
        .command(Command::PrepareGame {
            epoch: 1,
            peer: opponent,
            room: ROOM,
            generation: 1,
            capability: [9; 32],
            local_port: 41_001,
            max_packet: 1200,
            dial: false,
        })
        .await;
    let rows = nodes[0]
        .table(1, Duration::from_secs(30), |rows| {
            state_of(rows, 2).is_some_and(|state| !is_value(state))
        })
        .await;
    assert!(
        matches!(
            state_of(&rows, 2),
            Some(LatencyState::Unavailable { .. } | LatencyState::Measuring)
        ),
        "{rows:?}"
    );
    let mut playing = seats.clone();
    playing[1] = playing[1].clone().at(STATUS_PLAYING, 0);
    let in_match = checkpoint(&playing, &[(0, PHASE_PLAYING)]);
    nodes[0].commit(&in_match).await;
    nodes[0].watch(1, true).await;
    let rows = nodes[0]
        .table(1, Duration::from_secs(10), |rows| {
            state_of(rows, 2).is_some_and(is_value)
                || state_of(rows, 2) == Some(LatencyState::Unavailable { reason: LatencyUnavailable::InMatch })
        })
        .await;
    assert!(state_of(&rows, 3).is_some());
    let _ = peer;

    // Member 1 leaves: its endpoint closes and it reports nothing more.
    nodes[0]
        .command(Command::Leave {
            epoch: 1,
            abandon: true,
        })
        .await;
    timeout(Duration::from_secs(10), async {
        while !nodes[0].latency.is_closed() {
            sleep(Duration::from_millis(20)).await;
        }
    })
    .await
    .expect("the latency endpoint closes with the room");
    while nodes[0].tables.try_recv().is_ok() {}
    sleep(Duration::from_millis(1500)).await;
    assert!(
        nodes[0].tables.try_recv().is_err(),
        "no table after leaving"
    );
    finish(nodes).await;
}

#[tokio::test]
async fn a_public_room_measures_over_relays_even_with_a_direct_route() {
    let infra = Infra::start().await;
    let mut members = infra.members(2, true).await;
    for member in &members {
        assert_eq!(
            member.latency_endpoint().addr().ip_addrs().count(),
            0,
            "a relay-only endpoint has no IP address"
        );
        assert!(member.latency_endpoint().bound_sockets().is_empty());
    }
    // A direct loopback route between the two hosts, on a test-only ALPN.
    const DIRECT: &[u8] = b"sf4e/test/direct";
    let direct_endpoint = |alpns: Vec<Vec<u8>>| async move {
        Endpoint::builder(presets::Minimal)
            .relay_mode(RelayMode::Disabled)
            .clear_ip_transports()
            .bind_addr((Ipv4Addr::LOCALHOST, 0))
            .unwrap()
            .alpns(alpns)
            .bind()
            .await
            .unwrap()
    };
    let listener = direct_endpoint(vec![DIRECT.to_vec()]).await;
    let dialer = direct_endpoint(Vec::new()).await;
    let address = super::openings::loopback(&listener);
    let accepting = listener.clone();
    let accepted = tokio::spawn(async move { accepting.accept().await.unwrap().await.unwrap() });
    let direct = dialer.connect(address, DIRECT).await.unwrap();
    let _direct_peer = accepted.await.unwrap();
    assert!(
        direct
            .paths()
            .iter()
            .any(|path| path.is_selected() && path.is_ip())
    );

    let checkpoint = checkpoint(&members.iter().map(Member::seat).collect::<Vec<_>>(), &[]);
    for member in &mut members {
        member.commit(&checkpoint).await;
        member.actor.latency_refresh().await;
        member.actor.latency_watch(1, ROOM, 1, true);
    }
    settle(&mut members, Duration::from_secs(40), |members| {
        members
            .iter()
            .all(|member| member.actor.latency.outbound.len() == 1)
    })
    .await;
    // Give the paths time to try for a direct one.
    let until = Instant::now() + Duration::from_secs(3);
    while Instant::now() < until {
        for member in &mut members {
            step(&mut member.actor, Duration::from_millis(10)).await;
        }
    }
    for member in &mut members {
        member.actor.latency_second().await;
        let slots = member
            .actor
            .latency
            .outbound
            .values()
            .map(|slot| &slot.guard.connection)
            .chain(
                member
                    .actor
                    .latency
                    .inbound
                    .values()
                    .map(|slot| &slot.guard.connection),
            );
        for connection in slots {
            let paths = connection.paths();
            assert!(!paths.is_empty());
            assert!(
                paths.iter().all(|path| path.is_relay()),
                "every latency path is a relay"
            );
        }
        let rows = member.latency_events().pop().expect("a table").1;
        assert!(
            matches!(
                rows[0].state,
                LatencyState::Value {
                    route: LatencyRoute::Relay,
                    ..
                }
            ),
            "{rows:?}"
        );
        assert_eq!(member.latency_endpoint().addr().ip_addrs().count(), 0);
    }
    assert!(direct.close_reason().is_none());
    listener.close().await;
    dialer.close().await;
    for member in members {
        member.stop().await;
    }
}

#[tokio::test]
async fn a_join_never_waits_for_the_latency_relay() {
    // Latency endpoints whose relay cannot be reached, as behind a firewall
    // that blocks the relays, finding each other on loopback.
    let lookup = MemoryLookup::new();
    let blocked: RelayUrl = "https://relay.invalid./".parse().unwrap();
    let net = {
        let (lookup, blocked) = (lookup.clone(), blocked.clone());
        LatencyNet::new(move || {
            Endpoint::builder(presets::Minimal)
                .relay_mode(RelayMode::custom([blocked.clone()]))
                .address_lookup(lookup.clone())
                .clear_ip_transports()
                .bind_addr((Ipv4Addr::LOCALHOST, 0))
                .expect("loopback")
        })
    };
    // The join's own work ends after half a second: it is not held for the
    // relay, which the endpoint never reaches.
    let started = Instant::now();
    let (done, latency) = beside(Some((net.clone(), false)), async {
        sleep(Duration::from_millis(500)).await;
        7
    })
    .await;
    assert_eq!(done, 7);
    assert!(
        started.elapsed() < Duration::from_secs(1),
        "{:?}",
        started.elapsed()
    );
    let latency = latency.expect("bound while the work ran");
    assert!(crate::service::network::known_home_region(latency.endpoint()).is_none());
    // Work that is done at once leaves no route: the bind is dropped.
    let (_, none) = beside(Some((net.clone(), false)), async {}).await;
    assert!(none.is_none());
    drop(latency);

    // Members bound that way: their routes carry no hint, and they still
    // measure each other.
    let mut members = Vec::new();
    for id in 1..=2 {
        members.push(member_on(net.clone(), Some(&lookup), id, false).await);
    }
    for member in &mut members {
        let (route, hint) = member.actor.latency_route();
        assert_eq!(route, Some(member.latency()));
        assert_eq!(hint, None);
    }
    let checkpoint = checkpoint(&members.iter().map(Member::seat).collect::<Vec<_>>(), &[]);
    for member in &mut members {
        member.commit(&checkpoint).await;
        member.actor.latency_refresh().await;
    }
    members[0].actor.latency_watch(1, ROOM, 1, true);
    settle(&mut members, Duration::from_secs(10), |members| {
        members[0].actor.latency.outbound.contains_key(&2)
    })
    .await;
    members[0].actor.latency_second().await;
    let rows = members[0].latency_events().pop().expect("a table").1;
    assert!(is_value(rows[0].state), "{rows:?}");
    for member in members {
        member.stop().await;
    }
}
