use super::*;
use crate::tournament::{
    Request,
    builtin::Builtin,
    test_support::{capabilities_json, capabilities_server, shared_pinning},
};

fn capabilities(features: &[&str]) -> Capabilities {
    serde_json::from_value(capabilities_json(features)).unwrap()
}

#[test]
fn the_game_says_whether_its_fighter_is_ready_with_each_claim() {
    let claim = |extra: Value| {
        let mut value = json!({ "op": "match_claim", "bridge_id": "brg_x", "match_id": "emt_x", "build": "b1" });
        if let Value::Object(extra) = extra {
            value.as_object_mut().unwrap().extend(extra);
        }
        serde_json::from_value::<Request>(value)
    };
    let ready = |request: Request| match request {
        Request::MatchClaim { ready, .. } => ready,
        _ => panic!("not a claim"),
    };
    assert!(ready(claim(json!({ "ready": true })).unwrap()));
    assert!(!ready(claim(json!({ "ready": false })).unwrap()));
    assert!(!ready(claim(json!({})).unwrap()));
    assert!(claim(json!({ "ready": "yes" })).is_err());
}

fn shared_in(dir: &std::path::Path) -> Arc<Shared> {
    shared_pinning(dir, &[])
}

// A Ready claim is not sent without its `ready` when the capabilities
// read fails: the claim fails and the game retries it. A bridge that does
// not read the field is asked again on the next Ready claim, so one
// upgraded while the match is played is heard; one that reads it is
// remembered, until a claim to it fails, since the bridge may have been
// put back to one that refuses it.
#[tokio::test]
async fn a_ready_claim_asks_until_the_bridge_is_known_to_read_ready() {
    let dir = std::env::temp_dir().join(prefixed_id("sf4-net-ready", random().unwrap()));
    let shared = shared_in(&dir);
    let mode = Arc::new(std::sync::atomic::AtomicU8::new(0));
    let (origin, asked) = capabilities_server(mode.clone()).await;
    let bridge = Approved {
        bridge_id: "brg_x".into(),
        origin,
        display_name: "Bridge".into(),
        approved_at: 0,
        keys: Vec::new(),
    };
    let reads = || async {
        features(&shared, &bridge, Need::Ready)
            .await
            .map(|offered| offered.ready)
    };
    let count = || asked.load(std::sync::atomic::Ordering::SeqCst);
    let set = |value| mode.store(value, std::sync::atomic::Ordering::SeqCst);

    assert!(reads().await.is_err());
    set(1);
    assert!(!reads().await.unwrap());
    assert!(!reads().await.unwrap());
    assert_eq!(count(), 3);
    set(2);
    assert!(reads().await.unwrap());
    set(0);
    assert!(reads().await.unwrap());
    assert_eq!(count(), 4);

    // The claims themselves: a Ready claim to a bridge known to read
    // `ready` carries it without asking again; once a claim to it fails,
    // the next asks again and, the bridge put back, sends none.
    let claim = Claim {
        endpoint_id: "a".repeat(64),
        helper_instance_id: "ins_test".into(),
        build_id: "b1".into(),
        ready: None,
        watch: None,
        held_binding: None,
    };
    let sent = std::sync::Mutex::new(Vec::new());
    let send = |fails: bool| {
        let sent = &sent;
        move |command: Value| async move {
            sent.lock().unwrap().push(command.get("ready").cloned());
            if fails {
                Err(Failure::Bridge(Refusal::Status(503)))
            } else {
                Ok(json!({}))
            }
        }
    };
    let claimed = |ready: bool, fails: bool| {
        send_claim(&shared, &bridge, claim.clone(), ready, send(fails))
    };
    assert!(claimed(true, false).await.is_ok());
    assert!(claimed(true, true).await.is_err());
    assert_eq!(count(), 4);
    set(1);
    assert!(claimed(true, false).await.is_ok());
    assert_eq!(count(), 5);
    // One that is not Ready asks nothing of a bridge it knows.
    set(2);
    assert!(claimed(true, false).await.is_ok());
    assert!(claimed(false, false).await.is_ok());
    assert_eq!(count(), 6);
    // Its failure forgets, so the next claim asks, and one asking fails
    // when the read does rather than going out without its `ready`.
    assert!(claimed(false, true).await.is_err());
    set(0);
    assert!(claimed(true, false).await.is_err());
    assert_eq!(count(), 7);
    set(2);
    assert!(claimed(true, false).await.is_ok());
    assert_eq!(count(), 8);
    assert_eq!(
        *sent.lock().unwrap(),
        vec![
            Some(json!(true)),
            Some(json!(true)),
            None,
            Some(json!(true)),
            None,
            None,
            Some(json!(true)),
        ]
    );
    let _ = std::fs::remove_dir_all(dir);
}

