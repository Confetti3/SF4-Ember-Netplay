//! The public handshake over a local, loopback-routed pair of endpoints, with
//! an explicit clock as in the transport tests.
use std::{
    net::Ipv4Addr,
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
    time::Duration,
};

use ember_protocol::{
    SigningIdentity,
    play::VERSION as TICKET_VERSION,
    rooms::{RoomTicket, TICKET_SECS},
};
use iroh::endpoint::{PortmapperConfig, presets};
use zeroize::Zeroizing;

use super::*;
use crate::wire::{CONTROL_ALPN, GAME_ALPN};

const BRIDGE: &str = "brg_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11";
const OTHER_BRIDGE: &str = "brg_7f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11";
const BUILD: &str = "public-room-build";

fn identity(seed: u8) -> SigningIdentity {
    SigningIdentity::from_seed(&Zeroizing::new([seed; 32])).unwrap()
}

fn key_text(seed: u8) -> String {
    identity(seed).public_key().to_b64u()
}

/// The account every test room is created for.
fn creator() -> String {
    identity(1).ember_id().to_string()
}

fn unix_now() -> u64 {
    now().unwrap()
}

async fn endpoint() -> Endpoint {
    Endpoint::builder(presets::Minimal)
        .clear_ip_transports()
        .bind_addr((Ipv4Addr::LOCALHOST, 0))
        .unwrap()
        .alpns(vec![CONTROL_ALPN.to_vec(), GAME_ALPN.to_vec()])
        .portmapper_config(PortmapperConfig::Disabled)
        .bind()
        .await
        .unwrap()
}

fn address(endpoint: &Endpoint) -> EndpointAddr {
    EndpointAddr::new(endpoint.id()).with_ip_addr(endpoint.bound_sockets()[0])
}

fn invite_for(endpoint: &Endpoint, now: u64) -> Invite {
    let relay = iroh::defaults::prod::default_relay_map()
        .urls::<Vec<_>>()
        .remove(0);
    Invite::create(endpoint.id(), relay, BUILD.into(), now, 3600).unwrap()
}

fn host_policy(key_seed: u8, kid: &str, bridge: &str) -> AdmissionPolicy {
    PublicHost::parse(&key_text(key_seed), kid, bridge, &creator())
        .unwrap()
        .policy()
}

fn mint(
    signer: u8,
    kid: &str,
    bridge: &str,
    room: &Invite,
    account: &EmberId,
    endpoint: &str,
    issued_at: u64,
) -> SignedRoomTicket {
    RoomTicket {
        version: TICKET_VERSION,
        bridge_id: bridge.into(),
        room_id: hex(&room.room()),
        ember_id: account.clone(),
        endpoint_id: endpoint.into(),
        issued_at,
        expires_at: issued_at + TICKET_SECS,
    }
    .sign(&identity(signer), kid)
    .unwrap()
}

struct World {
    /// The one clock reading every ticket and invitation here derives from, so
    /// a test never straddles a second boundary.
    now: u64,
    host: Endpoint,
    guest: Endpoint,
    other: Endpoint,
    invite: Invite,
    account: EmberId,
}

impl World {
    async fn new() -> Self {
        let host = endpoint().await;
        let now = unix_now();
        let invite = invite_for(&host, now);
        Self {
            now,
            guest: endpoint().await,
            other: endpoint().await,
            host,
            invite,
            account: identity(1).ember_id().clone(),
        }
    }

    /// A ticket that is valid for `guest` unless `change` alters it.
    fn ticket(&self, change: impl FnOnce(&mut RoomTicket)) -> SignedRoomTicket {
        let mut ticket = RoomTicket {
            version: TICKET_VERSION,
            bridge_id: BRIDGE.into(),
            room_id: hex(&self.invite.room()),
            ember_id: self.account.clone(),
            endpoint_id: self.guest.id().to_string(),
            issued_at: self.now,
            expires_at: 0,
        };
        change(&mut ticket);
        ticket.expires_at = ticket.issued_at + TICKET_SECS;
        ticket.sign(&identity(9), "k1").unwrap()
    }

