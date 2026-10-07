use super::*;
use crate::coordination::Proposal;
use openraft::BasicNode;
#[tokio::test]
async fn authenticated_iroh_commit_and_host_loss() {
    let mut transports = Vec::new();
    let mut nodes = Vec::new();
    let mut tasks = JoinSet::new();
    for id in 1..=3 {
        let transport = IrohRpc::bind([7; 16], id, false).await.unwrap();
        let node = Arc::new(Coordinator::new(id, transport.clone()).await.unwrap());
        transports.push(transport);
        nodes.push(node);
    }
    for transport in &transports {
        for (i, peer) in transports.iter().enumerate() {
            if !Arc::ptr_eq(transport, peer) {
                transport.admit(i as u64 + 1, peer.address()).await.unwrap();
            }
        }
    }
    for i in 0..3 {
        tasks.spawn(transports[i].clone().serve(nodes[i].clone()));
    }
    nodes[0]
        .raft()
        .initialize(
            transports
                .iter()
                .enumerate()
                .map(|(i, t)| (i as u64 + 1, BasicNode::new(t.identity().to_string())))
                .collect::<BTreeMap<_, _>>(),
        )
        .await
        .unwrap();
    nodes[0]
        .raft()
        .wait(Some(Duration::from_secs(15)))
        .current_leader(1, "Iroh leader")
        .await
        .unwrap();
    let proposal = Proposal {
        request: "first".into(),
        dedup_id: "test:first".into(),
        term: nodes[0].raft().metrics().borrow().current_term,
        base: 0,
        checkpoint: "active generation 42".into(),
        admin: None,
    };
    assert!(nodes[0].propose(proposal).await.unwrap().accepted);
    tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            if nodes[1].committed().await.revision() == 1 && nodes[2].committed().await.revision() == 1
            {
                break;
            }
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
    })
    .await
    .unwrap();
    nodes[0].raft().shutdown().await.unwrap();
    transports[0].close().await;
    nodes[1].raft().trigger().elect().await.unwrap();
    tokio::time::timeout(Duration::from_secs(15), async {
        loop {
            if nodes[1].raft().metrics().borrow().current_leader.is_some()
                || nodes[2].raft().metrics().borrow().current_leader.is_some()
            {
                break;
            }
            tokio::time::sleep(Duration::from_millis(50)).await;
        }
    })
    .await
    .unwrap();
    let successor = if nodes[1].raft().metrics().borrow().current_leader == Some(2) {
        &nodes[1]
    } else {
        &nodes[2]
    };
    assert_eq!(
        successor.committed().await.checkpoint(),
        "active generation 42"
    );
    for i in 1..3 {
        nodes[i].raft().shutdown().await.unwrap();
        transports[i].close().await;
    }
    tasks.abort_all();
    while tasks.join_next().await.is_some() {}
}

#[tokio::test]
async fn primary_binding_is_unique_until_incarnation_revoke() {
    let router = IrohRpc::bind([8; 16], 1, false).await.unwrap();
    let first = IrohRpc::bind([8; 16], 2, false).await.unwrap();
    let second = IrohRpc::bind([8; 16], 3, false).await.unwrap();
    let primary = router.identity();

    router
        .admit_bound(2, first.address(), Some(primary))
        .await
        .unwrap();
    assert!(
        router
            .admit_bound(3, second.address(), Some(primary))
            .await
            .is_err()
    );
    router.admit_bound(3, second.address(), None).await.unwrap();
    router.revoke(2).await;
    router
        .admit_bound(3, second.address(), Some(primary))
        .await
        .unwrap();

    router.close().await;
    first.close().await;
    second.close().await;
}

#[tokio::test]
async fn own_incarnation_and_coordination_key_are_never_bound() {
    let router = IrohRpc::bind([10; 16], 1, false).await.unwrap();
    let peer = IrohRpc::bind([10; 16], 2, false).await.unwrap();

    assert!(router.admit(1, peer.address()).await.is_err());
    assert!(router.admit(2, router.address()).await.is_err());
    assert!(!router.has_binding(1).await);
    router.admit(2, peer.address()).await.unwrap();

    router.close().await;
    peer.close().await;
}

