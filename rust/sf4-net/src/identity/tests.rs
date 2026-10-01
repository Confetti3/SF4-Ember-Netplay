//! Identity store behavior, named after the spec's ID-xx acceptance tests
//! where one applies. DPAPI tests use the real current-user DPAPI.
use std::{
    fs,
    path::{Path, PathBuf},
    sync::atomic::{AtomicUsize, Ordering},
};

use super::*;

struct Dir(PathBuf);

impl Dir {
    fn new() -> Self {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let path = std::env::temp_dir().join(format!(
            "sf4-identity-test-{}-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed),
            b64u(&random::<6>().unwrap())
        ));
        Self(path)
    }

    fn store(&self) -> Identity {
        Identity::open(self.0.clone(), false, envelope::TEST_COST)
    }

    fn wine_store(&self) -> Identity {
        Identity::open(self.0.clone(), true, envelope::TEST_COST)
    }

    fn path(&self, name: &str) -> PathBuf {
        self.0.join(name)
    }
}

impl Drop for Dir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn id(store: &Identity) -> EmberId {
    store.status().ember_id.expect("an ember id")
}

fn retired_folders(dir: &Path) -> usize {
    fs::read_dir(dir.join(RETIRED_DIR)).map_or(0, |entries| entries.count())
}

#[test]
fn fresh_store_is_disabled_and_creates_nothing() {
    let dir = Dir::new();
    let store = dir.store();
    assert_eq!(store.status().state, State::Disabled);
    assert!(store.status().ember_id.is_none());
    assert!(!dir.0.exists());
}

// ID-02, ID-04
#[test]
fn dpapi_identity_survives_restart() {
    let dir = Dir::new();
    let mut store = dir.store();
    let status = store.enable(Protection::Default).unwrap();
    assert_eq!(status.state, State::Ready);
    assert_eq!(status.backend, Some(Backend::Dpapi));
    let first = id(&store);
    assert!(store.signer().is_some());
    assert!(dir.path(KEY_FILE).exists() && dir.path(STATE_FILE).exists());
    for _ in 0..3 {
        let reopened = dir.store();
        assert_eq!(reopened.status().state, State::Ready);
        assert_eq!(id(&reopened), first);
    }
    assert_eq!(store.enable(Protection::Default), Err(Failure::WrongState));
}

#[test]
fn passphrase_identity_locks_and_unlocks() {
    let dir = Dir::new();
    let mut store = dir.store();
    store
        .enable(Protection::Passphrase("a long passphrase"))
        .unwrap();
    let first = id(&store);
    let mut reopened = dir.store();
    let status = reopened.status();
    assert_eq!(status.state, State::Locked);
    assert_eq!(status.ember_id.as_ref(), Some(&first));
    assert!(reopened.signer().is_none());
    assert_eq!(
        reopened.unlock("wrong passphrase"),
        Err(Failure::WrongPassphrase)
    );
    assert_eq!(reopened.status().state, State::Locked);
    assert_eq!(
        reopened.unlock("a long passphrase").unwrap().state,
        State::Ready
    );
    assert_eq!(id(&reopened), first);
    assert!(reopened.signer().is_some());
}

// ID-17: no silent DPAPI under Wine/Proton.
#[test]
fn wine_requires_a_passphrase() {
    let dir = Dir::new();
    let mut store = dir.wine_store();
    assert!(store.status().passphrase_required);
    assert_eq!(
        store.enable(Protection::Default),
        Err(Failure::PassphraseRequired)
    );
    assert!(!dir.path(KEY_FILE).exists());
    let status = store
        .enable(Protection::Passphrase("wine passphrase"))
        .unwrap();
    assert_eq!(status.backend, Some(Backend::Passphrase));
}

// ID-06
#[test]
fn concurrent_enable_installs_one_identity() {
    let dir = Dir::new();
    let path = dir.0.clone();
    let handles: Vec<_> = (0..4)
        .map(|_| {
            let path = path.clone();
            std::thread::spawn(move || {
                let mut store = Identity::open(path, false, envelope::TEST_COST);
                match store.enable(Protection::Default) {
                    Ok(_) | Err(Failure::WrongState) => {}
                    Err(other) => panic!("{other:?}"),
                }
                Identity::open(store.dir.clone(), false, envelope::TEST_COST)
                    .status()
                    .ember_id
                    .unwrap()
            })
        })
        .collect();
    let ids: Vec<EmberId> = handles.into_iter().map(|h| h.join().unwrap()).collect();
    assert!(ids.windows(2).all(|pair| pair[0] == pair[1]), "{ids:?}");
    let keys = fs::read_dir(&dir.0)
        .unwrap()
        .flatten()
        .filter(|e| e.file_name() == KEY_FILE)
        .count();
    assert_eq!(keys, 1);
}