    /// `guest` dials the host with `connect` while the host accepts under
    /// `policy`. Returns the host's result and the guest's.
    async fn attempt(
        &self,
        ticket: &SignedRoomTicket,
        policy: AdmissionPolicy,
        held: BTreeMap<EmberId, EndpointId>,
        clock: u64,
    ) -> (io::Result<ControlChannel>, io::Result<ControlChannel>) {
        let (client, server) = tokio::join!(
            async {
                let connection = self
                    .guest
                    .connect(address(&self.host), CONTROL_ALPN)
                    .await
                    .unwrap();
                connect_public_on(connection, &self.invite, ticket).await
            },
            async {
                let connection = self.host.accept().await.unwrap().await.unwrap();
                accept_public_control_at(connection, &self.invite, policy, held, || Ok(clock))
                    .await
            }
        );
        (server, client)
    }

    async fn close(self) {
        self.host.close().await;
        self.guest.close().await;
        self.other.close().await;
    }
}

fn policy() -> AdmissionPolicy {
    host_policy(9, "k1", BRIDGE)
}

/// The host turned the guest away and said so: the guest's error is the
/// refusal, not a silent close or a timeout.
fn assert_refused(outcome: (io::Result<ControlChannel>, io::Result<ControlChannel>), why: &str) {
    assert!(outcome.0.is_err(), "host accepted: {why}");
    match &outcome.1 {
        Ok(_) => panic!("guest was told it was in: {why}"),
        Err(error) => assert!(
            transport::is_admission_refused(error),
            "guest saw '{error}' rather than a refusal: {why}"
        ),
    }
}

/// The host closed without an answer: not a refusal, not a timeout.
fn assert_closed_silently(guest: &io::Result<ControlChannel>, why: &str) {
    match guest {
        Ok(_) => panic!("guest was told it was in: {why}"),
        Err(error) => assert!(
            !transport::is_admission_refused(error) && !transport::is_handshake_timeout(error),
            "guest saw '{error}': {why}"
        ),
    }
}

#[tokio::test]
async fn a_ticket_for_this_endpoint_is_admitted_and_names_its_account() {
    let world = World::new().await;
    let ticket = world.ticket(|_| {});
    let (server, client) = world
        .attempt(&ticket, policy(), BTreeMap::new(), world.now)
        .await;
    assert_eq!(server.unwrap().account, Some(world.account.clone()));
    assert_eq!(client.unwrap().account, None);
    world.close().await;
}

#[tokio::test]
async fn a_ticket_the_host_cannot_verify_or_that_does_not_fit_is_refused() {
    let world = World::new().await;
    let now = world.now;
    let good = world.ticket(|_| {});

    let wrong_key = host_policy(8, "k1", BRIDGE);
    let wrong_kid = host_policy(9, "k2", BRIDGE);
    let wrong_bridge = host_policy(9, "k1", OTHER_BRIDGE);
    for (policy, why) in [
        (wrong_key, "signed by another key"),
        (wrong_kid, "another key id"),
        (wrong_bridge, "another bridge"),
    ] {
        assert_refused(
            world.attempt(&good, policy, BTreeMap::new(), now).await,
            why,
        );
    }

    let other_room = world.ticket(|ticket| ticket.room_id = "f".repeat(32));
    assert_refused(
        world
            .attempt(&other_room, policy(), BTreeMap::new(), now)
            .await,
        "another room's ticket",
    );
    let other_endpoint = world.ticket(|ticket| ticket.endpoint_id = world.other.id().to_string());
    assert_refused(
        world
            .attempt(&other_endpoint, policy(), BTreeMap::new(), now)
            .await,
        "another endpoint's ticket",
    );
    // The same ticket is good right up to its expiry and not after it.
    let timed = world.ticket(|_| {});
    let issued = timed.ticket.issued_at;
    let (server, _client) = world
        .attempt(&timed, policy(), BTreeMap::new(), issued + TICKET_SECS - 1)
        .await;
    assert!(server.is_ok());
    assert_refused(
        world
            .attempt(&timed, policy(), BTreeMap::new(), issued + TICKET_SECS)
            .await,
        "expired ticket",
    );
    // Issued ahead of the host's clock by at most the skew.
    let early = world.ticket(|ticket| ticket.issued_at += 30);
    let (server, _client) = world.attempt(&early, policy(), BTreeMap::new(), now).await;
    assert!(server.is_ok());
    let too_early = world.ticket(|ticket| ticket.issued_at += 31);
    assert_refused(
        world
            .attempt(&too_early, policy(), BTreeMap::new(), now)
            .await,
        "ticket from the future",
    );
    world.close().await;
}