// Bindings, permits and room tickets all take their key here. A built-in
// that does not name the key is refused at once; an approved bridge asks
// its origin, which here refuses the connection.
#[tokio::test]
async fn a_builtins_unknown_key_stays_untrusted() {
    let dir = std::env::temp_dir().join(prefixed_id("sf4-net-pinned", random().unwrap()));
    let closed = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let origin = format!("http://{}", closed.local_addr().unwrap());
    drop(closed);
    let x = ember_protocol::SigningIdentity::from_seed(&zeroize::Zeroizing::new([9; 32]))
        .unwrap()
        .public_key()
        .to_b64u();
    let pin = Builtin {
        bridge_id: prefixed_id("brg", [9; 16]),
        origin,
        display_name: "Pinned".into(),
        keys: vec![("k9".into(), x)],
    };
    let shared = shared_pinning(&dir, std::slice::from_ref(&pin));
    let mut bridge = pin.entry();
    assert!(bridge_key(&shared, &bridge, "k9").await.is_ok());
    let refused = bridge_key(&shared, &bridge, "k5").await.unwrap_err();
    assert_eq!(refused.as_str(), "untrusted_signature");
    // The same entry under an ID this build does not pin.
    bridge.bridge_id = prefixed_id("brg", [8; 16]);
    let fetched = bridge_key(&shared, &bridge, "k5").await.unwrap_err();
    assert_eq!(fetched.as_str(), "bridge_unreachable");
    let _ = std::fs::remove_dir_all(dir);
}

// The game names each watcher in the room from the binding it applies.
#[test]
fn the_game_sees_the_watchers_its_binding_names() {
    let id = |byte: u8| {
        ember_protocol::SigningIdentity::from_seed(&zeroize::Zeroizing::new([byte; 32]))
            .unwrap()
            .ember_id()
            .clone()
    };
    let fighter = |byte: u8| ember_protocol::play::BoundFighter {
        ember_id: id(byte),
        endpoint_id: format!("{byte:02x}").repeat(32),
    };
    let mut binding = Binding {
        version: ember_protocol::play::VERSION,
        bridge_id: prefixed_id("brg", [1; 16]),
        match_id: prefixed_id("emt", [2; 16]),
        assignment_generation: Counter(1),
        binding_revision: Counter(3),
        room_id: "0123456789abcdef0123456789abcdef".into(),
        build_id: "b1".into(),
        games_to_win: 2,
        rules_digest: String::new(),
        roster_digest: String::new(),
        fighters: [fighter(1), fighter(2)],
        issued_at: 100,
        starting_wins: [0, 0],
        watchers: Vec::new(),
    };
    assert_eq!(binding_view(&binding, Some(0))["watchers"], json!([]));
    binding.watchers = vec![ember_protocol::play::BoundWatcher {
        ember_id: id(3),
        endpoint_id: "03".repeat(32),
    }];
    let view = binding_view(&binding, Some(1));
    assert_eq!(
        view["watchers"],
        json!([{ "ember_id": id(3).to_string(), "endpoint_id": "03".repeat(32) }])
    );
    assert_eq!(view["local_slot"], json!(1));
}

