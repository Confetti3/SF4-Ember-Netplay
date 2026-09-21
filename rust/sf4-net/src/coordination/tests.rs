use super::*;
use serde_json::json;
use std::{
    collections::BTreeSet,
    sync::{
        Weak,
        atomic::{AtomicU64, Ordering},
    },
    time::Duration,
};

#[derive(Default)]
struct Bus {
    peers: Mutex<BTreeMap<u64, Weak<Coordinator>>>,
    isolated: Mutex<BTreeSet<u64>>,
    append_delay_ms: AtomicU64,
    snapshot_delay_ms: AtomicU64,
    snapshot_rpc_delay_ms: AtomicU64,
    snapshot_rpc_count: AtomicU64,
}
struct TestNetwork {
    source: u64,
    bus: Arc<Bus>,
}
impl RpcTransport for TestNetwork {
    fn call(
        &self,
        target: u64,
        _: String,
        method: &'static str,
        body: Vec<u8>,
    ) -> Pin<Box<dyn Future<Output = io::Result<Vec<u8>>> + Send + '_>> {
        Box::pin(async move {
            let isolated = self.bus.isolated.lock().await;
            if isolated.contains(&target) || isolated.contains(&self.source) {
                return Err(io::Error::other("test partition"));
            }
            drop(isolated);
            if method == "append" {
                let delay = self.bus.append_delay_ms.load(Ordering::Relaxed);
                if delay != 0 {
                    tokio::time::sleep(Duration::from_millis(delay)).await;
                }
            } else if method == "snapshot" {
                self.bus.snapshot_rpc_count.fetch_add(1, Ordering::Relaxed);
                let rpc_delay = self.bus.snapshot_rpc_delay_ms.load(Ordering::Relaxed);
                if rpc_delay != 0 {
                    tokio::time::sleep(Duration::from_millis(rpc_delay)).await;
                }
                if body
                    .windows(b"\"offset\":0".len())
                    .any(|window| window == b"\"offset\":0")
                {
                    let delay = self.bus.snapshot_delay_ms.load(Ordering::Relaxed);
                    if delay != 0 {
                        tokio::time::sleep(Duration::from_millis(delay)).await;
                    }
                }
            }
            let peer = self
                .bus
                .peers
                .lock()
                .await
                .get(&target)
                .and_then(Weak::upgrade)
                .ok_or_else(|| io::Error::other("test peer stopped"))?;
            peer.dispatch(self.source, method, &body).await
        })
    }
}
async fn cluster(count: u64) -> (Arc<Bus>, Vec<Arc<Coordinator>>) {
    let bus = Arc::new(Bus::default());
    let mut nodes = Vec::new();
    for id in 1..=count {
        let node = Arc::new(
            Coordinator::new(
                id,
                Arc::new(TestNetwork {
                    source: id,
                    bus: bus.clone(),
                }),
            )
            .await
            .unwrap(),
        );
        bus.peers.lock().await.insert(id, Arc::downgrade(&node));
        nodes.push(node);
    }
    nodes[0]
        .raft
        .initialize(
            (1..=count)
                .map(|id| (id, BasicNode::new(id.to_string())))
                .collect::<BTreeMap<_, _>>(),
        )
        .await
        .unwrap();
    nodes[0]
        .raft
        .wait(Some(Duration::from_secs(5)))
        .current_leader(1, "initial leader")
        .await
        .unwrap();
    (bus, nodes)
}

async fn wait_for_successor(nodes: &[Arc<Coordinator>], context: &str) -> usize {
    let leader = tokio::time::timeout(Duration::from_secs(15), async {
        'election: loop {
            for node in &nodes[1..] {
                if let Some(leader) = node.raft.metrics().borrow().current_leader
                    && leader != 1
                {
                    break 'election leader;
                }
            }
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
    })
    .await
    .unwrap_or_else(|_| panic!("{context} did not elect a successor"));
    nodes
        .iter()
        .position(|node| node.incarnation == leader)
        .unwrap_or_else(|| panic!("{context} elected unknown successor {leader}"))
}

fn proposal(node: &Coordinator, request: &str, base: u64, data: &str) -> Proposal {
    Proposal {
        request: request.into(),
        dedup_id: format!("test:{request}"),
        term: node.raft.metrics().borrow().current_term,
        base,
        checkpoint: data.into(),
        admin: None,
    }
}

