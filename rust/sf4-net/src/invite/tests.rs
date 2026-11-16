use super::*;
use ember_protocol::relay;

fn invite() -> Invite {
    let id = iroh::SecretKey::generate().public();
    let relay = relay_map().urls::<Vec<_>>().remove(0);
    Invite::create(id, relay, "test-sidecar-hash".into(), 100, 3600).unwrap()
}

fn legacy(invite: &Invite) -> String {
    format!(
        "{LEGACY_PREFIX}{}",
        URL_SAFE_NO_PAD.encode(serde_json::to_vec(invite).unwrap())
    )
}

#[test]
fn a_renewed_invitation_keeps_its_room_and_route() {
    let original = invite();
    let renewed = original.clone().renewed(3000, 3600).unwrap();
    assert_eq!(renewed.expires(), 6600);
    assert_eq!(renewed.room(), original.room());
    assert_eq!(renewed.endpoint(), original.endpoint());
    assert_eq!(renewed.build(), original.build());
    assert!(bool::from(renewed.capability.ct_eq(&original.capability)));
    // The old text is still refused once its own hour is over; the
    // renewed text is accepted.
    let build = original.build().to_owned();
    assert!(Invite::parse_for_build(&original.encode().unwrap(), 3700, &build).is_err());
    assert!(Invite::parse_for_build(&renewed.encode().unwrap(), 3700, &build).is_ok());
    assert!(original.clone().renewed(3000, 0).is_err());
    assert!(original.renewed(3000, MAX_INVITE_LIFETIME_SECS + 1).is_err());
}

#[test]
fn a_reader_with_a_slower_clock_still_takes_a_fresh_week_long_invitation() {
    let id = iroh::SecretKey::generate().public();
    let relay = relay_map().urls::<Vec<_>>().remove(0);
    let issued = 1_000_000;
    let fresh = Invite::create(id, relay, "build".into(), issued, MAX_INVITE_LIFETIME_SECS).unwrap();
    let renewed = fresh.clone().renewed(issued + 100, MAX_INVITE_LIFETIME_SECS).unwrap();
    for invite in [&fresh, &renewed] {
        for text in [invite.encode().unwrap(), invite.encode_discord().unwrap()] {
            // Two minutes and most of a day behind the issuer: still a
            // real invitation.
            assert!(Invite::parse_for_build(&text, issued - 120, "build").is_ok());
            assert!(Invite::parse_for_build(&text, issued - 23 * 60 * 60, "build").is_ok());
            // More than a day behind: not one this build issues.
            assert!(Invite::parse_for_build(&text, issued - 25 * 60 * 60, "build").is_err());
            // Expiry is exact: past the week it is refused.
            assert!(Invite::parse_for_build(&text, invite.expires(), "build").is_err());
        }
    }
}

#[test]
fn a_refused_invitation_names_why() {
    let original = invite();
    let token = original.encode().unwrap();
    let build = original.build().to_owned();
    assert!(Invite::parse_for_build(&token, 101, &build).is_ok());
    // Expired: an hour-old invitation from a room since closed.
    assert!(Invite::parse_for_build(&token, 3700, &build).is_err());
    assert_eq!(Invite::rejection_reason(&token, 3700, &build), "expired");
    assert_eq!(
        Invite::rejection_reason(&format!("{token}\r\n"), 3700, &build),
        "expired"
    );
    // Another package.
    assert!(Invite::parse_for_build(&token, 101, "other-sidecar").is_err());
    assert_eq!(
        Invite::rejection_reason(&token, 101, "other-sidecar"),
        "other_build"
    );
    // An older invitation format.
    let mut old = original.clone();
    old.version = VERSION - 1;
    assert_eq!(
        Invite::rejection_reason(&legacy(&old), 101, &build),
        "old_version"
    );
    // Cut at a whole Base64 group: it still decodes, with part of the build.
    let groups = &token[..token.len() - 8];
    assert!(Invite::parse_for_build(groups, 101, &build).is_err());
    assert_eq!(Invite::rejection_reason(groups, 101, &build), "malformed");
    // Cut short or not an invitation at all.
    for text in [&token[..token.len() - 10], "hello", "", "sf4e2:!!!"] {
        assert!(Invite::parse_for_build(text, 101, &build).is_err());
        assert_eq!(Invite::rejection_reason(text, 101, &build), "malformed");
    }
    // A Discord invitation takes the local build, so only expiry shows.
    let discord = original.encode_discord().unwrap();
    assert_eq!(Invite::rejection_reason(&discord, 3700, &build), "expired");
}

