//! The signed admission a bridge issues for each join of a listed room.
use serde::{Deserialize, Serialize};

use crate::{
    EmberId, Error, PublicKey, Result, SigningIdentity,
    encoding::is_prefixed_id,
    play::{VERSION, is_hex},
    sign::{BridgeSigned, Domain, bridge_sign, bridge_verify},
    skill::SkillBadge,
};

use super::{TICKET_SECS, TICKET_SKEW_SECS};

/// Admission to one room for one helper endpoint, signed by the bridge.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RoomTicket {
    pub version: u8,
    pub bridge_id: String,
    pub room_id: String,
    pub ember_id: EmberId,
    /// The joiner's helper endpoint.
    pub endpoint_id: String,
    pub issued_at: u64,
    pub expires_at: u64,
    /// The joiner's skill badge, for a joiner who asked for it
    /// (`TicketRequest::skill_badge`) and has one. Written only when present,
    /// so a ticket without one is byte for byte what it was before.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub badge: Option<SkillBadge>,
}

impl RoomTicket {
    pub fn check(&self) -> Result<()> {
        let fields = [
            (self.version == VERSION, "version"),
            (is_prefixed_id(&self.bridge_id, "brg"), "bridge_id"),
            (is_hex(&self.room_id, 32), "room_id"),
            (is_hex(&self.endpoint_id, 64), "endpoint_id"),
            (
                self.issued_at.checked_add(TICKET_SECS) == Some(self.expires_at),
                "expires_at",
            ),
        ];
        if let Some((_, field)) = fields.iter().find(|(valid, _)| !valid) {
            return Err(Error::InvalidField(field));
        }
        self.badge.as_ref().map_or(Ok(()), SkillBadge::check)
    }

    pub fn sign(&self, key: &SigningIdentity, kid: &str) -> Result<SignedRoomTicket> {
        let (ticket, kid, signature) = bridge_sign(self, key, kid)?;
        Ok(SignedRoomTicket {
            ticket,
            kid,
            signature,
        })
    }

    /// Whether this ticket admits `endpoint_id` to `room_id` at `now` (unix
    /// seconds): the room and endpoint match, it was not issued more than the
    /// allowed skew ahead of `now`, and it has not expired.
    pub fn admits(&self, room_id: &str, endpoint_id: &str, now: u64) -> bool {
        self.room_id == room_id
            && self.endpoint_id == endpoint_id
            && self.issued_at <= now.saturating_add(TICKET_SKEW_SECS)
            && now < self.expires_at
    }
}

impl BridgeSigned for RoomTicket {
    const DOMAIN: Domain = Domain::RoomTicket;

    fn check(&self) -> Result<()> {
        RoomTicket::check(self)
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SignedRoomTicket {
    pub ticket: RoomTicket,
    pub kid: String,
    pub signature: String,
}

impl SignedRoomTicket {
    /// Checks the signature against the bridge key the room was created with
    /// and the ticket's own consistency. It does not look at the clock; see
    /// [`RoomTicket::admits`]. Whether it is the expected bridge is the
    /// caller's check.
    pub fn verify(&self, key: &PublicKey, kid: &str) -> Result<&RoomTicket> {
        bridge_verify(&self.ticket, &self.kid, &self.signature, key, kid)
    }
}

#[cfg(test)]
mod tests {
    use crate::{
        json,
        rooms::tests::{ROOM, key, ticket},
    };

    use super::*;

    #[test]
    fn tickets_verify_only_under_their_domain_key_and_kid() {
        let (bridge, player) = (key(9), key(1));
        let signed = ticket(player.ember_id()).sign(&bridge, "k1").unwrap();
        assert_eq!(
            signed.verify(&bridge.public_key(), "k1").unwrap(),
            &signed.ticket
        );
        assert_eq!(
            signed.verify(&bridge.public_key(), "k2"),
            Err(Error::Mismatch("kid"))
        );
        assert!(signed.verify(&player.public_key(), "k1").is_err());
        let mut tampered = signed.clone();
        tampered.ticket.endpoint_id = "b".repeat(64);
        assert_eq!(
            tampered.verify(&bridge.public_key(), "k1"),
            Err(Error::InvalidSignature)
        );
        let mut tampered = signed.clone();
        tampered.ticket.room_id = "f".repeat(32);
        assert!(tampered.verify(&bridge.public_key(), "k1").is_err());
        let mut tampered = signed;
        tampered.ticket.ember_id = key(2).ember_id().clone();
        assert!(tampered.verify(&bridge.public_key(), "k1").is_err());
    }

    #[test]
    fn a_ticket_signature_is_not_a_binding_or_permit_signature() {
        let (bridge, player) = (key(9), key(1));
        let value = json::to_value(&ticket(player.ember_id())).unwrap();
        let signature = bridge.sign(Domain::RoomTicket, &value);
        let public = bridge.public_key();
        assert!(
            public
                .verify(Domain::RoomTicket, &value, &signature)
                .is_ok()
        );
        for other in [Domain::Binding, Domain::Permit] {
            assert!(public.verify(other, &value, &signature).is_err());
            assert_ne!(
                other.signing_bytes(&value),
                Domain::RoomTicket.signing_bytes(&value)
            );
        }
        assert!(
            Domain::RoomTicket
                .signing_bytes(&value)
                .starts_with(b"EMBER:ROOM-TICKET:1\n")
        );
    }

    #[test]
    fn signing_refuses_an_inconsistent_ticket() {
        let bridge = key(9);
        let id = key(1).ember_id().clone();
        let good = ticket(&id);
        let broken = [
            RoomTicket {
                version: 2,
                ..good.clone()
            },
            RoomTicket {
                bridge_id: "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11".into(),
                ..good.clone()
            },
            RoomTicket {
                room_id: "abc".into(),
                ..good.clone()
            },
            RoomTicket {
                endpoint_id: "A".repeat(64),
                ..good.clone()
            },
            RoomTicket {
                expires_at: good.issued_at + TICKET_SECS + 1,
                ..good.clone()
            },
            RoomTicket {
                expires_at: good.issued_at,
                ..good.clone()
            },
            // A badge that is not one.
            RoomTicket {
                badge: Some(SkillBadge::class(0, true)),
                ..good.clone()
            },
        ];
        for ticket in broken {
            assert!(ticket.sign(&bridge, "k1").is_err(), "{ticket:?}");
        }
        assert!(good.sign(&bridge, "k1").is_ok());
    }

    #[test]
    fn admission_checks_room_endpoint_and_the_clock() {
        let t = ticket(key(1).ember_id());
        let endpoint = "a".repeat(64);
        assert!(t.admits(ROOM, &endpoint, 1000));
        assert!(t.admits(ROOM, &endpoint, 1059));
        assert!(!t.admits(ROOM, &endpoint, 1060));
        assert!(!t.admits(ROOM, &endpoint, 5000));
        assert!(!t.admits(&"f".repeat(32), &endpoint, 1000));
        assert!(!t.admits(ROOM, &"b".repeat(64), 1000));
        // Issued up to 30 s ahead of the verifier's clock is accepted.
        assert!(t.admits(ROOM, &endpoint, 970));
        assert!(!t.admits(ROOM, &endpoint, 969));
        assert!(t.admits(ROOM, &endpoint, 0) == (1000 <= TICKET_SKEW_SECS));
        assert!(!ticket(key(1).ember_id()).admits(ROOM, &endpoint, u64::MAX));
    }
}
