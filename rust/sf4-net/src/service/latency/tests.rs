//! Tests for ping to each member (MEMBER_LATENCY.md section 5). This file
//! holds the harness and the roster, authority, store, value, endpoint and
//! subscription tests; `tests/` holds the openings, the slots and the
//! network tests.
use std::{net::Ipv4Addr, sync::Arc, time::Duration};

use iroh::{
    Endpoint, EndpointAddr, EndpointId, RelayMode, address_lookup::MemoryLookup, endpoint::presets,
};
use serde_json::{Value, json};
use tokio::{
    sync::mpsc,
    time::{Instant, sleep, sleep_until, timeout},
};

use super::{
    authority::{AuthorityState, Inputs, Observer, PeerBinding, Tuple, UnavailableReason},
    endpoint::{LatencyEndpoint, LatencyNet, bind_latency},
    *,
};
use crate::{
    coordination::{Committed, committed_latency_roster},
    recovery::RecoverySession,
    service::{
        Command, Event, IPC_QUEUE_CAPACITY, Request,
        tests::{endpoint as primary_endpoint, test_actor},
    },
};

mod network;
mod openings;
mod slots;

pub(super) const ROOM: [u8; 16] = [0x5a; 16];
pub(super) const STATUS_IDLE: u8 = 0;
pub(super) const STATUS_SEATED: u8 = 2;
pub(super) const STATUS_PLAYING: u8 = 4;
pub(super) const STATUS_WATCHING: u8 = 5;
pub(super) const PHASE_WAITING: u64 = 1;
pub(super) const PHASE_PLAYING: u64 = 3;

/// A roster row as the native checkpoint writes it.
#[derive(Clone)]
pub(super) struct Seat {
    pub member: u64,
    pub incarnation: u64,
    pub primary: EndpointId,
    pub latency: Option<String>,
    pub relay: Option<&'static str>,
    pub status: u8,
    pub table: i8,
    pub frozen: bool,
    pub in_snapshot: bool,
}

impl Seat {
    pub fn new(
        member: u64,
        incarnation: u64,
        primary: EndpointId,
        latency: Option<EndpointId>,
    ) -> Self {
        Self {
            member,
            incarnation,
            primary,
            latency: latency.map(|id| id.to_string()),
            relay: None,
            status: STATUS_IDLE,
            table: -1,
            frozen: false,
            in_snapshot: true,
        }
    }

    pub fn at(mut self, status: u8, table: i8) -> Self {
        self.status = status;
        self.table = table;
        self
    }
}

/// The committed `SessionProposal` envelope of a room holding `seats`, with
/// tables `(id, phase)`.
pub(super) fn checkpoint(seats: &[Seat], tables: &[(i64, u64)]) -> String {
    let rows: Vec<Value> = seats
        .iter()
        .map(|seat| {
            let latency = match &seat.latency {
                Some(endpoint) => match seat.relay {
                    Some(relay) => json!({"endpoint": endpoint, "relay": relay}),
                    None => json!({"endpoint": endpoint}),
                },
                None => json!({}),
            };
            let mut row = json!({
                "member": seat.member,
                "endpoint": {"host": "room", "user": seat.primary.to_string()},
                "incarnation": seat.incarnation,
                "data": {
                    "authenticatedEndpoint": seat.primary.to_string(),
                    "incarnation": seat.incarnation,
                    "roomMember": seat.member,
                    "latency": latency,
                },
            });
            if seat.frozen {
                row["frozen"] = json!(true);
            }
            row
        })
        .collect();
    let snapshot_members: Vec<Value> = seats
        .iter()
        .filter(|seat| seat.in_snapshot && !seat.frozen)
        .map(|seat| {
            json!({"id": seat.member, "status": seat.status, "table": seat.table,
                   "incarnation": seat.incarnation})
        })
        .collect();
    let tables: Vec<Value> = tables
        .iter()
        .map(|(id, phase)| json!({"id": id, "phase": phase}))
        .collect();
    json!({
        "version": 1,
        "request": 1,
        "term": 1,
        "base_revision": 0,
        "checkpoint": {
            "members": rows,
            "room": {"version": 2, "snapshot": {"members": snapshot_members, "tables": tables}},
        },
        "effects": [],
    })
    .to_string()
}