#[tokio::test]
async fn a_signature_made_for_other_fields_does_not_carry_over() {
    let world = World::new().await;
    let now = world.now;
    let mut ticket = world.ticket(|_| {});
    ticket.ticket.ember_id = identity(2).ember_id().clone();
    assert_refused(
        world.attempt(&ticket, policy(), BTreeMap::new(), now).await,
        "edited ember id",
    );
    // A ticket signed by the bridge key, minted outside the helper.
    let minted = mint(
        9,
        "k1",
        BRIDGE,
        &world.invite,
        &world.account,
        &world.guest.id().to_string(),
        now,
    );
    let (server, _client) = world.attempt(&minted, policy(), BTreeMap::new(), now).await;
    assert!(server.is_ok());
    world.close().await;
}

#[tokio::test]
async fn a_banned_account_is_refused_and_others_are_not() {
    let world = World::new().await;
    let now = world.now;
    let ticket = world.ticket(|_| {});
    let mut host = PublicHost::parse(&key_text(9), "k1", BRIDGE, &creator()).unwrap();
    host.ban(world.account.clone()).unwrap();
    assert!(host.is_banned(&world.account));
    assert_refused(
        world
            .attempt(&ticket, host.policy(), BTreeMap::new(), now)
            .await,
        "banned account",
    );
    let mut elsewhere = PublicHost::parse(&key_text(9), "k1", BRIDGE, &creator()).unwrap();
    elsewhere.ban(identity(2).ember_id().clone()).unwrap();
    let (server, _client) = world
        .attempt(&ticket, elsewhere.policy(), BTreeMap::new(), now)
        .await;
    assert!(server.is_ok());
    world.close().await;
}

#[tokio::test]
async fn an_account_held_by_another_endpoint_is_refused_but_a_reconnect_is_not() {
    let world = World::new().await;
    let now = world.now;
    let ticket = world.ticket(|_| {});
    let held_elsewhere = BTreeMap::from([(world.account.clone(), world.other.id())]);
    assert_refused(
        world.attempt(&ticket, policy(), held_elsewhere, now).await,
        "account already connected from another endpoint",
    );
    let held_here = BTreeMap::from([(world.account.clone(), world.guest.id())]);
    let (server, _client) = world.attempt(&ticket, policy(), held_here, now).await;
    assert_eq!(server.unwrap().account, Some(world.account.clone()));
    world.close().await;
}

#[tokio::test]
async fn the_invitation_rules_still_apply_inside_a_public_proof() {
    let world = World::new().await;
    let now = world.now;
    // Another room of the same host: its capability and room do not match.
    let stranger = invite_for(&world.host, now);
    let ticket = world.ticket(|_| {});
    let (server, client) = tokio::join!(
        async {
            let connection = world.host.accept().await.unwrap().await.unwrap();
            accept_public_control_at(connection, &world.invite, policy(), BTreeMap::new(), || {
                Ok(now)
            })
            .await
        },
        async {
            let connection = world
                .guest
                .connect(address(&world.host), CONTROL_ALPN)
                .await
                .unwrap();
            connect_public_on(connection, &stranger, &ticket).await
        }
    );
    assert!(server.is_err());
    // A proof for another room's capability is refused like any other.
    assert!(transport::is_admission_refused(&client.err().unwrap()));
    // The host's own invitation, once expired, admits nobody, though the
    // ticket is still inside its minute.
    let expiry = world.invite.expires();
    let fresh = world.ticket(|ticket| ticket.issued_at = expiry);
    assert_refused(
        world
            .attempt(&fresh, policy(), BTreeMap::new(), expiry + 1)
            .await,
        "expired invitation",
    );
    world.close().await;
}

