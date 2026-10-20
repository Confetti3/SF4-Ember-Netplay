//! Opening admission, match suspension, the dialer, stale completions,
//! aborted openings and reservations (MEMBER_LATENCY.md 5, Rust unit 4, 5,
//! 7, 12, 13 and 14).
use iroh::endpoint::{
    ConnectOptions, Connection, ConnectionError, QuicTransportConfig, RecvStream, SendStream,
    VarInt,
};

use super::*;
use crate::service::latency::{
    endpoint::{LATENCY_ALPN, latency_transport},
    opening::{
        ACCEPT_DEADLINE, AdmissionTable, DIAL_DEADLINE, FRAME_LEN, MAX_PENDING, REFUSED,
        REPLACE_WINDOW, decode_frame, encode_frame, lock,
    },
};

/// `endpoint`'s loopback address.
pub(super) fn loopback(endpoint: &Endpoint) -> EndpointAddr {
    let port = endpoint.bound_sockets()[0].port();
    EndpointAddr::new(endpoint.id()).with_ip_addr((Ipv4Addr::LOCALHOST, port).into())
}

/// A latency connection from `from` to `to`, with the dialling side's
/// limits unless `config` says otherwise.
pub(super) async fn connect(
    from: Endpoint,
    to: EndpointAddr,
    config: QuicTransportConfig,
) -> Result<Connection, ()> {
    let options = ConnectOptions::new().with_transport_config(config);
    let connecting = from
        .connect_with_opts(to, LATENCY_ALPN, options)
        .await
        .map_err(|_| ())?;
    connecting.await.map_err(|_| ())
}

/// Dials `acceptor`'s latency endpoint from `from` while `acceptor` takes the
/// incoming connection into an opening task.
pub(super) async fn dial_into(
    acceptor: &mut Actor,
    from: &Endpoint,
    config: QuicTransportConfig,
) -> Connection {
    let to = loopback(acceptor.latency.endpoint.as_ref().unwrap().endpoint());
    let dial = tokio::spawn(connect(from.clone(), to, config));
    accept_one(acceptor).await;
    dial.await.unwrap().expect("connected")
}

/// The dialer's half of an opening on `connection`.
pub(super) async fn open(
    connection: &Connection,
    source: u64,
    target: u64,
) -> Option<(SendStream, RecvStream, (u64, u64))> {
    let (mut send, mut recv) = connection.open_bi().await.ok()?;
    send.write_all(&encode_frame(source, target)).await.ok()?;
    let mut frame = [0u8; FRAME_LEN];
    timeout(Duration::from_secs(5), recv.read_exact(&mut frame))
        .await
        .ok()?
        .ok()?;
    Some((send, recv, decode_frame(&frame)?))
}

/// Whether `connection` was closed as not authorized.
pub(super) async fn refused(connection: &Connection) -> bool {
    closed(connection).await
        && matches!(connection.close_reason(), Some(ConnectionError::ApplicationClosed(close))
            if close.reason.as_ref() == REFUSED)
}

/// Reaps one inbound opening of `actor` and installs it as the actor would.
async fn reap_inbound(actor: &mut Actor) -> bool {
    let result = timeout(
        ACCEPT_DEADLINE + Duration::from_secs(2),
        actor.latency.openings_in.join_next_with_id(),
    )
    .await
    .expect("an inbound opening ends")
    .expect("one is running");
    let opened = matches!(&result, Ok((_, Some(_))));
    actor.latency_inbound_done(result).await;
    opened
}

async fn reap_outbound(actor: &mut Actor) -> bool {
    let result = timeout(
        DIAL_DEADLINE + Duration::from_secs(2),
        actor.latency.openings_out.join_next_with_id(),
    )
    .await
    .expect("an outbound opening ends")
    .expect("one is running");
    let opened = matches!(&result, Ok((_, Ok(_))));
    actor.latency_outbound_done(result).await;
    opened
}

fn permits(actor: &Actor) -> usize {
    actor.latency.permits.available_permits()
}

fn pending(actor: &Actor) -> usize {
    lock(&actor.latency.admissions).pending.len()
}