#[test]
fn relay_regions_are_stable_codes_and_never_a_url() {
    // Wire IDs never move: each one names exactly this relay.
    let wire = [
        ("https://use1-1.relay.n0.iroh.link./", "use1"),
        ("https://usw1-1.relay.n0.iroh.link./", "usw1"),
        ("https://euc1-1.relay.n0.iroh.link./", "euc1"),
        ("https://aps1-1.relay.n0.iroh.link./", "aps1"),
        ("https://relay.embernetplay.link./", "use1"),
        ("https://relay-eu.embernetplay.link./", "euc1"),
    ];
    assert_eq!(RELAYS, wire);
    let n0 = iroh::defaults::prod::default_relay_map();
    for (index, code) in relay::REGIONS.iter().enumerate() {
        assert_eq!(RELAYS[index].1, *code);
        assert!(n0.contains(&pinned(index)), "{code}");
    }
    // Exactly these relays are active; of n0's, only Asia's is.
    let expected: std::collections::BTreeSet<RelayUrl> = [
        "https://relay.embernetplay.link./",
        "https://relay-eu.embernetplay.link./",
        "https://aps1-1.relay.n0.iroh.link./",
    ]
    .iter()
    .map(|url| url.parse().unwrap())
    .collect();
    assert_eq!(
        relay_map().urls::<Vec<_>>().into_iter().collect::<std::collections::BTreeSet<_>>(),
        expected
    );
    for url in [
        "https://use1-1.relay.n0.iroh.link./",
        "https://usw1-1.relay.n0.iroh.link./",
        "https://euc1-1.relay.n0.iroh.link./",
    ] {
        assert!(!expected.contains(&url.parse().unwrap()), "{url}");
    }
    for (url, code) in RELAYS {
        assert!(relay::is_region(code), "{code}");
        assert_eq!(relay_region(&url.parse().unwrap()), code);
    }
    // Every region has exactly one active relay, which maps back to it.
    let active = relay_map().urls::<Vec<_>>();
    assert_eq!(active.len(), ACTIVE.len());
    let mut seen = std::collections::BTreeSet::new();
    for relay in &active {
        let code = relay_region(relay);
        assert!(seen.insert(code), "{code}");
        assert_eq!(relay_for_region(code).as_ref(), Some(relay));
    }
    // Neither of n0's US relays is active: both are blocked by some US
    // providers' router filters. Ember's relay, coded use1, covers both
    // US regions, so no relay of this build reports usw1. Europe is on
    // Ember's relay as well, in place of n0's.
    assert_eq!(relay_for_region("use1"), Some(pinned(4)));
    assert_eq!(relay_for_region("euc1"), Some(pinned(5)));
    assert_eq!(relay_for_region("aps1"), Some(pinned(3)));
    for index in [0, 1, 2] {
        assert!(!active.contains(&pinned(index)));
    }
    for code in ["usw1", "other", "", "USE1", "euc2"] {
        assert!(relay_for_region(code).is_none(), "{code}");
    }
    for other in [
        "https://use1-1.relay.n0.iroh.link.evil/",
        "https://relay.example.com/",
        "https://use1-1.relay.n0.iroh.link./extra",
    ] {
        assert_eq!(relay_region(&other.parse().unwrap()), "other");
    }
}

#[test]
fn a_public_route_is_a_class_and_a_region_never_an_address() {
    let ip = |addr: &str| TransportAddr::Ip(addr.parse().unwrap());
    assert_eq!(public_route(Some(&ip("203.0.113.7:45760"))), "direct");
    assert_eq!(public_route(Some(&ip("[2001:db8::1]:57845"))), "direct");
    for (url, code) in RELAYS {
        let relay = TransportAddr::Relay(url.parse().unwrap());
        assert_eq!(public_route(Some(&relay)), format!("relay:{code}"));
    }
    for other in [
        "https://relay.example.com/",
        "https://use1-1.relay.n0.iroh.link.evil/",
    ] {
        let relay = TransportAddr::Relay(other.parse().unwrap());
        assert_eq!(public_route(Some(&relay)), "relay:other");
    }
    assert_eq!(public_route(None), "unavailable");
}

