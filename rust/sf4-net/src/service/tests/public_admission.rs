//! A public host actor admitting a member again: reconnect past the ticket's
//! minute, the decision repeated when a handshake completes, retirement of a
//! seat, and replacements parked behind a draining control.
use ember_protocol::rooms::{RoomTicket, SignedRoomTicket, TICKET_SECS};

use super::public_support::{BRIDGE, BUILD, host_state, identity, member_admission};
use super::*;
use crate::public_room::{self, PublicRoom};

fn ticket_at(
    invite: &Invite,
    account: &ember_protocol::SigningIdentity,
    endpoint: &Endpoint,
    issued_at: u64,
) -> SignedRoomTicket {
    RoomTicket {
        version: ember_protocol::play::VERSION,
        bridge_id: BRIDGE.into(),
        room_id: invite
            .room()
            .iter()
            .map(|byte| format!("{byte:02x}"))
            .collect(),
        ember_id: account.ember_id().clone(),
        endpoint_id: endpoint.id().to_string(),
        issued_at,
        expires_at: issued_at + TICKET_SECS,
    }
    .sign(&identity(9), "k1")
    .unwrap()
}

/// `remote` dials the host actor with `ticket`; what the actor does with the
/// connection is run to its end. The dialer's result.
async fn redial(
    actor: &mut Actor,
    host: &Endpoint,
    invite: &Invite,
    remote: &Endpoint,
    ticket: &SignedRoomTicket,
) -> io::Result<transport::ControlChannel> {
    let (client, ()) = tokio::join!(
        async {
            let connection = remote.connect(address(host), CONTROL_ALPN).await.unwrap();
            public_room::connect_public_on(connection, invite, ticket).await
        },
        async {
            let connection = host.accept().await.unwrap().await.unwrap();
            actor.accept_public_incoming(1, connection);
        }
    );
    if let Ok(Some(completion)) = timeout(Duration::from_secs(5), actor.tasks.join_next()).await {
        actor.completed(completion.unwrap()).await.unwrap();
    }
    client
}