#[test]
fn probe_binding_requires_committed_fighter_pair_and_exact_revision() {
    let checkpoint = json!({
        "version": 1,
        "request": 9,
        "term": 4,
        "base_revision": 8,
        "checkpoint": {
            "members": [
                {"member": 1, "incarnation": 11, "data": {"authenticatedEndpoint": "source"}},
                {"member": 2, "incarnation": 22, "data": {"authenticatedEndpoint": "target"}},
                {"member": 3, "incarnation": 33, "data": {"authenticatedEndpoint": "spectator"}}
            ],
            "room": {
                "version": 2,
                "snapshot": {
                    "members": [
                        {"id": 1, "fighter": 3},
                        {"id": 2, "fighter": 7},
                        {"id": 3, "fighter": 12}
                    ],
                    "tables": [
                        {"revision": 42, "p1": 1, "p2": 2},
                        {"revision": 43, "p1": 1, "p2": 3}
                    ]
                }
            }
        }
    });
    let encoded = checkpoint.to_string();
    assert!(committed_probe_pair_bound(
        &encoded, 11, 22, "source", "target", 42
    ));
    // The player UI offers a connection check before Ready publishes a
    // fighter selection. The authenticated occupied seats are sufficient.
    let mut before_ready = checkpoint.clone();
    before_ready["checkpoint"]["room"]["snapshot"]["members"][0]["fighter"] = json!(-1);
    before_ready["checkpoint"]["room"]["snapshot"]["members"][1]["fighter"] = json!(-1);
    assert!(committed_probe_pair_bound(
        &before_ready.to_string(),
        11,
        22,
        "source",
        "target",
        42
    ));
    assert!(!committed_probe_pair_bound(
        &encoded, 11, 22, "source", "target", 43
    ));
    assert!(!committed_probe_pair_bound(
        &encoded,
        11,
        33,
        "source",
        "spectator",
        42
    ));
    assert!(!committed_probe_pair_bound(
        &encoded,
        11,
        22,
        "stale-source",
        "target",
        42
    ));
    assert!(!committed_probe_pair_bound(
        &encoded, 44, 22, "source", "target", 42
    ));
}

#[test]
fn compressed_coordination_wire_is_versioned_bounded_and_verified() {
    let payload = "room-checkpoint".repeat(32 * 1024);
    let wire = CompressedJsonWire::encode(&payload).unwrap();
    let encoded = serde_json::to_vec(&wire).unwrap();
    assert!(encoded.len() < payload.len() / 4);
    assert_eq!(wire.decode::<String>().unwrap(), payload);

    let mut corrupted = CompressedJsonWire::encode(&payload).unwrap();
    corrupted.digest[0] ^= 0x80;
    assert!(corrupted.decode::<String>().is_err());
}
async fn stop(nodes: Vec<Arc<Coordinator>>) {
    for node in nodes {
        node.raft.shutdown().await.unwrap();
    }
}

#[tokio::test]
async fn simultaneous_authority_claims_share_one_fresh_read_barrier() {
    let (_bus, nodes) = cluster(3).await;
    nodes[0].authority_barriers.store(0, Ordering::Relaxed);
    let mut claims = tokio::task::JoinSet::new();
    for _ in 0..MAX_MEMBERS {
        let leader = nodes[0].clone();
        claims.spawn(async move { leader.authority_claim().await.unwrap() });
    }
    while let Some(claim) = claims.join_next().await {
        let claim = claim.unwrap();
        assert_eq!(claim.incarnation, 1);
        assert_eq!(claim.leader, Some(1));
    }
    assert_eq!(nodes[0].authority_barriers.load(Ordering::Relaxed), 1);
    stop(nodes).await;
}

#[tokio::test]
async fn commitment_deduplication_and_checkpoint_transfer() {
    let (_, nodes) = cluster(1).await;
    let request = proposal(&nodes[0], "one", 0, "complete private room state");
    let first = nodes[0].propose(request.clone()).await.unwrap();
    assert!(first.accepted && first.revision == 1);
    assert_eq!(nodes[0].propose(request).await.unwrap(), first);
    assert!(
        !nodes[0]
            .propose(proposal(&nodes[0], "one", 0, "different candidate"))
            .await
            .unwrap()
            .accepted
    );
    assert!(
        !nodes[0]
            .propose(proposal(&nodes[0], "stale", 0, "wrong"))
            .await
            .unwrap()
            .accepted
    );
    let mut store = nodes[0].store.clone();
    let snapshot = store.build_snapshot().await.unwrap();
    let mut corrupted = snapshot.snapshot.get_ref().clone();
    let last = corrupted.len() - 1;
    corrupted[last] ^= 0x80;
    let mut rejected = Store::default();
    assert!(
        rejected
            .install_snapshot(&snapshot.meta, Box::new(Cursor::new(corrupted)))
            .await
            .is_err()
    );
    assert_eq!(rejected.committed().await.revision, 0);
    let mut restored = Store::default();
    restored
        .install_snapshot(&snapshot.meta, snapshot.snapshot)
        .await
        .unwrap();
    assert_eq!(
        restored.committed().await.checkpoint,
        "complete private room state"
    );
    assert_eq!(restored.committed().await.revision, 1);
    stop(nodes).await;
}

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
async fn two_member_partition_never_commits_a_second_host() {
    let (bus, nodes) = cluster(2).await;
    assert_eq!(nodes[0].applied_voter_ids().await, BTreeSet::from([1, 2]));
    assert_eq!(nodes[1].applied_voter_ids().await, BTreeSet::from([1, 2]));
    assert!(
        nodes[0]
            .propose(proposal(&nodes[0], "initial", 0, "host one"))
            .await
            .unwrap()
            .accepted
    );
    bus.isolated.lock().await.insert(1);
    let attempted = tokio::time::timeout(
        Duration::from_millis(350),
        nodes[0].propose(proposal(&nodes[0], "partitioned", 1, "invalid")),
    )
    .await;
    assert!(attempted.is_err() || attempted.unwrap().is_err());
    assert_eq!(nodes[0].committed().await.revision, 1);
    assert_eq!(nodes[1].committed().await.revision, 1);
    stop(nodes).await;
}