#[tokio::test]
async fn a_full_room_refuses_a_good_proof_and_an_open_one_admits_it() {
    let world = World::new().await;
    let ticket = world.ticket(|_| {});
    for full in [true, false] {
        let (server, client) = tokio::join!(
            async {
                let connection = world.host.accept().await.unwrap().await.unwrap();
                accept_public_control(connection, &world.invite, policy(), BTreeMap::new(), full)
                    .await
            },
            async {
                let connection = world
                    .guest
                    .connect(address(&world.host), CONTROL_ALPN)
                    .await
                    .unwrap();
                connect_public_on(connection, &world.invite, &ticket).await
            }
        );
        if full {
            assert_refused((server, client), "room full");
        } else {
            assert!(server.is_ok() && client.is_ok(), "room with space");
        }
    }
    world.close().await;
}

#[tokio::test]
async fn a_host_that_never_answers_is_a_timeout_and_not_a_refusal() {
    let world = World::new().await;
    let ticket = world.ticket(|_| {});
    let (done, finished) = tokio::sync::oneshot::channel::<()>();
    let (client, _connection) = tokio::time::timeout(Duration::from_secs(60), async {
        tokio::join!(
            async {
                let connection = world
                    .guest
                    .connect(address(&world.host), CONTROL_ALPN)
                    .await
                    .unwrap();
                let outcome = connect_public_on(connection, &world.invite, &ticket).await;
                let _ = done.send(());
                outcome
            },
            // The host takes the connection and says nothing until the guest
            // has given up.
            async {
                let connection = world.host.accept().await.unwrap().await.unwrap();
                let _ = finished.await;
                connection
            }
        )
    })
    .await
    .unwrap();
    let error = client.err().expect("the guest got no answer");
    assert!(transport::is_handshake_timeout(&error), "guest saw '{error}'");
    assert!(!transport::is_admission_refused(&error));
    assert!(!transport::is_host_unreachable(&error));
    world.close().await;
}

#[tokio::test]
async fn a_public_host_reads_only_a_public_proof_and_a_private_host_only_a_private_one() {
    let world = World::new().await;
    let now = world.now;
    // A plain RoomProof dialing a public host.
    let (server, client) = tokio::join!(
        async {
            let connection = world.host.accept().await.unwrap().await.unwrap();
            accept_public_control_at(connection, &world.invite, policy(), BTreeMap::new(), || {
                Ok(now)
            })
            .await
        },
        async {
            let connection = world
                .guest
                .connect(address(&world.host), CONTROL_ALPN)
                .await
                .unwrap();
            transport::connect_control_on(connection, &world.invite).await
        }
    );
    assert!(server.is_err(), "plain proof at a public host");
    // A proof that does not parse is closed on, with no refusal to learn from.
    assert_closed_silently(&client, "plain proof at a public host");

    // A PublicRoomProof dialing a private host.
    let ticket = world.ticket(|_| {});
    let (server, client) = tokio::join!(
        async {
            let connection = world.host.accept().await.unwrap().await.unwrap();
            transport::accept_control(connection, &world.invite).await
        },
        async {
            let connection = world
                .guest
                .connect(address(&world.host), CONTROL_ALPN)
                .await
                .unwrap();
            connect_public_on(connection, &world.invite, &ticket).await
        }
    );
    assert!(server.is_err(), "public proof at a private host");
    // The private handshake has no refusal frame: its host stays silent.
    assert_closed_silently(&client, "public proof at a private host");

    // And the private pair still works.
    let (server, client) = tokio::join!(
        async {
            let connection = world.host.accept().await.unwrap().await.unwrap();
            transport::accept_control(connection, &world.invite).await
        },
        async {
            let connection = world
                .guest
                .connect(address(&world.host), CONTROL_ALPN)
                .await
                .unwrap();
            transport::connect_control_on(connection, &world.invite).await
        }
    );
    assert_eq!(server.unwrap().account, None);
    assert!(client.is_ok());
    world.close().await;
}

