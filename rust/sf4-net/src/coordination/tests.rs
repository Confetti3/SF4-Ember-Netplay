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

#[test]
fn the_largest_rpc_bodies_fit_their_transport_bounds() {
    let high = u64::MAX;
    let log_id = LogId::new(openraft::CommittedLeaderId::new(high, high), high);
    let vote = serde_json::to_vec(&VoteRequest::new(Vote::new(high, high), Some(log_id))).unwrap();
    assert!(
        vote.len() <= MAX_VOTE_REQUEST,
        "vote request {}",
        vote.len()
    );

    // A remote proposal is a probe reservation; its identifiers are capped
    // at 128 bytes, which JSON can escape to six bytes each.
    let escaped = "\u{1}".repeat(128);
    let propose = serde_json::to_vec(&Proposal {
        request: escaped.clone(),
        dedup_id: escaped,
        term: high,
        base: high,
        checkpoint: String::new(),
        admin: Some(AdminEntry::ProbeReservation(ProbeReservation {
            room: [u8::MAX; 16],
            source_incarnation: high,
            target_incarnation: high,
            request: high,
            pair_revision: high,
            term: high,
            expires: high,
        })),
    })
    .unwrap();
    assert!(
        propose.len() <= MAX_PROPOSE_REQUEST,
        "propose {}",
        propose.len()
    );

    // A full credit window of snapshot data, with the metadata of a joint
    // configuration that names every member.
    let ids = (0..MAX_MEMBERS as u64)
        .map(|index| high - index)
        .collect::<BTreeSet<_>>();
    let nodes = ids
        .iter()
        .map(|id| (*id, BasicNode::new("f".repeat(128))))
        .collect::<BTreeMap<_, _>>();
    let membership = openraft::Membership::new(vec![ids.clone(), ids.clone()], nodes);
    let snapshot = serde_json::to_vec(&InstallSnapshotWire::encode(InstallSnapshotRequest {
        vote: Vote::new_committed(high, high),
        meta: SnapshotMeta {
            last_log_id: Some(log_id),
            last_membership: StoredMembership::new(Some(log_id), membership),
            snapshot_id: format!("{:?}", Some(log_id)),
        },
        offset: high,
        data: vec![u8::MAX; SNAPSHOT_FRAGMENT_BYTES * SNAPSHOT_CREDIT_WINDOW],
        done: true,
    }))
    .unwrap();
    assert!(
        snapshot.len() <= MAX_SNAPSHOT_REQUEST,
        "snapshot {}",
        snapshot.len()
    );

    let responses = [
        serde_json::to_vec(&AuthorityClaim {
            incarnation: high,
            term: high,
            leader: Some(high),
            revision: high,
            voters: ids.clone(),
            members: ids,
        })
        .unwrap(),
        serde_json::to_vec(&AppendEntriesResponse::<u64>::HigherVote(
            Vote::new_committed(high, high),
        ))
        .unwrap(),
        serde_json::to_vec(&AppendEntriesResponse::<u64>::PartialSuccess(Some(log_id))).unwrap(),
        serde_json::to_vec(&VoteResponse::new(
            Vote::new_committed(high, high),
            Some(log_id),
            true,
        ))
        .unwrap(),
        serde_json::to_vec(&InstallSnapshotResponse {
            vote: Vote::new_committed(high, high),
        })
        .unwrap(),
        serde_json::to_vec(&Receipt {
            accepted: true,
            revision: high,
        })
        .unwrap(),
    ];
    for response in responses {
        assert!(
            response.len() <= MAX_RPC_RESPONSE,
            "response {}",
            response.len()
        );
    }
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
async fn a_follower_announces_each_commit_and_reads_its_checkpoint_once() {
    let (_bus, nodes) = cluster(3).await;
    let mut commits = nodes[1].committed_changes().await;
    let (empty, checkpoint) = nodes[1].committed_unless(None).await;
    assert_eq!((empty.revision, checkpoint.as_deref()), (0, Some("")));
    assert!(
        nodes[0]
            .propose(proposal(&nodes[0], "one", 0, "first state"))
            .await
            .unwrap()
            .accepted
    );
    // The follower applies on its own task; nothing polls it.
    tokio::time::timeout(Duration::from_secs(5), async {
        while *commits.borrow_and_update() != 1 {
            commits.changed().await.unwrap();
        }
    })
    .await
    .expect("the follower's commit was announced");
    let (mark, checkpoint) = nodes[1].committed_unless(Some(&empty)).await;
    assert_eq!(mark.revision, 1);
    assert_eq!(mark.request, "one");
    assert_eq!(checkpoint.as_deref(), Some("first state"));
    assert_eq!(nodes[1].committed_unless(Some(&mark)).await, (mark, None));

    // An installed snapshot announces its revision too.
    let mut store = nodes[0].store.clone();
    let snapshot = store.build_snapshot().await.unwrap();
    let mut restored = Store::default();
    let mut restored_commits = restored.committed_changes().await;
    restored
        .install_snapshot(&snapshot.meta, snapshot.snapshot)
        .await
        .unwrap();
    assert!(restored_commits.has_changed().unwrap());
    assert_eq!(*restored_commits.borrow_and_update(), 1);
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

/// The id of the entry at `index` in `node`'s log, if it is still held.
async fn log_id_at(node: &Coordinator, index: Option<u64>) -> Option<LogId<u64>> {
    let index = index?;
    node.store
        .0
        .lock()
        .await
        .log
        .get(&index)
        .map(|entry| entry.log_id)
}

#[tokio::test]
async fn only_a_voter_asks_for_a_vote_and_only_as_a_candidate() {
    let (bus, nodes) = cluster(3).await;
    let learner = Arc::new(
        Coordinator::new(
            4,
            Arc::new(TestNetwork {
                source: 4,
                bus: bus.clone(),
            }),
        )
        .await
        .unwrap(),
    );
    bus.peers.lock().await.insert(4, Arc::downgrade(&learner));
    nodes[0]
        .raft
        .add_learner(4, BasicNode::new("4"), true)
        .await
        .unwrap();
    let follower = &nodes[1];
    tokio::time::timeout(Duration::from_secs(5), async {
        while !follower.applied_member_ids().await.contains(&4) {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
    })
    .await
    .unwrap();
    let term = follower.current_term();
    let last = follower.raft.metrics().borrow().last_log_index;
    let last_log_id = log_id_at(follower, last).await;
    let request =
        |vote: Vote<u64>| serde_json::to_vec(&VoteRequest::new(vote, last_log_id)).unwrap();

    // A learner is not a candidate.
    assert!(
        follower
            .dispatch(4, "vote", &request(Vote::new(term + 5, 4)))
            .await
            .is_err()
    );
    // A voter campaigns with the uncommitted vote of a candidate; a request
    // carrying a committed vote is not one.
    assert!(
        follower
            .dispatch(3, "vote", &request(Vote::new_committed(term + 5, 3)))
            .await
            .is_err()
    );
    assert_eq!(follower.current_term(), term);
    assert_eq!(follower.current_leader(), Some(1));
    // A voter's candidate request is still answered.
    assert!(
        follower
            .dispatch(3, "vote", &request(Vote::new(term, 3)))
            .await
            .is_ok()
    );
    assert!(
        nodes[0]
            .propose(proposal(&nodes[0], "after", 0, "leader checkpoint"))
            .await
            .unwrap()
            .accepted
    );
    learner.raft.shutdown().await.unwrap();
    stop(nodes).await;
}

/// A leader that commits its own removal still delivers that commit to the
/// followers that appended the change, and the survivors carry on.
#[tokio::test]
async fn a_leader_removing_itself_still_commits_and_hands_over() {
    let (bus, nodes) = cluster(3).await;
    nodes[0]
        .raft
        .change_membership(BTreeSet::from([2, 3]), false)
        .await
        .unwrap();
    tokio::time::timeout(Duration::from_secs(5), async {
        while nodes[1].applied_voter_ids().await != BTreeSet::from([2, 3])
            || nodes[2].applied_voter_ids().await != BTreeSet::from([2, 3])
        {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
    })
    .await
    .expect("followers never applied the leader's own removal");
    bus.isolated.lock().await.insert(1);
    nodes[1].raft.trigger().elect().await.unwrap();
    let successor = wait_for_successor(&nodes, "self-removal").await;
    let receipt = nodes[successor]
        .propose(proposal(&nodes[successor], "after-removal", 0, "state"))
        .await
        .unwrap();
    assert!(receipt.accepted);
    stop(nodes).await;
}

/// The helper's leave: the leader keeps itself as a learner behind a single
/// successor voter and asks that successor to elect itself at once.
#[tokio::test]
async fn a_singleton_handoff_elects_the_successor_and_replicates() {
    let (_, nodes) = cluster(3).await;
    nodes[0]
        .raft
        .change_membership(BTreeSet::from([2]), true)
        .await
        .unwrap();
    tokio::time::timeout(Duration::from_secs(5), async {
        while nodes[1].applied_voter_ids().await != BTreeSet::from([2]) {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
    })
    .await
    .expect("successor never applied the singleton membership");
    assert_eq!(nodes[1].dispatch(1, "elect", &[0]).await.unwrap(), vec![1]);
    nodes[1]
        .raft
        .wait(Some(Duration::from_secs(5)))
        .current_leader(2, "handoff successor")
        .await
        .unwrap();
    let receipt = nodes[1]
        .propose(proposal(&nodes[1], "after-handoff", 0, "handed over"))
        .await
        .unwrap();
    assert!(receipt.accepted);
    tokio::time::timeout(Duration::from_secs(5), async {
        while nodes[2].committed().await.checkpoint != "handed over" {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
    })
    .await
    .expect("the retained learner never applied the successor's commit");
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

#[tokio::test]
async fn an_abandoned_joint_membership_is_finished_without_its_old_quorum() {
    let (bus, nodes) = cluster(1).await;
    let mut learners = Vec::new();
    for id in [2, 3] {
        let learner = Arc::new(
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
        bus.peers.lock().await.insert(id, Arc::downgrade(&learner));
        nodes[0]
            .raft
            .add_learner(id, BasicNode::new(id.to_string()), true)
            .await
            .unwrap();
        learners.push(learner);
    }
    nodes[0]
        .raft
        .change_membership(BTreeSet::from([1, 2, 3]), true)
        .await
        .unwrap();
    // The caller of a shrink to one voter gives up before its joint entry
    // commits. The entry commits later and nothing proposes the uniform half.
    bus.append_delay_ms.store(400, Ordering::Relaxed);
    let goal = BTreeSet::from([1]);
    assert!(
        tokio::time::timeout(
            Duration::from_millis(100),
            nodes[0].raft.change_membership(goal.clone(), true),
        )
        .await
        .is_err()
    );
    tokio::time::timeout(Duration::from_secs(10), async {
        while nodes[0].applied_joint_goal().await.is_none() {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
    })
    .await
    .expect("the abandoned joint configuration commits");
    assert_eq!(nodes[0].applied_joint_goal().await, Some(goal.clone()));
    // Then the other two drop: the old half has lost its quorum, so a read
    // that needs both halves cannot finish it.
    bus.append_delay_ms.store(0, Ordering::Relaxed);
    bus.isolated.lock().await.extend([2, 3]);
    assert!(!matches!(
        tokio::time::timeout(Duration::from_secs(1), nodes[0].raft.ensure_linearizable()).await,
        Ok(Ok(_))
    ));
    assert!(
        tokio::time::timeout(Duration::from_secs(5), nodes[0].finish_joint_membership())
            .await
            .expect("the uniform half commits with the goal's own quorum")
            .unwrap()
    );
    assert_eq!(nodes[0].applied_joint_goal().await, None);
    assert_eq!(nodes[0].applied_voter_ids().await, goal);
    tokio::time::timeout(Duration::from_secs(5), nodes[0].raft.ensure_linearizable())
        .await
        .expect("the room commits again")
        .unwrap();
    assert!(!nodes[0].finish_joint_membership().await.unwrap());
    for learner in learners {
        let _ = learner.raft.shutdown().await;
    }
    stop(nodes).await;
}

// Relay-sized appends and snapshot transfer to new replicas.
mod transfer;
// Departed-member history and its bound.
mod history;
// Server-owned rooms: one voter, learners that follow the host only.
mod server_owned;
