mod support;
use ember_reports::{config::Limits, store::Store};
use std::{sync::Arc, time::Duration};
use support::Temp;

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
    let a = "a".repeat(32);
    let reservation = store.reserve(&a, 2).await.unwrap();
    let contender = store.clone();
    let mut task = tokio::spawn(async move { contender.enqueue(&"b".repeat(32), b"{}").await });
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
    drop(store.reserve(&"b".repeat(32), 2).await.unwrap());
    store.enqueue(&"c".repeat(32), b"{}").await.unwrap();
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
    let a = "a".repeat(32);
    let b = "b".repeat(32);
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
    store.enqueue(&"a".repeat(32), b"{}").await.unwrap();
    assert!(store.reserve(&"b".repeat(32), 2).await.is_err());
    store.delivered(&"a".repeat(32)).await.unwrap();
    store.enqueue(&"b".repeat(32), b"{}").await.unwrap();
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
        let a = "a".repeat(32);
        let b = "b".repeat(32);
        store
            .reserve(&a, 2)
            .await
            .unwrap()
            .commit(b"{}", Some(b"MDMP0001"))
            .await
            .unwrap();
        // Model a previous failed write whose staging cleanup also failed.
        let staging = store.dump_path(&"c".repeat(32)).with_extension("tmp");
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
