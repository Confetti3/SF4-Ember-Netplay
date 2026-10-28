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
        method: Method,
        body: Vec<u8>,
    ) -> Pin<Box<dyn Future<Output = io::Result<Vec<u8>>> + Send + '_>> {
        Box::pin(async move {
            let isolated = self.bus.isolated.lock().await;
            if isolated.contains(&target) || isolated.contains(&self.source) {
                return Err(io::Error::other("test partition"));
            }
            drop(isolated);
            if method == Method::Append {
                let delay = self.bus.append_delay_ms.load(Ordering::Relaxed);
                if delay != 0 {
                    tokio::time::sleep(Duration::from_millis(delay)).await;
                }
            } else if method == Method::Snapshot {
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

/// A checkpoint of about `bytes` bytes shaped like a native one: JSON rows
/// of numbers and hex, so it compresses about as well as the real thing.
fn sample_checkpoint(bytes: usize) -> String {
    let mut state = 0x9e37_79b9_7f4a_7c15u64;
    let mut next = || {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        state
    };
    let mut journal = Vec::new();
    let mut length = 0;
    let mut frame = 0u64;
    while length < bytes {
        frame += 1 + next() % 3;
        let row = json!({
            "frame": frame,
            "member": next() % 16,
            "input": format!("{:016x}", next()),
            "state": format!("{:016x}{:016x}{:016x}", next(), next(), next()),
            "confirmed": next() % 2 == 0,
        });
        length += row.to_string().len() + 1;
        journal.push(row);
    }
    json!({"checkpoint": {"members": [], "room": {"snapshot": {"members": [], "tables": []}}, "journal": journal}})
        .to_string()
}

/// A checkpoint of about `bytes` bytes made of room snapshots that differ
/// in a few fields, as a room's per-recipient snapshots do; zlib shrinks
/// these about ten times, like the appends `docs/design/MATCH_WATCH.md`
/// measured.
fn alike_checkpoint(bytes: usize) -> String {
    let mut snapshots = Vec::new();
    let mut length = 0;
    let mut recipient = 0u64;
    while length < bytes {
        recipient += 1;
        let members = (0..8)
            .map(|member| {
                json!({
                    "id": member,
                    "name": format!("Player {member}"),
                    "fighter": (member * 7 + recipient) % 44,
                    "costume": member % 10,
                    "ready": member % 2 == 0,
                    "endpoint": format!("{:064x}", member * 0x1234_5678_9abc_def1u64),
                })
            })
            .collect::<Vec<_>>();
        let snapshot = json!({
            "recipient": recipient,
            "revision": 1000 + recipient,
            "snapshot": {"members": members, "tables": [{"revision": recipient, "p1": 1, "p2": 2}]},
        });
        length += snapshot.to_string().len() + 1;
        snapshots.push(snapshot);
    }
    json!({"checkpoint": {"members": [], "room": {"snapshots": snapshots}}}).to_string()
}

/// Wire 1's append body: the request as JSON, then zlib, blake3 and base64
/// in a JSON envelope. Kept here to measure against and to show a helper of
/// wire 2 refuses it.
fn wire_one_body<T: Serialize>(value: &T) -> Vec<u8> {
    use base64::{Engine as _, engine::general_purpose::URL_SAFE_NO_PAD};
    let raw = serde_json::to_vec(value).unwrap();
    let mut encoder = ZlibEncoder::new(Vec::new(), Compression::fast());
    encoder.write_all(&raw).unwrap();
    let compressed = encoder.finish().unwrap();
    serde_json::to_vec(&json!({
        "version": 1,
        "length": raw.len() as u64,
        "digest": blake3::hash(&raw).as_bytes(),
        "data": URL_SAFE_NO_PAD.encode(compressed),
    }))
    .unwrap()
}

#[test]
fn the_rpc_method_bytes_are_fixed() {
    let methods = [
        (Method::Append, 1, "append"),
        (Method::Vote, 2, "vote"),
        (Method::Snapshot, 3, "snapshot"),
        (Method::Authority, 4, "authority"),
        (Method::Propose, 5, "propose"),
        (Method::Elect, 6, "elect"),
        (Method::Remove, 7, "remove"),
    ];
    for (method, byte, name) in methods {
        assert_eq!(method.byte(), byte);
        assert_eq!(Method::from_byte(byte), Some(method));
        assert_eq!(method.name(), name);
    }
    for byte in [0, 8, 255] {
        assert_eq!(Method::from_byte(byte), None);
    }
}

#[test]
fn the_coordination_wire_is_versioned_bounded_and_verified() {
    let checkpoint = sample_checkpoint(64 * 1024);
    let request = append_request(6, &checkpoint);
    let body = encode_append(&request, &CompressedCheckpoints::default()).unwrap();
    assert_eq!(body[0], COORDINATION_WIRE_VERSION);
    assert!(body.len() < checkpoint.len() / 2, "{}", body.len());
    // The checkpoint is not in the JSON header.
    let header_length = u32::from_le_bytes(body[1..5].try_into().unwrap()) as usize;
    let header: serde_json::Value = serde_json::from_slice(&body[5..5 + header_length]).unwrap();
    assert_eq!(header["entries"][0]["payload"]["Normal"]["checkpoint"], json!(""));
    let decoded = decode_append(&body).unwrap();
    let EntryPayload::Normal(proposal) = &decoded.entries[0].payload else {
        panic!("normal entry");
    };
    assert_eq!(&*proposal.checkpoint, checkpoint.as_str());
    assert_eq!(decoded.vote, request.vote);
    assert_eq!(decoded.leader_commit, request.leader_commit);

    // Every damaged body is refused, none panics.
    let refused = |body: &[u8]| decode_append(body).is_err();
    for length in 0..body.len().min(64) {
        assert!(refused(&body[..length]), "cut at {length}");
    }
    assert!(refused(&body[..body.len() - 1]));
    let mut longer = body.clone();
    longer.push(0);
    assert!(refused(&longer));
    let mut version = body.clone();
    version[0] = 1;
    assert!(refused(&version));
    // zlib's checksum covers the inflated bytes.
    let mut checksum = body.clone();
    *checksum.last_mut().unwrap() ^= 0x01;
    assert!(refused(&checksum));
    // A stream whose adler32 trailer is cut off holds every checkpoint byte,
    // and it is refused all the same.
    let compressed = compress_checkpoint(&checkpoint).unwrap();
    let length = checkpoint.len() as u64;
    assert_eq!(&*inflate_checkpoint(&compressed, length).unwrap(), checkpoint.as_str());
    let untrailed = &compressed[..compressed.len() - 4];
    assert!(inflate_checkpoint(untrailed, length).is_err());
    let mut cut_trailer = encode_append(&append_request(6, ""), &CompressedCheckpoints::default()).unwrap();
    cut_trailer.truncate(cut_trailer.len() - 12);
    cut_trailer.extend_from_slice(&length.to_le_bytes());
    cut_trailer.extend_from_slice(&(untrailed.len() as u32).to_le_bytes());
    cut_trailer.extend_from_slice(untrailed);
    assert!(refused(&cut_trailer));
    // A stream longer than its declared length is refused.
    assert!(inflate_checkpoint(&compressed, length - 1).is_err());
    // The checks that need no inflating run before any checkpoint is
    // inflated: the zlib bytes are only sliced off the body.
    let mut garbage = cut_trailer.clone();
    let tail = garbage.len() - untrailed.len();
    garbage[tail..].fill(0xa5);
    let (_, read) = read_append(&garbage).unwrap();
    assert_eq!(read, vec![(length, &garbage[tail..])]);
    assert!(refused(&garbage));
    // A declared text length that does not match.
    let lengths = 5 + header_length;
    let mut declared = body.clone();
    declared[lengths] ^= 0x01;
    assert!(refused(&declared));
    let mut huge = body.clone();
    huge[lengths..lengths + 8].copy_from_slice(&(MAX_SNAPSHOT as u64 + 1).to_le_bytes());
    assert!(refused(&huge));
    // A checkpoint that is not UTF-8.
    let mut binary = append_request(6, "");
    let invalid = [0xff, 0xfe, 0xfd];
    let mut encoder = ZlibEncoder::new(Vec::new(), Compression::fast());
    encoder.write_all(&invalid).unwrap();
    let zlib = encoder.finish().unwrap();
    let mut not_text = encode_append(&binary, &CompressedCheckpoints::default()).unwrap();
    not_text.truncate(not_text.len() - 12);
    not_text.extend_from_slice(&(invalid.len() as u64).to_le_bytes());
    not_text.extend_from_slice(&(zlib.len() as u32).to_le_bytes());
    not_text.extend_from_slice(&zlib);
    assert!(refused(&not_text));
    // Checkpoint text in the header rather than beside it.
    if let EntryPayload::Normal(proposal) = &mut binary.entries[0].payload {
        proposal.checkpoint = "smuggled".into();
    }
    let mut header = serde_json::to_vec(&binary).unwrap();
    let mut smuggled = vec![COORDINATION_WIRE_VERSION];
    smuggled.extend_from_slice(&(header.len() as u32).to_le_bytes());
    smuggled.append(&mut header);
    smuggled.extend_from_slice(&[0; 12]);
    assert!(refused(&smuggled));
    // An empty checkpoint goes as two zero lengths and reads back empty.
    let empty = encode_append(&append_request(6, ""), &CompressedCheckpoints::default()).unwrap();
    assert!(empty.ends_with(&[0; 12]));
    let EntryPayload::Normal(proposal) = &decode_append(&empty).unwrap().entries[0].payload else {
        panic!("normal entry");
    };
    assert!(proposal.checkpoint.is_empty());

    // A snapshot body carries its data as it is.
    let leader = openraft::CommittedLeaderId::new(3, 1);
    let snapshot = InstallSnapshotRequest::<RoomTypes> {
        vote: Vote::new_committed(3, 1),
        meta: SnapshotMeta {
            last_log_id: Some(LogId::new(leader, 7)),
            last_membership: StoredMembership::default(),
            snapshot_id: "7".into(),
        },
        offset: 0,
        data: (0..=255).collect(),
        done: true,
    };
    let body = encode_install_snapshot(snapshot.clone()).unwrap();
    assert!(body.ends_with(&snapshot.data));
    let decoded = decode_install_snapshot(&body).unwrap();
    assert_eq!((decoded.offset, decoded.done), (0, true));
    assert_eq!(decoded.data, snapshot.data);
    let mut version = body.clone();
    version[0] = 1;
    assert!(decode_install_snapshot(&version).is_err());
    for length in 0..32 {
        assert!(decode_install_snapshot(&body[..length]).is_err());
    }
    let window = InstallSnapshotRequest {
        data: vec![0; SNAPSHOT_FRAGMENT_BYTES * SNAPSHOT_CREDIT_WINDOW + 1],
        ..snapshot
    };
    assert!(decode_install_snapshot(&encode_install_snapshot(window).unwrap()).is_err());
}

/// A helper of wire 1 that reached this one (the ALPN keeps it out first)
/// is refused on every body it can send, and its snapshot is not read.
#[tokio::test]
async fn wire_one_bodies_and_snapshots_are_refused_without_a_panic() {
    let (_bus, nodes) = cluster(1).await;
    let node = &nodes[0];
    let append = wire_one_body(&append_request(6, "first state"));
    assert!(node.dispatch(2, Method::Append, &append).await.is_err());
    let heartbeat = wire_one_body(&AppendEntriesRequest::<RoomTypes> {
        vote: Vote::new_committed(3, 2),
        prev_log_id: None,
        entries: vec![],
        leader_commit: None,
    });
    assert!(node.dispatch(2, Method::Append, &heartbeat).await.is_err());
    let leader = openraft::CommittedLeaderId::new(3, 2);
    let snapshot = serde_json::to_vec(&json!({
        "vote": Vote::new_committed(3, 2),
        "meta": SnapshotMeta::<u64, BasicNode> {
            last_log_id: Some(LogId::new(leader, 1)),
            last_membership: StoredMembership::default(),
            snapshot_id: "1".into(),
        },
        "offset": 0,
        "data": "AQID",
        "done": false,
    }))
    .unwrap();
    assert!(node.dispatch(2, Method::Snapshot, &snapshot).await.is_err());

    // A snapshot under the old magic is refused, as installed and as read.
    let source = node.store().snapshot_source().await;
    let (meta, mut data) = snapshot_of(&source).unwrap();
    assert_eq!(&data[..8], b"sf4rs002");
    assert!(decode_snapshot(&data).is_ok());
    data[..8].copy_from_slice(b"sf4rs001");
    assert!(decode_snapshot(&data).is_err());
    assert!(
        node.store()
            .clone()
            .install_snapshot(&meta, Box::new(Cursor::new(data)))
            .await
            .is_err()
    );
    // The node still serves.
    assert!(node.dispatch(2, Method::Authority, &[0]).await.is_ok());
    stop(nodes).await;
}

/// The append body sizes of a 460 KB checkpoint on both wires.
#[test]
fn a_460_kb_checkpoint_append_is_smaller_on_wire_two() {
    for (shape, checkpoint) in [
        ("varied", sample_checkpoint(460 * 1024)),
        ("alike", alike_checkpoint(460 * 1024)),
    ] {
        let request = append_request(6, &checkpoint);
        let wire_one = wire_one_body(&request);
        let wire_two = encode_append(&request, &CompressedCheckpoints::default()).unwrap();
        eprintln!(
            "{shape} checkpoint {} bytes: wire 1 append {} bytes, wire 2 append {} bytes ({:.1}%)",
            checkpoint.len(),
            wire_one.len(),
            wire_two.len(),
            100.0 * wire_two.len() as f64 / wire_one.len() as f64
        );
        assert!(wire_two.len() < wire_one.len());
    }
}

/// The leader compresses its proposal once; the appends to both followers
/// take those bytes.
#[tokio::test]
async fn a_proposal_is_compressed_once_for_every_follower() {
    let (_bus, nodes) = cluster(3).await;
    let checkpoint = sample_checkpoint(64 * 1024);
    let receipt = nodes[0]
        .propose(proposal(&nodes[0], "one", 0, &checkpoint))
        .await
        .unwrap();
    assert!(receipt.accepted);
    tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            let mut applied = true;
            for node in &nodes[1..] {
                applied &= node.store().committed().await.checkpoint() == checkpoint;
            }
            if applied {
                break;
            }
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
    })
    .await
    .unwrap();
    assert_eq!(
        nodes[0]
            .network
            .appends
            .checkpoints
            .compressions
            .load(Ordering::Relaxed),
        1
    );
    stop(nodes).await;
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
        checkpoint: "".into(),
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
    let snapshot = encode_install_snapshot(InstallSnapshotRequest {
        vote: Vote::new_committed(high, high),
        meta: SnapshotMeta {
            last_log_id: Some(log_id),
            last_membership: StoredMembership::new(Some(log_id), membership),
            snapshot_id: format!("{:?}", Some(log_id)),
        },
        offset: high,
        data: vec![u8::MAX; SNAPSHOT_FRAGMENT_BYTES * SNAPSHOT_CREDIT_WINDOW],
        done: true,
    })
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

/// Records every RPC body and answers each append with success.
#[derive(Default)]
struct RecordingTransport {
    bodies: std::sync::Mutex<Vec<(u64, &'static str, Vec<u8>)>>,
}

impl RpcTransport for RecordingTransport {
    fn call(
        &self,
        target: u64,
        _: String,
        method: Method,
        body: Vec<u8>,
    ) -> Pin<Box<dyn Future<Output = io::Result<Vec<u8>>> + Send + '_>> {
        self.bodies.lock().unwrap().push((target, method.name(), body));
        Box::pin(async { Ok(serde_json::to_vec(&AppendEntriesResponse::<u64>::Success).unwrap()) })
    }
}

fn append_request(leader_commit: u64, checkpoint: &str) -> AppendEntriesRequest<RoomTypes> {
    let leader = openraft::CommittedLeaderId::new(3, 1);
    AppendEntriesRequest {
        vote: Vote::new_committed(3, 1),
        prev_log_id: Some(LogId::new(leader, 6)),
        leader_commit: Some(LogId::new(leader, leader_commit)),
        entries: vec![Entry {
            log_id: LogId::new(leader, 7),
            payload: EntryPayload::Normal(Proposal {
                request: "7".into(),
                dedup_id: "native:1:3:7".into(),
                term: 3,
                base: 6,
                checkpoint: checkpoint.into(),
                admin: None,
            }),
        }],
    }
}

#[tokio::test]
async fn followers_sent_the_same_append_share_one_encoded_body() {
    let transport = Arc::new(RecordingTransport::default());
    let mut network = Network::new(transport.clone());
    let mut first = network.new_client(2, &BasicNode::new("2")).await;
    let mut second = network.new_client(3, &BasicNode::new("3")).await;
    let checkpoint =
        json!({"checkpoint": {"members": [], "journal": "x".repeat(64 * 1024)}}).to_string();
    let option = || RPCOption::new(Duration::from_secs(1));
    let request = append_request(6, &checkpoint);
    first
        .append_entries(request.clone(), option())
        .await
        .unwrap();
    second
        .append_entries(request.clone(), option())
        .await
        .unwrap();
    let encodes = || {
        network
            .appends
            .encodes
            .load(std::sync::atomic::Ordering::Relaxed)
    };
    assert_eq!(encodes(), 1);
    {
        let bodies = transport.bodies.lock().unwrap();
        assert_eq!(bodies.len(), 2);
        assert_eq!((bodies[0].0, bodies[0].1), (2, "append"));
        assert_eq!((bodies[1].0, bodies[1].1), (3, "append"));
        assert_eq!(bodies[0].2, bodies[1].2);
        // The bytes each follower would have encoded for itself.
        let own = encode_append(&request, &CompressedCheckpoints::default()).unwrap();
        assert_eq!(bodies[0].2, own);
        // The follower reads the checkpoint back unchanged.
        let decoded = decode_append(&bodies[0].2).unwrap();
        let EntryPayload::Normal(proposal) = &decoded.entries[0].payload else {
            panic!("normal entry");
        };
        assert_eq!(&*proposal.checkpoint, checkpoint.as_str());
    }
    // Another commit index is another request, encoded on its own.
    let next = append_request(7, &checkpoint);
    first.append_entries(next.clone(), option()).await.unwrap();
    assert_eq!(encodes(), 2);
    // Requests in step a commit behind are still served from the memo.
    second.append_entries(request, option()).await.unwrap();
    second.append_entries(next, option()).await.unwrap();
    assert_eq!(encodes(), 2);
    let bodies = transport.bodies.lock().unwrap();
    assert_eq!(bodies[2].2, bodies[4].2);
    assert_eq!(bodies[0].2, bodies[3].2);
    assert_ne!(bodies[0].2, bodies[2].2);
}

#[tokio::test]
async fn concurrent_streams_encode_one_append_once() {
    let transport = Arc::new(RecordingTransport::default());
    let mut network = Network::new(transport.clone());
    let checkpoint = "y".repeat(256 * 1024);
    let request = append_request(6, &checkpoint);
    let mut tasks = tokio::task::JoinSet::new();
    for id in 2..=9 {
        let mut peer = network
            .new_client(id, &BasicNode::new(id.to_string()))
            .await;
        let request = request.clone();
        tasks.spawn(async move {
            peer.append_entries(request, RPCOption::new(Duration::from_secs(1)))
                .await
                .unwrap();
        });
    }
    while let Some(result) = tasks.join_next().await {
        result.unwrap();
    }
    assert_eq!(
        network
            .appends
            .encodes
            .load(std::sync::atomic::Ordering::Relaxed),
        1
    );
    let bodies = transport.bodies.lock().unwrap();
    assert_eq!(bodies.len(), 8);
    assert!(bodies.iter().all(|(_, _, body)| *body == bodies[0].2));
}

#[test]
fn a_shared_checkpoint_serializes_as_the_same_string() {
    let proposal = Proposal {
        request: "7".into(),
        dedup_id: "d".into(),
        term: 1,
        base: 0,
        checkpoint: "{\"a\":\"\\u0001\"}".into(),
        admin: None,
    };
    assert_eq!(
        serde_json::to_string(&proposal).unwrap(),
        r#"{"request":"7","dedup_id":"d","term":1,"base":0,"checkpoint":"{\"a\":\"\\u0001\"}","admin":null}"#
    );
    let back: Proposal = serde_json::from_str(&serde_json::to_string(&proposal).unwrap()).unwrap();
    assert_eq!(back.checkpoint, proposal.checkpoint);
    let committed = Committed::new(4, proposal.checkpoint.clone(), "7".into(), 1);
    assert_eq!(
        serde_json::to_string(&committed).unwrap(),
        r#"{"revision":4,"checkpoint":"{\"a\":\"\\u0001\"}","request":"7","term":1}"#
    );
}

/// A snapshot built from an older state machine never replaces one that was
/// installed while it was being encoded outside the lock.
#[tokio::test]
async fn a_snapshot_installed_during_a_build_is_kept() {
    let older = Store::default();
    let newer = Store::default();
    assert_eq!(older.apply_checkpoints(&["first".into()]).await, 1);
    assert_eq!(
        newer
            .apply_checkpoints(&["first".into(), "second".into()])
            .await,
        2
    );
    // The build reads its source under the lock, then encodes without it.
    let source = older.snapshot_source().await;
    let (stale_meta, stale_data) = snapshot_of(&source).unwrap();
    // Meanwhile a newer snapshot is installed.
    let (installed_meta, installed_data) = snapshot_of(&newer.snapshot_source().await).unwrap();
    older
        .clone()
        .install_snapshot(
            &installed_meta,
            Box::new(Cursor::new(installed_data.clone())),
        )
        .await
        .unwrap();
    // The build's store step keeps the installed one.
    assert!(!older.keep_snapshot(&stale_meta, &stale_data).await);
    let current = older.clone().get_current_snapshot().await.unwrap().unwrap();
    assert_eq!(current.meta.last_log_id, installed_meta.last_log_id);
    assert_eq!((*current.snapshot).into_inner(), installed_data);
    assert_eq!(older.committed().await.checkpoint(), "second");
    // A build of the same or a later state replaces it as before.
    let rebuilt = older.clone().build_snapshot().await.unwrap();
    assert_eq!(rebuilt.meta.last_log_id, installed_meta.last_log_id);
    assert_eq!(older.apply_checkpoints(&["third".into()]).await, 1);
    let later = older.clone().build_snapshot().await.unwrap();
    assert!(later.meta.last_log_id > installed_meta.last_log_id);
    let current = older.clone().get_current_snapshot().await.unwrap().unwrap();
    assert_eq!(current.meta.last_log_id, later.meta.last_log_id);
    assert_eq!(
        decode_snapshot(&(*current.snapshot).into_inner())
            .unwrap()
            .committed
            .checkpoint(),
        "third"
    );
}

/// The snapshot is encoded while the store lock is free.
#[tokio::test]
async fn a_snapshot_build_does_not_hold_the_store_lock_while_encoding() {
    let store = Store::default();
    store.apply_checkpoints(&["x".repeat(512 * 1024)]).await;
    let source = store.snapshot_source().await;
    // The lock is free once the source is read.
    let guard = tokio::time::timeout(Duration::from_millis(100), store.hold_lock())
        .await
        .expect("store lock free after the source read");
    drop(guard);
    let (meta, data) = snapshot_of(&source).unwrap();
    assert!(store.keep_snapshot(&meta, &data).await);
}

/// The lock-free committed revision follows every commit and every
/// installed snapshot.
#[tokio::test]
async fn the_committed_revision_is_read_without_the_store_lock() {
    let (_bus, nodes) = cluster(1).await;
    let node = &nodes[0];
    assert_eq!(node.committed_revision(), 0);
    let receipt = node
        .propose(proposal(node, "1", 0, "first state"))
        .await
        .unwrap();
    assert!(receipt.accepted);
    assert_eq!(node.committed_revision(), 1);
    let store = node.store();
    assert_eq!(store.apply_checkpoints(&["second".into()]).await, 1);
    assert_eq!(node.committed_revision(), node.store().committed().await.revision());
    store.install_checkpoint_snapshot("replaced").await;
    assert_eq!(node.committed_revision(), node.store().committed().await.revision());
    // Read while the store is locked.
    let guard = store.hold_lock().await;
    assert_eq!(node.committed_revision(), 2);
    drop(guard);
    stop(nodes).await;
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
async fn a_follower_announces_each_commit_and_shares_its_checkpoint() {
    let (_bus, nodes) = cluster(3).await;
    let mut commits = nodes[1].store().committed_changes().await;
    let empty = nodes[1].store().committed().await;
    assert_eq!((empty.revision, empty.digest()), (0, [0; 32]));
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
    // One shared value per commit: a second look is the same value, and its
    // digest is the checkpoint's.
    let first = nodes[1].store().committed().await;
    assert_eq!((first.revision, first.request.as_str()), (1, "one"));
    assert_eq!(&*first.checkpoint, "first state");
    assert_eq!(first.digest(), crate::recovery::sha256(b"first state"));
    let again = nodes[1].store().committed().await;
    assert!(Arc::ptr_eq(&first, &again) && first.same_commit(&again));
    assert!(!first.same_commit(&empty));

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
    // A value rebuilt from a snapshot is still the same commit.
    let rebuilt = restored.committed().await;
    assert!(!Arc::ptr_eq(&rebuilt, &first) && rebuilt.same_commit(&first));
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
        &*restored.committed().await.checkpoint,
        "complete private room state"
    );
    assert_eq!(restored.committed().await.revision, 1);
    stop(nodes).await;
}

#[tokio::test]
async fn two_member_partition_never_commits_a_second_host() {
    let (bus, nodes) = cluster(2).await;
    assert_eq!(nodes[0].store().applied_voters().await, BTreeSet::from([1, 2]));
    assert_eq!(nodes[1].store().applied_voters().await, BTreeSet::from([1, 2]));
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
    assert_eq!(nodes[0].store().committed().await.revision, 1);
    assert_eq!(nodes[1].store().committed().await.revision, 1);
    stop(nodes).await;
}

#[tokio::test]
async fn admitted_voter_can_commit_its_own_non_native_departure() {
    let (_, nodes) = cluster(2).await;
    assert_eq!(nodes[0].store().applied_voters().await, BTreeSet::from([1, 2]));
    let response = nodes[0].dispatch(2, Method::Remove, &[0]).await.unwrap();
    assert_eq!(response, vec![1]);
    tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            if nodes[0].store().applied_voters().await == BTreeSet::from([1]) {
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
            let voters = nodes[0].store().applied_voters().await;
            let members = nodes[0].store().applied_members().await;
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
    assert_eq!(nodes[0].dispatch(3, Method::Remove, &[0]).await.unwrap(), vec![1]);
    tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            if !nodes[0].store().applied_members().await.contains(&3) {
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
        while !follower.store().applied_members().await.contains(&4) {
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
            .dispatch(4, Method::Vote, &request(Vote::new(term + 5, 4)))
            .await
            .is_err()
    );
    // A voter campaigns with the uncommitted vote of a candidate; a request
    // carrying a committed vote is not one.
    assert!(
        follower
            .dispatch(3, Method::Vote, &request(Vote::new_committed(term + 5, 3)))
            .await
            .is_err()
    );
    assert_eq!(follower.current_term(), term);
    assert_eq!(follower.current_leader(), Some(1));
    // A voter's candidate request is still answered.
    assert!(
        follower
            .dispatch(3, Method::Vote, &request(Vote::new(term, 3)))
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
        while nodes[1].store().applied_voters().await != BTreeSet::from([2, 3])
            || nodes[2].store().applied_voters().await != BTreeSet::from([2, 3])
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
        while nodes[1].store().applied_voters().await != BTreeSet::from([2]) {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
    })
    .await
    .expect("successor never applied the singleton membership");
    assert_eq!(nodes[1].dispatch(1, Method::Elect, &[0]).await.unwrap(), vec![1]);
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
        while &*nodes[2].store().committed().await.checkpoint != "handed over" {
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
        &*nodes[successor].store().committed().await.checkpoint,
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
        while nodes[0].store().applied_joint_goal().await.is_none() {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
    })
    .await
    .expect("the abandoned joint configuration commits");
    assert_eq!(nodes[0].store().applied_joint_goal().await, Some(goal.clone()));
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
    assert_eq!(nodes[0].store().applied_joint_goal().await, None);
    assert_eq!(nodes[0].store().applied_voters().await, goal);
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

// Store helpers the latency tests drive the state machine with.
mod store_support;
// Relay-sized appends and snapshot transfer to new replicas.
mod transfer;
// Departed-member history and its bound.
mod history;
// Server-owned rooms: one voter, learners that follow the host only.
mod server_owned;
