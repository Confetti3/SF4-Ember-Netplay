//! A server-owned room's coordination: its host is the only voter and every
//! other node is a learner that takes the log from the host and nobody else.
use openraft::Membership;

use super::*;

async fn node(bus: &Arc<Bus>, id: u64, ownership: Ownership) -> Arc<Coordinator> {
    let node = Arc::new(
        Coordinator::new_owned(
            [0; 16],
            id,
            Arc::new(TestNetwork {
                source: id,
                bus: bus.clone(),
            }),
            ownership,
        )
        .await
        .unwrap(),
    );
    bus.peers.lock().await.insert(id, Arc::downgrade(&node));
    node
}

/// A host that has bootstrapped itself as the only voter.
async fn host(bus: &Arc<Bus>) -> Arc<Coordinator> {
    let host = node(bus, 1, Ownership::Host).await;
    host.raft
        .initialize(BTreeMap::from([(1, BasicNode::new("1"))]))
        .await
        .unwrap();
    host.raft
        .wait(Some(Duration::from_secs(5)))
        .current_leader(1, "host leads")
        .await
        .unwrap();
    host
}

async fn add_learner(host: &Coordinator, id: u64) {
    host.raft
        .add_learner(id, BasicNode::new(id.to_string()), true)
        .await
        .unwrap();
}