#[tokio::test]
async fn admitted_voter_can_commit_its_own_non_native_departure() {
    let (_, nodes) = cluster(2).await;
    assert_eq!(nodes[0].applied_voter_ids().await, BTreeSet::from([1, 2]));
    let response = nodes[0].dispatch(2, "remove", &[0]).await.unwrap();
    assert_eq!(response, vec![1]);
    tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            if nodes[0].applied_voter_ids().await == BTreeSet::from([1]) {
                break;
            }
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
    })
    .await
    .unwrap();
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

#[tokio::test]
async fn coordination_only_learner_is_removed_before_departure_proof() {
    let (bus, nodes) = cluster(2).await;
    let learner = Arc::new(
        Coordinator::new(
            3,
            Arc::new(TestNetwork {
                source: 3,
                bus: bus.clone(),
            }),
        )
        .await
        .unwrap(),
    );
    bus.peers.lock().await.insert(3, Arc::downgrade(&learner));
    nodes[0]
        .raft
        .add_learner(3, BasicNode::new("3"), true)
        .await
        .unwrap();
    tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            let voters = nodes[0].applied_voter_ids().await;
            let members = nodes[0].applied_member_ids().await;
            if voters == BTreeSet::from([1, 2]) && members.contains(&3) {
                break;
            }
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
    })
    .await
    .unwrap();

    // A native Join rejection leaves the helper as an authenticated
    // learner. Its authenticated self-removal must use RemoveNodes,
    // rather than the voter-only replacement transition.
    assert_eq!(nodes[0].dispatch(3, "remove", &[0]).await.unwrap(), vec![1]);
    tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            if !nodes[0].applied_member_ids().await.contains(&3) {
                break;
            }
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
    })
    .await
    .unwrap();
    learner.raft.shutdown().await.unwrap();
    stop(nodes).await;
}
#[tokio::test]
async fn majority_recovers_and_old_leader_is_fenced() {
    let (bus, nodes) = cluster(3).await;
    assert!(
        nodes[0]
            .propose(proposal(&nodes[0], "initial", 0, "live match generation 9"))
            .await
            .unwrap()
            .accepted
    );
    bus.isolated.lock().await.insert(1);
    nodes[1].raft.trigger().elect().await.unwrap();
    let successor = wait_for_successor(&nodes, "majority recovery").await;
    assert_eq!(
        nodes[successor].committed().await.checkpoint,
        "live match generation 9"
    );
    assert!(
        nodes[successor]
            .propose(proposal(
                &nodes[successor],
                "recovered",
                1,
                "same match generation 9"
            ))
            .await
            .unwrap()
            .accepted
    );
    bus.isolated.lock().await.clear();
    nodes[0]
        .raft
        .wait(Some(Duration::from_secs(8)))
        .current_leader(nodes[successor].incarnation, "old host demoted")
        .await
        .unwrap();
    assert!(
        nodes[0]
            .propose(proposal(&nodes[0], "old-host", 2, "invalid"))
            .await
            .is_err()
    );
    stop(nodes).await;
}
#[tokio::test]
async fn sixteen_members_commit_with_one_failed_host() {
    let (bus, nodes) = cluster(16).await;
    assert!(
        nodes[0]
            .propose(proposal(&nodes[0], "initial", 0, "four tables"))
            .await
            .unwrap()
            .accepted
    );
    bus.isolated.lock().await.insert(1);
    nodes[1].raft.trigger().elect().await.unwrap();
    let successor = wait_for_successor(&nodes, "sixteen-member recovery").await;
    assert!(
        nodes[successor]
            .propose(proposal(
                &nodes[successor],
                "recovered",
                1,
                "four tables preserved"
            ))
            .await
            .unwrap()
            .accepted
    );
    stop(nodes).await;
}
