//! Public (server-owned) rooms: the control handshake a ticket-admitted member
//! presents and the checks a public host runs on it. See
//! `docs/design/PUBLIC_ROOMS.md`.
//!
//! A private room's `RoomProof` is untouched. The public proof is a distinct
//! struct, and both are `deny_unknown_fields`, so a private host cannot read a
//! public proof and a public host cannot read a private one; neither needs to
//! sniff the other.
use std::{
    collections::{BTreeMap, BTreeSet},
    io,
    sync::Arc,
    time::{SystemTime, UNIX_EPOCH},
};

use ember_protocol::{
    EmberId, PublicKey,
    encoding::is_prefixed_id,
    rooms::{MAX_ROOM_BANS, RoomTicket, SignedRoomTicket},
};
use iroh::{Endpoint, EndpointAddr, EndpointId, endpoint::Connection};
use serde::{Deserialize, Serialize};

use crate::{
    invite::{Invite, RoomProof},
    transport::{self, ControlChannel},
};

/// Bans a public host remembers: `MAX_ROOM_BANS`, the room's lifetime cap.
/// Bans are never evicted; the room authority closes the room before it asks
/// for more.
pub(crate) const MAX_BANS: usize = MAX_ROOM_BANS;
const MAX_KID: usize = 64;

/// The proof a joiner presents to a public host: the invitation's own fields
/// and the ticket the bridge signed for this endpoint. No `Debug`: the
/// capability must not reach logs.
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct PublicRoomProof {
    pub version: u16,
    pub room: [u8; 16],
    pub capability: [u8; 32],
    pub build: String,
    pub ticket: SignedRoomTicket,
}

impl PublicRoomProof {
    pub fn new(proof: RoomProof, ticket: SignedRoomTicket) -> Self {
        Self {
            version: proof.version,
            room: proof.room,
            capability: proof.capability,
            build: proof.build,
            ticket,
        }
    }

    /// The invitation half, checked by `Invite::admit` like any other proof.
    fn room_proof(&self) -> RoomProof {
        RoomProof {
            version: self.version,
            room: self.room,
            capability: self.capability,
            build: self.build.clone(),
        }
    }
}

fn refused() -> io::Error {
    io::Error::new(io::ErrorKind::ConnectionAborted, "admission refused")
}

pub(crate) fn hex(bytes: &[u8]) -> String {
    use std::fmt::Write;
    bytes.iter().fold(String::new(), |mut text, byte| {
        let _ = write!(text, "{byte:02x}");
        text
    })
}

fn now() -> io::Result<u64> {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|elapsed| elapsed.as_secs())
        .map_err(|_| refused())
}

/// A member's seat: the account the endpoint was admitted with and, once the
/// room holds the member's coordination admission, the incarnation the seat
/// is bound to. The binding is what makes a seat eligible to return past its
/// ticket's minute, and it ends with that incarnation.
#[derive(Clone, PartialEq, Eq)]
struct Seat {
    account: EmberId,
    incarnation: Option<u64>,
}

/// What an accept task checks a proof against. The ban set is shared copy on
/// write, so a snapshot for each incoming connection is cheap.
#[derive(Clone)]
pub(crate) struct AdmissionPolicy {
    key: PublicKey,
    kid: String,
    bridge_id: String,
    banned: Arc<BTreeSet<EmberId>>,
    /// The seats of the endpoints admitted as members of this room. Shared
    /// copy on write, like the ban set. Only a seat bound to a coordination
    /// admission is eligible for the reconnect exception (`decide`).
    members: Arc<BTreeMap<EndpointId, Seat>>,
    /// The account the room was created for. Until a member has been
    /// admitted, only this account is.
    creator: EmberId,
    /// Whether any member has been admitted yet. It stays set when the room
    /// empties again.
    opened: bool,
}

impl AdmissionPolicy {
    /// The checks, in order: the ticket's signature and key id, the admission
    /// decision (`decide`) at `now`, which the caller reads after the proof
    /// has arrived, and the invitation's own rules. `held` maps an account to
    /// the endpoint that holds a control under it. Any failure is the same
    /// refusal; nothing says which check failed. Returns the verified ticket,
    /// which the room's actor decides on again before it installs the control.
    pub fn check(
        &self,
        invite: &Invite,
        proof: &PublicRoomProof,
        remote: EndpointId,
        now: u64,
        held: &BTreeMap<EmberId, EndpointId>,
    ) -> io::Result<RoomTicket> {
        let room_proof = proof.room_proof();
        let ticket = proof
            .ticket
            .verify(&self.key, &self.kid)
            .map_err(|_| refused())?;
        if self.decide(ticket, &hex(&invite.room()), remote, now, held)? {
            // The room may have outlived the invitation, and a seat it holds
            // is not an invitation's to take away.
            invite.admit_member(&room_proof)?;
        } else {
            invite.admit(&room_proof, now)?;
        }
        Ok(ticket.clone())
    }