#[test]
fn the_two_proofs_are_not_readable_as_each_other() {
    let id = iroh::SecretKey::generate().public();
    let relay = iroh::defaults::prod::default_relay_map()
        .urls::<Vec<_>>()
        .remove(0);
    let invite = Invite::create(id, relay, BUILD.into(), unix_now(), 3600).unwrap();
    let ticket = RoomTicket {
        version: TICKET_VERSION,
        bridge_id: BRIDGE.into(),
        room_id: hex(&invite.room()),
        ember_id: identity(1).ember_id().clone(),
        endpoint_id: "a".repeat(64),
        issued_at: 1000,
        expires_at: 1000 + TICKET_SECS,
    }
    .sign(&identity(9), "k1")
    .unwrap();
    let public = serde_json::to_vec(&PublicRoomProof::new(invite.proof(), ticket)).unwrap();
    let private = serde_json::to_vec(&invite.proof()).unwrap();
    assert!(serde_json::from_slice::<RoomProof>(&public).is_err());
    assert!(serde_json::from_slice::<PublicRoomProof>(&private).is_err());
    assert!(serde_json::from_slice::<PublicRoomProof>(&public).is_ok());
    assert!(serde_json::from_slice::<RoomProof>(&private).is_ok());
}

#[test]
fn a_host_needs_a_valid_key_key_id_and_bridge() {
    let key = key_text(9);
    assert!(PublicHost::parse(&key, "k1", BRIDGE, &creator()).is_some());
    assert!(PublicHost::parse(&key, &"k".repeat(64), BRIDGE, &creator()).is_some());
    for (key, kid, bridge, why) in [
        ("", "k1", BRIDGE, "empty key"),
        ("not a key", "k1", BRIDGE, "malformed key"),
        (&key[..42], "k1", BRIDGE, "short key"),
        (&key, "", BRIDGE, "empty key id"),
        (&key, &"k".repeat(65)[..], BRIDGE, "long key id"),
        (&key, "k\u{e9}", BRIDGE, "non-ASCII key id"),
        (&key, "k1", "", "empty bridge"),
        (
            &key,
            "k1",
            "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11",
            "other id kind",
        ),
        (&key, "k1", "brg_nope", "malformed bridge"),
    ] {
        assert!(
            PublicHost::parse(key, kid, bridge, &creator()).is_none(),
            "{why}"
        );
    }
    for creator in ["", "not an id", &"a".repeat(64)] {
        assert!(PublicHost::parse(&key, "k1", BRIDGE, creator).is_none());
    }
}

/// `count` distinct accounts, none of them the ones other tests use by seed.
fn accounts(count: usize) -> Vec<EmberId> {
    (0..count)
        .map(|index| {
            let mut seed = [0u8; 32];
            seed[..8].copy_from_slice(&(index as u64 + 1).to_le_bytes());
            SigningIdentity::from_seed(&Zeroizing::new(seed))
                .unwrap()
                .ember_id()
                .clone()
        })
        .collect()
}

#[test]
fn a_ban_lasts_for_the_rooms_life() {
    let mut host = PublicHost::parse(&key_text(9), "k1", BRIDGE, &creator()).unwrap();
    let accounts = accounts(MAX_BANS);
    for account in &accounts {
        host.ban(account.clone()).unwrap();
    }
    // Every ban up to the cap is still held, the first included.
    assert_eq!(host.policy.banned.len(), MAX_BANS);
    for account in &accounts {
        assert!(host.is_banned(account));
    }
    // Banning again changes nothing, even when the set is full.
    assert_eq!(host.ban(accounts[0].clone()), Ok(()));
    assert_eq!(host.policy.banned.len(), MAX_BANS);
    // A snapshot taken before a ban does not see it.
    let mut other = PublicHost::parse(&key_text(9), "k1", BRIDGE, &creator()).unwrap();
    let snapshot = other.policy();
    let late = identity(77).ember_id().clone();
    other.ban(late.clone()).unwrap();
    assert!(!snapshot.is_banned(&late));
    assert!(other.is_banned(&late));
}