fn charged(actor: &Actor, identity: (EndpointId, u64)) -> usize {
    lock(&actor.latency.admissions)
        .charged
        .get(&identity)
        .map_or(0, Vec::len)
}

/// Waits until `actor`'s admission table marks an opening pending.
async fn until_pending(actor: &Actor) {
    timeout(Duration::from_secs(5), async {
        while pending(actor) == 0 {
            sleep(Duration::from_millis(5)).await;
        }
    })
    .await
    .expect("past the identity check");
}

fn dialer() -> QuicTransportConfig {
    latency_transport(0)
}

#[tokio::test]
async fn a_withheld_stream_or_a_partial_frame_closes_at_the_deadline_and_frees_the_permit() {
    let (_lookup, mut members) = room(2).await;
    let (a, b) = members.split_at_mut(1);
    let (a, b) = (&mut a[0], &b[0]);
    // No stream at all.
    let started = Instant::now();
    let withheld = dial_into(&mut a.actor, b.latency_endpoint(), dialer()).await;
    assert_eq!(permits(&a.actor), MAX_PENDING - 1);
    assert!(!reap_inbound(&mut a.actor).await);
    let elapsed = started.elapsed();
    assert!(
        elapsed >= ACCEPT_DEADLINE - Duration::from_millis(100),
        "{elapsed:?}"
    );
    assert!(
        elapsed < ACCEPT_DEADLINE + Duration::from_secs(1),
        "{elapsed:?}"
    );
    assert!(closed(&withheld).await);
    assert_eq!(permits(&a.actor), MAX_PENDING);
    assert!(a.actor.latency.handles.is_empty());

    // Part of a frame.
    let partial = dial_into(&mut a.actor, b.latency_endpoint(), dialer()).await;
    let (mut send, _recv) = partial.open_bi().await.unwrap();
    send.write_all(&encode_frame(b.incarnation(), a.incarnation())[..10])
        .await
        .unwrap();
    assert!(!reap_inbound(&mut a.actor).await);
    assert!(closed(&partial).await);
    assert_eq!(permits(&a.actor), MAX_PENDING);
    assert_eq!(pending(&a.actor), 0);
    assert!(a.actor.latency.inbound.is_empty());
    for member in members {
        member.stop().await;
    }
}

#[tokio::test]
async fn a_peer_that_gives_no_write_credit_is_closed_at_the_deadline() {
    let (_lookup, mut members) = room(2).await;
    let (a, b) = members.split_at_mut(1);
    let (a, b) = (&mut a[0], &b[0]);
    let stingy = QuicTransportConfig::builder()
        .max_concurrent_bidi_streams(VarInt::from_u32(0))
        .stream_receive_window(VarInt::from_u32(0))
        .build();
    let connection = dial_into(&mut a.actor, b.latency_endpoint(), stingy).await;
    let (mut send, _recv) = connection.open_bi().await.unwrap();
    send.write_all(&encode_frame(b.incarnation(), a.incarnation()))
        .await
        .unwrap();
    let started = Instant::now();
    assert!(
        !reap_inbound(&mut a.actor).await,
        "our frame could not be written"
    );
    assert!(started.elapsed() >= ACCEPT_DEADLINE - Duration::from_millis(500));
    assert!(closed(&connection).await);
    assert_eq!(permits(&a.actor), MAX_PENDING);
    assert!(a.actor.latency.inbound.is_empty());
    for member in members {
        member.stop().await;
    }
}