// ID-07
#[test]
fn crash_at_any_step_never_replaces_an_established_key() {
    for step in [
        Step::WriteTemp,
        Step::FlushTemp,
        Step::InstallKey,
        Step::ReadBack,
        Step::WriteContinuity,
    ] {
        for protection in [
            Protection::Default,
            Protection::Passphrase("crash passphrase"),
        ] {
            let dir = Dir::new();
            let mut store = dir.store();
            store.set_faults(move |at| at == step);
            let outcome = store.enable(protection);
            let mut reopened = dir.store();
            if matches!(protection, Protection::Passphrase(_))
                && reopened.status().state == State::Locked
            {
                reopened.unlock("crash passphrase").unwrap();
            }
            let state = reopened.status().state;
            match &outcome {
                Ok(status) => {
                    assert_eq!(state, State::Ready, "{step:?}");
                    assert_eq!(reopened.status().ember_id, status.ember_id, "{step:?}");
                }
                // A failed read-back of an installed passphrase file still
                // leaves that file as the identity, unlocked above.
                Err(_) => assert!(matches!(state, State::Disabled | State::Ready), "{step:?}"),
            }
            // Whatever happened, enabling again ends with exactly one ID, and
            // a key that was installed is the one kept.
            let before = reopened.status().ember_id;
            if state == State::Disabled {
                reopened.enable(protection).unwrap();
            }
            let after = dir.store().status().ember_id;
            if before.is_some() {
                assert_eq!(after, before, "{step:?}");
            }
            assert!(after.is_some());
            assert!(
                fs::read_dir(&dir.0)
                    .unwrap()
                    .flatten()
                    .all(|entry| !entry.file_name().to_string_lossy().starts_with(TEMP_PREFIX)),
                "{step:?} left a temporary file"
            );
        }
    }
}

#[test]
fn leftover_temporary_files_are_not_identities() {
    let dir = Dir::new();
    fs::create_dir_all(&dir.0).unwrap();
    fs::write(dir.path(".tmp-abc"), b"partial").unwrap();
    let store = dir.store();
    assert_eq!(store.status().state, State::Disabled);
    assert!(!dir.path(".tmp-abc").exists());
}

// ID-08
#[test]
fn unreadable_keys_are_reported_and_kept() {
    let dir = Dir::new();
    let mut store = dir.store();
    store.enable(Protection::Default).unwrap();
    let expected = id(&store);
    let original = fs::read(dir.path(KEY_FILE)).unwrap();

    // A blob from another user or machine fails to unprotect.
    let mut file: serde_json::Value = serde_json::from_slice(&original).unwrap();
    let mut blob = decode_b64u_vec(file["blob"].as_str().unwrap(), 4096, "b").unwrap();
    let last = blob.len() - 1;
    blob[last] ^= 0x5a;
    file["blob"] = b64u(&blob).into();
    fs::write(dir.path(KEY_FILE), serde_json::to_vec(&file).unwrap()).unwrap();
    let mut broken = dir.store();
    let status = broken.status();
    assert_eq!(
        (status.state, status.reason),
        (State::Unavailable, Some(Failure::DecryptFailed))
    );
    assert_eq!(status.ember_id.as_ref(), Some(&expected));
    assert_eq!(broken.enable(Protection::Default), Err(Failure::WrongState));

    fs::write(dir.path(KEY_FILE), b"{not json").unwrap();
    let status = dir.store().status();
    assert_eq!(
        (status.state, status.reason),
        (State::Unavailable, Some(Failure::Corrupt))
    );

    fs::write(
        dir.path(KEY_FILE),
        br#"{"format":"ember-key-future","version":9}"#,
    )
    .unwrap();
    assert_eq!(
        dir.store().status().reason,
        Some(Failure::UnsupportedEnvelope)
    );

    fs::remove_file(dir.path(KEY_FILE)).unwrap();
    fs::create_dir(dir.path(KEY_FILE)).unwrap();
    let status = dir.store().status();
    assert_eq!(status.state, State::Unavailable);
    assert!(matches!(
        status.reason,
        Some(Failure::PermissionDenied | Failure::Io)
    ));
    fs::remove_dir(dir.path(KEY_FILE)).unwrap();

    // Restoring the original file restores the original identity.
    fs::write(dir.path(KEY_FILE), &original).unwrap();
    assert_eq!(id(&dir.store()), expected);
}