#[test]
fn a_full_ban_set_refuses_another_and_forgets_nothing() {
    assert_eq!(MAX_BANS, 512);
    let mut host = PublicHost::parse(&key_text(9), "k1", BRIDGE, &creator()).unwrap();
    let accounts = accounts(MAX_BANS + 3);
    for account in &accounts[..MAX_BANS] {
        host.ban(account.clone()).unwrap();
    }
    for refused in &accounts[MAX_BANS..] {
        assert_eq!(host.ban(refused.clone()), Err(BanLimit));
        assert!(!host.is_banned(refused));
    }
    assert_eq!(host.policy.banned.len(), MAX_BANS);
    for kept in &accounts[..MAX_BANS] {
        assert!(host.is_banned(kept));
    }
}

/// A host that holds `guest` as a member under `world.account`.
fn host_holding(world: &World, banned: bool) -> AdmissionPolicy {
    let mut host = PublicHost::parse(&key_text(9), "k1", BRIDGE, &creator()).unwrap();
    host.seat_member(world.guest.id(), world.account.clone());
    if banned {
        host.ban(world.account.clone()).unwrap();
    }
    host.policy()
}

#[tokio::test]
async fn a_member_returns_with_its_ticket_after_the_ticket_has_expired() {
    let world = World::new().await;
    let now = world.now;
    // Issued ten minutes ago: long past its minute.
    let stale = world.ticket(|ticket| ticket.issued_at = now - 10 * TICKET_SECS);
    let (server, _client) = world
        .attempt(&stale, host_holding(&world, false), BTreeMap::new(), now)
        .await;
    assert_eq!(server.unwrap().account, Some(world.account.clone()));
    // The same ticket from an endpoint the room does not hold is refused.
    assert_refused(
        world
            .attempt(&stale, policy(), BTreeMap::new(), now)
            .await,
        "expired ticket, unknown endpoint",
    );
    // So is another member's endpoint presenting it, or the member's own
    // endpoint under another account.
    let mut elsewhere = PublicHost::parse(&key_text(9), "k1", BRIDGE, &creator()).unwrap();
    elsewhere.seat_member(world.other.id(), world.account.clone());
    assert_refused(
        world
            .attempt(&stale, elsewhere.policy(), BTreeMap::new(), now)
            .await,
        "expired ticket, another endpoint is the member",
    );
    let mut renamed = PublicHost::parse(&key_text(9), "k1", BRIDGE, &creator()).unwrap();
    renamed.seat_member(world.guest.id(), identity(2).ember_id().clone());
    assert_refused(
        world
            .attempt(&stale, renamed.policy(), BTreeMap::new(), now)
            .await,
        "expired ticket, endpoint admitted as another account",
    );
    world.close().await;
}

#[tokio::test]
async fn a_banned_member_is_refused_even_with_a_current_ticket_or_an_old_one() {
    let world = World::new().await;
    let now = world.now;
    let stale = world.ticket(|ticket| ticket.issued_at = now - 10 * TICKET_SECS);
    assert_refused(
        world
            .attempt(&stale, host_holding(&world, true), BTreeMap::new(), now)
            .await,
        "expired ticket, banned account",
    );
    let current = world.ticket(|_| {});
    assert_refused(
        world
            .attempt(&current, host_holding(&world, true), BTreeMap::new(), now)
            .await,
        "current ticket, banned account",
    );
    world.close().await;
}

