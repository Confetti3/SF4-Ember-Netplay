//! A player's class in their room ticket: under the bridge's signature when
//! present, and absent byte for byte, so a ticket without one is what every
//! older reader (which refuses unknown fields) already reads.
use ember_protocol::{
    SigningIdentity, json,
    play::VERSION,
    rooms::{RoomTicket, SignedRoomTicket, TICKET_SECS},
    skill::SkillBadge,
};
use zeroize::Zeroizing;

fn key(byte: u8) -> SigningIdentity {
    SigningIdentity::from_seed(&Zeroizing::new([byte; 32])).unwrap()
}

fn ticket(badge: Option<SkillBadge>) -> RoomTicket {
    RoomTicket {
        version: VERSION,
        bridge_id: "brg_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11".into(),
        room_id: "0123456789abcdef0123456789abcdef".into(),
        ember_id: key(1).ember_id().clone(),
        endpoint_id: "a".repeat(64),
        issued_at: 1000,
        expires_at: 1000 + TICKET_SECS,
        badge,
    }
}

#[test]
fn a_ticket_without_a_class_is_what_it_was() {
    let value = json::to_value(&ticket(None)).unwrap();
    let canonical = String::from_utf8(value.canonical()).unwrap();
    assert!(!canonical.contains("badge"), "{canonical}");
}

#[test]
fn the_class_is_under_the_signature() {
    let bridge = key(9);
    let badged = ticket(Some(SkillBadge::class(4, true)));
    let wire = serde_json::to_string(&badged.sign(&bridge, "k1").unwrap()).unwrap();
    let read: SignedRoomTicket = serde_json::from_str(&wire).unwrap();
    assert_eq!(read.verify(&bridge.public_key(), "k1").unwrap(), &badged);
    let mut forged = read;
    forged.ticket.badge = Some(SkillBadge::class(10, true));
    assert!(forged.verify(&bridge.public_key(), "k1").is_err());
}
