use super::*;

#[tokio::test]
async fn bounded_relay_sized_append_does_not_forfeit_healthy_quorum() {
    let (bus, nodes) = cluster(3).await;
    // Public-relay admission carries a roughly 320 KiB private checkpoint.
    // A healthy append can take longer than one second while still staying
    // within the transport's bounded ten-second RPC allowance.
    bus.append_delay_ms.store(4_000, Ordering::Relaxed);
    let checkpoint = "x".repeat(320 * 1024);
    let receipt = tokio::time::timeout(
        Duration::from_secs(10),
        nodes[0].propose(proposal(&nodes[0], "relay-sized", 0, &checkpoint)),
    )
    .await
    .expect("healthy bounded append lost quorum")
    .expect("healthy bounded append failed");
    assert!(receipt.accepted);
    assert_eq!(
        nodes[0].committed().await.checkpoint.len(),
        checkpoint.len()
    );
    stop(nodes).await;
}

#[tokio::test]
async fn bounded_relay_snapshot_bootstraps_a_fresh_learner() {
    let (bus, mut nodes) = cluster(1).await;
    for revision in 0..12 {
        let request = format!("snapshot-{revision}");
        assert!(
            nodes[0]
                .propose(proposal(
                    &nodes[0],
                    &request,
                    revision,
                    &"s".repeat(32 * 1024)
                ))
                .await
                .unwrap()
                .accepted
        );
    }
    tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            if nodes[0].store.0.lock().await.purged.is_some() {
                break;
            }
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
    })
    .await
    .expect("leader did not compact a snapshot");

    let learner = Arc::new(
        Coordinator::new(
            2,
            Arc::new(TestNetwork {
                source: 2,
                bus: bus.clone(),
            }),
        )
        .await
        .unwrap(),
    );
    bus.peers.lock().await.insert(2, Arc::downgrade(&learner));
    nodes.push(learner);
    bus.snapshot_delay_ms.store(6_000, Ordering::Relaxed);
    tokio::time::timeout(
        Duration::from_secs(14),
        nodes[0].raft.add_learner(2, BasicNode::new("2"), true),
    )
    .await
    .expect("bounded relay snapshot stranded learner")
    .expect("bounded relay snapshot admission failed");
    tokio::time::timeout(Duration::from_secs(14), async {
        loop {
            if nodes[1].committed().await.revision == 12 {
                break;
            }
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
    })
    .await
    .expect("bounded relay snapshot never reached learner");
    stop(nodes).await;
}

#[tokio::test]
async fn bounded_relay_snapshot_batches_a_four_chunk_credit_window() {
    let (bus, mut nodes) = cluster(1).await;
    let checkpoint = "w".repeat(512 * 1024);
    for revision in 0..12 {
        let request = format!("window-{revision}");
        assert!(
            nodes[0]
                .propose(proposal(&nodes[0], &request, revision, &checkpoint))
                .await
                .unwrap()
                .accepted
        );
    }
    tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            if nodes[0].store.0.lock().await.purged.is_some() {
                break;
            }
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
    })
    .await
    .expect("leader did not compact the relay-sized snapshot");
    let encoded_snapshot_bytes = nodes[0]
        .store
        .0
        .lock()
        .await
        .snapshot
        .as_ref()
        .expect("missing compacted snapshot")
        .1
        .len();
    assert!(
        encoded_snapshot_bytes < checkpoint.len() / 4,
        "repetitive checkpoint snapshot was not compressed"
    );

    let learner = Arc::new(
        Coordinator::new(
            2,
            Arc::new(TestNetwork {
                source: 2,
                bus: bus.clone(),
            }),
        )
        .await
        .unwrap(),
    );
    bus.peers.lock().await.insert(2, Arc::downgrade(&learner));
    nodes.push(learner);
    // Model a relay RTT on every OpenRaft snapshot RPC. Sending one 16 KiB
    // fragment per RPC cannot finish a 512 KiB checkpoint in this bound;
    // one four-fragment credit window can.
    bus.snapshot_rpc_delay_ms.store(350, Ordering::Relaxed);
    nodes[0]
        .raft
        .add_learner(2, BasicNode::new("2"), true)
        .await
        .expect("relay snapshot learner admission failed");
    let caught_up = tokio::time::timeout(Duration::from_secs(14), async {
        loop {
            if nodes[1].committed().await.revision == 12 {
                break;
            }
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
    })
    .await;
    assert!(
        caught_up.is_ok(),
        "relay snapshot did not use the four-chunk credit window; RPCs={}",
        bus.snapshot_rpc_count.load(Ordering::Relaxed)
    );
    assert!(
        bus.snapshot_rpc_count.load(Ordering::Relaxed) <= 10,
        "relay snapshot exceeded four-fragment RPC windows"
    );
    stop(nodes).await;
}

#[tokio::test]
async fn snapshot_preserves_removed_member_provenance_for_new_replica() {
    let (_, nodes) = cluster(2).await;
    assert_eq!(nodes[0].dispatch(2, "remove", &[0]).await.unwrap(), vec![1]);
    tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            if nodes[0].applied_member_ids().await == BTreeSet::from([1]) {
                break;
            }
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
    })
    .await
    .unwrap();

    let mut source = nodes[0].store.clone();
    let snapshot = source.build_snapshot().await.unwrap();
    let mut restored = Store::default();
    restored
        .install_snapshot(&snapshot.meta, snapshot.snapshot)
        .await
        .unwrap();
    let (current, history) = restored.applied_membership_provenance().await;
    assert_eq!(current, BTreeSet::from([1]));
    assert_eq!(history, BTreeSet::from([1, 2]));
    stop(nodes).await;
}