#[tokio::test]
async fn one_identity_has_one_pending_opening_and_three_per_minute() {
    let (_lookup, mut members) = room(2).await;
    let (a, b) = members.split_at_mut(1);
    let (a, b) = (&mut a[0], &b[0]);
    let identity = (b.latency(), b.incarnation());
    // A second opening while the first is pending is refused.
    let first = dial_into(&mut a.actor, b.latency_endpoint(), dialer()).await;
    until_pending(&a.actor).await;
    let second = dial_into(&mut a.actor, b.latency_endpoint(), dialer()).await;
    assert!(refused(&second).await);
    assert!(!reap_inbound(&mut a.actor).await);
    first.close(0u32.into(), b"done");
    assert!(!reap_inbound(&mut a.actor).await);
    assert_eq!(pending(&a.actor), 0);
    assert_eq!(
        charged(&a.actor, identity),
        1,
        "the refused one was not charged"
    );

    // Two complete openings: each replaces the slot. The fourth is refused.
    let mut held = Vec::new();
    for _ in 0..2 {
        let connection = dial_into(&mut a.actor, b.latency_endpoint(), dialer()).await;
        let opened = open(&connection, b.incarnation(), a.incarnation()).await;
        assert_eq!(
            opened.as_ref().map(|o| o.2),
            Some((a.incarnation(), b.incarnation()))
        );
        assert!(reap_inbound(&mut a.actor).await);
        assert_eq!(a.actor.latency.inbound.len(), 1);
        held.push((connection, opened));
    }
    assert!(
        closed(&held[0].0).await,
        "the replaced slot's connection is closed"
    );
    assert!(held[1].0.close_reason().is_none());
    assert_eq!(charged(&a.actor, identity), 3);
    let fourth = dial_into(&mut a.actor, b.latency_endpoint(), dialer()).await;
    assert!(refused(&fourth).await);
    assert!(!reap_inbound(&mut a.actor).await);
    assert!(a.actor.latency.handles.is_empty());
    for member in members {
        member.stop().await;
    }
}

#[tokio::test]
async fn a_fifth_concurrent_incoming_is_refused_before_tls() {
    let (lookup, mut members) = room(5).await;
    let (a, rest) = members.split_at_mut(1);
    let a = &mut a[0];
    let mut withheld = Vec::new();
    for other in rest.iter() {
        withheld.push(dial_into(&mut a.actor, other.latency_endpoint(), dialer()).await);
    }
    assert_eq!(a.actor.latency.openings_in.len(), MAX_PENDING);
    assert_eq!(permits(&a.actor), 0);
    let stranger = bind_latency(&local_net(&lookup), crate::transport::Transport::Online)
        .await
        .unwrap();
    let to = loopback(a.latency_endpoint());
    let dial = tokio::spawn(connect(stranger.clone(), to, dialer()));
    accept_one(&mut a.actor).await;
    assert_eq!(
        a.actor.latency.openings_in.len(),
        MAX_PENDING,
        "no task for it"
    );
    assert!(
        timeout(Duration::from_secs(5), dial)
            .await
            .unwrap()
            .unwrap()
            .is_err()
    );
    drop(withheld);
    stranger.close().await;
    for member in members {
        member.stop().await;
    }
}

#[tokio::test]
async fn a_stranger_is_closed_right_after_tls_and_the_permit_waits_for_the_slot() {
    let (lookup, mut members) = room(2).await;
    let (a, b) = members.split_at_mut(1);
    let (a, b) = (&mut a[0], &b[0]);
    let stranger = bind_latency(&local_net(&lookup), crate::transport::Transport::Online)
        .await
        .unwrap();
    let connection = dial_into(&mut a.actor, &stranger, dialer()).await;
    assert!(
        refused(&connection).await,
        "TLS completed, then the identity check refused it"
    );
    assert!(!reap_inbound(&mut a.actor).await);
    assert_eq!(permits(&a.actor), MAX_PENDING);

    let connection = dial_into(&mut a.actor, b.latency_endpoint(), dialer()).await;
    let opened = open(&connection, b.incarnation(), a.incarnation()).await;
    assert!(opened.is_some());
    sleep(Duration::from_millis(100)).await;
    // Complete but not installed: the permit and the mark are still held.
    assert_eq!(permits(&a.actor), MAX_PENDING - 1);
    assert_eq!(pending(&a.actor), 1);
    assert!(a.actor.latency.inbound.is_empty());
    assert!(reap_inbound(&mut a.actor).await);
    assert_eq!(a.actor.latency.inbound.len(), 1);
    assert_eq!(permits(&a.actor), MAX_PENDING);
    assert_eq!(pending(&a.actor), 0);
    stranger.close().await;
    for member in members {
        member.stop().await;
    }
}