#[tokio::test]
async fn a_member_returning_late_still_has_to_present_a_ticket_that_fits() {
    let world = World::new().await;
    let now = world.now;
    let old = |change: fn(&mut RoomTicket)| world.ticket(|ticket| {
        ticket.issued_at = now - 10 * TICKET_SECS;
        change(ticket);
    });
    let cases = [
        (old(|ticket| ticket.room_id = "f".repeat(32)), "another room"),
        (
            old(|ticket| ticket.endpoint_id = "e".repeat(64)),
            "another endpoint",
        ),
    ];
    for (ticket, why) in cases {
        assert_refused(
            world
                .attempt(&ticket, host_holding(&world, false), BTreeMap::new(), now)
                .await,
            why,
        );
    }
    let stale = old(|_| {});
    let wrong_key = {
        let mut host = PublicHost::parse(&key_text(8), "k1", BRIDGE, &creator()).unwrap();
        host.seat_member(world.guest.id(), world.account.clone());
        host.policy()
    };
    assert_refused(
        world.attempt(&stale, wrong_key, BTreeMap::new(), now).await,
        "another signing key",
    );
    // The room's invitation lapsing does not end a member's seat.
    let expiry = world.invite.expires();
    let (server, _client) = world
        .attempt(
            &stale,
            host_holding(&world, false),
            BTreeMap::new(),
            expiry + 1,
        )
        .await;
    assert!(server.is_ok());
    assert_refused(
        world
            .attempt(&stale, policy(), BTreeMap::new(), expiry + 1)
            .await,
        "expired invitation, unknown endpoint",
    );
    world.close().await;
}

/// A ticket proof arrives after the host began to wait for it: the host reads
/// the clock when it has the proof, not when the dial began.
#[tokio::test]
async fn the_clock_is_read_after_the_proof_arrives() {
    let world = World::new().await;
    let ticket = world.ticket(|_| {});
    let expiry = ticket.ticket.expires_at;
    for (late, why) in [(false, "presented in time"), (true, "presented after expiry")] {
        let clock = Arc::new(AtomicU64::new(world.now));
        let (server, client) = tokio::join!(
            async {
                let connection = world.host.accept().await.unwrap().await.unwrap();
                let clock = clock.clone();
                accept_public_control_at(
                    connection,
                    &world.invite,
                    policy(),
                    BTreeMap::new(),
                    move || Ok(clock.load(Ordering::SeqCst)),
                )
                .await
            },
            async {
                let connection = world
                    .guest
                    .connect(address(&world.host), CONTROL_ALPN)
                    .await
                    .unwrap();
                // The dial has begun; the ticket runs out before the proof is sent.
                if late {
                    clock.store(expiry, Ordering::SeqCst);
                }
                connect_public_on(connection, &world.invite, &ticket).await
            }
        );
        assert_eq!(server.is_ok(), !late, "{why}");
        assert_eq!(client.is_ok(), !late, "{why}");
    }
    world.close().await;
}

/// An unsigned ticket for the pure decision.
fn ticket_of(account: &EmberId, endpoint: EndpointId, room: &str, issued_at: u64) -> RoomTicket {
    RoomTicket {
        version: TICKET_VERSION,
        bridge_id: BRIDGE.into(),
        room_id: room.into(),
        ember_id: account.clone(),
        endpoint_id: endpoint.to_string(),
        issued_at,
        expires_at: issued_at + TICKET_SECS,
    }
}

#[test]
fn until_a_member_is_admitted_only_the_creator_is() {
    let (creator, other) = (identity(1).ember_id().clone(), identity(2).ember_id().clone());
    let (first, second) = (
        iroh::SecretKey::generate().public(),
        iroh::SecretKey::generate().public(),
    );
    let room = "ab".repeat(16);
    let none = BTreeMap::new();
    let mut host = PublicHost::parse(&key_text(9), "k1", BRIDGE, creator.as_str()).unwrap();

    let by_other = ticket_of(&other, second, &room, 1000);
    let by_creator = ticket_of(&creator, first, &room, 1000);
    assert!(host.decide(&by_other, &room, second, 1000, &none).is_err());
    assert_eq!(host.decide(&by_creator, &room, first, 1000, &none).ok(), Some(false));

    // Once the creator is in, anyone with a current ticket is.
    host.admit_member(first, creator.clone());
    assert_eq!(host.decide(&by_other, &room, second, 1000, &none).ok(), Some(false));
    // A ticket that is not current is still not.
    assert!(host.decide(&by_other, &room, second, 1060, &none).is_err());

    // The room empties: the first rule does not come back.
    host.sync_members(&BTreeSet::new(), &BTreeSet::new());
    assert_eq!(host.decide(&by_other, &room, second, 1000, &none).ok(), Some(false));
}

