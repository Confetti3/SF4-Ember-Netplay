use super::*;

fn profile(id_byte: u8, origin: &str) -> BridgeProfile {
    BridgeProfile {
        bridge_id: ember_protocol::encoding::prefixed_id("brg", [id_byte; 16]),
        origin: origin.into(),
        display_name: "Bridge".into(),
        api_versions: vec!["v1".into()],
        capabilities_url: String::new(),
        signing_keys_url: String::new(),
    }
}

fn temp_dir(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("sf4-bridges-{name}-{}", std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    dir
}

#[test]
fn approvals_persist_and_replace_by_origin() {
    let dir = temp_dir("approvals");
    let mut store = Store::open(dir.clone(), &[]);
    assert!(store.list().is_empty());
    store
        .approve(&profile(1, "https://bridge.one.example"), Vec::new())
        .unwrap();
    store
        .approve(&profile(2, "http://127.0.0.1:8787"), Vec::new())
        .unwrap();
    assert!(
        store
            .approve(&profile(3, "http://10.0.0.1:8787"), Vec::new())
            .is_err()
    );
    assert!(
        store
            .approve(&profile(4, "https://bridge.one.example/path"), Vec::new())
            .is_err()
    );
    let reopened = Store::open(dir.clone(), &[]);
    assert_eq!(reopened.list().len(), 2);
    let mut store = reopened;
    let displaced = store
        .approve(&profile(5, "https://bridge.one.example"), Vec::new())
        .unwrap();
    assert_eq!(displaced, vec![profile(1, "").bridge_id]);
    assert_eq!(store.list().len(), 2);
    assert!(store.get(&profile(1, "").bridge_id).is_none());
    store.forget(&profile(5, "").bridge_id).unwrap();
    assert_eq!(Store::open(dir.clone(), &[]).list().len(), 1);
    let _ = fs::remove_dir_all(&dir);
}

#[test]
fn removal_persists_and_approval_clears_it() {
    let dir = temp_dir("removed");
    let mut store = Store::open(dir.clone(), &[]);
    let one = profile(1, "https://bridge.one.example");
    store.approve(&one, Vec::new()).unwrap();
    assert!(store.removed().is_empty());
    store.forget(&one.bridge_id).unwrap();
    assert_eq!(store.removed(), vec![one.bridge_id.clone()]);
    let mut reopened = Store::open(dir.clone(), &[]);
    assert!(reopened.list().is_empty());
    assert_eq!(reopened.removed(), vec![one.bridge_id.clone()]);
    // Approving a different bridge leaves the record alone.
    reopened
        .approve(&profile(2, "https://bridge.two.example"), Vec::new())
        .unwrap();
    assert_eq!(reopened.removed(), vec![one.bridge_id.clone()]);
    reopened.approve(&one, Vec::new()).unwrap();
    assert!(reopened.removed().is_empty());
    assert!(Store::open(dir.clone(), &[]).removed().is_empty());
    let _ = fs::remove_dir_all(&dir);
}

#[test]
fn removed_record_dedupes_and_caps() {
    let dir = temp_dir("removed-cap");
    let mut store = Store::open(dir.clone(), &[]);
    let id = |n: u8| profile(n, "").bridge_id;
    store.forget(&id(1)).unwrap();
    store.forget(&id(2)).unwrap();
    store.forget(&id(1)).unwrap();
    assert_eq!(store.removed(), vec![id(2), id(1)]);
    let last = MAX_RECORDED as u8 + 10;
    for n in 3..=last {
        store.forget(&id(n)).unwrap();
    }
    let removed = Store::open(dir.clone(), &[]).removed();
    assert_eq!(removed.len(), MAX_RECORDED);
    assert_eq!(removed.last(), Some(&id(last)));
    assert!(!removed.contains(&id(1)));
    assert!(!removed.contains(&id(2)));
    assert_eq!(removed, store.removed());
    let _ = fs::remove_dir_all(&dir);
}

#[test]
fn garbage_removed_record_reads_as_empty() {
    let dir = temp_dir("removed-garbage");
    fs::create_dir_all(&dir).unwrap();
    for text in [
        "not json".to_owned(),
        r#"{"format":"other","version":1,"removed":[]}"#.to_owned(),
        r#"{"format":"ember-bridges-removed","version":2,"removed":[]}"#.to_owned(),
        r#"{"format":"ember-bridges-removed","version":1,"removed":[],"extra":1}"#.to_owned(),
        " ".repeat(MAX_FILE + 1),
    ] {
        fs::write(dir.join(REMOVED_FILE), text).unwrap();
        assert!(Store::open(dir.clone(), &[]).removed().is_empty());
    }
    // Entries that are not bridge IDs are dropped; valid ones kept once.
    let good = profile(7, "").bridge_id;
    let text = format!(
        r#"{{"format":"ember-bridges-removed","version":1,"removed":["junk","{good}","{good}"]}}"#
    );
    fs::write(dir.join(REMOVED_FILE), text).unwrap();
    assert_eq!(Store::open(dir.clone(), &[]).removed(), vec![good]);
    let _ = fs::remove_dir_all(&dir);
}

const PINNED_ORIGIN: &str = "https://bridge-staging.example";

fn key(seed: u8) -> String {
    ember_protocol::SigningIdentity::from_seed(&zeroize::Zeroizing::new([seed; 32]))
        .unwrap()
        .public_key()
        .to_b64u()
}

/// A built-in at `PINNED_ORIGIN` with ID byte 9 and the key of seed 9.
fn pinned() -> Vec<Builtin> {
    vec![Builtin {
        bridge_id: profile(9, "").bridge_id,
        origin: PINNED_ORIGIN.into(),
        display_name: "Ember bridge (staging)".into(),
        keys: vec![("k9".into(), key(9))],
    }]
}

fn signing_key(kid: &str, x: String) -> SigningKey {
    SigningKey {
        kty: "OKP".into(),
        crv: "Ed25519".into(),
        alg: "EdDSA".into(),
        usage: "sig".into(),
        kid: kid.into(),
        x,
    }
}

/// How the store lists `bridge_id`.
fn listed(store: &Store, bridge_id: &str) -> Option<Listed> {
    store
        .list()
        .into_iter()
        .find(|listed| listed.bridge.bridge_id == bridge_id)
}

#[test]
fn a_builtin_is_trusted_without_being_saved() {
    let dir = temp_dir("builtin");
    let pin = profile(9, PINNED_ORIGIN);
    let mut store = Store::open(dir.clone(), &pinned());
    let trusted = listed(&store, &pin.bridge_id).expect("the built-in");
    assert!(trusted.builtin && store.pins(&pin.bridge_id));
    assert_eq!(trusted.bridge.origin, PINNED_ORIGIN);
    assert!(trusted.bridge.key("k9").is_some());
    // Listed after the player's own, and marked for the native side.
    store
        .approve(&profile(1, "https://bridge.one.example"), Vec::new())
        .unwrap();
    let json = serde_json::to_value(store.list()).unwrap();
    assert_eq!(json[0]["bridge_id"], profile(1, "").bridge_id);
    assert!(json[0].get("builtin").is_none());
    assert_eq!(json[1]["bridge_id"], pin.bridge_id);
    assert_eq!(json[1]["builtin"], true);
    // The file holds only the player's approval.
    let file = fs::read_to_string(dir.join(FILE)).unwrap();
    assert!(!file.contains(&pin.bridge_id) && !file.contains("builtin"));
    assert_eq!(Store::open(dir.clone(), &[]).list().len(), 1);
    assert_eq!(Store::open(dir.clone(), &pinned()).list(), store.list());
    // A file that claims a built-in is not read.
    fs::write(
        dir.join(FILE),
        file.replace("\"keys\"", "\"builtin\": true, \"keys\""),
    )
    .unwrap();
    assert!(Store::open(dir.clone(), &[]).list().is_empty());
    let _ = fs::remove_dir_all(&dir);
}

#[test]
fn a_builtin_is_dormant_until_used() {
    let dir = temp_dir("builtin-dormant");
    let pin = profile(9, PINNED_ORIGIN);
    let active = |store: &Store| listed(store, &pin.bridge_id).unwrap().active;
    let mut store = Store::open(dir.clone(), &pinned());
    assert!(!active(&store));
    assert!(
        serde_json::to_value(store.list()).unwrap()[0]
            .get("active")
            .is_none()
    );
    // Approving another bridge records no use.
    store
        .approve(&profile(1, "https://bridge.one.example"), Vec::new())
        .unwrap();
    assert!(!dir.join(ACTIVATED_FILE).exists());
    // Used, it stays active across a restart, and bridges.json is untouched.
    let before = fs::read(dir.join(FILE)).unwrap();
    store.activate(&pin.bridge_id).unwrap();
    assert!(active(&store));
    assert_eq!(
        serde_json::to_value(store.list()).unwrap()[1]["active"],
        true
    );
    assert_eq!(fs::read(dir.join(FILE)).unwrap(), before);
    assert!(active(&Store::open(dir.clone(), &pinned())));
    // Only a built-in is activated.
    store.activate(&profile(1, "").bridge_id).unwrap();
    assert!(!listed(&store, &profile(1, "").bridge_id).unwrap().active);
    // Removing it clears the use; approving it again is a use.
    store.forget(&pin.bridge_id).unwrap();
    let text = fs::read_to_string(dir.join(ACTIVATED_FILE)).unwrap();
    assert!(!text.contains(&pin.bridge_id));
    store.approve(&pin, Vec::new()).unwrap();
    assert!(active(&store));
    assert!(active(&Store::open(dir.clone(), &pinned())));
    // A garbled record leaves every built-in dormant.
    fs::write(dir.join(ACTIVATED_FILE), "not json").unwrap();
    assert!(!active(&Store::open(dir.clone(), &pinned())));
    let _ = fs::remove_dir_all(&dir);
}

#[test]
fn a_removed_builtin_stays_removed_until_approved_again() {
    let dir = temp_dir("builtin-removed");
    let pin = profile(9, PINNED_ORIGIN);
    let mut store = Store::open(dir.clone(), &pinned());
    store.forget(&pin.bridge_id).unwrap();
    assert!(store.get(&pin.bridge_id).is_none());
    assert_eq!(store.removed(), vec![pin.bridge_id.clone()]);
    let mut store = Store::open(dir.clone(), &pinned());
    assert!(store.list().is_empty());
    // Approved again by its address: the pinned entry comes back with
    // its pinned keys, whatever the origin served, and is still not saved.
    store
        .approve(&pin, vec![signing_key("other", key(5))])
        .unwrap();
    let restored = listed(&store, &pin.bridge_id).unwrap();
    assert!(restored.builtin);
    assert!(restored.bridge.key("k9").is_some() && restored.bridge.key("other").is_none());
    assert!(store.removed().is_empty());
    assert!(Store::open(dir.clone(), &[]).list().is_empty());
    assert!(
        Store::open(dir.clone(), &pinned())
            .get(&pin.bridge_id)
            .is_some()
    );
    // Its ID is never approved at another origin.
    let elsewhere = profile(9, "https://bridge.one.example");
    let refused = store.approve(&elsewhere, Vec::new()).unwrap_err();
    assert_eq!(refused.as_str(), "bridge_changed");
    let _ = fs::remove_dir_all(&dir);
}

#[test]
fn an_approval_cannot_evict_a_builtin() {
    let dir = temp_dir("builtin-evict");
    let pin = profile(9, PINNED_ORIGIN);
    let mut store = Store::open(dir.clone(), &pinned());
    let other = profile(2, PINNED_ORIGIN);
    let refused = store.approve(&other, Vec::new()).unwrap_err();
    assert_eq!(refused.as_str(), "bridge_changed");
    assert_eq!(store.list(), Store::open(dir.clone(), &pinned()).list());
    // Removed, it gives the origin up to the bridge the player approves.
    store.forget(&pin.bridge_id).unwrap();
    assert!(store.approve(&other, Vec::new()).unwrap().is_empty());
    assert!(!listed(&store, &other.bridge_id).unwrap().builtin);
    // Approving the built-in again replaces that bridge.
    let displaced = store.approve(&pin, Vec::new()).unwrap();
    assert_eq!(displaced, vec![other.bridge_id.clone()]);
    assert_eq!(store.list().len(), 1);
    assert!(Store::open(dir.clone(), &[]).list().is_empty());
    let _ = fs::remove_dir_all(&dir);
}

#[test]
fn builtins_take_none_of_the_players_approvals() {
    let dir = temp_dir("builtin-cap");
    let mut store = Store::open(dir.clone(), &pinned());
    for n in 1..=MAX_BRIDGES as u8 {
        let origin = format!("https://bridge{n}.example");
        store
            .approve(&profile(20 + n, &origin), Vec::new())
            .unwrap();
    }
    assert_eq!(store.list().len(), MAX_BRIDGES + 1);
    let refused = store
        .approve(&profile(99, "https://one-more.example"), Vec::new())
        .unwrap_err();
    assert_eq!(refused.as_str(), "too_many_bridges");
    // The built-in itself can still be removed and approved again.
    let pin = profile(9, PINNED_ORIGIN);
    store.forget(&pin.bridge_id).unwrap();
    store.approve(&pin, Vec::new()).unwrap();
    assert!(store.get(&pin.bridge_id).is_some());
    let _ = fs::remove_dir_all(&dir);
}

/// A build without the built-in approved it, and another bridge elsewhere.
fn older_approvals(dir: &Path) {
    let mut older = Store::open(dir.to_owned(), &[]);
    older
        .approve(&profile(9, PINNED_ORIGIN), Vec::new())
        .unwrap();
    older
        .approve(&profile(1, "https://bridge.one.example"), Vec::new())
        .unwrap();
}

#[test]
fn an_older_approval_of_a_builtin_counts_as_using_it() {
    let dir = temp_dir("builtin-older");
    older_approvals(&dir);
    let pin = profile(9, PINNED_ORIGIN);
    let store = Store::open(dir.clone(), &pinned());
    let ids: Vec<_> = store
        .list()
        .into_iter()
        .map(|listed| (listed.bridge.bridge_id, listed.builtin, listed.active))
        .collect();
    assert_eq!(
        ids,
        vec![
            (profile(1, "").bridge_id, false, false),
            (pin.bridge_id.clone(), true, true)
        ]
    );
    // The approval stays on disk for a build without the built-in.
    let mut store = store;
    store
        .approve(&profile(2, "https://bridge.two.example"), Vec::new())
        .unwrap();
    store.activate(&pin.bridge_id).unwrap();
    assert!(Store::open(dir.clone(), &[]).get(&pin.bridge_id).is_some());
    let _ = fs::remove_dir_all(&dir);
}

#[test]
fn removing_a_builtin_removes_an_older_approval_of_it() {
    let pin = profile(9, PINNED_ORIGIN);
    // An older build approved the built-in itself, or another bridge at its
    // origin. Removed here, neither comes back on the next start, with or
    // without the built-in.
    for (name, older) in [
        ("itself", profile(9, PINNED_ORIGIN)),
        ("origin", profile(3, PINNED_ORIGIN)),
    ] {
        let dir = temp_dir(&format!("builtin-older-removed-{name}"));
        let mut before = Store::open(dir.clone(), &[]);
        before.approve(&older, Vec::new()).unwrap();
        before
            .approve(&profile(1, "https://bridge.one.example"), Vec::new())
            .unwrap();
        let mut store = Store::open(dir.clone(), &pinned());
        assert!(store.get(&pin.bridge_id).is_some());
        store.forget(&pin.bridge_id).unwrap();
        for builtins in [pinned(), Vec::new()] {
            let reopened = Store::open(dir.clone(), &builtins);
            assert!(reopened.get(&older.bridge_id).is_none(), "{name}");
            assert!(reopened.get(&pin.bridge_id).is_none(), "{name}");
            assert!(reopened.get(&profile(1, "").bridge_id).is_some(), "{name}");
        }
        let _ = fs::remove_dir_all(&dir);
    }
}

#[test]
fn a_builtins_keys_are_never_replaced() {
    let dir = temp_dir("builtin-keys");
    let pin = profile(9, PINNED_ORIGIN);
    let mut store = Store::open(dir.clone(), &pinned());
    let refused = store
        .set_keys(&pin.bridge_id, vec![signing_key("k5", key(5))])
        .unwrap_err();
    assert_eq!(refused.as_str(), "bridge_pinned");
    let kept = store.get(&pin.bridge_id).unwrap();
    assert!(kept.key("k9").is_some() && kept.key("k5").is_none());
    // An approved bridge's keys still follow its origin.
    let one = profile(1, "https://bridge.one.example");
    store.approve(&one, Vec::new()).unwrap();
    store
        .set_keys(&one.bridge_id, vec![signing_key("k5", key(5))])
        .unwrap();
    assert!(store.get(&one.bridge_id).unwrap().key("k5").is_some());
    let _ = fs::remove_dir_all(&dir);
}

#[test]
fn an_unusable_pin_is_not_trusted() {
    let dir = temp_dir("builtin-unusable");
    let good = pinned().remove(0);
    let unusable = [
        Builtin {
            bridge_id: "brg_FILL_IN".into(),
            ..good.clone()
        },
        Builtin {
            origin: "http://bridge.example".into(),
            ..good.clone()
        },
        Builtin {
            keys: Vec::new(),
            ..good.clone()
        },
        Builtin {
            keys: vec![good.keys[0].clone(), ("bad".into(), "FILL_IN".into())],
            ..good.clone()
        },
    ];
    for pin in unusable {
        let store = Store::open(dir.clone(), &[pin]);
        assert!(store.list().is_empty() && !store.pins(&good.bridge_id));
    }
    assert!(!dir.exists());
}

#[test]
fn no_folder_means_no_store() {
    let mut store = Store::open(PathBuf::new(), &pinned());
    assert!(store.list().is_empty());
    assert!(
        store
            .approve(&profile(1, "https://bridge.one.example"), Vec::new())
            .is_err()
    );
    assert!(!std::path::Path::new(FILE).exists());
}

/// One bridge is found as `list` gives it, whichever record holds it: a
/// player's approval, a built-in, an approval a live built-in covers (the
/// built-in is the one found), and a removed built-in (not found).
#[test]
fn one_bridge_is_found_as_the_listing_gives_it() {
    let dir = temp_dir("get");
    let pin = profile(9, PINNED_ORIGIN);
    // A build without the built-in saved an approval of it.
    Store::open(dir.clone(), &[])
        .approve(&pin, Vec::new())
        .unwrap();
    let mut store = Store::open(dir.clone(), &pinned());
    store
        .approve(&profile(1, "https://bridge.one.example"), Vec::new())
        .unwrap();
    let ids = [1u8, 9, 2].map(|byte| profile(byte, "").bridge_id);
    for id in &ids {
        assert_eq!(
            store.get(id),
            listed(&store, id).map(|listed| listed.bridge),
            "{id}"
        );
    }
    assert!(store.get(&ids[0]).is_some());
    let found = store.get(&ids[1]).expect("the built-in");
    assert!(found.key("k9").is_some(), "the built-in's record is found");
    assert!(store.get(&ids[2]).is_none());
    store.forget(&ids[1]).unwrap();
    assert_eq!(store.get(&ids[1]), None);
    assert_eq!(
        store.get(&ids[1]),
        listed(&store, &ids[1]).map(|l| l.bridge)
    );
    let _ = fs::remove_dir_all(&dir);
}