    /// The room's one admission policy, for a verified `ticket` presented by
    /// `remote` at `now`; the handshake and the actor's check before it
    /// installs the control both run it. Whether the ticket was taken as a
    /// returning member's is the result.
    ///
    /// Two things are kept apart. `held` is the account reservations of the
    /// controls the room has (live or waiting), which only the duplicate
    /// account rule reads. The reconnect exception reads the seats this policy
    /// holds, and a seat is eligible only while it is bound to its member's
    /// coordination admission; a control that outlives its seat confers
    /// nothing.
    ///
    /// A ticket lives only a minute, but a member whose control drops redials
    /// with the one it joined with. An endpoint this room currently holds as a
    /// member, presenting a ticket for its own room, endpoint and the account
    /// it was admitted with, is therefore taken whatever the ticket's clock
    /// says. It holds no more than the seat it already has. Anyone else,
    /// including a known endpoint under another account, needs a ticket that
    /// is current, and, while no member has been admitted, must be the
    /// room's creator. A ticket from another bridge, a banned account, and an
    /// account another endpoint already holds are refused in every case.
    pub fn decide(
        &self,
        ticket: &RoomTicket,
        room: &str,
        remote: EndpointId,
        now: u64,
        held: &BTreeMap<EmberId, EndpointId>,
    ) -> io::Result<bool> {
        let endpoint = remote.to_string();
        let rejoining = self.members.get(&remote).is_some_and(|seat| {
            seat.incarnation.is_some() && seat.account == ticket.ember_id
        }) && ticket.room_id == room
            && ticket.endpoint_id == endpoint;
        let welcome = rejoining
            || (ticket.admits(room, &endpoint, now)
                && (self.opened || ticket.ember_id == self.creator));
        if !welcome
            || ticket.bridge_id != self.bridge_id
            || self.banned.contains(&ticket.ember_id)
            || held
                .get(&ticket.ember_id)
                .is_some_and(|holder| *holder != remote)
        {
            return Err(refused());
        }
        Ok(rejoining)
    }

    #[cfg(test)]
    pub fn is_banned(&self, account: &EmberId) -> bool {
        self.banned.contains(account)
    }
}

/// A public room's host side: the key its tickets verify under and the
/// accounts it has banned for the room's life.
pub(crate) struct PublicHost {
    policy: AdmissionPolicy,
}

impl PublicHost {
    /// `ticket_key` is the bridge's public key as `/v1/signing-keys` publishes
    /// it (43 characters of unpadded base64url); `creator` is the Ember ID the
    /// room was created for. `None` when any field is malformed.
    pub fn parse(
        ticket_key: &str,
        ticket_kid: &str,
        bridge_id: &str,
        creator: &str,
    ) -> Option<Self> {
        let key = PublicKey::from_b64u(ticket_key).ok()?;
        let creator = EmberId::parse(creator).ok()?;
        let kid_valid =
            !ticket_kid.is_empty() && ticket_kid.len() <= MAX_KID && ticket_kid.is_ascii();
        (kid_valid && is_prefixed_id(bridge_id, "brg")).then(|| Self {
            policy: AdmissionPolicy {
                key,
                kid: ticket_kid.to_owned(),
                bridge_id: bridge_id.to_owned(),
                banned: Arc::new(BTreeSet::new()),
                members: Arc::new(BTreeMap::new()),
                creator,
                opened: false,
            },
        })
    }

    pub fn policy(&self) -> AdmissionPolicy {
        self.policy.clone()
    }

    /// The decision on a verified `ticket` against the room's current state,
    /// for `remote` at `now`; see `AdmissionPolicy::decide`.
    pub fn decide(
        &self,
        ticket: &RoomTicket,
        room: &str,
        remote: EndpointId,
        now: u64,
        held: &BTreeMap<EmberId, EndpointId>,
    ) -> io::Result<bool> {
        self.policy.decide(ticket, room, remote, now, held)
    }

    #[cfg(test)]
    pub fn is_banned(&self, account: &EmberId) -> bool {
        self.policy.is_banned(account)
    }

    /// Records that a control from `endpoint` was installed under `account`.
    /// This ends the creator-only phase. The seat it opens is not eligible to
    /// return past its ticket until `sync_members` binds it to the member's
    /// coordination admission. A bound seat keeps its account.
    pub fn admit_member(&mut self, endpoint: EndpointId, account: EmberId) {
        self.policy.opened = true;
        match self.policy.members.get(&endpoint) {
            Some(seat) if seat.incarnation.is_some() || seat.account == account => {}
            _ => {
                Arc::make_mut(&mut self.policy.members).insert(
                    endpoint,
                    Seat {
                        account,
                        incarnation: None,
                    },
                );
            }
        }
    }