#[test]
fn discord_ticket_preserves_capability_and_host_checks_build_and_expiry() {
    for (url, _) in RELAYS {
        let relay: RelayUrl = url.parse().unwrap();
        let mut original = invite();
        original.relay = relay;
        original.build = "a".repeat(64);
        let token = original.encode_discord().unwrap();
        assert_eq!(token.len(), 127);
        let guest = Invite::parse_for_build(&token, 101, original.build()).unwrap();
        assert_eq!(guest.room, original.room);
        assert_eq!(guest.endpoint, original.endpoint);
        assert_eq!(guest.relay, original.relay);
        assert_eq!(guest.capability, original.capability);
        assert_eq!(guest.encode_discord().unwrap(), token);
        original.admit(&guest.proof(), 101).unwrap();
        let wrong = Invite::parse_for_build(&token, 101, "wrong-build").unwrap();
        assert!(original.admit(&wrong.proof(), 101).is_err());
        assert!(Invite::parse_for_build(&token, 3700, original.build()).is_err());
        assert!(original.admit(&guest.proof(), 3700).is_err());
        assert!(Invite::parse_for_build(&original.encode().unwrap(), 101, "wrong").is_err());
        assert!(Invite::parse_for_build(&legacy(&original), 101, original.build()).is_ok());
        for length in 0..token.len() {
            assert!(Invite::parse_for_build(&token[..length], 101, original.build()).is_err());
        }
        assert!(
            Invite::parse_for_build(&(token.clone() + "A"), 101, original.build()).is_err()
        );
        assert!(Invite::parse_for_build(&token, 101, "").is_err());
        let mut bytes = URL_SAFE_NO_PAD.decode(&token[5..]).unwrap();
        bytes[34] = 255;
        assert!(
            Invite::parse_for_build(
                &format!("emd1:{}", URL_SAFE_NO_PAD.encode(bytes)),
                101,
                original.build()
            )
            .is_err()
        );
    }
}

#[test]
fn clipboard_invites_accept_outer_whitespace_without_relaxing_validation() {
    let mut original = invite();
    original.build = "a".repeat(64);
    let mut tokens = vec![
        original.encode().unwrap(),
        legacy(&original),
        original.encode_discord().unwrap(),
    ];
    original.coordination_endpoint = Some(iroh::SecretKey::generate().public());
    original.authority_term = 1;
    original.authority_incarnation = 1;
    let recovery = original.encode().unwrap();
    assert_eq!(recovery.len(), 277);
    // Matches the laptop capture: a 277-character invitation plus CRLF.
    tokens.push(recovery);
    tokens.push(original.encode_discord().unwrap());
    for token in tokens {
        let canonical = Invite::parse_for_build(&token, 101, original.build()).unwrap();
        for pasted in [format!("{token}\r\n"), format!(" \t{token}\r\n ")] {
            let parsed = Invite::parse_for_build(&pasted, 101, original.build())
                .expect("clipboard whitespace must not reject a valid invitation");
            assert_eq!(parsed.encode().unwrap(), canonical.encode().unwrap());
            assert!(Invite::parse_for_build(&pasted, 3700, original.build()).is_err());
            if !token.starts_with("emd") {
                assert!(Invite::parse_for_build(&pasted, 101, "wrong-build").is_err());
            }
        }
        let split = token.find(':').unwrap() + 5;
        for inserted in [" ", "\r\n", "\\"] {
            let mut damaged = token.clone();
            damaged.insert_str(split, inserted);
            assert!(Invite::parse_for_build(&damaged, 101, original.build()).is_err());
        }
        let oversized = format!("{}{token}", " ".repeat(MAX_INVITE_LENGTH));
        assert!(Invite::parse_for_build(&oversized, 101, original.build()).is_err());
    }
    assert!(Invite::parse_for_build(" \t\r\n", 101, original.build()).is_err());
}

#[test]
fn compact_invites_preserve_every_field_and_accept_legacy() {
    for (url, _) in RELAYS {
        let relay: RelayUrl = url.parse().unwrap();
        let mut original = invite();
        original.relay = relay;
        original.build = "a".repeat(64); // Production sidecar SHA-256 string.
        let compact = original.encode().unwrap();
        let old = legacy(&original);
        assert_eq!(compact.len(), 213);
        assert!(compact.len() * 2 < old.len());
        for token in [compact, old] {
            let decoded = Invite::parse(&token, 101).unwrap();
            assert_eq!(decoded.endpoint, original.endpoint);
            assert_eq!(decoded.relay, original.relay);
            assert_eq!(decoded.expires, original.expires);
            original.admit(&decoded.proof(), 101).unwrap();
        }
    }
}