#[tokio::test]
async fn preparing_a_game_refuses_fresh_openings_and_rejects_completed_ones() {
    let (_lookup, mut members) = room(2).await;
    let peer = members[1].primary();
    // Completed before the preparation, installed after it: rejected.
    {
        let (a, b) = members.split_at_mut(1);
        let (a, b) = (&mut a[0], &b[0]);
        let connection = dial_into(&mut a.actor, b.latency_endpoint(), dialer()).await;
        assert!(
            open(&connection, b.incarnation(), a.incarnation())
                .await
                .is_some()
        );
        sleep(Duration::from_millis(100)).await;
        prepare_game(&mut a.actor, peer).await;
        assert!(reap_inbound(&mut a.actor).await, "it had completed");
        assert!(a.actor.latency.inbound.is_empty());
        assert!(closed(&connection).await);
        assert_eq!(permits(&a.actor), MAX_PENDING);
        assert_eq!(pending(&a.actor), 0);

        // A fresh one during the match is refused at the identity check.
        let connection = dial_into(&mut a.actor, b.latency_endpoint(), dialer()).await;
        assert!(refused(&connection).await);
        assert!(!reap_inbound(&mut a.actor).await);
    }
    // An outbound opening that completes during the match is not installed.
    members[0].actor.games.clear();
    members[0].actor.latency_games_changed();
    members[0].actor.latency_watch(1, ROOM, 1, true);
    assert_eq!(members[0].actor.latency.openings_out.len(), 1);
    let b = &mut members[1];
    timeout(Duration::from_secs(5), async {
        while b.actor.latency.inbound.is_empty() {
            step(&mut b.actor, Duration::from_millis(10)).await;
        }
    })
    .await
    .expect("member 2 installed its side");
    let remote = b
        .actor
        .latency
        .inbound
        .values()
        .next()
        .unwrap()
        .guard
        .connection
        .clone();
    sleep(Duration::from_millis(100)).await;
    prepare_game(&mut members[0].actor, peer).await;
    assert!(reap_outbound(&mut members[0].actor).await);
    assert!(members[0].actor.latency.outbound.is_empty());
    assert!(closed(&remote).await);
    for member in members {
        member.stop().await;
    }
}

#[tokio::test]
async fn one_deadline_covers_the_whole_dial_and_then_it_backs_off() {
    let (_lookup, mut members) = room(2).await;
    let (a, b) = members.split_at_mut(1);
    let (a, b) = (&mut a[0], &b[0]);
    // Member 2 takes the connection and the frame and never answers.
    let silent = b.latency_endpoint().clone();
    let holder = tokio::spawn(async move {
        let connection = silent.accept().await.unwrap().await.unwrap();
        let (_send, mut recv) = connection.accept_bi().await.unwrap();
        let mut frame = [0u8; FRAME_LEN];
        recv.read_exact(&mut frame).await.unwrap();
        connection.closed().await;
    });
    let started = Instant::now();
    a.actor.latency_watch(1, ROOM, 1, true);
    assert!(!reap_outbound(&mut a.actor).await);
    let elapsed = started.elapsed();
    assert!(
        elapsed >= DIAL_DEADLINE - Duration::from_millis(100),
        "{elapsed:?}"
    );
    assert!(
        elapsed < DIAL_DEADLINE + Duration::from_secs(1),
        "{elapsed:?}"
    );
    let dial = &a.actor.latency.dials[&2];
    assert_eq!(dial.failure, Some(DialFailure::Unreachable));
    assert_eq!(dial.backoff, REDIAL_FIRST * 2);
    assert!(dial.next > Instant::now() + REDIAL_FIRST - Duration::from_secs(1));
    // Backing off: no new dial yet.
    a.actor.latency_dial();
    assert!(a.actor.latency.openings_out.is_empty());
    a.actor.latency_report();
    let rows = a.latency_events().pop().unwrap().1;
    assert_eq!(
        rows[0].state,
        LatencyState::Unavailable {
            reason: LatencyUnavailable::Unreachable
        }
    );
    assert!(a.actor.latency.handles.is_empty());
    holder.abort();
    for member in members {
        member.stop().await;
    }
}