/// One RPC written by hand, so a test chooses the method byte and body.
async fn raw_rpc(connection: &Connection, method: u8, body: &[u8]) -> io::Result<Vec<u8>> {
    let (mut send, mut recv) = connection.open_bi().await.map_err(|_| failure())?;
    send.write_all(&[12; 16]).await?;
    send.write_u64(2).await?;
    send.write_u64(1).await?;
    send.write_u8(method).await?;
    send.write_u32(body.len() as u32).await?;
    send.write_all(body).await?;
    send.finish().map_err(|_| failure())?;
    read_body(&mut recv, MAX_RPC_RESPONSE).await
}

#[tokio::test]
async fn served_connections_and_bodies_stay_within_their_bounds() {
    let room = [12; 16];
    let server = IrohRpc::bind(room, 1, false).await.unwrap();
    let client = IrohRpc::bind(room, 2, false).await.unwrap();
    server.admit(2, client.address()).await.unwrap();
    let coordinator = Arc::new(
        Coordinator::new_for_room(room, 1, server.clone())
            .await
            .unwrap(),
    );
    coordinator
        .raft()
        .initialize(BTreeMap::from([(
            1,
            BasicNode::new(server.identity().to_string()),
        )]))
        .await
        .unwrap();
    coordinator
        .raft()
        .wait(Some(Duration::from_secs(5)))
        .current_leader(1, "single leader")
        .await
        .unwrap();
    let serving = tokio::spawn(server.clone().serve(coordinator.clone()));
    let connect = || client.endpoint.connect(server.address(), ALPN);

    let first = connect().await.unwrap();
    let second = connect().await.unwrap();
    assert!(raw_rpc(&first, 4, &[0]).await.is_ok());
    assert!(raw_rpc(&second, 4, &[0]).await.is_ok());
    // A third connection from the same endpoint is closed at once.
    let third = connect().await.unwrap();
    assert!(
        timeout(Duration::from_secs(10), third.closed())
            .await
            .is_ok()
    );

    // A size beyond its method's limit fails the stream from the header
    // alone, before any body byte arrives.
    let (mut send, mut recv) = first.open_bi().await.unwrap();
    send.write_all(&room).await.unwrap();
    send.write_u64(2).await.unwrap();
    send.write_u64(1).await.unwrap();
    send.write_u8(4).await.unwrap();
    send.write_u32(2).await.unwrap();
    assert!(matches!(
        timeout(
            Duration::from_secs(3),
            read_body(&mut recv, MAX_RPC_RESPONSE)
        )
        .await,
        Ok(Err(_))
    ));
    drop(send);

    // A body beyond its method's size fails its own stream only.
    assert!(raw_rpc(&first, 4, &[0, 0]).await.is_err());
    assert!(
        raw_rpc(&first, 2, &vec![b' '; MAX_VOTE_REQUEST + 1])
            .await
            .is_err()
    );
    assert!(raw_rpc(&first, 4, &[0]).await.is_ok());

    // A closed connection frees its place for a new one.
    second.close(0u32.into(), b"done");
    timeout(Duration::from_secs(10), async {
        loop {
            if let Ok(next) = connect().await
                && raw_rpc(&next, 4, &[0]).await.is_ok()
            {
                break;
            }
            sleep(Duration::from_millis(50)).await;
        }
    })
    .await
    .unwrap();

    serving.abort();
    coordinator.raft().shutdown().await.unwrap();
    server.close().await;
    client.close().await;
}