#[test]
fn compact_invites_reject_truncation_unknown_relay_and_bad_fields() {
    let original = invite();
    let token = original.encode().unwrap();
    let bytes = URL_SAFE_NO_PAD
        .decode(token.strip_prefix(PREFIX).unwrap())
        .unwrap();
    let encoded = |bytes: &[u8]| format!("{PREFIX}{}", URL_SAFE_NO_PAD.encode(bytes));
    for length in 0..92 {
        assert!(Invite::parse(&encoded(&bytes[..length]), 101).is_err());
    }
    for (offset, value) in [(0, 255), (34, 255), (91, 255)] {
        let mut bad = bytes.clone();
        bad[offset] = value;
        assert!(Invite::parse(&encoded(&bad), 101).is_err());
    }
    for range in [35..51, 51..83] {
        let mut bad = bytes.clone();
        bad[range].fill(0);
        assert!(Invite::parse(&encoded(&bad), 101).is_err());
    }
    assert!(Invite::parse(&encoded(&vec![0; 220]), 101).is_err());
    assert!(Invite::parse(&token, 3700).is_err());
    assert!(Invite::parse(&token, 0).is_ok());
    let mut maximum = original.clone();
    maximum.build = "x".repeat(128);
    assert_eq!(
        Invite::parse(&maximum.encode().unwrap(), 101)
            .unwrap()
            .build(),
        maximum.build()
    );
}

#[test]
fn invite_roundtrips_without_private_addresses_or_secret_key() {
    let original = invite();
    let token = original.encode().unwrap();
    let decoded = Invite::parse(&token, 101).unwrap();
    assert_eq!(decoded.endpoint(), original.endpoint());
    assert!(decoded.address().ip_addrs().next().is_none());
    assert!(original.admit(&decoded.proof(), 101).is_ok());
    assert!(!format!("{decoded:?}").contains(&token));
}

#[test]
fn host_rejects_modified_capability_build_room_and_expired_policy() {
    let original = invite();
    let mut proof = original.proof();
    proof.capability[0] ^= 1;
    assert!(original.admit(&proof, 101).is_err());
    proof = original.proof();
    proof.build.push('x');
    assert!(original.admit(&proof, 101).is_err());
    proof = original.proof();
    proof.room[0] ^= 1;
    assert!(original.admit(&proof, 101).is_err());
    assert!(original.admit(&original.proof(), 3700).is_err());
    let mut extended = original.clone();
    extended.expires = 4000;
    assert!(original.admit(&extended.proof(), 3700).is_err());
}

#[test]
fn a_proof_names_the_helper_coordination_wire_beside_the_sidecar_build() {
    let original = invite();
    let proof = original.proof();
    assert_eq!(proof.build, "test-sidecar-hash+coordination2");
    assert_eq!(original.build(), "test-sidecar-hash");
    assert!(original.admit(&proof, 101).is_ok());
    // A helper of the same Sidecar.dll that names only the Sidecar hash,
    // as wire 1 helpers do, or another wire, is refused.
    for build in [
        "test-sidecar-hash",
        "test-sidecar-hash+coordination1",
        "test-sidecar-hash+coordination3",
    ] {
        let mut other = original.proof();
        other.build = build.into();
        assert!(original.admit(&other, 101).is_err(), "{build}");
        assert!(original.admit_member(&other).is_err(), "{build}");
    }
    // A wire 1 host compares a proof against its bare Sidecar hash, so
    // it refuses this helper's proof the same way.
    assert_ne!(proof.build, original.build());
    // The invitation text is unchanged: it still carries the bare build.
    let token = original.encode().unwrap();
    let parsed = Invite::parse_for_build(&token, 101, "test-sidecar-hash").unwrap();
    assert_eq!(parsed.build(), "test-sidecar-hash");
}

#[test]
fn untrusted_invites_reject_arbitrary_routing_and_lengths() {
    for relay in [
        "http://localhost/",
        "https://example.com/",
        "https://use1-1.relay.n0.iroh.link.evil/",
        "https://user@use1-1.relay.n0.iroh.link./",
        "https://use1-1.relay.n0.iroh.link./extra",
    ] {
        let mut unsafe_invite = invite();
        unsafe_invite.relay = relay.parse().unwrap();
        assert!(unsafe_invite.encode().is_err());
        assert!(Invite::parse(&legacy(&unsafe_invite), 101).is_err());
    }
    assert!(Invite::parse(&"x".repeat(MAX_INVITE_LENGTH + 1), 101).is_err());
    assert!(Invite::parse("sf4e1:invalid!", 101).is_err());
    let mut wrong = invite();
    wrong.version = 2;
    assert!(Invite::parse(&wrong.encode().unwrap(), 101).is_err());
}