/// Latency endpoints on loopback that find each other through `lookup`.
pub(super) fn local_net(lookup: &MemoryLookup) -> LatencyNet {
    let lookup = lookup.clone();
    LatencyNet::new(move || {
        Endpoint::builder(presets::Minimal)
            .relay_mode(RelayMode::Disabled)
            .address_lookup(lookup.clone())
            .clear_ip_transports()
            .bind_addr((Ipv4Addr::LOCALHOST, 0))
            .expect("loopback")
    })
}

/// One room member: its actor with a primary endpoint, a recovery session of
/// its own and a bound latency endpoint.
pub(super) struct Member {
    pub actor: Actor,
    pub events: mpsc::Receiver<Event>,
    pub recovery: RecoverySession,
    pub member: u64,
}

impl Member {
    pub fn primary(&self) -> EndpointId {
        self.actor.endpoint.id()
    }

    pub fn latency(&self) -> EndpointId {
        self.latency_endpoint().id()
    }

    pub fn latency_endpoint(&self) -> &Endpoint {
        self.actor
            .latency
            .endpoint
            .as_ref()
            .expect("latency endpoint")
            .endpoint()
    }

    pub fn incarnation(&self) -> u64 {
        self.recovery.incarnation
    }

    /// Its committed row, idle.
    pub fn seat(&self) -> Seat {
        Seat::new(
            self.member,
            self.incarnation(),
            self.primary(),
            Some(self.latency()),
        )
    }

    /// Applies `checkpoint` to this member's store as one committed entry.
    pub async fn commit(&self, checkpoint: &str) {
        let accepted = self
            .recovery
            .coordinator
            .store()
            .apply_checkpoints(&[checkpoint.to_owned()])
            .await;
        assert_eq!(accepted, 1);
    }

    /// The latency events since the last call.
    pub fn latency_events(&mut self) -> Vec<(u64, Vec<MemberLatencyRow>)> {
        let mut found = Vec::new();
        while let Ok(event) = self.events.try_recv() {
            if let Event::MemberLatency { view, members, .. } = event {
                found.push((view, members));
            }
        }
        found
    }

    pub async fn stop(self) {
        let Self {
            mut actor,
            recovery,
            ..
        } = self;
        actor.clear_room();
        recovery.stop().await;
        actor.endpoint.close().await;
    }
}