// ID-09
#[test]
fn missing_key_with_continuity_needs_recovery() {
    let dir = Dir::new();
    let mut store = dir.store();
    store.enable(Protection::Default).unwrap();
    let expected = id(&store);
    fs::remove_file(dir.path(KEY_FILE)).unwrap();
    let mut recovering = dir.store();
    let status = recovering.status();
    assert_eq!(
        (status.state, status.reason),
        (State::RecoveryRequired, Some(Failure::KeyMissing))
    );
    assert_eq!(status.ember_id, Some(expected));
    assert_eq!(
        recovering.enable(Protection::Default),
        Err(Failure::WrongState)
    );
    assert!(!dir.path(KEY_FILE).exists());
}

// ID-10
#[test]
fn missing_continuity_is_repaired_from_the_key() {
    let dir = Dir::new();
    let mut store = dir.store();
    store.enable(Protection::Default).unwrap();
    let expected = id(&store);
    fs::remove_file(dir.path(STATE_FILE)).unwrap();
    let repaired = dir.store();
    assert_eq!(repaired.status().state, State::Ready);
    assert_eq!(id(&repaired), expected);
    assert!(dir.path(STATE_FILE).exists());

    fs::write(dir.path(STATE_FILE), b"garbage").unwrap();
    assert_eq!(id(&dir.store()), expected);
    assert!(parse_continuity(&fs::read(dir.path(STATE_FILE)).unwrap()).is_ok());
}

#[test]
fn continuity_for_another_key_needs_recovery() {
    let (a, b) = (Dir::new(), Dir::new());
    a.store().enable(Protection::Default).unwrap();
    b.store().enable(Protection::Default).unwrap();
    fs::copy(b.path(STATE_FILE), a.path(STATE_FILE)).unwrap();
    let status = a.store().status();
    assert_eq!(
        (status.state, status.reason),
        (State::RecoveryRequired, Some(Failure::MetadataMismatch))
    );
}

// ID-11
#[test]
fn backup_restores_the_same_id_elsewhere() {
    let (a, b, backup_dir) = (Dir::new(), Dir::new(), Dir::new());
    let mut source = a.store();
    source.enable(Protection::Default).unwrap();
    let expected = id(&source);
    fs::create_dir_all(&backup_dir.0).unwrap();
    let backup = backup_dir.path("ember.backup");
    source.export(&backup, "backup passphrase").unwrap();
    let bytes = fs::read(&backup).unwrap();

    let mut target = b.store();
    let preview = target.preview_import(&bytes, "backup passphrase").unwrap();
    assert_eq!(preview.ember_id, expected);
    assert!(!preview.same_identity && !preview.replaces_existing);
    assert_eq!(
        target
            .import(
                &bytes,
                "backup passphrase",
                &expected,
                Protection::Default,
                false
            )
            .unwrap()
            .state,
        State::Ready
    );
    assert_eq!(id(&target), expected);
    assert_eq!(id(&b.store()), expected);
    // Importing the same identity again changes nothing.
    assert!(
        target
            .preview_import(&bytes, "backup passphrase")
            .unwrap()
            .same_identity
    );
    target
        .import(
            &bytes,
            "backup passphrase",
            &expected,
            Protection::Default,
            false,
        )
        .unwrap();
    assert_eq!(retired_folders(&b.0), 0);

    // A passphrase-protected destination works the same way.
    let c = Dir::new();
    let mut wine = c.wine_store();
    wine.import(
        &bytes,
        "backup passphrase",
        &expected,
        Protection::Passphrase("local"),
        false,
    )
    .unwrap();
    let mut reopened = c.wine_store();
    reopened.unlock("local").unwrap();
    assert_eq!(id(&reopened), expected);
}

// ID-12 (file-level cases live in envelope tests)
#[test]
fn import_fails_closed_without_touching_the_store() {
    let (a, b) = (Dir::new(), Dir::new());
    let mut source = a.store();
    source.enable(Protection::Default).unwrap();
    let expected = id(&source);
    let backup = a.path("ember.backup");
    source.export(&backup, "right").unwrap();
    let bytes = fs::read(&backup).unwrap();
    let mut target = b.store();
    assert_eq!(
        target.preview_import(&bytes, "wrong").err(),
        Some(Failure::WrongPassphrase)
    );
    assert_eq!(
        target
            .import(&bytes, "wrong", &expected, Protection::Default, false)
            .err(),
        Some(Failure::WrongPassphrase)
    );
    let other = Dir::new();
    let mut unrelated = other.store();
    unrelated.enable(Protection::Default).unwrap();
    assert_eq!(
        target
            .import(&bytes, "right", &id(&unrelated), Protection::Default, false)
            .err(),
        Some(Failure::ConfirmationMismatch)
    );
    assert!(!b.0.exists());
}