    /// Brings the seats in line with the room's coordination roster. `live`
    /// is the (endpoint, incarnation) pair of each admission that has not been
    /// retired; `connected` the endpoints with an installed control. A seat
    /// bound to an incarnation lasts exactly as long as that admission, however
    /// many controls remain. A seat not yet bound binds to a live admission of
    /// its endpoint, and until one exists is kept only while its control is.
    pub fn sync_members(
        &mut self,
        live: &BTreeSet<(EndpointId, u64)>,
        connected: &BTreeSet<EndpointId>,
    ) {
        let synced: BTreeMap<EndpointId, Seat> = self
            .policy
            .members
            .iter()
            .filter_map(|(endpoint, seat)| {
                let incarnation = match seat.incarnation {
                    Some(bound) if live.contains(&(*endpoint, bound)) => Some(bound),
                    Some(_) => return None,
                    None => live
                        .iter()
                        .find(|(member, _)| member == endpoint)
                        .map(|(_, incarnation)| *incarnation),
                };
                if incarnation.is_none() && !connected.contains(endpoint) {
                    return None;
                }
                Some((
                    *endpoint,
                    Seat {
                        account: seat.account.clone(),
                        incarnation,
                    },
                ))
            })
            .collect();
        if synced != *self.policy.members {
            self.policy.members = Arc::new(synced);
        }
    }

    /// A seat held and bound at once, for the tests that need a member the
    /// room recognizes.
    #[cfg(test)]
    pub fn seat_member(&mut self, endpoint: EndpointId, account: EmberId) {
        self.admit_member(endpoint, account);
        self.sync_members(&BTreeSet::from([(endpoint, 1)]), &BTreeSet::new());
    }

    /// Bans `account` for the room's life. Banning a banned account changes
    /// nothing. Once `MAX_BANS` accounts are held another is refused and
    /// every existing ban stays.
    pub fn ban(&mut self, account: EmberId) -> Result<(), BanLimit> {
        if self.policy.banned.contains(&account) {
            return Ok(());
        }
        if self.policy.banned.len() >= MAX_BANS {
            return Err(BanLimit);
        }
        Arc::make_mut(&mut self.policy.banned).insert(account);
        Ok(())
    }
}

/// The ban set is full.
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct BanLimit;

/// The public marker of a room: the host's admission state, or the ticket a
/// member presents to the host. A room carrying either is server-owned.
pub(crate) enum PublicRoom {
    Host(PublicHost),
    Member { ticket: SignedRoomTicket },
}

/// Accepts a control connection on a public host. Reads only a
/// `PublicRoomProof`; a plain `RoomProof` fails to parse and is closed on
/// without an answer. A well-formed proof that is refused, including every
/// proof while the room is `full`, is answered with `PublicRefused`.
pub(crate) async fn accept_public_control(
    connection: Connection,
    invite: &Invite,
    policy: AdmissionPolicy,
    held: BTreeMap<EmberId, EndpointId>,
    full: bool,
) -> io::Result<ControlChannel> {
    accept_public_control_at(connection, invite, policy, held, move || {
        if full { Err(refused()) } else { now() }
    })
    .await
}

/// `clock` is read once the proof has arrived, so a ticket is judged at the
/// moment it is presented and not at the moment the dial began.
pub(crate) async fn accept_public_control_at(
    connection: Connection,
    invite: &Invite,
    policy: AdmissionPolicy,
    held: BTreeMap<EmberId, EndpointId>,
    clock: impl FnOnce() -> io::Result<u64>,
) -> io::Result<ControlChannel> {
    transport::accept_control_with(connection, |proof: PublicRoomProof, connection| {
        let now = clock()?;
        policy
            .check(invite, &proof, connection.remote_id(), now, &held)
            .map(|ticket| Some((ticket.ember_id.clone(), ticket)))
    })
    .await
}

/// `transport::connect_control` presenting a ticket.
pub(crate) async fn connect_public(
    endpoint: &Endpoint,
    invite: &Invite,
    ticket: &SignedRoomTicket,
) -> io::Result<ControlChannel> {
    connect_public_at(endpoint, invite.address(), invite, ticket).await
}

/// `connect_public` dialing `address` in place of the invitation's own.
pub(crate) async fn connect_public_at(
    endpoint: &Endpoint,
    address: EndpointAddr,
    invite: &Invite,
    ticket: &SignedRoomTicket,
) -> io::Result<ControlChannel> {
    let proof = PublicRoomProof::new(invite.proof(), ticket.clone());
    transport::connect_control_at(endpoint, address, invite, &proof, true).await
}

/// `transport::connect_control_to` presenting a ticket.
pub(crate) async fn connect_public_to(
    endpoint: &Endpoint,
    address: EndpointAddr,
    invite: &Invite,
    ticket: &SignedRoomTicket,
) -> io::Result<ControlChannel> {
    let proof = PublicRoomProof::new(invite.proof(), ticket.clone());
    transport::connect_control_to_with(endpoint, address, &proof, true).await
}

/// The local-harness form of `connect_public` over an existing connection.
#[cfg(test)]
pub(crate) async fn connect_public_on(
    connection: Connection,
    invite: &Invite,
    ticket: &SignedRoomTicket,
) -> io::Result<ControlChannel> {
    let proof = PublicRoomProof::new(invite.proof(), ticket.clone());
    transport::connect_public_on_proof(connection, &proof, Some(invite.endpoint())).await
}

#[cfg(test)]
mod tests;