/// A member's control drops and it redials with the ticket it joined with,
/// after the ticket's minute. The host takes it from an endpoint it holds as a
/// member and from nobody else.
#[tokio::test]
async fn a_dropped_member_returns_past_its_ticket_but_a_stranger_does_not() {
    timeout(Duration::from_secs(90), async {
        let host = endpoint().await;
        let (remote, stranger) = (endpoint().await, endpoint().await);
        let invite =
            Invite::create(host.id(), test_relay(), BUILD.into(), now().unwrap(), 3600).unwrap();
        let (events, _rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(host.clone(), events);
        actor.epoch = 1;
        actor.room = Some(invite.room());
        actor.hosted = Some(invite.clone());
        actor.room_invite = Some(invite.clone());
        actor.public = Some(PublicRoom::Host(host_state()));
        let player = identity(1);
        let issued = now().unwrap();

        // Joined with a current ticket.
        let first = ticket_at(&invite, &player, &remote, issued);
        let control = redial(&mut actor, &host, &invite, &remote, &first).await;
        assert!(control.is_ok() && actor.controls.contains_key(&remote.id()));
        drop(control);
        // The control drops, and the member is admitted as a coordination member.
        actor.controls.clear();
        let route = endpoint().await;
        actor.remember_admission(member_admission(invite.room(), 0x55, remote.id(), &route));

        // A minute and more later.
        let stale = ticket_at(&invite, &player, &remote, issued - 10 * TICKET_SECS);
        assert!(
            redial(&mut actor, &host, &invite, &remote, &stale)
                .await
                .is_ok()
        );
        actor.controls.clear();
        // An endpoint the room does not hold is refused the same ticket's kind.
        let other = ticket_at(&invite, &identity(2), &stranger, issued - 10 * TICKET_SECS);
        assert!(
            redial(&mut actor, &host, &invite, &stranger, &other)
                .await
                .is_err()
        );
        // The known endpoint with another account's old ticket is refused too.
        let wrong = ticket_at(&invite, &identity(2), &remote, issued - 10 * TICKET_SECS);
        assert!(
            redial(&mut actor, &host, &invite, &remote, &wrong)
                .await
                .is_err()
        );

        // Once its seat is retired the member is a stranger.
        let seat = actor.admissions.remove(&0x55).unwrap();
        assert!(
            redial(&mut actor, &host, &invite, &remote, &stale)
                .await
                .is_err()
        );
        actor.remember_admission(seat);
        // A ban ends the welcome whatever the seat.
        actor.controls.clear();
        actor.ban_account(7, 1, player.ember_id().as_str()).unwrap();
        assert!(
            redial(&mut actor, &host, &invite, &remote, &stale)
                .await
                .is_err()
        );
        actor.tasks.abort_all();
        route.close().await;
        host.close().await;
    })
    .await
    .unwrap();
}

/// The accept task's side of a handshake: `remote` dials `host` and is
/// admitted under `policy` as the clock stood at `clock`. The host's channel.
async fn handshake_at(
    host: &Endpoint,
    invite: &Invite,
    remote: &Endpoint,
    ticket: &SignedRoomTicket,
    policy: crate::public_room::AdmissionPolicy,
    clock: u64,
) -> transport::ControlChannel {
    let (client, server) = tokio::join!(
        async {
            let connection = remote.connect(address(host), CONTROL_ALPN).await.unwrap();
            public_room::connect_public_on(connection, invite, ticket).await
        },
        async {
            let connection = host.accept().await.unwrap().await.unwrap();
            public_room::accept_public_control_at(
                connection,
                invite,
                policy,
                BTreeMap::new(),
                move || Ok(clock),
            )
            .await
        }
    );
    client.unwrap();
    server.unwrap()
}

/// The accept task decides against a snapshot; the actor decides again on its
/// own state when the handshake completes.
#[tokio::test]
async fn the_actor_decides_again_when_a_handshake_completes() {
    timeout(Duration::from_secs(60), async {
        let host = endpoint().await;
        let (remote, route) = (endpoint().await, endpoint().await);
        let invite =
            Invite::create(host.id(), test_relay(), BUILD.into(), now().unwrap(), 3600).unwrap();
        let (events, _rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(host.clone(), events);
        actor.epoch = 1;
        actor.room = Some(invite.room());
        actor.hosted = Some(invite.clone());
        actor.room_invite = Some(invite.clone());
        actor.public = Some(PublicRoom::Host(host_state()));
        let player = identity(1);
        let issued = now().unwrap() - 10 * TICKET_SECS;
        let stale = ticket_at(&invite, &player, &remote, issued);

        // A member that was known when its handshake began, redialing with the
        // ticket it joined with long ago.
        let mut known = host_state();
        known.seat_member(remote.id(), player.ember_id().clone());
        let channel = handshake_at(&host, &invite, &remote, &stale, known.policy(), issued).await;

        // Its seat was retired while the handshake was unfinished: refused.
        assert!(actor.refuse_public_control(&channel));
        // Still seated, it is taken.
        let room = invite.room();
        if let Some(PublicRoom::Host(state)) = actor.public.as_mut() {
            state.admit_member(remote.id(), player.ember_id().clone());
        }
        actor.remember_admission(member_admission(room, 0x55, remote.id(), &route));
        assert!(!actor.refuse_public_control(&channel));
        // A ban that landed meanwhile ends it.
        actor.ban_account(7, 1, player.ember_id().as_str()).unwrap();
        assert!(actor.refuse_public_control(&channel));
        channel.connection.close(1u32.into(), b"done");

        // A newcomer whose ticket was good when the handshake read it and has
        // expired by the time the actor installs the control.
        let mut fresh = test_actor(host.clone(), mpsc::channel(IPC_QUEUE_CAPACITY).0);
        fresh.epoch = 1;
        fresh.room = Some(invite.room());
        fresh.public = Some(PublicRoom::Host(host_state()));
        let channel = handshake_at(
            &host,
            &invite,
            &remote,
            &stale,
            host_state().policy(),
            issued + TICKET_SECS - 1,
        )
        .await;
        assert!(fresh.refuse_public_control(&channel));
        // The same newcomer with a current ticket is let in.
        let current = ticket_at(&invite, &player, &remote, now().unwrap());
        let channel = handshake_at(
            &host,
            &invite,
            &remote,
            &current,
            host_state().policy(),
            now().unwrap(),
        )
        .await;
        assert!(!fresh.refuse_public_control(&channel));
        channel.connection.close(1u32.into(), b"done");
        route.close().await;
        remote.close().await;
        host.close().await;
    })
    .await
    .unwrap();
}

/// `handshake_at` that keeps the dialer's side alive, so the host's channel
/// stays open. The dialer's channel, then the host's.
async fn handshake_pair_at(
    host: &Endpoint,
    invite: &Invite,
    remote: &Endpoint,
    ticket: &SignedRoomTicket,
    policy: crate::public_room::AdmissionPolicy,
    clock: u64,
) -> (transport::ControlChannel, transport::ControlChannel) {
    let (client, server) = tokio::join!(
        async {
            let connection = remote.connect(address(host), CONTROL_ALPN).await.unwrap();
            public_room::connect_public_on(connection, invite, ticket).await
        },
        async {
            let connection = host.accept().await.unwrap().await.unwrap();
            public_room::accept_public_control_at(
                connection,
                invite,
                policy,
                BTreeMap::new(),
                move || Ok(clock),
            )
            .await
        }
    );
    (client.unwrap(), server.unwrap())
}

/// A public host actor whose creator `player` is its member `remote`, with a
/// control installed.
struct SeatedHost {
    host: Endpoint,
    remote: Endpoint,
    route: Endpoint,
    invite: Invite,
    player: ember_protocol::SigningIdentity,
    actor: Actor,
    events: mpsc::Receiver<Event>,
    /// When the tickets of `stale` were issued.
    stale_at: u64,
    _client: transport::ControlChannel,
}

impl SeatedHost {
    async fn start() -> Self {
        let host = endpoint().await;
        let (remote, route) = (endpoint().await, endpoint().await);
        let invite =
            Invite::create(host.id(), test_relay(), BUILD.into(), now().unwrap(), 3600).unwrap();
        let (events_tx, events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(host.clone(), events_tx);
        actor.epoch = 1;
        actor.room = Some(invite.room());
        actor.hosted = Some(invite.clone());
        actor.room_invite = Some(invite.clone());
        actor.public = Some(PublicRoom::Host(host_state()));
        let player = identity(1);
        let current = ticket_at(&invite, &player, &remote, now().unwrap());
        let (client, server) = handshake_pair_at(
            &host,
            &invite,
            &remote,
            &current,
            host_state().policy(),
            now().unwrap(),
        )
        .await;
        actor.remember_public_member(remote.id(), player.ember_id());
        actor
            .controls
            .insert(remote.id(), crate::control::ControlWorker::start(server));
        Self {
            host,
            remote,
            route,
            invite,
            player,
            actor,
            events,
            stale_at: now().unwrap() - 10 * TICKET_SECS,
            _client: client,
        }
    }

    /// The member's coordination admission, as the host holds it.
    fn admit(&mut self, incarnation: u64) {
        let seat = member_admission(
            self.invite.room(),
            incarnation,
            self.remote.id(),
            &self.route,
        );
        self.actor.remember_admission(seat);
    }

    /// A seat retired by the roster while its control stays open.
    fn retire(&mut self, incarnation: u64) {
        self.actor.pending_retired_incarnations.insert(incarnation);
        assert!(self.actor.controls.contains_key(&self.remote.id()));
    }

    fn stale(&self) -> SignedRoomTicket {
        self.ticket_issued(self.stale_at)
    }

    fn ticket_issued(&self, issued: u64) -> SignedRoomTicket {
        ticket_at(&self.invite, &self.player, &self.remote, issued)
    }

    /// A control from `remote` with `ticket`, handshaken as the clock stood at
    /// `clock`. The dialer's side first.
    async fn dial(
        &self,
        ticket: &SignedRoomTicket,
        clock: u64,
    ) -> (transport::ControlChannel, transport::ControlChannel) {
        handshake_pair_at(
            &self.host,
            &self.invite,
            &self.remote,
            ticket,
            host_state().policy(),
            clock,
        )
        .await
    }

    /// Parks `channel` for the peer, overdue, behind the worker it holds.
    fn park(&mut self, channel: transport::ControlChannel) {
        let peer = self.remote.id();
        let behind = self.actor.controls.get(&peer).map(|old| old.id());
        self.actor.parked_controls.insert(
            peer,
            ParkedControl {
                epoch: 1,
                channel,
                joined_invite: None,
                behind,
                since: tokio::time::Instant::now() - CONTROL_REPLACE_DRAIN_LIMIT * 2,
            },
        );
    }

    async fn stop(mut self) {
        self.actor.tasks.abort_all();
        self.actor.controls.clear();
        self.actor.clear_room();
        self.route.close().await;
        self.remote.close().await;
        self.host.close().await;
    }
}

async fn closed(connection: &iroh::endpoint::Connection) {
    timeout(Duration::from_secs(10), async {
        while connection.close_reason().is_none() {
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
    })
    .await
    .expect("the connection was closed");
}

/// Reservations and eligibility are separate. A control holds its account for
/// the duplicate-account rule for as long as it is open, but the reconnect
/// exception ends with the seat: not before the coordination admission, and
/// not after it retires.
///
/// A retired member whose old control is still open and who presents a current
/// ticket from the same endpoint is admitted as a newcomer (the endpoint holds
/// its own reservation, so the duplicate-account rule has no one to refuse);
/// another endpoint under that account is still refused until the control goes.
#[tokio::test]
async fn a_retired_seat_keeps_no_reconnect_exception_through_its_control() {
    timeout(Duration::from_secs(60), async {
        let mut room = SeatedHost::start().await;
        let issued = room.stale_at;
        let stale = room.stale();
        let current = room.ticket_issued(now().unwrap());

        // A control alone, before any coordination admission: no exception.
        let (_stale_client, stale_channel) = room.dial(&stale, issued).await;
        assert!(room.actor.refuse_public_control(&stale_channel));

        // Admitted: the seat is eligible, and its endpoint is the member.
        room.admit(0x55);
        assert!(!room.actor.refuse_public_control(&stale_channel));

        // The seat retires while the old control stays open.
        room.retire(0x55);
        assert!(room.actor.refuse_public_control(&stale_channel));
        let (_current_client, current_channel) = room.dial(&current, now().unwrap()).await;
        assert!(!room.actor.refuse_public_control(&current_channel));
        // The account is still reserved by the open control.
        let other = endpoint().await;
        let elsewhere = ticket_at(&room.invite, &room.player, &other, now().unwrap());
        let (_other_client, other_channel) = handshake_pair_at(
            &room.host,
            &room.invite,
            &other,
            &elsewhere,
            host_state().policy(),
            now().unwrap(),
        )
        .await;
        assert!(room.actor.refuse_public_control(&other_channel));
        // Once the control is gone the account is free.
        room.actor.controls.clear();
        assert!(!room.actor.refuse_public_control(&other_channel));
        assert!(room.actor.refuse_public_control(&stale_channel));
        other.close().await;
        room.stop().await;
    })
    .await
    .unwrap();
}

/// The install is the admission boundary: a seat retired while a replacement
/// waited behind its worker is refused when the worker lets go, and the old
/// worker's close is reported.
#[tokio::test]
async fn a_replacement_parked_across_a_retirement_is_refused_at_settle() {
    timeout(Duration::from_secs(60), async {
        let mut room = SeatedHost::start().await;
        room.admit(0x55);
        let peer = room.remote.id();
        let old = room.actor.controls[&peer].id();
        let (_client, candidate) = room.dial(&room.stale(), room.stale_at).await;
        let connection = candidate.connection.clone();
        // The early check passes, as it did when the candidate completed.
        assert!(!room.actor.refuse_public_control(&candidate));
        room.park(candidate);
        room.retire(0x55);
        room.actor.poll_controls().await.unwrap();
        assert!(room.actor.parked_controls.is_empty());
        assert!(!room.actor.controls.contains_key(&peer));
        closed(&connection).await;
        let Event::ControlClosed { control, .. } = next(&mut room.events, "control_closed").await
        else {
            panic!("not a close");
        };
        assert_eq!(control, old);
        room.stop().await;
    })
    .await
    .unwrap();
}

/// A ticket that was current when the candidate's handshake read it and has
/// expired by the time it settles is refused; a current one is installed.
#[tokio::test]
async fn a_replacement_whose_ticket_expired_while_parked_is_refused_at_settle() {
    timeout(Duration::from_secs(60), async {
        let mut room = SeatedHost::start().await;
        let peer = room.remote.id();
        room.actor.controls.clear();
        let issued = room.stale_at;
        let (_client, candidate) = room.dial(&room.stale(), issued + TICKET_SECS - 1).await;
        let connection = candidate.connection.clone();
        room.park(candidate);
        room.actor.poll_controls().await.unwrap();
        assert!(room.actor.parked_controls.is_empty());
        assert!(!room.actor.controls.contains_key(&peer));
        closed(&connection).await;

        let current = room.ticket_issued(now().unwrap());
        let (_client, candidate) = room.dial(&current, now().unwrap()).await;
        let id = candidate.connection.stable_id() as u64;
        room.park(candidate);
        room.actor.poll_controls().await.unwrap();
        assert!(room.actor.parked_controls.is_empty());
        assert_eq!(room.actor.controls[&peer].id(), id);
        room.stop().await;
    })
    .await
    .unwrap();
}

/// A member whose control is still draining reconnects with the ticket it
/// joined with, past its minute: its seat is bound, so the replacement is
/// decided as a returning member's and installed.
#[tokio::test]
async fn a_retained_member_replaces_its_draining_control_past_its_ticket() {
    timeout(Duration::from_secs(60), async {
        let mut room = SeatedHost::start().await;
        room.admit(0x55);
        let peer = room.remote.id();
        let (_client, candidate) = room.dial(&room.stale(), room.stale_at).await;
        let id = candidate.connection.stable_id() as u64;
        room.actor
            .completed_control(1, Ok(candidate), None)
            .await
            .unwrap();
        timeout(Duration::from_secs(10), async {
            while !room.actor.parked_controls.is_empty() {
                room.actor.poll_controls().await.unwrap();
                tokio::time::sleep(Duration::from_millis(20)).await;
            }
        })
        .await
        .expect("the replacement settles");
        assert_eq!(room.actor.controls[&peer].id(), id);
        room.stop().await;
    })
    .await
    .unwrap();
}

/// A newcomer waiting behind a worker is a reservation, not a member: it
/// does not seat its endpoint, so a stale ticket gets no exception meanwhile,
/// even once the endpoint's coordination admission is held.
#[tokio::test]
async fn a_waiting_candidate_cannot_grant_itself_the_reconnect_exception() {
    timeout(Duration::from_secs(60), async {
        let mut room = SeatedHost::start().await;
        let peer = room.remote.id();
        // A room that has not seated this endpoint yet.
        room.actor.public = Some(PublicRoom::Host(host_state()));
        let (_client, newcomer) = room
            .dial(&room.ticket_issued(now().unwrap()), now().unwrap())
            .await;
        room.actor
            .completed_control(1, Ok(newcomer), None)
            .await
            .unwrap();
        assert!(
            room.actor.parked_controls.contains_key(&peer),
            "still waiting"
        );
        room.admit(0x66);
        let (_stale_client, stale) = room.dial(&room.stale(), room.stale_at).await;
        assert!(room.actor.refuse_public_control(&stale));
        room.stop().await;
    })
    .await
    .unwrap();
}