// ID-14
#[test]
fn different_identity_import_requires_confirmation_and_keeps_the_old_store() {
    let (a, b) = (Dir::new(), Dir::new());
    let mut source = a.store();
    source.enable(Protection::Default).unwrap();
    let imported_id = id(&source);
    let backup = a.path("ember.backup");
    source.export(&backup, "pass").unwrap();
    let bytes = fs::read(&backup).unwrap();

    let mut target = b.store();
    target.enable(Protection::Default).unwrap();
    let old_id = id(&target);
    let old_key = fs::read(b.path(KEY_FILE)).unwrap();
    assert!(
        target
            .preview_import(&bytes, "pass")
            .unwrap()
            .replaces_existing
    );
    assert_eq!(
        target
            .import(&bytes, "pass", &imported_id, Protection::Default, false)
            .err(),
        Some(Failure::ReplacementNotConfirmed)
    );
    assert_eq!(id(&b.store()), old_id);

    target
        .import(&bytes, "pass", &imported_id, Protection::Default, true)
        .unwrap();
    assert_eq!(id(&b.store()), imported_id);
    assert_eq!(retired_folders(&b.0), 1);
    let folder = fs::read_dir(b.0.join(RETIRED_DIR))
        .unwrap()
        .next()
        .unwrap()
        .unwrap()
        .path();
    assert_eq!(fs::read(folder.join(KEY_FILE)).unwrap(), old_key);
}

#[test]
fn backup_recovers_a_missing_key() {
    let dir = Dir::new();
    let mut store = dir.store();
    store.enable(Protection::Default).unwrap();
    let expected = id(&store);
    let created = store.status().created_at;
    let backup = dir.0.with_extension("backup");
    store.export(&backup, "pass").unwrap();
    let bytes = fs::read(&backup).unwrap();
    fs::remove_file(&backup).unwrap();
    fs::remove_file(dir.path(KEY_FILE)).unwrap();

    let mut recovering = dir.store();
    assert_eq!(recovering.status().state, State::RecoveryRequired);
    let preview = recovering.preview_import(&bytes, "pass").unwrap();
    assert!(preview.same_identity && !preview.replaces_existing);
    recovering
        .import(&bytes, "pass", &expected, Protection::Default, false)
        .unwrap();
    let restored = dir.store();
    assert_eq!(restored.status().state, State::Ready);
    assert_eq!(id(&restored), expected);
    assert_eq!(restored.status().created_at, created);
}

#[test]
fn reset_retires_and_allows_a_new_identity() {
    let dir = Dir::new();
    let mut store = dir.store();
    store.enable(Protection::Default).unwrap();
    let old = id(&store);
    let other = Dir::new();
    let mut unrelated = other.store();
    unrelated.enable(Protection::Default).unwrap();
    assert_eq!(store.reset(None), Err(Failure::ConfirmationMismatch));
    assert_eq!(
        store.reset(Some(&id(&unrelated))),
        Err(Failure::ConfirmationMismatch)
    );
    assert_eq!(store.reset(Some(&old)).unwrap().state, State::Disabled);
    assert_eq!(retired_folders(&dir.0), 1);
    store.enable(Protection::Default).unwrap();
    assert_ne!(id(&store), old);
}

// ID-15
#[test]
fn no_seed_in_exports_status_or_debug() {
    let dir = Dir::new();
    let mut store = dir.store();
    store.enable(Protection::Default).unwrap();
    let seed = store.signer().unwrap().seed();
    let seed_b64 = b64u(seed.as_slice());
    let seed_hex: String = seed.iter().map(|b| format!("{b:02x}")).collect();
    let backup = dir.0.with_extension("backup");
    store.export(&backup, "pass").unwrap();
    let mut texts = vec![
        String::from_utf8(fs::read(&backup).unwrap()).unwrap(),
        String::from_utf8(fs::read(dir.path(KEY_FILE)).unwrap()).unwrap(),
        String::from_utf8(fs::read(dir.path(STATE_FILE)).unwrap()).unwrap(),
        serde_json::to_string(&store.status()).unwrap(),
        format!("{store:?}"),
        format!("{:?}", store.signer().unwrap()),
    ];
    texts.push(format!(
        "{:?}",
        store
            .preview_import(&fs::read(&backup).unwrap(), "pass")
            .unwrap()
    ));
    fs::remove_file(&backup).unwrap();
    for text in texts {
        assert!(
            !text.contains(&seed_b64) && !text.contains(&seed_hex),
            "{text}"
        );
        assert!(!text.contains("pass\""), "{text}");
    }
}

#[test]
fn oversized_files_are_refused_before_reading() {
    let dir = Dir::new();
    fs::create_dir_all(&dir.0).unwrap();
    fs::write(dir.path(KEY_FILE), vec![b' '; MAX_BACKUP_FILE + 1]).unwrap();
    let status = dir.store().status();
    assert_eq!(
        (status.state, status.reason),
        (State::Unavailable, Some(Failure::Corrupt))
    );
}