#[test]
fn a_returning_member_is_taken_only_while_the_room_still_holds_its_seat() {
    let (creator, other) = (identity(1).ember_id().clone(), identity(2).ember_id().clone());
    let endpoint = iroh::SecretKey::generate().public();
    let room = "ab".repeat(16);
    let none = BTreeMap::new();
    let mut host = PublicHost::parse(&key_text(9), "k1", BRIDGE, creator.as_str()).unwrap();
    host.seat_member(endpoint, other.clone());
    let old = ticket_of(&other, endpoint, &room, 1000);
    // Long past its minute, taken as the member it is.
    assert_eq!(host.decide(&old, &room, endpoint, 9000, &none).ok(), Some(true));
    // Under another account, or for another room, it is a newcomer's old ticket.
    let renamed = ticket_of(&creator, endpoint, &room, 1000);
    assert!(host.decide(&renamed, &room, endpoint, 9000, &none).is_err());
    let elsewhere = "cd".repeat(16);
    assert!(host.decide(&old, &elsewhere, endpoint, 9000, &none).is_err());
    // An account held by another endpoint ends it.
    let held = BTreeMap::from([(other.clone(), iroh::SecretKey::generate().public())]);
    assert!(host.decide(&old, &room, endpoint, 9000, &held).is_err());
    // Once the seat is retired the ticket has to be current.
    host.sync_members(&BTreeSet::new(), &BTreeSet::new());
    assert!(host.decide(&old, &room, endpoint, 9000, &none).is_err());
    assert_eq!(host.decide(&old, &room, endpoint, 1000, &none).ok(), Some(false));
    host.ban(other.clone()).unwrap();
    assert!(host.decide(&old, &room, endpoint, 1000, &none).is_err());
}

/// The reconnect exception belongs to a seat bound to a live coordination
/// admission. A control alone confers none, and retiring the admission ends
/// the seat whatever controls remain.
#[test]
fn a_seat_is_eligible_only_while_its_admission_is_live() {
    let (creator, other) = (identity(1).ember_id().clone(), identity(2).ember_id().clone());
    let endpoint = iroh::SecretKey::generate().public();
    let room = "ab".repeat(16);
    let none = BTreeMap::new();
    let connected = BTreeSet::from([endpoint]);
    let mut host = PublicHost::parse(&key_text(9), "k1", BRIDGE, creator.as_str()).unwrap();
    host.admit_member(endpoint, other.clone());
    let old = ticket_of(&other, endpoint, &room, 1000);

    // Installed but not admitted by the coordination roster: no exception.
    assert!(host.decide(&old, &room, endpoint, 9000, &none).is_err());
    host.sync_members(&BTreeSet::new(), &connected);
    assert!(host.decide(&old, &room, endpoint, 9000, &none).is_err());

    // The admission binds the seat, which then outlives its control.
    host.sync_members(&BTreeSet::from([(endpoint, 7)]), &connected);
    assert_eq!(host.decide(&old, &room, endpoint, 9000, &none).ok(), Some(true));
    host.sync_members(&BTreeSet::from([(endpoint, 7)]), &BTreeSet::new());
    assert_eq!(host.decide(&old, &room, endpoint, 9000, &none).ok(), Some(true));

    // The admission retires while a control is still open: the seat is gone,
    // a later admission does not bring it back, and a current ticket is a
    // newcomer's.
    host.sync_members(&BTreeSet::new(), &connected);
    assert!(host.decide(&old, &room, endpoint, 9000, &none).is_err());
    host.sync_members(&BTreeSet::from([(endpoint, 8)]), &connected);
    assert!(host.decide(&old, &room, endpoint, 9000, &none).is_err());
    assert_eq!(host.decide(&old, &room, endpoint, 1000, &none).ok(), Some(false));
}