/// Starts member 1's view and lets member 2 install its side of the
/// opening; member 1's completion is queued, not reaped.
async fn queue_outbound(members: &mut [Member]) -> Connection {
    members[0].actor.latency_watch(1, ROOM, 1, true);
    let b = &mut members[1];
    timeout(Duration::from_secs(5), async {
        while b.actor.latency.inbound.is_empty() {
            step(&mut b.actor, Duration::from_millis(10)).await;
        }
    })
    .await
    .expect("member 2 installed its side");
    let remote = b
        .actor
        .latency
        .inbound
        .values()
        .next()
        .unwrap()
        .guard
        .connection
        .clone();
    sleep(Duration::from_millis(100)).await;
    remote
}

#[tokio::test]
async fn a_stale_attempt_or_revocation_is_not_installed() {
    let (_lookup, mut members) = room(2).await;
    let remote = queue_outbound(&mut members).await;
    members[0].actor.latency.dials.get_mut(&2).unwrap().attempt += 1000;
    assert!(reap_outbound(&mut members[0].actor).await);
    assert!(members[0].actor.latency.outbound.is_empty());
    assert!(closed(&remote).await);

    members[0].actor.latency.stop_view();
    members[1].actor.latency.inbound.clear();
    let remote = queue_outbound(&mut members).await;
    let peer = members[1].primary();
    let revocation = members[0].actor.latency.authority().revocation;
    prepare_game(&mut members[0].actor, peer).await;
    members[0].actor.games.clear();
    members[0].actor.latency_games_changed();
    assert_eq!(
        members[0].actor.latency.authority().revocation,
        revocation + 1
    );
    assert!(members[0].actor.latency.authority().local().is_some());
    assert!(reap_outbound(&mut members[0].actor).await);
    assert!(members[0].actor.latency.outbound.is_empty());
    assert!(closed(&remote).await);
    for member in members {
        member.stop().await;
    }
}

/// Commits `seats` to member 1's store and lets its actor read it.
async fn observe(member: &mut Member, seats: &[Seat]) {
    member.commit(&checkpoint(seats, &[])).await;
    member.actor.latency_refresh().await;
}

#[tokio::test]
async fn a_completion_queued_before_a_departure_and_rejoin_is_never_installed() {
    let (_lookup, mut members) = room(3).await;
    let seats: Vec<Seat> = members.iter().map(Member::seat).collect();
    let rejoined = {
        let mut seats = seats.clone();
        seats[1].incarnation += 1000;
        seats
    };
    let departed = vec![seats[0].clone(), seats[2].clone()];

    // Inbound.
    let connection = {
        let (a, b) = members.split_at_mut(1);
        let (a, b) = (&mut a[0], &b[0]);
        let connection = dial_into(&mut a.actor, b.latency_endpoint(), dialer()).await;
        assert!(
            open(&connection, b.incarnation(), a.incarnation())
                .await
                .is_some()
        );
        connection
    };
    sleep(Duration::from_millis(100)).await;
    let revocation = members[0].actor.latency.authority().revocation;
    observe(&mut members[0], &departed).await;
    observe(&mut members[0], &rejoined).await;
    assert_eq!(
        members[0].actor.latency.authority().revocation,
        revocation,
        "one step each"
    );
    assert!(reap_inbound(&mut members[0].actor).await);
    assert!(members[0].actor.latency.inbound.is_empty());
    assert!(closed(&connection).await);
    assert_eq!(pending(&members[0].actor), 0);
    assert_eq!(permits(&members[0].actor), MAX_PENDING);

    // Outbound, back on the original roster.
    observe(&mut members[0], &seats).await;
    let remote = queue_outbound(&mut members).await;
    observe(&mut members[0], &departed).await;
    observe(&mut members[0], &rejoined).await;
    assert_eq!(members[0].actor.latency.authority().revocation, revocation);
    assert!(reap_outbound(&mut members[0].actor).await);
    assert!(members[0].actor.latency.outbound.is_empty());
    assert!(closed(&remote).await);
    members[0].latency_events();
    members[0].actor.latency_report();
    let rows = members[0].latency_events().pop().unwrap().1;
    assert!(
        rows.iter()
            .all(|row| !matches!(row.state, LatencyState::Value { .. })),
        "{rows:?}"
    );
    for member in members {
        member.stop().await;
    }
}

