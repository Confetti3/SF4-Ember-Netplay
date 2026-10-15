mod support;
use ember_reports::{config::Limits, store::Store};
use std::{
    sync::Arc,
    time::{Duration, SystemTime},
};
use support::{Temp, id};

#[tokio::test]
async fn competing_reservations_cannot_overbook_and_drop_releases_capacity() {
    let temp = Temp::new();
    let store = Arc::new(
        Store::new(
            temp.0.clone(),
            Limits {
                outbox_count: 1,
                ..Limits::default()
            },
        )
        .await
        .unwrap(),
    );
    let a = id(&"a".repeat(32));
    let reservation = store.reserve(&a, 2).await.unwrap();
    let contender = store.clone();
    let mut task =
        tokio::spawn(async move { contender.enqueue(&id(&"b".repeat(32)), b"{}").await });
    assert!(
        tokio::time::timeout(Duration::from_millis(20), &mut task)
            .await
            .is_err()
    );
    reservation
        .commit(b"{}", Some(b"MDMPaccepted"))
        .await
        .unwrap();
    assert!(task.await.unwrap().is_err());
    assert_eq!(std::fs::read(store.dump_path(&a)).unwrap(), b"MDMPaccepted");
    store.delivered(&a).await.unwrap();
    drop(store.reserve(&id(&"b".repeat(32)), 2).await.unwrap());
    store.enqueue(&id(&"c".repeat(32)), b"{}").await.unwrap();
}

#[tokio::test]
async fn failed_staged_write_cleans_new_dump_without_pruning_accepted_evidence() {
    let temp = Temp::new();
    let store = Store::new(
        temp.0.clone(),
        Limits {
            dump_count: 1,
            ..Limits::default()
        },
    )
    .await
    .unwrap();
    let a = id(&"a".repeat(32));
    let b = id(&"b".repeat(32));
    store
        .reserve(&a, 2)
        .await
        .unwrap()
        .commit(b"{}", Some(b"MDMPaccepted"))
        .await
        .unwrap();
    let reservation = store.reserve(&b, 2).await.unwrap();
    // Force the event publish to fail after both files were staged. This
    // directory represents an external filesystem fault, not a stored event.
    std::fs::create_dir(temp.0.join("outbox").join(format!("{b}.json"))).unwrap();
    assert!(
        reservation
            .commit(b"{}", Some(b"MDMPrejected"))
            .await
            .is_err()
    );
    assert_eq!(store.event(&a).await.unwrap().unwrap(), b"{}");
    assert_eq!(std::fs::read(store.dump_path(&a)).unwrap(), b"MDMPaccepted");
    assert!(!store.dump_path(&b).exists());
    assert!(!store.dump_path(&b).with_extension("tmp").exists());
    assert!(!temp.0.join("outbox").join(format!("{b}.tmp")).exists());
}

#[tokio::test]
async fn reservations_account_for_bytes_as_well_as_count() {
    let temp = Temp::new();
    let store = Store::new(
        temp.0.clone(),
        Limits {
            outbox_bytes: 3,
            ..Limits::default()
        },
    )
    .await
    .unwrap();
    store.enqueue(&id(&"a".repeat(32)), b"{}").await.unwrap();
    assert!(store.reserve(&id(&"b".repeat(32)), 2).await.is_err());
    store.delivered(&id(&"a".repeat(32))).await.unwrap();
    store.enqueue(&id(&"b".repeat(32)), b"{}").await.unwrap();
}

#[tokio::test]
async fn leftover_dump_staging_cannot_renew_admission_headroom() {
    for (dump_count, dump_bytes) in [(1, 32), (4, 8)] {
        let temp = Temp::new();
        let store = Store::new(
            temp.0.clone(),
            Limits {
                dump_count,
                dump_bytes,
                ..Limits::default()
            },
        )
        .await
        .unwrap();
        let a = id(&"a".repeat(32));
        let b = id(&"b".repeat(32));
        store
            .reserve(&a, 2)
            .await
            .unwrap()
            .commit(b"{}", Some(b"MDMP0001"))
            .await
            .unwrap();
        // Model a previous failed write whose staging cleanup also failed.
        let staging = store.dump_path(&id(&"c".repeat(32))).with_extension("tmp");
        std::fs::write(&staging, b"MDMPtemp").unwrap();
        assert!(
            store
                .reserve(&b, 2)
                .await
                .unwrap()
                .commit(b"{}", Some(b"MDMP0002"))
                .await
                .is_err()
        );
        assert!(store.event(&b).await.unwrap().is_none());
        assert_eq!(std::fs::read(store.dump_path(&a)).unwrap(), b"MDMP0001");
        assert_eq!(std::fs::read_dir(temp.0.join("dumps")).unwrap().count(), 2);
        std::fs::remove_file(staging).unwrap();
        store
            .reserve(&b, 2)
            .await
            .unwrap()
            .commit(b"{}", Some(b"MDMP0002"))
            .await
            .unwrap();
        assert_eq!(std::fs::read(store.dump_path(&b)).unwrap(), b"MDMP0002");
        assert_eq!(std::fs::read_dir(temp.0.join("dumps")).unwrap().count(), 1);
    }
}

#[tokio::test]
async fn bounded_outbox_and_dump_retention_oldest_first() {
    let temp = Temp::new();
    let limits = Limits {
        outbox_count: 1,
        dump_bytes: 10,
        dump_count: 3,
        ..Limits::default()
    };
    let store = Store::new(temp.0.clone(), limits).await.unwrap();
    let a = id(&"a".repeat(32));
    let b = id(&"b".repeat(32));
    let c = id(&"c".repeat(32));
    store
        .reserve(&a, 2)
        .await
        .unwrap()
        .commit(b"{}", Some(b"MDMP01"))
        .await
        .unwrap();
    assert!(store.enqueue(&b, b"{}").await.is_err());
    let first = store.dump_path(&a);
    let file = std::fs::File::options().write(true).open(&first).unwrap();
    file.set_modified(SystemTime::now() - Duration::from_secs(60))
        .unwrap();
    drop(file);
    store.delivered(&a).await.unwrap();
    store
        .reserve(&b, 2)
        .await
        .unwrap()
        .commit(b"{}", Some(b"MDMP02"))
        .await
        .unwrap();
    let second = store.dump_path(&b);
    assert!(!first.exists());
    assert!(second.exists());
    std::fs::File::options()
        .write(true)
        .open(&second)
        .unwrap()
        .set_modified(SystemTime::now() - Duration::from_secs(31 * 86400))
        .unwrap();
    std::fs::write(temp.0.join("dumps/operator-notes.txt"), "keep").unwrap();
    store.prune_dumps(SystemTime::now()).await.unwrap();
    assert!(!second.exists());
    assert!(temp.0.join("dumps/operator-notes.txt").exists());
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        assert_eq!(
            std::fs::metadata(temp.0.join("outbox").join(format!("{b}.json")))
                .unwrap()
                .permissions()
                .mode()
                & 0o777,
            0o600
        );
    }
    store.delivered(&b).await.unwrap();
    store
        .reserve(&c, 2)
        .await
        .unwrap()
        .commit(b"{}", Some(b"MDMP03"))
        .await
        .unwrap();
    assert!(store.dump_path(&c).exists());
}
