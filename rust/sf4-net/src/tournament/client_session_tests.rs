//! A session is proven only by the identity it is for, however far its
//! opening has got when the identity is replaced.
use super::*;
use crate::tournament::{
    builtin::Builtin,
    test_support::{hold_first_request, json_reply, shared_pinning},
};

/// Replaces the player's identity, which is `old`, with another one.
fn replace_identity(shared: &Shared, old: &EmberId) {
    use crate::identity::Protection;
    let mut identity = shared.identity.lock().unwrap();
    identity.reset(Some(old)).unwrap();
    identity
        .enable(Protection::Passphrase("another long passphrase"))
        .unwrap();
}

/// A loopback listener and its origin.
async fn loopback() -> (tokio::net::TcpListener, String) {
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let origin = format!("http://{}", listener.local_addr().unwrap());
    (listener, origin)
}

/// Opens a session to `bridge` for the player's identity while `listener`
/// holds the first request, which must start with `held`, and answers
/// everything with `reply`. The identity is replaced during the hold: the
/// opening is refused as `identity_changed`, nothing more is sent, and no
/// session is cached.
async fn refused_while_held(
    listener: tokio::net::TcpListener,
    builtins: &[Builtin],
    bridge: Approved,
    held: &str,
    reply: String,
) {
    use crate::identity::Protection;
    let dir = std::env::temp_dir().join(ember_protocol::encoding::prefixed_id(
        "sf4-net-session",
        rand::random::<[u8; 16]>(),
    ));
    let shared = shared_pinning(&dir, builtins);
    shared
        .identity
        .lock()
        .unwrap()
        .enable(Protection::Passphrase("a long passphrase"))
        .unwrap();
    let first = current_id(&shared).unwrap();

    let (mut asked, release) = hold_first_request(listener, reply);
    let opening = {
        let (shared, bridge, first) = (shared.clone(), bridge.clone(), first.clone());
        tokio::spawn(async move { session(&shared, &bridge, Some(&first)).await.map(|_| ()) })
    };
    let line = asked.recv().await.unwrap();
    assert!(line.starts_with(held), "{line}");
    replace_identity(&shared, &first);
    assert_ne!(current_id(&shared).unwrap(), first);
    release.send(()).unwrap();

    let refused = opening.await.unwrap();
    assert!(
        matches!(&refused, Err(Failure(code)) if code == "identity_changed"),
        "{refused:?}"
    );
    assert!(
        asked.try_recv().is_err(),
        "a request was sent after the hold"
    );
    assert!(shared.client.cached(&bridge, &first).is_none());
    let _ = std::fs::remove_dir_all(&dir);
}

// An identity replaced while a session is being opened never proves it:
// the session is refused before anything is signed, and none is cached.
#[tokio::test]
async fn a_session_is_proven_only_by_the_identity_it_is_for() {
    // A bridge that holds the challenge request until it is let go.
    let (listener, origin) = loopback().await;
    let bridge = Approved {
        bridge_id: ember_protocol::encoding::prefixed_id("brg", [1; 16]),
        origin,
        display_name: "Bridge".into(),
        approved_at: 0,
        keys: Vec::new(),
    };
    refused_while_held(
        listener,
        &[],
        bridge,
        "POST /v1/auth/challenges",
        json_reply("201 Created", "{}"),
    )
    .await;
}

// The same while a built-in's origin is being checked, which comes before
// the challenge is asked for: the identity is found replaced when the
// proof begins, and no challenge is requested.
#[tokio::test]
async fn an_identity_replaced_during_the_pin_check_proves_nothing() {
    let (listener, origin) = loopback().await;
    let x = ember_protocol::SigningIdentity::from_seed(&Zeroizing::new([9; 32]))
        .unwrap()
        .public_key()
        .to_b64u();
    let pin = Builtin {
        bridge_id: ember_protocol::encoding::prefixed_id("brg", [9; 16]),
        origin: origin.clone(),
        display_name: "Pinned".into(),
        keys: vec![("k9".into(), x)],
    };
    // The pinned origin describes the pinned bridge, once it is let go.
    let profile = BridgeProfile {
        bridge_id: pin.bridge_id.clone(),
        origin: origin.clone(),
        display_name: pin.display_name.clone(),
        api_versions: vec!["v1".into()],
        capabilities_url: format!("{origin}/v1/capabilities"),
        signing_keys_url: format!("{origin}/v1/signing-keys"),
    };
    refused_while_held(
        listener,
        std::slice::from_ref(&pin),
        pin.entry(),
        &format!("GET {WELL_KNOWN_PATH}"),
        json_reply("200 OK", &serde_json::to_string(&profile).unwrap()),
    )
    .await;
}