/// A member of room `ROOM` at epoch 1 whose latency endpoints are built by
/// `net`. With `lookup`, its endpoint's loopback address is registered there.
pub(super) async fn member_on(
    net: LatencyNet,
    lookup: Option<&MemoryLookup>,
    member: u64,
    relay_only: bool,
) -> Member {
    let primary = primary_endpoint().await;
    let (events_tx, events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(primary.clone(), events_tx);
    actor.latency.net = net;
    actor.epoch = 1;
    actor.room = Some(ROOM);
    let recovery = RecoverySession::host_in(
        ROOM,
        primary.id(),
        crate::transport::Transport::from(false),
        None,
        false,
    )
    .await
    .expect("recovery");
    actor.install_recovery(recovery.clone()).await;
    let latency = bind_latency(&actor.latency.net, relay_only.into())
        .await
        .expect("latency endpoint");
    if let Some(lookup) = lookup {
        let socket = latency.bound_sockets()[0];
        lookup.add_endpoint_info(
            EndpointAddr::new(latency.id())
                .with_ip_addr((Ipv4Addr::LOCALHOST, socket.port()).into()),
        );
    }
    actor.latency.endpoint = Some(LatencyEndpoint::new(latency));
    Member {
        actor,
        events,
        recovery,
        member,
    }
}

/// A loopback member.
pub(super) async fn member(lookup: &MemoryLookup, member: u64) -> Member {
    member_on(local_net(lookup), Some(lookup), member, false).await
}

/// `count` loopback members of one room, every one committed idle and read
/// by its actor.
pub(super) async fn room(count: u64) -> (MemoryLookup, Vec<Member>) {
    let lookup = MemoryLookup::new();
    let mut members = Vec::new();
    for id in 1..=count {
        members.push(member(&lookup, id).await);
    }
    let seats: Vec<Seat> = members.iter().map(Member::seat).collect();
    let checkpoint = checkpoint(&seats, &[]);
    for member in &mut members {
        member.commit(&checkpoint).await;
        member.actor.latency_refresh().await;
        assert!(member.actor.latency.authority().local().is_some());
    }
    (lookup, members)
}

/// Runs one actor's latency arms for `window`.
pub(super) async fn step(actor: &mut Actor, window: Duration) {
    let deadline = Instant::now() + window;
    loop {
        tokio::select! {
            incoming = accept(actor.latency.endpoint.as_ref()), if actor.latency.endpoint.is_some() => {
                actor.latency_incoming(incoming);
            }
            Some(result) = actor.latency.openings_in.join_next_with_id(), if !actor.latency.openings_in.is_empty() => {
                actor.latency_inbound_done(result).await;
            }
            Some(result) = actor.latency.openings_out.join_next_with_id(), if !actor.latency.openings_out.is_empty() => {
                actor.latency_outbound_done(result).await;
            }
            () = sleep_until(deadline) => return,
        }
    }
}

/// Steps every member in turn until `done` holds, or panics after `limit`.
pub(super) async fn settle(
    members: &mut [Member],
    limit: Duration,
    done: impl Fn(&[Member]) -> bool,
) {
    let deadline = Instant::now() + limit;
    loop {
        for member in members.iter_mut() {
            step(&mut member.actor, Duration::from_millis(10)).await;
        }
        if done(members) {
            return;
        }
        assert!(Instant::now() < deadline, "latency did not settle");
    }
}

/// Accepts the next incoming latency connection on `actor` and spawns its
/// opening, without reaping anything.
pub(super) async fn accept_one(actor: &mut Actor) {
    let incoming = timeout(
        Duration::from_secs(5),
        accept(actor.latency.endpoint.as_ref()),
    )
    .await
    .expect("incoming latency connection");
    actor.latency_incoming(incoming);
}

/// Prepares a game on `actor`, as the native side does before a match.
pub(super) async fn prepare_game(actor: &mut Actor, peer: EndpointId) {
    let id = 7_000;
    actor
        .command(Request {
            id,
            command: Command::PrepareGame {
                epoch: actor.epoch,
                peer,
                room: ROOM,
                generation: 1,
                capability: [7; 32],
                local_port: 41_000,
                max_packet: 1200,
                dial: false,
            },
        })
        .await
        .expect("prepare game");
    assert!(!actor.games.is_empty(), "the game slot is held at once");
}

/// Waits until `connection` reports why it closed.
pub(super) async fn closed(connection: &iroh::endpoint::Connection) -> bool {
    timeout(Duration::from_secs(5), connection.closed())
        .await
        .is_ok()
}

fn roster(seats: &[Seat], tables: &[(i64, u64)]) -> crate::coordination::LatencyRoster {
    committed_latency_roster(&checkpoint(seats, tables)).expect("roster")
}

fn key(seed: u8) -> EndpointId {
    iroh::SecretKey::from_bytes(&[seed; 32]).public()
}

#[test]
fn roster_takes_idle_members_spectators_and_other_tables_but_not_frozen_or_missing_rows() {
    let seats = vec![
        Seat::new(1, 11, key(1), Some(key(101))),
        Seat::new(2, 22, key(2), Some(key(102))).at(STATUS_WATCHING, 0),
        Seat::new(3, 33, key(3), Some(key(103))).at(STATUS_SEATED, 1),
        Seat {
            frozen: true,
            ..Seat::new(4, 44, key(4), Some(key(104)))
        },
        Seat {
            in_snapshot: false,
            ..Seat::new(5, 55, key(5), Some(key(105)))
        },
        Seat::new(6, 66, key(6), Some(key(106))).at(STATUS_PLAYING, 1),
        Seat::new(7, 77, key(7), Some(key(107))).at(STATUS_WATCHING, 1),
    ];
    let roster = roster(&seats, &[(0, PHASE_WAITING), (1, PHASE_PLAYING)]);
    let members: Vec<u64> = roster.rows().iter().map(|row| row.member).collect();
    assert_eq!(members, [1, 2, 3, 6, 7]);
    let row = roster.row(2).unwrap();
    assert_eq!(
        (row.incarnation, row.primary, row.latency),
        (22, key(2), Some(key(102)))
    );
    assert!(!row.in_match, "watching a table that is not playing");
    assert!(!roster.row(3).unwrap().in_match, "seated at another table");
    assert!(roster.row(6).unwrap().in_match, "playing");
    assert!(roster.row(7).unwrap().in_match, "watching a running match");
    assert_eq!(roster.by_latency(key(103)).unwrap().member, 3);
    assert!(
        roster.by_latency(key(104)).is_none(),
        "frozen rows are not members"
    );
}

#[test]
fn roster_of_more_than_sixteen_active_rows_is_invalid() {
    let mut seats: Vec<Seat> = (1..=16)
        .map(|id| Seat::new(id, 100 + id, key(id as u8), Some(key(100 + id as u8))))
        .collect();
    seats.push(Seat {
        frozen: true,
        ..Seat::new(40, 140, key(40), None)
    });
    assert_eq!(
        roster(&seats, &[]).rows().len(),
        16,
        "frozen rows do not count"
    );
    seats.push(Seat::new(17, 117, key(17), Some(key(117))));
    assert!(committed_latency_roster(&checkpoint(&seats, &[])).is_err());
}

#[test]
fn roster_drops_duplicate_and_primary_equal_latency_ids_but_keeps_their_members() {
    let seats = vec![
        Seat::new(1, 11, key(1), Some(key(50))),
        Seat::new(2, 22, key(2), Some(key(50))),
        // Another member's primary endpoint.
        Seat::new(3, 33, key(3), Some(key(1))),
        Seat {
            relay: Some("use1"),
            ..Seat::new(4, 44, key(4), Some(key(104)))
        },
        Seat {
            relay: Some("other"),
            ..Seat::new(5, 55, key(5), Some(key(105)))
        },
        Seat {
            latency: Some(key(106).to_string().to_uppercase()),
            ..Seat::new(6, 66, key(6), None)
        },
        Seat {
            latency: Some("not an endpoint".into()),
            ..Seat::new(7, 77, key(7), None)
        },
    ];
    let roster = roster(&seats, &[]);
    assert_eq!(roster.rows().len(), 7);
    for member in [1, 2, 3, 6, 7] {
        assert_eq!(roster.row(member).unwrap().latency, None, "member {member}");
    }
    let hinted = roster.row(4).unwrap();
    assert_eq!(
        (hinted.latency, hinted.relay.as_deref()),
        (Some(key(104)), Some("use1"))
    );
    let unhinted = roster.row(5).unwrap();
    assert_eq!(
        (unhinted.latency, unhinted.relay.as_deref()),
        (Some(key(105)), None)
    );
}

#[test]
fn malformed_rosters_are_invalid() {
    let good = vec![
        Seat::new(1, 11, key(1), Some(key(101))),
        Seat::new(2, 22, key(2), Some(key(102))),
    ];
    assert!(committed_latency_roster(&checkpoint(&good, &[])).is_ok());
    for text in [
        "",
        "{",
        "[]",
        r#"{"checkpoint": {}}"#,
        r#"{"checkpoint": {"members": []}}"#,
    ] {
        assert!(committed_latency_roster(text).is_err(), "{text}");
    }
    let mut zero = good.clone();
    zero[1].incarnation = 0;
    let mut same_incarnation = good.clone();
    same_incarnation[1].incarnation = 11;
    let mut same_member = good.clone();
    same_member[1].member = 1;
    for seats in [zero, same_incarnation, same_member] {
        assert!(committed_latency_roster(&checkpoint(&seats, &[])).is_err());
    }
}

/// A committed value as the store would hold it.
fn committed(revision: u64, checkpoint: &str) -> Arc<Committed> {
    Arc::new(
        serde_json::from_value(json!({"revision": revision, "checkpoint": checkpoint,
            "request": format!("r{revision}"), "term": 1}))
        .unwrap(),
    )
}

struct Fixture {
    observer: Observer,
    local: Tuple,
    owner: u64,
    count: u64,
}

impl Fixture {
    fn new() -> Self {
        Self {
            observer: Observer::default(),
            local: Tuple {
                primary: key(1),
                latency: key(101),
                incarnation: 11,
            },
            owner: 11,
            count: 0,
        }
    }

    fn inputs(&self, committed: Option<Arc<Committed>>, has_game: bool) -> Inputs {
        Inputs {
            room: Some(ROOM),
            epoch: 1,
            endpoints: Some((self.local.primary, self.local.latency)),
            store: committed.map(|value| (self.owner, value, self.count)),
            has_game,
        }
    }

    /// One observation of a new commit.
    fn next(&mut self, committed: Arc<Committed>, has_game: bool) -> authority::Change {
        self.count += 1;
        let inputs = self.inputs(Some(committed), has_game);
        self.observer.observe(inputs)
    }

    fn again(&mut self, committed: Arc<Committed>, has_game: bool) -> authority::Change {
        let inputs = self.inputs(Some(committed), has_game);
        self.observer.observe(inputs)
    }

    fn reason(&self) -> Option<UnavailableReason> {
        match self.observer.current().state {
            AuthorityState::Unavailable(reason) => Some(reason),
            AuthorityState::Valid { .. } => None,
        }
    }
}

fn pair(local_incarnation: u64) -> String {
    checkpoint(
        &[
            Seat::new(1, local_incarnation, key(1), Some(key(101))),
            Seat::new(2, 22, key(2), Some(key(102))),
        ],
        &[],
    )
}

#[test]
fn authority_needs_exactly_our_row() {
    let mut fixture = Fixture::new();
    fixture.next(committed(1, &pair(11)), false);
    assert_eq!(fixture.reason(), None);
    let authority = fixture.observer.current();
    let remote = PeerBinding {
        member: 2,
        primary: key(2),
        latency: key(102),
        incarnation: 22,
    };
    assert!(authority.authorizes(&fixture.local, &remote));
    assert_eq!(
        authority.binding_for(key(102)),
        Some((fixture.local, remote))
    );
    assert!(authority.binding_for(key(101)).is_none(), "never ourselves");
    assert!(!authority.authorizes(
        &fixture.local,
        &PeerBinding {
            incarnation: 23,
            ..remote
        }
    ));
    assert!(!authority.authorizes(
        &fixture.local,
        &PeerBinding {
            member: 3,
            ..remote
        }
    ));
    assert!(!authority.authorizes(
        &fixture.local,
        &PeerBinding {
            primary: key(9),
            ..remote
        }
    ));
    let stale = Tuple {
        incarnation: 12,
        ..fixture.local
    };
    assert!(!authority.authorizes(&stale, &remote));

    // Our row without our latency endpoint, or missing, is no row of ours.
    let unrouted = checkpoint(
        &[
            Seat::new(1, 11, key(1), None),
            Seat::new(2, 22, key(2), Some(key(102))),
        ],
        &[],
    );
    fixture.next(committed(2, &unrouted), false);
    assert_eq!(fixture.reason(), Some(UnavailableReason::NoLocalRow));
    let missing = checkpoint(&[Seat::new(2, 22, key(2), Some(key(102)))], &[]);
    fixture.next(committed(3, &missing), false);
    assert_eq!(fixture.reason(), Some(UnavailableReason::NoLocalRow));
    assert!(fixture.observer.current().binding_for(key(102)).is_none());
}

#[test]
fn local_incarnation_change_and_leaving_valid_revoke_and_each_commit_is_parsed_once() {
    let mut fixture = Fixture::new();
    let first = committed(1, &pair(11));
    assert!(!fixture.next(first.clone(), false).revoked);
    let start = fixture.observer.current().revocation;
    // Reading the same commit again changes nothing and parses nothing.
    let parses = fixture.observer.parses;
    for _ in 0..3 {
        assert_eq!(
            fixture.again(first.clone(), false),
            authority::Change::default()
        );
    }
    assert_eq!(fixture.observer.parses, parses);
    assert_eq!(fixture.observer.current().revocation, start);

    // Valid, then invalid, then valid: one revocation, one parse each.
    assert!(fixture.next(committed(2, "{not json"), false).revoked);
    assert_eq!(fixture.reason(), Some(UnavailableReason::InvalidCommit));
    assert!(!fixture.next(committed(3, &pair(11)), false).revoked);
    assert_eq!(fixture.reason(), None);
    assert_eq!(fixture.observer.parses, parses + 2);
    assert_eq!(fixture.observer.current().revocation, start + 1);

    // A new local incarnation is a new binding.
    fixture.local.incarnation = 12;
    fixture.owner = 12;
    fixture.count = 0;
    let change = fixture.next(committed(4, &pair(12)), false);
    assert!(change.revoked);
    assert_eq!(fixture.reason(), None);
    assert_eq!(fixture.observer.current().revocation, start + 2);

    // A count that moved by more than one skipped a commit.
    fixture.count += 1;
    assert!(fixture.next(committed(5, &pair(12)), false).revoked);
    assert_eq!(fixture.observer.current().revocation, start + 3);
}

#[test]
fn a_game_is_in_match_and_revokes_until_it_is_gone() {
    let mut fixture = Fixture::new();
    let value = committed(1, &pair(11));
    fixture.next(value.clone(), false);
    let start = fixture.observer.current().revocation;
    assert!(fixture.again(value.clone(), true).revoked);
    assert_eq!(fixture.reason(), Some(UnavailableReason::InMatch));
    assert_eq!(fixture.observer.current().revocation, start + 1);
    assert!(fixture.observer.current().binding_for(key(102)).is_none());
    assert!(!fixture.again(value.clone(), true).revoked);
    assert!(!fixture.again(value, false).revoked);
    assert_eq!(fixture.reason(), None);
    assert_eq!(fixture.observer.current().revocation, start + 1);
}

#[test]
fn values_are_whole_milliseconds_from_one_to_999() {
    assert_eq!(milliseconds(Duration::from_micros(0)), 1);
    assert_eq!(milliseconds(Duration::from_micros(1_499)), 1);
    assert_eq!(milliseconds(Duration::from_micros(61_500)), 62);
    assert_eq!(milliseconds(Duration::from_micros(61_499)), 61);
    assert_eq!(milliseconds(Duration::from_secs(5)), 999);
}

#[test]
fn opening_frames_round_trip_and_reject_anything_else() {
    let frame = opening::encode_frame(7, 42);
    assert_eq!(&frame[..5], b"LTO1\x01");
    assert_eq!(opening::decode_frame(&frame), Some((7, 42)));
    for byte in [0, 4, 5, 7] {
        let mut bad = frame;
        bad[byte] ^= 1;
        assert_eq!(opening::decode_frame(&bad), None, "byte {byte}");
    }
}

#[tokio::test]
async fn repeated_reads_of_an_unchanged_commit_neither_revoke_nor_parse() {
    let (_lookup, mut members) = room(2).await;
    let actor = &mut members[0].actor;
    let revocation = actor.latency.authority().revocation;
    let parses = actor.latency.observer.parses;
    for _ in 0..5 {
        actor.latency_refresh().await;
        actor.latency_tick().await;
    }
    assert_eq!(actor.latency.authority().revocation, revocation);
    assert_eq!(actor.latency.observer.parses, parses);
    // Clearing the room revokes and forgets its roster, so a table for the
    // next room never names this one's members.
    actor.latency_clear();
    assert_eq!(actor.latency.authority().revocation, revocation + 1);
    assert!(actor.latency.observer.last_roster().is_none());
    for member in members {
        member.stop().await;
    }
}

#[tokio::test]
async fn a_batch_with_an_invalid_then_a_valid_checkpoint_is_a_skip_and_revokes() {
    let (_lookup, mut members) = room(2).await;
    let seats: Vec<Seat> = members.iter().map(Member::seat).collect();
    let valid = checkpoint(&seats, &[]);
    let member = &mut members[0];
    let (_, before) = member.recovery.coordinator.committed_observed().await;
    let revocation = member.actor.latency.authority().revocation;
    let accepted = member
        .recovery
        .coordinator
        .store()
        .apply_checkpoints(&["{\"checkpoint\": 1}".into(), valid])
        .await;
    assert_eq!(accepted, 2);
    let (_, after) = member.recovery.coordinator.committed_observed().await;
    assert_eq!(after, before + 2);
    member.actor.latency_refresh().await;
    let authority = member.actor.latency.authority();
    assert_eq!(
        authority.revocation,
        revocation + 1,
        "the invalid commit was never seen"
    );
    assert!(authority.local().is_some(), "and the valid one is in force");
    for member in members {
        member.stop().await;
    }
}

#[tokio::test]
async fn two_snapshots_at_one_revision_skip_close_slots_and_reject_a_queued_opening() {
    let (_lookup, mut members) = room(3).await;
    let seats: Vec<Seat> = members.iter().map(Member::seat).collect();
    let valid = checkpoint(&seats, &[]);
    // Member 1 holds an outbound slot to member 2.
    members[0].actor.latency_watch(1, ROOM, 1, true);
    settle(&mut members[..2], Duration::from_secs(10), |members| {
        members[0].actor.latency.outbound.contains_key(&2)
    })
    .await;
    let slot = members[0].actor.latency.outbound[&2]
        .guard
        .connection
        .clone();
    // Member 3 opens to member 1; member 1's opening completes but is not
    // reaped.
    members[2].actor.latency_watch(1, ROOM, 1, true);
    let (first, rest) = members.split_at_mut(1);
    let third = &mut rest[1];
    accept_one(&mut first[0].actor).await;
    timeout(Duration::from_secs(5), async {
        while third.actor.latency.outbound.is_empty() {
            step(&mut third.actor, Duration::from_millis(10)).await;
        }
    })
    .await
    .expect("member 3's dial completes");
    sleep(Duration::from_millis(100)).await;
    // Two snapshots at the committed revision: another checkpoint, then the
    // original one again.
    let store = first[0].recovery.coordinator.store();
    let (committed, _) = first[0].recovery.coordinator.committed_observed().await;
    let revision = committed.revision();
    store
        .install_checkpoint_snapshot("{\"checkpoint\": 2}")
        .await;
    store.install_checkpoint_snapshot(&valid).await;
    let (committed, _) = first[0].recovery.coordinator.committed_observed().await;
    assert_eq!(committed.revision(), revision);
    let revocation = first[0].actor.latency.authority().revocation;
    let result = first[0]
        .actor
        .latency
        .openings_in
        .join_next_with_id()
        .await
        .unwrap();
    assert!(
        matches!(&result, Ok((_, Some(_)))),
        "the opening had completed"
    );
    first[0].actor.latency_inbound_done(result).await;
    assert_eq!(
        first[0].actor.latency.authority().revocation,
        revocation + 1
    );
    assert!(
        first[0].actor.latency.inbound.is_empty(),
        "the queued opening is rejected"
    );
    assert!(first[0].actor.latency.outbound.is_empty());
    assert!(
        slot.close_reason().is_some(),
        "the existing slot's connection is closed"
    );
    assert!(first[0].actor.latency.authority().local().is_some());
    for member in members {
        member.stop().await;
    }
}

#[tokio::test]
async fn the_endpoint_guard_closes_an_obsolete_or_aborted_tasks_endpoint() {
    let lookup = MemoryLookup::new();
    let net = local_net(&lookup);
    // Dropped: closed.
    let endpoint = bind_latency(&net, false.into()).await.unwrap();
    drop(LatencyEndpoint::new(endpoint.clone()));
    timeout(Duration::from_secs(5), async {
        while !endpoint.is_closed() {
            sleep(Duration::from_millis(10)).await;
        }
    })
    .await
    .expect("closed on drop");

    // A task aborted while it holds the endpoint closes it.
    let (sender, receiver) = tokio::sync::oneshot::channel();
    let task = tokio::spawn(async move {
        let guard = bind_for_room(net, false.into()).await.expect("bound");
        let _ = sender.send(guard.endpoint().clone());
        std::future::pending::<()>().await;
        drop(guard);
    });
    let endpoint = receiver.await.unwrap();
    task.abort();
    timeout(Duration::from_secs(5), async {
        while !endpoint.is_closed() {
            sleep(Duration::from_millis(10)).await;
        }
    })
    .await
    .expect("closed on abort");

    // A completion for another epoch, or a failed host, drops its endpoint.
    let (events, _events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(primary_endpoint().await, events);
    actor.epoch = 2;
    let stale = bind_latency(&local_net(&lookup), false.into())
        .await
        .unwrap();
    actor.latency_adopt(1, Some(LatencyEndpoint::new(stale.clone())));
    assert!(actor.latency.endpoint.is_none());
    actor.opening = true;
    let failed_host = bind_latency(&local_net(&lookup), false.into())
        .await
        .unwrap();
    actor
        .completed_hosted(
            2,
            Err(crate::service::failed("relay_unavailable")),
            Some(LatencyEndpoint::new(failed_host.clone())),
        )
        .await
        .unwrap();
    assert!(actor.latency.endpoint.is_none());
    timeout(Duration::from_secs(5), async {
        while !stale.is_closed() || !failed_host.is_closed() {
            sleep(Duration::from_millis(10)).await;
        }
    })
    .await
    .expect("both closed");
    actor.endpoint.close().await;
}

#[tokio::test]
async fn the_subscription_has_a_lease_a_single_view_and_no_events_after_it_stops() {
    let (_lookup, mut members) = room(2).await;
    members[0].actor.latency_watch(1, ROOM, 5, true);
    settle(&mut members, Duration::from_secs(10), |members| {
        members[0].actor.latency.outbound.contains_key(&2)
    })
    .await;
    members[0].actor.latency_second().await;
    let events = members[0].latency_events();
    let (view, rows) = events.last().expect("a table").clone();
    assert_eq!(view, 5);
    assert_eq!(rows.len(), 1);
    assert!(
        matches!(rows[0].state, LatencyState::Value { .. }),
        "{rows:?}"
    );

    // Stale epoch, room or view: ignored.
    members[0].actor.latency_watch(2, ROOM, 6, true);
    members[0].actor.latency_watch(1, [1; 16], 6, true);
    members[0].actor.latency_watch(1, ROOM, 6, false);
    assert_eq!(members[0].actor.latency.view.as_ref().unwrap().view, 5);

    // Renewal keeps the outbound slot; a new view replaces it.
    members[0].actor.latency_watch(1, ROOM, 5, true);
    assert!(members[0].actor.latency.outbound.contains_key(&2));
    let old = members[0].actor.latency.outbound[&2]
        .guard
        .connection
        .clone();
    members[0].actor.latency_watch(1, ROOM, 6, true);
    assert!(members[0].actor.latency.outbound.is_empty());
    assert!(old.close_reason().is_some());
    settle(&mut members, Duration::from_secs(10), |members| {
        members[0].actor.latency.outbound.contains_key(&2)
    })
    .await;

    // The lease lapses.
    members[0].actor.latency.view.as_mut().unwrap().lease = Instant::now();
    members[0].latency_events();
    members[0].actor.latency_second().await;
    assert!(members[0].actor.latency.view.is_none());
    assert!(members[0].actor.latency.outbound.is_empty());
    assert!(
        members[0].latency_events().is_empty(),
        "no table after the view stopped"
    );

    // on: false for the active view stops it the same way.
    members[0].actor.latency_watch(1, ROOM, 7, true);
    members[0].actor.latency_watch(1, ROOM, 7, false);
    assert!(members[0].actor.latency.view.is_none());
    members[0].actor.latency_second().await;
    assert!(members[0].latency_events().is_empty());
    for member in members {
        member.stop().await;
    }
}