async fn committed_revision(node: &Coordinator, revision: u64, why: &str) {
    tokio::time::timeout(Duration::from_secs(15), async {
        while node.committed().await.revision != revision {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
    })
    .await
    .unwrap_or_else(|_| panic!("{why}"));
}

#[test]
fn ownership_decides_who_may_lead_and_how_large_the_room_is() {
    let member = Ownership::Member { host: 1 };
    assert_eq!(Ownership::Private.max_nodes(), MAX_MEMBERS);
    assert_eq!(Ownership::Host.max_nodes(), MAX_MEMBERS + 1);
    assert_eq!(member.max_nodes(), MAX_MEMBERS + 1);
    assert!(Ownership::Private.campaigns() && Ownership::Host.campaigns());
    assert!(!member.campaigns());

    // A member serves its host's appends and snapshots and nobody else's,
    // and never grants a vote.
    for method in ["append", "snapshot", "authority"] {
        assert!(member.serves(1, method), "{method} from the host");
        assert!(!member.serves(2, method), "{method} from another node");
    }
    assert!(!member.serves(1, "vote"));
    // Nothing leads, or is asked to campaign at, a host.
    for method in ["append", "snapshot", "vote", "elect"] {
        assert!(!Ownership::Host.serves(2, method), "{method}");
    }
    for method in ["authority", "propose", "remove"] {
        assert!(Ownership::Host.serves(2, method), "{method}");
    }
    assert!(Ownership::Private.serves(2, "vote") && Ownership::Private.serves(2, "append"));

    let only = BTreeSet::from([1]);
    assert!(Ownership::Host.permits_voters(1, &only));
    assert!(!Ownership::Host.permits_voters(1, &BTreeSet::from([1, 2])));
    assert!(!Ownership::Host.permits_voters(1, &BTreeSet::from([2])));
    assert!(!member.permits_voters(2, &BTreeSet::from([2])));
    assert!(Ownership::Private.permits_voters(1, &BTreeSet::from([1, 2, 3])));
}

#[tokio::test]
async fn the_host_stays_the_only_voter_as_sixteen_learners_join() {
    let bus = Arc::new(Bus::default());
    let host = host(&bus).await;
    let mut nodes = vec![host.clone()];
    for id in 2..=17u64 {
        let member = node(&bus, id, Ownership::Member { host: 1 }).await;
        add_learner(&host, id).await;
        nodes.push(member);
        assert_eq!(host.applied_voter_ids().await, BTreeSet::from([1]));
        assert_eq!(host.applied_member_ids().await.len(), nodes.len());
    }
    // Seventeen nodes: one more than a private room may hold. Every learner
    // took the membership and now takes a commit.
    assert!(
        host.propose(proposal(&host, "full", 0, "sixteen players"))
            .await
            .unwrap()
            .accepted
    );
    for member in &nodes[1..] {
        committed_revision(member, 1, "a learner missed the commit").await;
        assert_eq!(member.applied_member_ids().await.len(), 17);
        assert_eq!(member.applied_voter_ids().await, BTreeSet::from([1]));
        assert_eq!(member.current_leader(), Some(1));
    }
    stop(nodes).await;
}

#[tokio::test]
async fn only_a_server_owned_room_takes_a_membership_of_seventeen_nodes() {
    let bus = Arc::new(Bus::default());
    let membership = |count: u64| {
        let ids = (1..=count).collect::<BTreeSet<_>>();
        let nodes = ids
            .iter()
            .map(|id| (*id, BasicNode::new(id.to_string())))
            .collect::<BTreeMap<_, _>>();
        Membership::new(vec![ids], nodes)
    };
    let append = |nodes: u64| {
        let log_id = LogId::new(openraft::CommittedLeaderId::new(1, 1), 0);
        let request = AppendEntriesRequest::<RoomTypes> {
            vote: Vote::new_committed(1, 1),
            prev_log_id: None,
            entries: vec![Entry {
                log_id,
                payload: EntryPayload::Membership(membership(nodes)),
            }],
            leader_commit: None,
        };
        serde_json::to_vec(&CompressedJsonWire::encode(&request).unwrap()).unwrap()
    };
    let private = node(&bus, 20, Ownership::Private).await;
    let member = node(&bus, 21, Ownership::Member { host: 1 }).await;
    // The same entry, from the same leader: sixteen nodes fit everywhere,
    // seventeen only where the host is not one of the members.
    assert!(private.dispatch(1, "append", &append(16)).await.is_ok());
    assert!(private.dispatch(1, "append", &append(17)).await.is_err());
    assert!(member.dispatch(1, "append", &append(17)).await.is_ok());

    // A snapshot's membership is bounded the same way.
    let snapshot = |nodes: u64| {
        let log_id = LogId::new(openraft::CommittedLeaderId::new(2, 1), 2);
        serde_json::to_vec(&InstallSnapshotWire::encode(InstallSnapshotRequest {
            vote: Vote::new_committed(2, 1),
            meta: SnapshotMeta {
                last_log_id: Some(log_id),
                last_membership: StoredMembership::new(Some(log_id), membership(nodes)),
                snapshot_id: "seventeen".into(),
            },
            offset: 0,
            data: vec![1, 2, 3],
            done: false,
        }))
        .unwrap()
    };
    assert!(
        private
            .dispatch(1, "snapshot", &snapshot(17))
            .await
            .is_err()
    );
    assert!(member.dispatch(1, "snapshot", &snapshot(17)).await.is_ok());

    // The largest body of each kind still fits its transport bound with the
    // seventeenth node.
    let high = u64::MAX;
    let ids = (0..=MAX_MEMBERS as u64)
        .map(|index| high - index)
        .collect::<BTreeSet<_>>();
    let nodes = ids
        .iter()
        .map(|id| (*id, BasicNode::new("f".repeat(128))))
        .collect::<BTreeMap<_, _>>();
    let log_id = LogId::new(openraft::CommittedLeaderId::new(high, high), high);
    let largest = serde_json::to_vec(&InstallSnapshotWire::encode(InstallSnapshotRequest {
        vote: Vote::new_committed(high, high),
        meta: SnapshotMeta {
            last_log_id: Some(log_id),
            last_membership: StoredMembership::new(
                Some(log_id),
                Membership::new(vec![ids.clone(), ids.clone()], nodes),
            ),
            snapshot_id: format!("{:?}", Some(log_id)),
        },
        offset: high,
        data: vec![u8::MAX; SNAPSHOT_FRAGMENT_BYTES * SNAPSHOT_CREDIT_WINDOW],
        done: true,
    }))
    .unwrap();
    assert!(largest.len() <= MAX_SNAPSHOT_REQUEST, "{}", largest.len());
    let claim = serde_json::to_vec(&AuthorityClaim {
        incarnation: high,
        term: high,
        leader: Some(high),
        revision: high,
        voters: ids.clone(),
        members: ids,
    })
    .unwrap();
    assert!(claim.len() <= MAX_RPC_RESPONSE, "{}", claim.len());
    stop(vec![private, member]).await;
}

/// Learners receive a hostile append, snapshot and vote request. The same
/// requests move an ordinary (private) learner, so a refusal below is the
/// ownership check and not a malformed body.
#[tokio::test]
async fn a_member_takes_nothing_from_a_node_that_is_not_its_host() {
    let bus = Arc::new(Bus::default());
    // The private baseline accepts hostile terms. Give it a separate host:
    // otherwise its higher-term replies race back into the server-owned room
    // and invalidate the second proposal before we can check the member.
    let control_bus = Arc::new(Bus::default());
    let control_host = host(&control_bus).await;
    let host = host(&bus).await;
    let member = node(&bus, 2, Ownership::Member { host: 1 }).await;
    let control = node(&control_bus, 3, Ownership::Private).await;
    add_learner(&host, 2).await;
    add_learner(&control_host, 3).await;
    let term = host.current_term();
    let hostile_term = term.max(control.current_term()) + 5;
    // The member follows the host.
    assert!(
        host.propose(proposal(&host, "first", 0, "state"))
            .await
            .unwrap()
            .accepted
    );
    committed_revision(&member, 1, "the member did not take the host's commit").await;

    let append = |source: u64, term: u64| {
        let request = AppendEntriesRequest::<RoomTypes> {
            vote: Vote::new_committed(term, source),
            prev_log_id: None,
            entries: vec![],
            leader_commit: None,
        };
        serde_json::to_vec(&CompressedJsonWire::encode(&request).unwrap()).unwrap()
    };
    let snapshot = |source: u64, term: u64| {
        let log_id = LogId::new(openraft::CommittedLeaderId::new(term, source), 1);
        let membership = Membership::new(
            vec![BTreeSet::from([source])],
            BTreeMap::from([(source, BasicNode::new("x"))]),
        );
        serde_json::to_vec(&InstallSnapshotWire::encode(InstallSnapshotRequest {
            vote: Vote::new_committed(term, source),
            meta: SnapshotMeta {
                last_log_id: Some(log_id),
                last_membership: StoredMembership::new(Some(log_id), membership),
                snapshot_id: "hostile".into(),
            },
            offset: 0,
            data: vec![1, 2, 3],
            done: false,
        }))
        .unwrap()
    };
    let last = control.raft.metrics().borrow().last_log_index;
    let last_log_id = log_id_at(&control, last).await;
    let vote =
        |term: u64| serde_json::to_vec(&VoteRequest::new(Vote::new(term, 1), last_log_id)).unwrap();

    // Another node claims leadership with a higher term.
    assert!(
        member
            .dispatch(4, "append", &append(4, hostile_term))
            .await
            .is_err()
    );
    assert!(
        control
            .dispatch(4, "append", &append(4, hostile_term))
            .await
            .is_ok()
    );
    assert!(
        member
            .dispatch(4, "snapshot", &snapshot(4, hostile_term + 1))
            .await
            .is_err()
    );
    assert!(
        control
            .dispatch(4, "snapshot", &snapshot(4, hostile_term + 1))
            .await
            .is_ok()
    );
    // Even the host's own candidacy gets no vote.
    assert!(
        member
            .dispatch(1, "vote", &vote(hostile_term + 2))
            .await
            .is_err()
    );
    assert!(
        control
            .dispatch(1, "vote", &vote(hostile_term + 2))
            .await
            .is_ok()
    );

    // None of it moved the member: same term, same leader, still replicating.
    assert_eq!(member.current_term(), term);
    assert_eq!(member.current_leader(), Some(1));
    assert!(control.current_term() > term, "the baseline learner moved");
    assert!(
        host.propose(proposal(&host, "second", 1, "state again"))
            .await
            .unwrap()
            .accepted
    );
    committed_revision(&member, 2, "the member stopped following the host").await;
    stop(vec![host, member, control, control_host]).await;
}

/// A member is bootstrapped by the host's snapshot when it joins after the
/// host compacted its log.
#[tokio::test]
async fn a_member_installs_the_snapshot_its_host_sends() {
    let bus = Arc::new(Bus::default());
    let host = host(&bus).await;
    for revision in 0..12 {
        let request = format!("snapshot-{revision}");
        assert!(
            host.propose(proposal(&host, &request, revision, &"s".repeat(8 * 1024)))
                .await
                .unwrap()
                .accepted
        );
    }
    tokio::time::timeout(Duration::from_secs(5), async {
        while host.store.0.lock().await.purged.is_none() {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
    })
    .await
    .expect("the host did not compact a snapshot");
    let member = node(&bus, 2, Ownership::Member { host: 1 }).await;
    add_learner(&host, 2).await;
    committed_revision(&member, 12, "the snapshot never reached the member").await;
    assert_eq!(member.applied_voter_ids().await, BTreeSet::from([1]));
    stop(vec![host, member]).await;
}

/// Two voters, the leader then cut off. A private follower campaigns after
/// its election timeout; the same node as a member of a server-owned room
/// never does, even when it holds a vote in the log.
#[tokio::test]
async fn a_member_never_campaigns_when_the_host_goes_silent() {
    async fn pair(ownership: Ownership) -> (Arc<Bus>, Vec<Arc<Coordinator>>, u64) {
        let bus = Arc::new(Bus::default());
        let first = node(&bus, 1, Ownership::Private).await;
        let second = node(&bus, 2, ownership).await;
        first
            .raft
            .initialize(BTreeMap::from([(1, BasicNode::new("1"))]))
            .await
            .unwrap();
        first
            .raft
            .wait(Some(Duration::from_secs(5)))
            .current_leader(1, "leader")
            .await
            .unwrap();
        // The second node holds a vote, as a private follower does. A member
        // never grants a vote, so it can only become a voter after the host
        // is already leading.
        add_learner(&first, 2).await;
        first
            .raft
            .change_membership(BTreeSet::from([1, 2]), true)
            .await
            .unwrap();
        second
            .raft
            .wait(Some(Duration::from_secs(5)))
            .current_leader(1, "follower")
            .await
            .unwrap();
        while second.applied_voter_ids().await != BTreeSet::from([1, 2]) {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
        let term = second.current_term();
        bus.isolated.lock().await.insert(1);
        (bus, vec![first, second], term)
    }
    let ((_, private, term_a), (_, owned, term_b)) = tokio::join!(
        pair(Ownership::Private),
        pair(Ownership::Member { host: 1 })
    );
    // A follower of a committed leader campaigns once the leader lease (12
    // seconds) and its election timeout (10 to 12) have passed and the next
    // tick (every 13.5 seconds) looks. Wait for the private one to, then
    // look at the member over the same time.
    assert!(!owned[1].raft.config().enable_elect);
    tokio::time::timeout(Duration::from_secs(75), async {
        while private[1].current_term() == term_a {
            tokio::time::sleep(Duration::from_millis(250)).await;
        }
    })
    .await
    .expect("the private follower never campaigned, so this test shows nothing");
    assert_eq!(owned[1].current_term(), term_b);
    assert_eq!(owned[1].current_leader(), Some(1));
    stop(private).await;
    stop(owned).await;
}