#[tokio::test]
async fn a_roster_change_aborts_the_openings_it_no_longer_authorizes() {
    let (_lookup, mut members) = room(3).await;
    let seats: Vec<Seat> = members.iter().map(Member::seat).collect();
    // Member 3 takes member 1's dial and frame and never answers.
    let silent = members[2].latency_endpoint().clone();
    let holder = tokio::spawn(async move {
        let connection = silent.accept().await.unwrap().await.unwrap();
        let (_send, mut recv) = connection.accept_bi().await.unwrap();
        let mut frame = [0u8; FRAME_LEN];
        recv.read_exact(&mut frame).await.unwrap();
        connection.closed().await;
    });
    // Member 2 opens to member 1 and stops part way through its frame.
    let inbound = {
        let (a, b) = members.split_at_mut(1);
        let (a, b) = (&mut a[0], &b[0]);
        let connection = dial_into(&mut a.actor, b.latency_endpoint(), dialer()).await;
        let (mut send, recv) = connection.open_bi().await.unwrap();
        send.write_all(&[0; 10]).await.unwrap();
        until_pending(&a.actor).await;
        (connection, send, recv)
    };
    members[0].actor.latency_watch(1, ROOM, 1, true);
    assert_eq!(members[0].actor.latency.openings_out.len(), 2);
    assert_eq!(members[0].actor.latency.handles.len(), 3);
    // Member 2 leaves and member 3 rejoins.
    let mut changed = vec![seats[0].clone(), seats[2].clone()];
    changed[1].incarnation += 1000;
    let revocation = members[0].actor.latency.authority().revocation;
    observe(&mut members[0], &changed).await;
    assert_eq!(members[0].actor.latency.authority().revocation, revocation);
    let started = Instant::now();
    while !members[0].actor.latency.openings_in.is_empty() {
        assert!(!reap_inbound(&mut members[0].actor).await);
    }
    while !members[0].actor.latency.openings_out.is_empty() {
        assert!(!reap_outbound(&mut members[0].actor).await);
    }
    assert!(
        started.elapsed() < Duration::from_secs(2),
        "aborted, not timed out"
    );
    assert!(members[0].actor.latency.handles.is_empty());
    assert_eq!(pending(&members[0].actor), 0);
    assert_eq!(permits(&members[0].actor), MAX_PENDING);
    assert!(closed(&inbound.0).await);
    holder.abort();
    for member in members {
        member.stop().await;
    }
}

#[tokio::test]
async fn every_reaped_opening_leaves_no_handle() {
    let (_lookup, mut members) = room(3).await;
    // Installed.
    let remote = queue_outbound(&mut members[..2]).await;
    assert!(reap_outbound(&mut members[0].actor).await);
    assert!(members[0].actor.latency.outbound.contains_key(&2));
    assert!(remote.close_reason().is_none());
    // Refused: member 3 is not in its own roster, so it refuses everyone.
    let seats: Vec<Seat> = members.iter().map(Member::seat).collect();
    members[2]
        .commit(&checkpoint(&[seats[0].clone(), seats[1].clone()], &[]))
        .await;
    members[2].actor.latency_refresh().await;
    assert!(members[2].actor.latency.authority().local().is_none());
    let (first, rest) = members.split_at_mut(1);
    let a = &mut first[0];
    let c = &mut rest[1];
    let reap = async {
        while a
            .actor
            .latency
            .dials
            .get(&3)
            .is_none_or(|dial| dial.failure.is_none())
        {
            assert!(!reap_outbound(&mut a.actor).await);
        }
    };
    let serve = async {
        loop {
            step(&mut c.actor, Duration::from_millis(10)).await;
        }
    };
    tokio::select! {
        () = reap => (),
        () = serve => unreachable!(),
    }
    assert_eq!(
        a.actor.latency.dials[&3].failure,
        Some(DialFailure::Refused)
    );
    // A deadline.
    let b = &rest[0];
    let _withheld = dial_into(&mut a.actor, b.latency_endpoint(), dialer()).await;
    assert!(!reap_inbound(&mut a.actor).await);
    assert!(a.actor.latency.handles.is_empty());
    for member in members {
        member.stop().await;
    }
}