#[test]
fn a_bridge_offers_what_its_capabilities_list() {
    let ready = Features::of(&capabilities(&["matches.play", CLAIM_READY_FEATURE]));
    assert_eq!(
        ready,
        Features {
            ready: true,
            ..Features::default()
        }
    );
    let watch = Features::of(&capabilities(&["matches.play", WATCH_FEATURE]));
    assert_eq!(
        watch,
        Features {
            watch: true,
            ..Features::default()
        }
    );
    assert!(Features::of(&capabilities(&[RANKING_FEATURE])).ranking);
    let social = Features::of(&capabilities(&[POINTS_FEATURE, FRIENDS_FEATURE]));
    assert!(social.levels && social.friends && !social.ranking);
    assert!(Features::of(&capabilities(&[POINTS_FEATURE])).levels);
    assert!(!Features::of(&capabilities(&[POINTS_FEATURE])).friends);
    assert!(Features::of(&capabilities(&[CLAIM_UNCHANGED_FEATURE])).unchanged);
    assert_eq!(
        Features::of(&capabilities(&["matches.play"])),
        Features::default()
    );
}

// A claim names the binding it holds only to a bridge that lists the
// feature, since an older one refuses the field, and says whether it did.
// A claim that fails makes the next one read the capabilities again, so
// a bridge put back to one without the feature is heard.
#[tokio::test]
async fn a_claim_names_its_binding_only_to_a_bridge_that_answers_it() {
    let dir = std::env::temp_dir().join(prefixed_id("sf4-net-held", random().unwrap()));
    let shared = shared_in(&dir);
    let mode = Arc::new(std::sync::atomic::AtomicU8::new(3));
    let (origin, _) = capabilities_server(mode.clone()).await;
    let bridge = Approved {
        bridge_id: "brg_x".into(),
        origin,
        display_name: "Bridge".into(),
        approved_at: 0,
        keys: Vec::new(),
    };
    let digest = ember_protocol::encoding::b64u(&[7; 32]);
    let claim = Claim {
        endpoint_id: "a".repeat(64),
        helper_instance_id: "ins_test".into(),
        build_id: "b1".into(),
        ready: None,
        watch: None,
        held_binding: Some(digest.clone()),
    };
    let sent = std::sync::Mutex::new(Vec::new());
    let send = |fails: bool| {
        let sent = &sent;
        move |command: Value| async move {
            sent.lock()
                .unwrap()
                .push(command.get("held_binding").cloned());
            if fails {
                Err(Failure::Bridge(Refusal::Status(503)))
            } else {
                Ok(json!({}))
            }
        }
    };
    let claimed =
        |claim: Claim, fails: bool| send_claim(&shared, &bridge, claim, false, send(fails));
    assert!(claimed(claim.clone(), false).await.unwrap().1);
    let unnamed = Claim {
        held_binding: None,
        ..claim.clone()
    };
    assert!(!claimed(unnamed, false).await.unwrap().1);
    assert!(claimed(claim.clone(), true).await.is_err());
    mode.store(2, std::sync::atomic::Ordering::SeqCst);
    assert!(!claimed(claim, false).await.unwrap().1);
    assert_eq!(
        *sent.lock().unwrap(),
        vec![Some(json!(digest)), None, Some(json!(digest)), None]
    );
    let _ = std::fs::remove_dir_all(dir);
}