/// One fixed invitation, private and with a recovery route, in every
/// text form this build writes or reads. The texts are frozen here: a
/// change to any of them is a change to what other builds exchange.
fn golden(recovery: bool) -> Invite {
    Invite {
        version: VERSION,
        endpoint: iroh::SecretKey::from_bytes(&[7; 32]).public(),
        relay: pinned(1),
        room: [0x11; 16],
        capability: [0x22; 32],
        expires: 1_000_000,
        build: "golden-build".into(),
        coordination_endpoint: recovery.then(|| iroh::SecretKey::from_bytes(&[9; 32]).public()),
        authority_term: if recovery { 3 } else { 0 },
        authority_incarnation: if recovery { 5 } else { 0 },
    }
}


const GOLDEN_SF4E1: &str = concat!(
    "sf4e1:eyJ2ZXJzaW9uIjoxLCJlbmRwb2ludCI6ImVhNGE2YzYzZTI5YzUyMGFiZWY1NTA3YjEzMmVjNW",
    "Y5OTU0Nzc2YWViZWJlN2I5MjQyMWVlYTY5MTQ0NmQyMmMiLCJyZWxheSI6Imh0dHBzOi8vdXN3MS0xLn",
    "JlbGF5Lm4wLmlyb2gubGluay4vIiwicm9vbSI6WzE3LDE3LDE3LDE3LDE3LDE3LDE3LDE3LDE3LDE3LD",
    "E3LDE3LDE3LDE3LDE3LDE3XSwiY2FwYWJpbGl0eSI6WzM0LDM0LDM0LDM0LDM0LDM0LDM0LDM0LDM0LD",
    "M0LDM0LDM0LDM0LDM0LDM0LDM0LDM0LDM0LDM0LDM0LDM0LDM0LDM0LDM0LDM0LDM0LDM0LDM0LDM0LD",
    "M0LDM0LDM0XSwiZXhwaXJlcyI6MTAwMDAwMCwiYnVpbGQiOiJnb2xkZW4tYnVpbGQiLCJjb29yZGluYX",
    "Rpb25fZW5kcG9pbnQiOm51bGwsImF1dGhvcml0eV90ZXJtIjowLCJhdXRob3JpdHlfaW5jYXJuYXRpb2",
    "4iOjB9",
);
const GOLDEN_SF4E2: &str = concat!(
    "sf4e2:AQDqSmxj4pxSCr71UHsTLsX5lUd2rr6-e5JCHuppFEbSLAERERERERERERERERERERERIiIiIi",
    "IiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiJAQg8AAAAAAGdvbGRlbi1idWlsZA",
);
const GOLDEN_SF4E3: &str = concat!(
    "sf4e3:AQDqSmxj4pxSCr71UHsTLsX5lUd2rr6-e5JCHuppFEbSLAERERERERERERERERERERERIiIiIi",
    "IiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiJAQg8AAAAAAP0XJDhaoMdbZPt4zWAvodmR_ev3axPFjt",
    "cC6sg16fYYAwAAAAAAAAAFAAAAAAAAAGdvbGRlbi1idWlsZA",
);
const GOLDEN_EMD1: &str = concat!(
    "emd1:AQDqSmxj4pxSCr71UHsTLsX5lUd2rr6-e5JCHuppFEbSLAERERERERERERERERERERERIiIiIiI",
    "iIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiJAQg8AAAAAAA",
);
// emd2 carries the same header bytes: `encode_discord` keeps only them.
const GOLDEN_EMD2: &str = concat!(
    "emd2:AQDqSmxj4pxSCr71UHsTLsX5lUd2rr6-e5JCHuppFEbSLAERERERERERERERERERERERIiIiIiI",
    "iIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiJAQg8AAAAAAA",
);