#[tokio::test]
async fn reservations_are_cancellation_safe_and_the_budget_reopens() {
    let (_lookup, mut members) = room(2).await;
    let peer = members[1].primary();
    let (a, b) = members.split_at_mut(1);
    let (a, b) = (&mut a[0], &b[0]);
    let identity = (b.latency(), b.incarnation());
    // Aborted while waiting for the stream.
    let waiting = dial_into(&mut a.actor, b.latency_endpoint(), dialer()).await;
    until_pending(&a.actor).await;
    a.actor.latency.openings_in.abort_all();
    assert!(!reap_inbound(&mut a.actor).await);
    assert_eq!(pending(&a.actor), 0);
    assert!(closed(&waiting).await);
    assert_eq!(charged(&a.actor, identity), 1, "its budget stays charged");
    assert_eq!(permits(&a.actor), MAX_PENDING);

    // Completed, then rejected for a revocation.
    let connection = dial_into(&mut a.actor, b.latency_endpoint(), dialer()).await;
    assert!(
        open(&connection, b.incarnation(), a.incarnation())
            .await
            .is_some()
    );
    sleep(Duration::from_millis(100)).await;
    prepare_game(&mut a.actor, peer).await;
    a.actor.games.clear();
    a.actor.latency_games_changed();
    assert!(reap_inbound(&mut a.actor).await);
    assert!(a.actor.latency.inbound.is_empty());
    assert_eq!(pending(&a.actor), 0);
    assert_eq!(permits(&a.actor), MAX_PENDING);
    assert!(closed(&connection).await);
    assert_eq!(charged(&a.actor, identity), 2);

    // Valid again with budget left: it opens. Then the budget is spent.
    let connection = dial_into(&mut a.actor, b.latency_endpoint(), dialer()).await;
    let _held = open(&connection, b.incarnation(), a.incarnation())
        .await
        .unwrap();
    assert!(reap_inbound(&mut a.actor).await);
    assert_eq!(a.actor.latency.inbound.len(), 1);
    let refused_one = dial_into(&mut a.actor, b.latency_endpoint(), dialer()).await;
    assert!(refused(&refused_one).await);
    assert!(!reap_inbound(&mut a.actor).await);
    // Once the oldest charge is older than the window, one more is allowed.
    lock(&a.actor.latency.admissions)
        .charged
        .get_mut(&identity)
        .unwrap()[0] = Instant::now().checked_sub(REPLACE_WINDOW).unwrap();
    let connection = dial_into(&mut a.actor, b.latency_endpoint(), dialer()).await;
    let _held = open(&connection, b.incarnation(), a.incarnation())
        .await
        .unwrap();
    assert!(reap_inbound(&mut a.actor).await);
    assert_eq!(charged(&a.actor, identity), 3);
    for member in members {
        member.stop().await;
    }
}

#[tokio::test]
async fn an_older_reservation_never_clears_a_newer_mark() {
    let table = Arc::new(std::sync::Mutex::new(AdmissionTable::default()));
    let binding = PeerBinding {
        member: 2,
        primary: key(2),
        latency: key(102),
        incarnation: 22,
    };
    let identity = (binding.latency, binding.incarnation);
    let task = tokio::spawn(async { tokio::task::id() }).await.unwrap();
    let now = Instant::now();
    let older = AdmissionTable::try_begin(&table, identity, binding, task, now).unwrap();
    assert!(AdmissionTable::try_begin(&table, identity, binding, task, now).is_none());
    // The roster dropped and readmitted the identity in between.
    lock(&table).pending.remove(&identity);
    let newer = AdmissionTable::try_begin(&table, identity, binding, task, now).unwrap();
    drop(older);
    assert!(lock(&table).pending.contains_key(&identity));
    drop(newer);
    assert!(lock(&table).pending.is_empty());
    assert_eq!(lock(&table).charged[&identity].len(), 2);
    // A spent budget reopens after the window.
    assert!(AdmissionTable::try_begin(&table, identity, binding, task, now).is_some());
    assert!(AdmissionTable::try_begin(&table, identity, binding, task, now).is_none());
    let later = now + REPLACE_WINDOW;
    assert!(AdmissionTable::try_begin(&table, identity, binding, task, later).is_some());
}