// The binding a claim named comes back as `Unchanged`, and the game reads
// the same answer as when the bridge sent the binding. A bridge that
// answers so to a claim that named none is refused. The very binding
// this run verified is not verified again while the bridge lists its
// key; one the bridge's keys no longer cover is neither named nor taken
// on trust.
#[tokio::test]
async fn an_unchanged_answer_is_the_binding_the_claim_named() {
    let dir = std::env::temp_dir().join(prefixed_id("sf4-net-unchanged", random().unwrap()));
    let shared = shared_in(&dir);
    shared
        .identity
        .lock()
        .unwrap()
        .enable(crate::identity::Protection::Passphrase("a test passphrase"))
        .unwrap();
    let me = client::current_id(&shared).unwrap();
    let closed = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let origin = format!("http://{}", closed.local_addr().unwrap());
    drop(closed);
    let signer =
        ember_protocol::SigningIdentity::from_seed(&zeroize::Zeroizing::new([5; 32])).unwrap();
    let bridge = Approved {
        bridge_id: prefixed_id("brg", [1; 16]),
        origin,
        display_name: "Bridge".into(),
        approved_at: 0,
        keys: vec![ember_protocol::api::SigningKey {
            kty: "OKP".into(),
            crv: "Ed25519".into(),
            alg: "EdDSA".into(),
            usage: "sig".into(),
            kid: "k5".into(),
            x: signer.public_key().to_b64u(),
        }],
    };
    let other = ember_protocol::SigningIdentity::from_seed(&zeroize::Zeroizing::new([6; 32]))
        .unwrap()
        .ember_id()
        .clone();
    let match_id = prefixed_id("emt", [2; 16]);
    let room_id = "0123456789abcdef0123456789abcdef".to_owned();
    let binding = Binding {
        version: ember_protocol::play::VERSION,
        bridge_id: bridge.bridge_id.clone(),
        match_id: match_id.clone(),
        assignment_generation: Counter(1),
        binding_revision: Counter(1),
        room_id: room_id.clone(),
        build_id: "b1".into(),
        games_to_win: 2,
        rules_digest: ember_protocol::encoding::b64u(&[3; 32]),
        roster_digest: ember_protocol::play::roster_digest(&me, &other).unwrap(),
        fighters: [
            ember_protocol::play::BoundFighter {
                ember_id: me.clone(),
                endpoint_id: shared.endpoint_id.clone(),
            },
            ember_protocol::play::BoundFighter {
                ember_id: other,
                endpoint_id: "b".repeat(64),
            },
        ],
        issued_at: 100,
        starting_wins: [0, 0],
        watchers: Vec::new(),
    };
    let signed = binding.sign(&signer, "k5").unwrap();
    let room = || ClaimAnswer::Room {
        room_id: room_id.clone(),
        invitation: "sf4e3:x".into(),
        binding: Some(Box::new(signed.clone())),
    };
    let unchanged = || ClaimAnswer::Unchanged {
        room_id: room_id.clone(),
        invitation: "sf4e3:x".into(),
    };

    // Nothing held yet: nothing to name, and an unchanged answer to a
    // claim that named nothing is refused.
    assert!(held_binding(&shared, &bridge, &match_id).unwrap().is_none());
    let refused = present(&shared, &bridge, &match_id, unchanged(), None).await;
    assert_eq!(refused.unwrap_err().as_str(), "bridge_invalid_response");

    let sent = present(&shared, &bridge, &match_id, room(), None)
        .await
        .unwrap()
        .unwrap();
    assert_eq!(sent["binding"]["local_slot"], json!(0));
    let held = held_binding(&shared, &bridge, &match_id).unwrap().unwrap();
    assert_eq!(held.digest, binding.digest().unwrap());
    let kept = present(&shared, &bridge, &match_id, unchanged(), Some(held.clone()))
        .await
        .unwrap()
        .unwrap();
    assert_eq!(kept, sent);

    // The held bytes are taken without a second signature check: here
    // they carry a signature that would fail one. Any other bytes are
    // checked and refused.
    let forged = SignedBinding {
        signature: binding
            .sign(
                &ember_protocol::SigningIdentity::from_seed(&zeroize::Zeroizing::new([9; 32]))
                    .unwrap(),
                "k5",
            )
            .unwrap()
            .signature,
        ..signed.clone()
    };
    let check = |signed: &SignedBinding| {
        let answer = ClaimAnswer::Room {
            room_id: room_id.clone(),
            invitation: "sf4e3:x".into(),
            binding: Some(Box::new(signed.clone())),
        };
        present(&shared, &bridge, &match_id, answer, None)
    };
    assert_eq!(
        check(&forged).await.unwrap_err().as_str(),
        "untrusted_signature"
    );
    play_state(&shared)
        .unwrap()
        .bindings
        .get_mut(&match_id)
        .unwrap()
        .signed = forged.clone();
    assert!(check(&forged).await.is_ok());

    // A bridge whose keys no longer cover the held binding: it is not
    // named, and the same bytes are checked again, which here means
    // asking the bridge for its keys.
    let rotated = Approved {
        keys: Vec::new(),
        ..bridge.clone()
    };
    assert!(
        held_binding(&shared, &rotated, &match_id)
            .unwrap()
            .is_none()
    );
    let answer = ClaimAnswer::Room {
        room_id: room_id.clone(),
        invitation: "sf4e3:x".into(),
        binding: Some(Box::new(forged)),
    };
    let refetched = present(&shared, &rotated, &match_id, answer, None).await;
    assert_eq!(refetched.unwrap_err().as_str(), "bridge_unreachable");

    // Leaving the match drops it.
    forget(&shared, &match_id, 0).unwrap();
    assert!(held_binding(&shared, &bridge, &match_id).unwrap().is_none());
    let _ = std::fs::remove_dir_all(dir);
}