#[test]
fn every_invitation_form_reads_and_writes_the_frozen_texts() {
    let (plain, recovery) = (golden(false), golden(true));
    let written = [
        legacy(&plain),
        plain.encode().unwrap(),
        recovery.encode().unwrap(),
        plain.encode_discord().unwrap(),
        recovery.encode_discord().unwrap(),
    ];
    assert_eq!(
        written,
        [GOLDEN_SF4E1, GOLDEN_SF4E2, GOLDEN_SF4E3, GOLDEN_EMD1, GOLDEN_EMD2]
    );
    let now = 999_000;
    let same = |a: &Invite, b: &Invite| {
        a.version == b.version
            && a.endpoint == b.endpoint
            && a.relay == b.relay
            && a.room == b.room
            && a.capability == b.capability
            && a.expires == b.expires
            && a.build == b.build
            && a.coordination_endpoint == b.coordination_endpoint
            && a.authority_term == b.authority_term
            && a.authority_incarnation == b.authority_incarnation
    };
    // Accepted: each text reads back as its invitation. A Discord text
    // carries the header alone, so emd2 reads as the private invitation.
    for (text, expected) in [
        (GOLDEN_SF4E1, &plain),
        (GOLDEN_SF4E2, &plain),
        (GOLDEN_SF4E3, &recovery),
        (GOLDEN_EMD1, &plain),
        (GOLDEN_EMD2, &plain),
    ] {
        let read = Invite::parse_for_build(text, now, "golden-build").unwrap();
        assert!(same(&read, expected), "{text}");
        assert!(same(&Invite::parse_for_build(&format!(" {text}
"), now, "golden-build").unwrap(), expected));
        if text.starts_with("sf4e") {
            assert!(same(&Invite::parse(text, now).unwrap(), expected), "{text}");
        } else {
            // `parse` reads no Discord text; it has no build to add.
            assert!(Invite::parse(text, now).is_err(), "{text}");
            // A Discord text takes whatever build the reader has.
            let other = Invite::parse_for_build(text, now, "other-build").unwrap();
            assert_eq!(other.build(), "other-build");
        }
    }
    // Refused, and why.
    for text in [GOLDEN_SF4E1, GOLDEN_SF4E2, GOLDEN_SF4E3] {
        assert!(Invite::parse_for_build(text, now, "other-build").is_err());
        assert_eq!(Invite::rejection_reason(text, now, "other-build"), "other_build");
        assert_eq!(Invite::rejection_reason(text, now, "golden-build-longer"), "malformed");
    }
    for text in written.iter() {
        assert!(Invite::parse_for_build(text, 1_000_000, "golden-build").is_err());
        assert_eq!(Invite::rejection_reason(text, 1_000_000, "golden-build"), "expired");
        assert!(Invite::parse(text, 1_000_000).is_err());
        for cut in [1, 4, 8] {
            let short = &text[..text.len() - cut];
            assert!(Invite::parse_for_build(short, now, "golden-build").is_err(), "{short}");
        }
        let body = &text[text.find(':').unwrap() + 1..];
        for prefix in ["sf4e0:", "sf4e4:", "emd0:", "emd3:", "SF4E2:", ""] {
            let other = format!("{prefix}{body}");
            assert!(Invite::parse_for_build(&other, now, "golden-build").is_err(), "{other}");
            assert_eq!(Invite::rejection_reason(&other, now, "golden-build"), "malformed");
        }
    }
    for text in [GOLDEN_EMD1, GOLDEN_EMD2] {
        assert!(Invite::parse_for_build(text, now, "").is_err());
        assert!(Invite::parse_for_build(text, now, &"b".repeat(129)).is_err());
    }
    // A text that would be accepted has no reason of its own.
    for text in written.iter() {
        assert_eq!(Invite::rejection_reason(text, now, "golden-build"), "malformed");
    }
    // Each cut of a compact text, at a whole Base64 group, names why.
    for (text, reason) in [
        (&GOLDEN_SF4E2[..GOLDEN_SF4E2.len() - 4], "malformed"),
        (&GOLDEN_SF4E3[..GOLDEN_SF4E3.len() - 4], "malformed"),
    ] {
        assert!(Invite::parse_for_build(text, now, "golden-build").is_err());
        assert_eq!(Invite::rejection_reason(text, now, "golden-build"), reason);
    }
    // Another invitation version.
    let mut old = plain.clone();
    old.version = VERSION - 1;
    for text in [legacy(&old), old.encode().unwrap()] {
        assert!(Invite::parse_for_build(&text, now, "golden-build").is_err());
        assert_eq!(Invite::rejection_reason(&text, now, "golden-build"), "old_version");
    }
}