#[tokio::test]
async fn concurrent_calls_share_one_dialed_connection() {
    let room = [13; 16];
    let server = IrohRpc::bind(room, 1, false).await.unwrap();
    let client = IrohRpc::bind(room, 2, false).await.unwrap();
    server.admit(2, client.address()).await.unwrap();
    client.admit(1, server.address()).await.unwrap();
    let coordinator = Arc::new(
        Coordinator::new_for_room(room, 1, server.clone())
            .await
            .unwrap(),
    );
    coordinator
        .raft()
        .initialize(BTreeMap::from([(
            1,
            BasicNode::new(server.identity().to_string()),
        )]))
        .await
        .unwrap();
    coordinator
        .raft()
        .wait(Some(Duration::from_secs(5)))
        .current_leader(1, "single leader")
        .await
        .unwrap();
    let serving = tokio::spawn(server.clone().serve(coordinator.clone()));
    let expected = server.identity().to_string();
    let call = || {
        <IrohRpc as RpcTransport>::call(client.as_ref(), 1, expected.clone(), "authority", vec![0])
    };
    let served = || {
        server
            .peers
            .lock()
            .unwrap()
            .get(&client.identity())
            .map_or(0, |load| load.connections)
    };

    for round in 0..2 {
        let results = tokio::join!(call(), call(), call(), call());
        for result in [results.0, results.1, results.2, results.3] {
            assert!(result.is_ok(), "round {round}: a concurrent call failed");
        }
        assert_eq!(served(), 1, "round {round}: calls dialed more than once");
        // A closed cached connection is replaced by a single new dial.
        client.connections.read().await[&1].close(0u32.into(), b"closed");
        timeout(Duration::from_secs(10), async {
            while served() != 0 {
                sleep(Duration::from_millis(25)).await;
            }
        })
        .await
        .unwrap();
    }

    serving.abort();
    coordinator.raft().shutdown().await.unwrap();
    server.close().await;
    client.close().await;
}

/// Drops `call` once the receiver reports that the RPC's stream reached it,
/// so the cancellation lands on a call that is waiting for its response
/// whatever the connection setup cost.
async fn cancel_once_received<F: Future>(
    call: F,
    received: &mut tokio::sync::mpsc::UnboundedReceiver<()>,
) {
    tokio::select! {
        _ = call => panic!("the stalled receiver answered the RPC"),
        arrived = timeout(Duration::from_secs(30), received.recv()) => {
            assert!(matches!(arrived, Ok(Some(()))), "the RPC never reached the receiver");
        }
    }
}

#[tokio::test]
async fn canceled_rpc_closes_the_cached_connection_before_retry() {
    let room = [11; 16];
    let caller = IrohRpc::bind(room, 1, false).await.unwrap();
    let receiver = IrohRpc::bind(room, 2, false).await.unwrap();
    caller.admit(2, receiver.address()).await.unwrap();
    let receiver_endpoint = receiver.endpoint.clone();
    let (received_tx, mut received) = tokio::sync::mpsc::unbounded_channel();
    let stalled = tokio::spawn(async move {
        let incoming = receiver_endpoint.accept().await.unwrap();
        let connection = incoming.await.unwrap();
        let _read = connection.accept_bi().await.unwrap();
        received_tx.send(()).unwrap();
        let _append = connection.accept_bi().await.unwrap();
        received_tx.send(()).unwrap();
        // Hold the connection and both streams open, never answering,
        // until the test aborts this task.
        std::future::pending::<()>().await;
    });

    // A canceled authority read shares the route with raft traffic and
    // must leave it open.
    let read = <IrohRpc as RpcTransport>::call(
        caller.as_ref(),
        2,
        receiver.identity().to_string(),
        "authority",
        vec![0],
    );
    cancel_once_received(read, &mut received).await;
    let cached = caller.connections.read().await.get(&2).cloned().unwrap();
    assert!(
        cached.close_reason().is_none(),
        "canceled authority read closed the shared route"
    );

    let call = <IrohRpc as RpcTransport>::call(
        caller.as_ref(),
        2,
        receiver.identity().to_string(),
        "append",
        vec![0],
    );
    cancel_once_received(call, &mut received).await;
    let cached = caller.connections.read().await.get(&2).cloned().unwrap();
    assert!(
        cached.close_reason().is_some(),
        "canceled RPC left a dead cached connection reusable"
    );
    stalled.abort();
    caller.close().await;
    receiver.close().await;
}