// A report is dropped for the refusals that sending it again cannot
// change, whichever side words them, and a pass leaves a bridge that
// cannot be reached alone for the rest of the pass.
#[test]
fn reports_settle_on_refusals_of_either_side_and_not_on_trouble_reaching_a_bridge() {
    let bridge = |code: ErrorCode| Failure::Bridge(Refusal::Error(code));
    for code in [
        ErrorCode::StaleRevision,
        ErrorCode::IdempotencyConflict,
        ErrorCode::InvalidSignature,
        ErrorCode::InvalidRequest,
        ErrorCode::NotFound,
        ErrorCode::Forbidden,
    ] {
        assert!(settled(&bridge(code)), "{code:?}");
    }
    for code in [ErrorCode::RateLimited, ErrorCode::Unauthenticated] {
        assert!(!settled(&bridge(code)), "{code:?}");
    }
    // A status alone, or a word of the bridge that spells a helper code,
    // settles nothing.
    for refusal in [
        Refusal::Status(502),
        Refusal::Reason("internal".into()),
        Refusal::Reason("bridge_not_approved".into()),
    ] {
        assert!(!settled(&Failure::Bridge(refusal.clone())), "{refusal:?}");
    }
    for code in [
        Code::InvalidRequest,
        Code::NotFound,
        Code::BridgeNotApproved,
    ] {
        assert!(settled(&Failure::new(code)), "{code:?}");
    }
    for code in [Code::BridgeUnreachable, Code::Internal, Code::IdentityBusy] {
        assert!(!settled(&Failure::new(code)), "{code:?}");
    }
    assert!(!settled(&Failure::Identity(crate::identity::Failure::Io)));

    for failure in [
        Failure::new(Code::BridgeUnreachable),
        Failure::new(Code::BridgeResponseTooLarge),
        Failure::Bridge(Refusal::Status(500)),
        Failure::Bridge(Refusal::Status(503)),
    ] {
        assert!(unreachable_bridge(&failure), "{failure:?}");
    }
    for failure in [
        Failure::new(Code::Internal),
        Failure::new(Code::IdentityBusy),
        Failure::Bridge(Refusal::Status(404)),
        bridge(ErrorCode::ServiceUnavailable),
    ] {
        assert!(!unreachable_bridge(&failure), "{failure:?}");
    }
}