#[tokio::test]
async fn retired_incarnation_cannot_replay_admission() {
    let router = IrohRpc::bind([9; 16], 1, false).await.unwrap();
    let departing = IrohRpc::bind([9; 16], 2, false).await.unwrap();
    let replacement = IrohRpc::bind([9; 16], 3, false).await.unwrap();
    let primary = router.identity();

    router
        .admit_bound(2, departing.address(), Some(primary))
        .await
        .unwrap();
    router.retire(2).await;

    // A disconnected member may retain an authenticated transport long
    // enough to finish its Leave proof, but replaying its old admission
    // must never restore append/vote authority or evict a fresh process.
    assert!(
        router
            .admit_bound(2, departing.address(), Some(primary))
            .await
            .is_err()
    );
    router
        .admit_bound(3, replacement.address(), Some(primary))
        .await
        .unwrap();

    router.close().await;
    departing.close().await;
    replacement.close().await;
}

/// A member that stops answering, such as a leader whose game was closed,
/// holds every RPC sent to it until the deadline, and each leader read sends
/// it another heartbeat. Those calls must not take the budget of the members
/// that still answer: there, a heartbeat refused before it was sent cost the
/// new leader its quorum proof.
#[tokio::test]
async fn unanswering_target_cannot_starve_calls_to_others() {
    let room = [14; 16];
    let caller = IrohRpc::bind(room, 1, false).await.unwrap();
    let live = IrohRpc::bind(room, 2, false).await.unwrap();
    let silent = IrohRpc::bind(room, 3, false).await.unwrap();
    live.admit(1, caller.address()).await.unwrap();
    caller.admit(2, live.address()).await.unwrap();
    caller.admit(3, silent.address()).await.unwrap();
    let coordinator = Arc::new(
        Coordinator::new_for_room(room, 2, live.clone())
            .await
            .unwrap(),
    );
    coordinator
        .raft()
        .initialize(BTreeMap::from([(
            2,
            BasicNode::new(live.identity().to_string()),
        )]))
        .await
        .unwrap();
    coordinator
        .raft()
        .wait(Some(Duration::from_secs(5)))
        .current_leader(2, "single leader")
        .await
        .unwrap();
    let serving = tokio::spawn(live.clone().serve(coordinator.clone()));
    // The silent member accepts the connection and every stream and never
    // answers one, so each call to it stays in flight until its deadline.
    let silent_endpoint = silent.endpoint.clone();
    let stalled = tokio::spawn(async move {
        let connection = silent_endpoint.accept().await.unwrap().await.unwrap();
        let mut streams = Vec::new();
        while let Ok(stream) = connection.accept_bi().await {
            streams.push(stream);
        }
    });

    // More calls than the whole budget used to allow.
    let attempts = 4 * caller.limit;
    let mut silent_calls = JoinSet::new();
    for _ in 0..attempts {
        let caller = caller.clone();
        let expected = silent.identity().to_string();
        silent_calls.spawn(async move {
            <IrohRpc as RpcTransport>::call(caller.as_ref(), 3, expected, "append", vec![0]).await
        });
    }
    // The calls past the target's bound are refused at once; the rest wait.
    let mut refused = 0;
    while refused < attempts - MAX_TARGET_IN_FLIGHT {
        let result = timeout(Duration::from_secs(5), silent_calls.join_next())
            .await
            .expect("a call past the target's bound waited")
            .unwrap()
            .unwrap();
        assert!(result.is_err(), "the silent member answered");
        refused += 1;
    }
    assert_eq!(
        caller.target_in_flight.lock().unwrap().get(&3).copied(),
        Some(MAX_TARGET_IN_FLIGHT)
    );

    // A call to the member that answers still goes out and succeeds.
    let answered = timeout(
        Duration::from_secs(5),
        <IrohRpc as RpcTransport>::call(
            caller.as_ref(),
            2,
            live.identity().to_string(),
            "authority",
            vec![0],
        ),
    )
    .await
    .expect("the call to the answering member waited");
    assert!(
        answered.is_ok(),
        "the silent member starved a call to another member"
    );
    assert!(
        silent_calls.try_join_next().is_none(),
        "a call within the target's bound ended early"
    );

    silent_calls.abort_all();
    while silent_calls.join_next().await.is_some() {}
    assert!(caller.target_in_flight.lock().unwrap().is_empty());
    stalled.abort();
    serving.abort();
    coordinator.raft().shutdown().await.unwrap();
    caller.close().await;
    live.close().await;
    silent.close().await;
}
