use super::probes::run_probe;
use super::*;

impl Actor {
    #[cfg(test)]
    fn stable_voters(&self, current: BTreeSet<u64>, desired: usize) -> BTreeSet<u64> {
        let mut voters: BTreeSet<_> = current
            .into_iter()
            .filter(|id| {
                self.admissions.contains_key(id)
                    && !self.pending_retired_incarnations.contains(id)
                    && !self.retired_incarnations.contains(id)
            })
            .collect();
        for id in self
            .admission_order
            .iter()
            .copied()
            .chain(self.admissions.keys().copied())
        {
            if voters.len() >= desired {
                break;
            }
            if self.admissions.contains_key(&id)
                && !self.pending_retired_incarnations.contains(&id)
                && !self.retired_incarnations.contains(&id)
            {
                voters.insert(id);
            }
        }
        voters
    }

    #[cfg(test)]
    async fn apply_pending_retirements(&mut self) -> io::Result<()> {
        let Some(recovery) = self.recovery.clone() else {
            return Ok(());
        };
        let applied = recovery.applied_member_ids().await;
        let confirmed: Vec<u64> = self
            .pending_retired_incarnations
            .iter()
            .copied()
            .filter(|incarnation| !applied.contains(incarnation))
            .collect();
        for incarnation in &confirmed {
            recovery.rpc.retire(*incarnation).await;
        }
        self.apply_confirmed_retirements(confirmed.into_iter().collect());
        Ok(())
    }
}

#[test]
fn leaving_leader_hands_off_to_a_reachable_voter() {
    use super::members::handoff_successor;
    let voters = BTreeSet::from([10, 20, 30]);
    // Voter 20 comes first by ID but its game is gone; handing it authority
    // could never commit and would strand the room without a quorum.
    assert_eq!(handoff_successor(&voters, 10, |id| id == 30), Some(30));
    // With every other voter reachable, voter order decides as before.
    assert_eq!(handoff_successor(&voters, 10, |_| true), Some(20));
    // With none reachable, still name one so the leave can try and report.
    assert_eq!(handoff_successor(&voters, 10, |_| false), Some(20));
    assert_eq!(handoff_successor(&BTreeSet::from([10]), 10, |_| true), None);
}

#[test]
fn native_wrapper_keeps_quote_heavy_json_below_frame_bound() {
    let payload = serde_json::json!({ "text": "\"".repeat(32_000) }).to_string();
    let wire = serde_json::to_string(&NativeControlMessage {
        kind: "native_control".into(),
        message_id: 19,
        payload: payload.clone(),
    })
    .unwrap();
    assert!(wire.len() < MAX_CONTROL_PAYLOAD);
    assert!(wire.contains(r#""payload":{"text":"#));
    let decoded: NativeControlMessage = serde_json::from_str(&wire).unwrap();
    assert_eq!(decoded.payload, payload);
}
use iroh::endpoint::{PortmapperConfig, presets};
use tokio::net::UdpSocket;

async fn endpoint() -> Endpoint {
    Endpoint::builder(presets::Minimal)
        .clear_ip_transports()
        .bind_addr((Ipv4Addr::LOCALHOST, 0))
        .unwrap()
        .portmapper_config(PortmapperConfig::Disabled)
        .alpns(vec![CONTROL_ALPN.to_vec(), GAME_ALPN.to_vec()])
        .bind()
        .await
        .unwrap()
}
fn address(endpoint: &Endpoint) -> EndpointAddr {
    EndpointAddr::new(endpoint.id()).with_ip_addr(endpoint.bound_sockets()[0])
}
async fn next(events: &mut mpsc::Receiver<Event>, kind: &str) -> Event {
    loop {
        let event = events.recv().await.unwrap();
        let value = serde_json::to_value(&event).unwrap();
        if value["type"] == kind {
            return event;
        }
        assert_ne!(
            value["type"], "error",
            "unexpected helper error: {}",
            value["code"]
        );
    }
}

fn test_actor(endpoint: Endpoint, events: mpsc::Sender<Event>) -> Actor {
    Actor {
        endpoint,
        relay_only: false,
        epoch: 0,
        opening: false,
        room: None,
        hosted: None,
        room_invite: None,
        host_address: None,
        controls: BTreeMap::new(),
        games: BTreeMap::new(),
        closed_generation: 0,
        tasks: JoinSet::new(),
        events: EventOutbox::new(events),
        recovery: None,
        admissions: BTreeMap::new(),
        admission_order: Vec::new(),
        applied_admission_members: BTreeSet::new(),
        incoming_transfer: None,
        pending_checkpoint_proposal: None,
        pending_checkpoint_retry: None,
        outgoing_transfer: None,
        committed_native_members: None,
        pending_retired_incarnations: BTreeSet::new(),
        retired_incarnations: BTreeSet::new(),
        last_exported_revision: 0,
        pending_checkpoint_ack: None,
        pending_checkpoint_committed: None,
        next_transport_message: TRANSPORT_MESSAGE_ID_BASE,
        next_coordination_operation: 1,
        pending_coordination_refresh: None,
        pending_membership_operation: None,
        pending_admission_operation: None,
        deferred_admissions: VecDeque::new(),
        pending_membership_publications: BTreeSet::new(),
        last_coordination_state: None,
        coordination_writable: false,
        last_control_rebound: None,
        unwritable_leader_since: None,
        reconnect_target: None,
        probe_reservations: BTreeMap::new(),
        pending_game_admissions: BTreeMap::new(),
        pending_probe_invalidations: BTreeMap::new(),
        probe_peers: BTreeSet::new(),
        probe_permissions: BTreeMap::new(),
        pending_probe_authorizations: BTreeMap::new(),
        retirement_started: None,
    }
}

#[tokio::test]
async fn near_limit_checkpoint_obeys_four_credit_window_and_cumulative_acks() {
    timeout(Duration::from_secs(10), async {
        let source_endpoint = endpoint().await;
        let target_endpoint = endpoint().await;
        let (source_events, mut source_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (target_events, mut target_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut source = test_actor(source_endpoint.clone(), source_events);
        let mut target = test_actor(target_endpoint.clone(), target_events);
        let room = [43; 16];
        source.epoch = 1;
        source.room = Some(room);
        target.epoch = 1;
        target.room = Some(room);
        let bytes: Vec<u8> = (0..recovery::MAX_CHECKPOINT_BYTES - 13)
            .map(|index| (index.wrapping_mul(73) & 255) as u8)
            .collect();
        let transfer = CheckpointTransfer::new(room, 17, 3, 8, 9, bytes.clone()).unwrap();
        source.start_outgoing_checkpoint(transfer.clone());

        match source_rx.recv().await.unwrap() {
            Event::CheckpointBegin {
                transfer,
                term,
                base_revision,
                revision,
                length,
                digest,
                ..
            } => {
                target
                    .checkpoint_begin(
                        1,
                        room,
                        transfer,
                        term,
                        base_revision,
                        revision,
                        length,
                        digest,
                    )
                    .await
                    .unwrap();
            }
            _ => panic!("checkpoint begin must precede chunks"),
        }

        let mut received = 0usize;
        let mut window = 0usize;
        while received < bytes.len() {
            let chunks =
                CHECKPOINT_WINDOW.min((bytes.len() - received).div_ceil(CHECKPOINT_CHUNK_BYTES));
            let mut cumulative_ack = 0;
            for _ in 0..chunks {
                match source_rx.recv().await.unwrap() {
                    Event::CheckpointChunk {
                        transfer,
                        term,
                        base_revision,
                        revision,
                        offset,
                        data,
                        ..
                    } => {
                        assert_eq!(offset as usize, received);
                        let decoded = URL_SAFE_NO_PAD.decode(&data).unwrap();
                        received += decoded.len();
                        target
                            .checkpoint_chunk(
                                1,
                                room,
                                transfer,
                                term,
                                base_revision,
                                revision,
                                offset,
                                data,
                            )
                            .await
                            .unwrap();
                        match target_rx.recv().await.unwrap() {
                            Event::CheckpointAck { offset, .. } => {
                                cumulative_ack = offset;
                            }
                            _ => panic!("each received chunk returns cumulative credit"),
                        }
                    }
                    _ => panic!("only four checkpoint chunks may occupy a window"),
                }
            }
            assert!(source_rx.try_recv().is_err());

            if window == 0 {
                source
                    .checkpoint_ack(1, room, transfer.transfer, cumulative_ack - 1)
                    .unwrap();
                assert!(matches!(
                    source_rx.recv().await,
                    Some(Event::Error { code, .. }) if code == "invalid_checkpoint_ack"
                ));
                assert_eq!(source.outgoing_transfer.as_ref().unwrap().acked_offset, 0);
            }
            source
                .checkpoint_ack(1, room, transfer.transfer, cumulative_ack)
                .unwrap();
            if received < bytes.len() && window == 0 {
                let next_offset = source.outgoing_transfer.as_ref().unwrap().next_offset;
                source
                    .checkpoint_ack(1, room, transfer.transfer, cumulative_ack)
                    .unwrap();
                assert_eq!(
                    source.outgoing_transfer.as_ref().unwrap().next_offset,
                    next_offset,
                    "duplicate cumulative ACK must not create credit"
                );
            }
            window += 1;
        }

        let (length, digest) = match source_rx.recv().await.unwrap() {
            Event::CheckpointEnd { length, digest, .. } => (length, digest),
            _ => panic!("checkpoint end must follow acknowledged chunks"),
        };
        assert_eq!(length as usize, bytes.len());
        assert_eq!(digest, transfer.digest_hex());
        assert_eq!(
            target
                .incoming_transfer
                .take()
                .unwrap()
                .finish()
                .unwrap()
                .bytes,
            bytes
        );
        source
            .checkpoint_ack(1, room, transfer.transfer, length)
            .unwrap();
        assert!(matches!(
            source_rx.recv().await,
            Some(Event::CheckpointCommitted {
                transfer: 17,
                revision: 9,
                ..
            })
        ));
        assert!(source.outgoing_transfer.is_none());
        source_endpoint.close().await;
        target_endpoint.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn committed_marker_precedes_the_next_revision_export() {
    let endpoint = endpoint().await;
    let (events, mut receiver) = mpsc::channel(LIFECYCLE_EVENT_RESERVE + 1);
    let mut actor = test_actor(endpoint.clone(), events);
    let room = [44; 16];
    actor.epoch = 1;
    actor.room = Some(room);
    assert!(actor.emit_bulk(Event::CheckpointAck {
        epoch: 1,
        room,
        transfer: 16,
        offset: 4,
    }));
    actor.pending_checkpoint_committed = Some(PendingCheckpointCommitted {
        epoch: 1,
        room,
        transfer: 17,
        term: 3,
        base_revision: 7,
        revision: 8,
        length: 4,
        digest: recovery::hex_digest(&recovery::sha256(b"old")),
    });
    let next = CheckpointTransfer::new(room, 18, 3, 8, 9, b"next".to_vec()).unwrap();

    actor.start_outgoing_checkpoint(next);

    assert!(matches!(
        receiver.recv().await,
        Some(Event::CheckpointAck { transfer: 16, .. })
    ));
    // Capacity became available after the marker pump failed. Pumping the
    // newer export alone must still leave that slot for the old marker.
    actor.pump_outgoing_checkpoint();
    assert!(receiver.try_recv().is_err());
    actor.pump_pending_checkpoint_committed();
    assert!(matches!(
        receiver.recv().await,
        Some(Event::CheckpointCommitted {
            transfer: 17,
            revision: 8,
            ..
        })
    ));
    actor.pump_outgoing_checkpoint();
    assert!(matches!(
        receiver.recv().await,
        Some(Event::CheckpointBegin {
            transfer: 18,
            base_revision: 8,
            revision: 9,
            ..
        })
    ));
    endpoint.close().await;
}

#[tokio::test]
async fn a_new_revision_does_not_replace_an_export_in_flight() {
    let endpoint = endpoint().await;
    let (events, _receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(endpoint.clone(), events);
    let room = [45; 16];
    actor.epoch = 1;
    actor.room = Some(room);
    let current = CheckpointTransfer::new(room, 17, 3, 7, 8, vec![7; 96 * 1024]).unwrap();
    let newer = CheckpointTransfer::new(room, 18, 3, 8, 9, vec![8; 96 * 1024]).unwrap();

    actor.start_outgoing_checkpoint(current);
    actor.start_outgoing_checkpoint(newer);

    let outgoing = actor.outgoing_transfer.as_ref().unwrap();
    assert_eq!(outgoing.transfer.transfer, 17);
    assert_eq!(outgoing.transfer.revision, 8);
    endpoint.close().await;
}

#[tokio::test]
async fn completed_receiver_accepts_exact_retry_before_delayed_commit_marker() {
    timeout(Duration::from_secs(5), async {
        let source_endpoint = endpoint().await;
        let target_endpoint = endpoint().await;
        let (source_events, mut source_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (target_events, mut target_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut source = test_actor(source_endpoint.clone(), source_events);
        let mut target = test_actor(target_endpoint.clone(), target_events);
        let room = [46; 16];
        source.epoch = 1;
        source.room = Some(room);
        target.epoch = 1;
        target.room = Some(room);
        let bytes: Vec<u8> = (0..CHECKPOINT_CHUNK_BYTES * (CHECKPOINT_WINDOW + 2) + 37)
            .map(|index| (index.wrapping_mul(41) & 255) as u8)
            .collect();
        let transfer = CheckpointTransfer::new(room, 19, 4, 9, 10, bytes.clone()).unwrap();
        source.start_outgoing_checkpoint(transfer.clone());

        // Complete the receiver's body, but delay its final ACK long enough
        // for the sender to expire. This pins the exact production retry
        // state: more than one four-credit window is retained while the
        // matching committed marker has not arrived yet.
        loop {
            match source_rx.recv().await.unwrap() {
                Event::CheckpointBegin {
                    transfer: transfer_id,
                    term,
                    base_revision,
                    revision,
                    length,
                    digest,
                    ..
                } => {
                    target
                        .checkpoint_begin(
                            1,
                            room,
                            transfer_id,
                            term,
                            base_revision,
                            revision,
                            length,
                            digest,
                        )
                        .await
                        .unwrap();
                }
                Event::CheckpointChunk {
                    transfer: transfer_id,
                    term,
                    base_revision,
                    revision,
                    offset,
                    data,
                    ..
                } => {
                    target
                        .checkpoint_chunk(
                            1,
                            room,
                            transfer_id,
                            term,
                            base_revision,
                            revision,
                            offset,
                            data,
                        )
                        .await
                        .unwrap();
                    let Event::CheckpointAck { offset, .. } = target_rx.recv().await.unwrap()
                    else {
                        panic!("valid checkpoint chunk must receive cumulative credit")
                    };
                    source.checkpoint_ack(1, room, transfer_id, offset).unwrap();
                }
                Event::CheckpointEnd { .. } => break,
                _ => panic!("unexpected checkpoint event before delayed marker"),
            }
        }
        assert_eq!(target.incoming_transfer.as_ref().unwrap().bytes, bytes);
        source.outgoing_transfer.as_mut().unwrap().started =
            tokio::time::Instant::now() - CHECKPOINT_TRANSFER_TIMEOUT - Duration::from_millis(1);
        source.expire_checkpoint_transfers();
        assert!(matches!(
            source_rx.recv().await,
            Some(Event::Error { code, .. }) if code == "checkpoint_send_timeout"
        ));

        target
            .checkpoint_begin(
                1,
                room,
                transfer.transfer + 1,
                transfer.term,
                transfer.base_revision,
                transfer.revision,
                transfer.bytes.len() as u32,
                transfer.digest_hex(),
            )
            .await
            .unwrap();
        assert!(matches!(
            target_rx.recv().await,
            Some(Event::Error { code, .. }) if code == "checkpoint_transfer_busy_incoming"
        ));
        assert_eq!(
            target.incoming_transfer.as_ref().unwrap().transfer,
            transfer.transfer,
            "a mismatched retry must not replace the retained body"
        );

        source.start_outgoing_checkpoint(transfer.clone());
        let Event::CheckpointBegin {
            transfer: transfer_id,
            term,
            base_revision,
            revision,
            length,
            digest,
            ..
        } = source_rx.recv().await.unwrap()
        else {
            panic!("retry must restart with its exact checkpoint begin")
        };
        target
            .checkpoint_begin(
                1,
                room,
                transfer_id,
                term,
                base_revision,
                revision,
                length,
                digest,
            )
            .await
            .unwrap();
        assert!(
            target_rx.try_recv().is_err(),
            "exact retry begin needs no credit"
        );

        loop {
            match source_rx.recv().await.unwrap() {
                Event::CheckpointChunk {
                    transfer: transfer_id,
                    term,
                    base_revision,
                    revision,
                    offset,
                    data,
                    ..
                } => {
                    target
                        .checkpoint_chunk(
                            1,
                            room,
                            transfer_id,
                            term,
                            base_revision,
                            revision,
                            offset,
                            data,
                        )
                        .await
                        .unwrap();
                    let Event::CheckpointAck { offset, .. } = target_rx.recv().await.unwrap()
                    else {
                        panic!("retained exact chunk must return its chunk-end credit")
                    };
                    source.checkpoint_ack(1, room, transfer_id, offset).unwrap();
                }
                Event::CheckpointEnd { length, .. } => {
                    assert_eq!(target.incoming_transfer.as_ref().unwrap().bytes, bytes);
                    source
                        .checkpoint_ack(1, room, transfer.transfer, length)
                        .unwrap();
                    break;
                }
                _ => panic!("unexpected checkpoint retry event"),
            }
        }
        assert!(matches!(
            source_rx.recv().await,
            Some(Event::CheckpointCommitted {
                transfer: 19,
                revision: 10,
                ..
            })
        ));
        source_endpoint.close().await;
        target_endpoint.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn bulk_checkpoint_events_preserve_lifecycle_capacity() {
    let endpoint = endpoint().await;
    let (events, mut receiver) = mpsc::channel(LIFECYCLE_EVENT_RESERVE + 1);
    let mut actor = test_actor(endpoint.clone(), events);
    actor.epoch = 1;
    actor.room = Some([49; 16]);
    assert!(actor.emit_bulk(Event::CheckpointAck {
        epoch: 1,
        room: [49; 16],
        transfer: 1,
        offset: 0,
    }));
    assert!(!actor.emit_bulk(Event::CheckpointAck {
        epoch: 1,
        room: [49; 16],
        transfer: 1,
        offset: 1,
    }));
    actor.emit(Event::RoomClosed { epoch: 1 }).unwrap();
    assert!(matches!(
        receiver.recv().await,
        Some(Event::CheckpointAck { .. })
    ));
    assert!(matches!(
        receiver.recv().await,
        Some(Event::RoomClosed { epoch: 1 })
    ));
    endpoint.close().await;
}

#[tokio::test]
async fn actor_replaces_authenticated_control_from_same_endpoint() {
    timeout(Duration::from_secs(15), async {
        let host = endpoint().await;
        let remote = endpoint().await;
        let relay = iroh::defaults::prod::default_relay_map()
            .urls::<Vec<_>>()
            .remove(0);
        let invite =
            Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600).unwrap();
        let room = invite.room();
        let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_fault, failure) = watch::channel(false);
        let mut actor = test_actor(host.clone(), events_tx);
        actor.epoch = 1;
        actor.room = Some(room);
        actor.hosted = Some(invite.clone());
        actor.room_invite = Some(invite.clone());
        let service = tokio::spawn(async move {
            let result = actor.run(command_rx, failure).await;
            actor.clear_room();
            actor.tasks.shutdown().await;
            result
        });
        let _service_scope = TaskScope(vec![service.abort_handle()]);

        let first_connection = remote.connect(address(&host), CONTROL_ALPN).await.unwrap();
        let _first = transport::connect_control_on(first_connection, &invite)
            .await
            .unwrap();
        let _ = next(&mut events, "connected").await;

        // A clean helper Leave closes the old control at the remote end,
        // but QUIC close delivery can race the next Join. The replacement
        // is authenticated by the same endpoint identity and room proof,
        // so it must supersede the stale worker without waiting for a
        // close notification tick.
        let second_connection = remote.connect(address(&host), CONTROL_ALPN).await.unwrap();
        let mut second = transport::connect_control_on(second_connection, &invite)
            .await
            .unwrap();
        let _ = next(&mut events, "connected").await;
        second
            .sender
            .send(&ControlFrame {
                message_id: 2,
                payload: b"replacement control".to_vec(),
            })
            .await
            .unwrap();
        assert!(matches!(
            next(&mut events, "message").await,
            Event::Message { payload, .. } if payload == "replacement control"
        ));
        host.close().await;
        remote.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn rejoin_expiry_exception_requires_current_native_membership() {
    let local = endpoint().await;
    let remote = endpoint().await;
    let (events, _receiver) = mpsc::channel(128);
    let mut actor = test_actor(local.clone(), events);
    actor.room = Some([7; 16]);
    actor.remember_admission(Admission {
        room: [7; 16],
        incarnation: 9,
        authority_term: 1,
        coordination_endpoint: remote.id(),
        coordination_address: address(&remote),
        primary_endpoint: remote.id(),
    });
    assert_eq!(actor.control_rejoin_member(remote.id()), None);
    actor.committed_native_members = Some(BTreeSet::from([remote.id()]));
    assert_eq!(actor.control_rejoin_member(remote.id()), Some(9));
    assert_eq!(actor.control_rejoin_member(local.id()), None);
    actor.pending_retired_incarnations.insert(9);
    assert_eq!(actor.control_rejoin_member(remote.id()), None);
    actor.pending_retired_incarnations.clear();
    actor.retired_incarnations.insert(9);
    assert_eq!(actor.control_rejoin_member(remote.id()), None);
    actor.retired_incarnations.clear();
    actor.room = Some([8; 16]);
    assert_eq!(actor.control_rejoin_member(remote.id()), None);
    actor.room = Some([7; 16]);
    actor.committed_native_members.as_mut().unwrap().clear();
    assert_eq!(actor.control_rejoin_member(remote.id()), None);
    local.close().await;
    remote.close().await;
}

#[tokio::test]
async fn incomplete_game_marker_does_not_block_actor_commands() {
    let local = endpoint().await;
    let remote = endpoint().await;
    let (client, server) = tokio::join!(remote.connect(address(&local), GAME_ALPN), async {
        local.accept().await.unwrap().await
    });
    let client = client.unwrap();
    let server = server.unwrap();
    let retired_connection = server.clone();
    let (events, mut receiver) = mpsc::channel(128);
    let mut actor = test_actor(local.clone(), events);
    actor.epoch = 7;
    actor.room = Some([71; 16]);
    actor.games.insert(
        remote.id(),
        GameSlot {
            auth: GameAuthorization {
                peer: remote.id(),
                key: crate::wire::MatchKey {
                    room: [71; 16],
                    generation: 3,
                },
                capability: [1; 32],
                max_packet: 1024,
            },
            local_port: 9,
            waiting: true,
            task: None,
            stats: None,
            route_connection: None,
            expires: tokio::time::Instant::now() + Duration::from_secs(60),
            prepare_deadline: None,
        },
    );
    actor.probe_peers.insert(remote.id());
    actor.probe_permissions.insert(
        remote.id(),
        ProbePermission {
            request: 1,
            pair_revision: 1,
            expires: tokio::time::Instant::now() + Duration::from_secs(20),
        },
    );
    timeout(
        Duration::from_millis(200),
        actor.completed(Completion::Incoming(7, Ok(server))),
    )
    .await
    .expect("remote marker blocked the actor")
    .unwrap();
    assert!(
        actor
            .command(Request {
                id: 17,
                command: Command::Status
            })
            .unwrap()
    );
    assert!(matches!(
        receiver.recv().await,
        Some(Event::Status { request_id: 17, .. })
    ));
    assert!(
        actor
            .command(Request {
                id: 19,
                command: Command::EndMatch {
                    epoch: 7,
                    generation: 3
                }
            })
            .unwrap()
    );
    assert!(actor.pending_game_admissions.is_empty());
    // Cancellation releases the preliminary connection immediately, so
    // a new match from this peer need not wait for the old deadline.
    let _ = actor.tasks.join_next().await;
    timeout(Duration::from_secs(1), client.closed())
        .await
        .unwrap();
    let (retry_client, retry_server) =
        tokio::join!(remote.connect(address(&local), GAME_ALPN), async {
            local.accept().await.unwrap().await
        });
    let retry_client = retry_client.unwrap();
    let retry_server = retry_server.unwrap();
    let retry_id = retry_server.stable_id();
    actor
        .completed(Completion::Incoming(7, Ok(retry_server)))
        .await
        .unwrap();
    actor
        .completed(Completion::ClassifiedGame(
            IncomingGame {
                epoch: 7,
                peer: remote.id(),
                connection: retired_connection,
                generation: Some(3),
                probe: Some((1, 1)),
                deadline: tokio::time::Instant::now() + Duration::from_secs(10),
            },
            Err(failed("old completion queued before cancellation")),
        ))
        .await
        .unwrap();
    assert_eq!(
        actor
            .pending_game_admissions
            .get(&remote.id())
            .unwrap()
            .0
            .stable_id(),
        retry_id
    );
    drop(retry_client);

    assert!(
        !actor
            .command(Request {
                id: 18,
                command: Command::Shutdown
            })
            .unwrap()
    );
    drop(client);
    local.close().await;
    remote.close().await;
}

#[tokio::test]
async fn completed_probe_keeps_connection_open_for_gameplay_upgrade() {
    timeout(Duration::from_secs(15), async {
        let host = endpoint().await;
        let guest = endpoint().await;
        let room = [47; 16];
        let request = 9;
        let pair_revision = 23;
        let (host_probe, guest_connection) = tokio::join!(
            run_probe(
                host.clone(),
                address(&guest),
                room,
                guest.id(),
                request,
                pair_revision,
                false,
            ),
            async {
                let connection = guest.accept().await.unwrap().await.unwrap();
                match transport::accept_game_stream(&connection).await.unwrap() {
                    transport::GameStream::Probe(send, recv) => {
                        serve_probe(connection, send, recv, room, request, pair_revision).await
                    }
                    transport::GameStream::Gameplay(_, _) => Err(failed("expected probe stream")),
                }
            }
        );
        let host_probe = host_probe.unwrap();
        let host_connection = host_probe.connection.unwrap();
        let guest_connection = guest_connection.unwrap();
        assert!(host_connection.close_reason().is_none());
        assert!(guest_connection.close_reason().is_none());
        assert_eq!(
            recovery::summarize_datagram_probe(
                &host_probe.samples_us,
                host_probe.metrics.expected,
                host_probe.metrics.sent
            )
            .status,
            "ready"
        );

        let key = MatchKey {
            room,
            generation: 1,
        };
        let host_auth = GameAuthorization {
            peer: guest.id(),
            key,
            capability: [71; 32],
            max_packet: 1024,
        };
        let guest_auth = GameAuthorization {
            peer: host.id(),
            ..host_auth.clone()
        };
        let (_deadline_sender, deadline) =
            watch::channel(tokio::time::Instant::now() + transport::PREPARED_GAME_TIMEOUT);
        let (host_game, guest_game) = tokio::join!(
            transport::accept_game_on(host_connection, host_auth, deadline),
            transport::connect_game_on(guest_connection, guest_auth),
        );
        assert!(host_game.is_ok());
        assert!(guest_game.is_ok());
        host.close().await;
        guest.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn closed_probe_invalidation_retries_after_bulk_backpressure() {
    timeout(Duration::from_secs(10), async {
        let local = endpoint().await;
        let remote = endpoint().await;
        let (connection, remote_connection) =
            tokio::join!(local.connect(address(&remote), GAME_ALPN), async {
                remote.accept().await.unwrap().await
            });
        let connection = connection.unwrap();
        let remote_connection = remote_connection.unwrap();
        let original_route = selected_probe_route(&connection);
        assert_ne!(original_route, "unavailable");

        let (events, mut receiver) = mpsc::channel(LIFECYCLE_EVENT_RESERVE + 1);
        let mut actor = test_actor(local.clone(), events);
        actor.epoch = 7;
        actor.room = Some([59; 16]);
        actor.probe_reservations.insert(
            remote.id(),
            ProbeReservation {
                reported: true,
                connection: connection.clone(),
                request: 23,
                pair_revision: 37,
                route: original_route.clone(),
            },
        );
        assert!(actor.emit_bulk(Event::CheckpointAck {
            epoch: 7,
            room: [59; 16],
            transfer: 1,
            offset: 0,
        }));

        remote_connection.close(1u32.into(), b"closed probe regression");
        connection.closed().await;
        assert!(connection.close_reason().is_some());
        assert_eq!(selected_probe_route(&connection), original_route);
        actor.invalidate_changed_probe_routes();
        assert!(!actor.probe_reservations.contains_key(&remote.id()));
        assert!(actor.pending_probe_invalidations.contains_key(&remote.id()));
        assert!(matches!(
            receiver.recv().await,
            Some(Event::CheckpointAck { .. })
        ));

        actor.invalidate_changed_probe_routes();
        assert!(!actor.pending_probe_invalidations.contains_key(&remote.id()));
        assert!(matches!(
            receiver.recv().await,
            Some(Event::ProbeResult {
                peer,
                request: 23,
                pair_revision: 37,
                route,
                status,
                ..
            }) if peer == remote.id() && route == "unavailable" && status == "invalidated"
        ));
        local.close().await;
        remote.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn obsolete_probe_completion_cannot_replace_newer_peer_state() {
    timeout(Duration::from_secs(10), async {
        let local = endpoint().await;
        let remote = endpoint().await;
        let (new_connection, new_remote) =
            tokio::join!(local.connect(address(&remote), GAME_ALPN), async {
                remote.accept().await.unwrap().await
            });
        let (old_connection, old_remote) =
            tokio::join!(local.connect(address(&remote), GAME_ALPN), async {
                remote.accept().await.unwrap().await
            });
        let new_connection = new_connection.unwrap();
        let old_connection = old_connection.unwrap();
        let old_observer = old_connection.clone();
        let (events, _receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(local.clone(), events);
        actor.epoch = 5;
        actor.room = Some([61; 16]);
        actor.probe_peers.insert(remote.id());
        actor.probe_permissions.insert(
            remote.id(),
            ProbePermission {
                request: 29,
                pair_revision: 41,
                expires: tokio::time::Instant::now() + Duration::from_secs(5),
            },
        );
        actor.probe_reservations.insert(
            remote.id(),
            ProbeReservation {
                reported: true,
                route: selected_probe_route(&new_connection),
                connection: new_connection,
                request: 29,
                pair_revision: 41,
            },
        );
        actor.pending_probe_invalidations.insert(
            remote.id(),
            PendingProbeInvalidation {
                request: 29,
                pair_revision: 41,
                route: "newer route".into(),
            },
        );

        actor
            .completed(Completion::Probe(
                5,
                Ok(ProbeCompletion {
                    peer: remote.id(),
                    request: 11,
                    pair_revision: 17,
                    samples_us: Vec::new(),
                    connection: Some(old_connection),
                    report: false,
                    route_changed: false,
                    metrics: crate::probe::Metrics::default(),
                }),
            ))
            .await
            .unwrap();

        assert!(actor.probe_peers.contains(&remote.id()));
        assert!(
            actor
                .probe_permissions
                .get(&remote.id())
                .is_some_and(
                    |permission| permission.request == 29 && permission.pair_revision == 41
                )
        );
        assert!(
            actor
                .probe_reservations
                .get(&remote.id())
                .is_some_and(
                    |reservation| reservation.request == 29 && reservation.pair_revision == 41
                )
        );
        assert!(
            actor
                .pending_probe_invalidations
                .get(&remote.id())
                .is_some_and(
                    |invalidation| invalidation.request == 29 && invalidation.pair_revision == 41
                )
        );
        old_observer.closed().await;
        assert!(old_observer.close_reason().is_some());
        drop(new_remote);
        drop(old_remote);
        local.close().await;
        remote.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn stable_voters_never_reselect_pending_or_retired_incarnations() {
    let local = endpoint().await;
    let (events, _events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(local.clone(), events);
    let room = [17; 16];
    for incarnation in 1..=4 {
        actor.admissions.insert(
            incarnation,
            Admission {
                room,
                incarnation,
                authority_term: 1,
                coordination_endpoint: local.id(),
                coordination_address: address(&local),
                primary_endpoint: local.id(),
            },
        );
        actor.admission_order.push(incarnation);
    }
    actor.pending_retired_incarnations.insert(2);
    actor.retired_incarnations.insert(3);

    assert_eq!(
        actor.stable_voters(BTreeSet::from([1, 2, 3]), 3),
        BTreeSet::from([1, 4])
    );
    local.close().await;
}

#[tokio::test]
async fn native_rebound_uses_only_the_applied_unambiguous_incarnation() {
    let primary = endpoint().await;
    let old_coordination = endpoint().await;
    let fresh_coordination = endpoint().await;
    let room = [18; 16];
    let admission = |incarnation, coordination: &Endpoint| Admission {
        room,
        incarnation,
        authority_term: 1,
        coordination_endpoint: coordination.id(),
        coordination_address: address(coordination),
        primary_endpoint: primary.id(),
    };
    // Incarnation identifiers are random. The retired value is
    // deliberately numerically larger than the fresh value so collection
    // order would select the wrong process without applied-membership
    // filtering.
    let admissions = BTreeMap::from([
        (900, admission(900, &old_coordination)),
        (100, admission(100, &fresh_coordination)),
    ]);
    assert_eq!(
        applied_member_incarnations(&admissions, &BTreeSet::from([100]), &BTreeSet::from([900]),),
        BTreeMap::from([(primary.id().to_string(), 100)])
    );
    assert!(
        applied_member_incarnations(&admissions, &BTreeSet::from([100, 900]), &BTreeSet::new(),)
            .is_empty()
    );
    primary.close().await;
    old_coordination.close().await;
    fresh_coordination.close().await;
}

#[tokio::test]
async fn fresh_learner_accepts_membership_only_from_invitation_authority() {
    timeout(Duration::from_secs(20), async {
        let host_primary = endpoint().await;
        let guest_primary = endpoint().await;
        let unrelated_primary = endpoint().await;
        let relay = iroh::defaults::prod::default_relay_map()
            .urls::<Vec<_>>()
            .remove(0);
        let seed = Invite::create(
            host_primary.id(),
            relay,
            "test-build".into(),
            now().unwrap(),
            3600,
        )
        .unwrap();
        let room = seed.room();
        let host = crate::recovery::RecoverySession::host(room, host_primary.id(), false)
            .await
            .unwrap();
        let guest = crate::recovery::RecoverySession::join(
            room,
            guest_primary.id(),
            host.incarnation,
            host.coordination_address.clone(),
            false,
        )
        .await
        .unwrap();
        assert_eq!(guest.coordinator.current_leader(), None);
        assert_eq!(
            guest.applied_membership_provenance().await,
            (BTreeSet::new(), BTreeSet::new())
        );

        let host_admission = host.advertise().await;
        let guest_admission = guest.advertise().await;
        let (events, _events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(guest_primary.clone(), events);
        actor.epoch = 1;
        actor.room = Some(room);
        // The compact sf4e2/emd2 invitation intentionally carries no
        // coordination incarnation. Its authenticated Admission bootstrap
        // has already bound the host's process to invite.endpoint().
        let compact = Invite::parse_for_build(
            &seed.encode_discord().unwrap(),
            now().unwrap(),
            "test-build",
        )
        .unwrap();
        assert_eq!(compact.authority_incarnation(), 0);
        actor.room_invite = Some(compact);
        actor.recovery = Some(guest.clone());
        actor.remember_admission(host_admission.clone());
        actor.remember_admission(guest_admission.clone());
        let payload = serde_json::to_string(&CoordinationControl::Membership {
            admissions: vec![host_admission, guest_admission],
            retired: Vec::new(),
        })
        .unwrap();

        assert!(
            actor
                .accept_coordination_control(unrelated_primary.id(), &payload)
                .await
                .is_err()
        );
        assert!(
            actor
                .accept_coordination_control(host_primary.id(), &payload)
                .await
                .unwrap()
        );

        guest.stop().await;
        host.stop().await;
        host_primary.close().await;
        guest_primary.close().await;
        unrelated_primary.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn delayed_admission_cannot_readd_authenticated_removed_voter() {
    timeout(Duration::from_secs(25), async {
        let host_primary = endpoint().await;
        let departing_primary = endpoint().await;
        let room = [29; 16];
        let host = crate::recovery::RecoverySession::host(room, host_primary.id(), false)
            .await
            .unwrap();
        let departing = crate::recovery::RecoverySession::join(
            room,
            departing_primary.id(),
            host.incarnation,
            host.coordination_address.clone(),
            false,
        )
        .await
        .unwrap();
        let admission = departing.advertise().await;
        host.add_learner(&admission).await.unwrap();
        host.promote_voters(BTreeSet::from([host.incarnation, admission.incarnation]))
            .await
            .unwrap();

        // This is the service Admission precheck before its await. The
        // authenticated method-7 removal then commits in that window.
        let (precheck_members, precheck_history) = host.applied_membership_provenance().await;
        assert!(precheck_members.contains(&admission.incarnation));
        assert!(precheck_history.contains(&admission.incarnation));
        assert_eq!(
            host.coordinator
                .dispatch(admission.incarnation, "remove", &[0])
                .await
                .unwrap(),
            vec![1]
        );
        let (members, history) = host.applied_membership_provenance().await;
        assert_eq!(members, BTreeSet::from([host.incarnation]));
        assert!(history.contains(&admission.incarnation));

        assert!(host.add_learner(&admission).await.is_err());
        assert!(host.rpc.is_retired(admission.incarnation).await);
        assert_eq!(
            host.applied_member_ids().await,
            BTreeSet::from([host.incarnation])
        );
        assert_eq!(
            host.applied_voter_ids().await,
            BTreeSet::from([host.incarnation])
        );

        departing.stop().await;
        host.stop().await;
        host_primary.close().await;
        departing_primary.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn successor_rejects_replayed_admission_from_applied_membership_history() {
    timeout(Duration::from_secs(20), async {
        let host = endpoint().await;
        let departed_primary = endpoint().await;
        let relay = iroh::defaults::prod::default_relay_map()
            .urls::<Vec<_>>()
            .remove(0);
        let seed =
            Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600).unwrap();
        let room = seed.room();
        let (events, _events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(host.clone(), events);
        let invite = actor.setup_host_recovery(seed).await.unwrap();
        actor.epoch = 1;
        actor.room = Some(room);
        actor.hosted = Some(invite.clone());
        actor.room_invite = Some(invite.clone());
        let recovery = actor.recovery.clone().unwrap();
        let departed = crate::recovery::RecoverySession::join(
            room,
            departed_primary.id(),
            recovery.incarnation,
            recovery.coordination_address.clone(),
            false,
        )
        .await
        .unwrap();
        let admission = departed.advertise().await;
        recovery.add_learner(&admission).await.unwrap();
        recovery
            .remove_nodes(BTreeSet::from([admission.incarnation]))
            .await
            .unwrap();
        assert!(
            !recovery
                .applied_member_ids()
                .await
                .contains(&admission.incarnation)
        );

        // Model a successor actor that receives both membership entries
        // before its one-second state tick. Its actor-local observation is
        // empty, while the replicated state machine has seen the member.
        actor.remember_admission(admission.clone());
        assert!(actor.applied_admission_members.is_empty());
        let payload = serde_json::to_string(&CoordinationControl::Admission {
            admission: admission.clone(),
        })
        .unwrap();
        assert!(
            actor
                .accept_coordination_control(departed_primary.id(), &payload)
                .await
                .is_err()
        );
        assert!(
            actor
                .pending_retired_incarnations
                .contains(&admission.incarnation)
        );
        assert!(
            !recovery
                .applied_member_ids()
                .await
                .contains(&admission.incarnation)
        );

        departed.stop().await;
        recovery.stop().await;
        host.close().await;
        departed_primary.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn retirement_cap_fences_rpc_and_admission_before_exact_history_saturates() {
    let host = endpoint().await;
    let departed_primary = endpoint().await;
    let relay = iroh::defaults::prod::default_relay_map()
        .urls::<Vec<_>>()
        .remove(0);
    let seed = Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600).unwrap();
    let room = seed.room();
    let (events, _events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(host.clone(), events);
    let _invite = actor.setup_host_recovery(seed).await.unwrap();
    let recovery = actor.recovery.clone().unwrap();
    let incarnation = u64::MAX - 1;
    let admission = Admission {
        room,
        incarnation,
        authority_term: 1,
        coordination_endpoint: departed_primary.id(),
        coordination_address: address(&departed_primary),
        primary_endpoint: departed_primary.id(),
    };
    recovery
        .rpc
        .admit_bound(
            incarnation,
            admission.coordination_address.clone(),
            admission.primary_endpoint,
        )
        .await
        .unwrap();
    actor.remember_admission(admission);
    actor
        .retired_incarnations
        .extend(1..=MAX_RETIRED_INCARNATIONS as u64);
    actor.pending_retired_incarnations.insert(incarnation);

    actor.apply_pending_retirements().await.unwrap();
    assert!(recovery.rpc.is_retired(incarnation).await);
    assert!(!actor.admissions.contains_key(&incarnation));
    assert!(!actor.admission_order.contains(&incarnation));
    assert!(actor.pending_retired_incarnations.contains(&incarnation));
    assert_eq!(actor.retired_incarnations.len(), MAX_RETIRED_INCARNATIONS);

    recovery.stop().await;
    host.close().await;
    departed_primary.close().await;
}

#[tokio::test]
async fn held_checkpoint_and_admission_writes_do_not_block_actor_or_import_stale_completion() {
    timeout(Duration::from_secs(20), async {
        let host_primary = endpoint().await;
        let voter_primary = endpoint().await;
        let joining_primary = endpoint().await;
        let relay = iroh::defaults::prod::default_relay_map()
            .urls::<Vec<_>>()
            .remove(0);
        let seed = Invite::create(
            host_primary.id(),
            relay,
            "test-build".into(),
            now().unwrap(),
            3600,
        )
        .unwrap();
        let room = seed.room();
        let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(host_primary.clone(), events_tx);
        let invite = actor.setup_host_recovery(seed).await.unwrap();
        actor.epoch = 1;
        actor.room = Some(room);
        actor.hosted = Some(invite.clone());
        actor.room_invite = Some(invite.clone());
        let recovery = actor.recovery.clone().unwrap();

        let voter = crate::recovery::RecoverySession::join(
            room,
            voter_primary.id(),
            recovery.incarnation,
            recovery.coordination_address.clone(),
            false,
        )
        .await
        .unwrap();
        let voter_admission = voter.advertise().await;
        recovery.add_learner(&voter_admission).await.unwrap();
        recovery
            .promote_voters(BTreeSet::from([
                recovery.incarnation,
                voter.incarnation,
            ]))
            .await
            .unwrap();
        actor.remember_admission(voter_admission);

        let joining = crate::recovery::RecoverySession::join(
            room,
            joining_primary.id(),
            recovery.incarnation,
            recovery.coordination_address.clone(),
            false,
        )
        .await
        .unwrap();
        let joining_admission = joining.advertise().await;
        let connecting = async {
            let connection = joining_primary
                .connect(address(&host_primary), CONTROL_ALPN)
                .await
                .unwrap();
            transport::connect_control_on(connection, &invite)
                .await
                .unwrap()
        };
        let accepting = async {
            let connection = host_primary.accept().await.unwrap().await.unwrap();
            transport::accept_control(connection, &invite).await.unwrap()
        };
        let (mut joining_control, host_control) = tokio::join!(connecting, accepting);
        actor.controls.insert(
            joining_primary.id(),
            ControlWorker::start(host_control),
        );

        // Hold the exact membership serialization lock and remove the
        // second voter. The checkpoint proposal now waits for quorum,
        // while an authenticated Admission waits behind this lock.
        let membership_gate = recovery.coordinator.membership_operations.lock().await;
        voter.stop().await;
        let term = recovery.coordinator.current_term();
        assert_eq!(recovery.coordinator.current_leader(), Some(recovery.incarnation));

        let (commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_failed_tx, failed_rx) = watch::channel(false);
        let service = tokio::spawn(async move {
            let result = actor.run(command_rx, failed_rx).await;
            (actor, result)
        });
        let body = serde_json::to_vec(&serde_json::json!({
            "version": 1,
            "request": 41,
            "term": term,
            "base_revision": 0,
            "checkpoint": {"room": "held-quorum"},
            "effects": [],
            "effects_digest": "4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945"
        }))
        .unwrap();
        let transfer = CheckpointTransfer::new(room, 41, term, 0, 1, body.clone()).unwrap();
        let digest = transfer.digest_hex();
        let send = |id, command| Request { id, command };
        commands
            .send(send(
                2,
                Command::CheckpointBegin {
                    epoch: 1,
                    room,
                    transfer: 41,
                    term,
                    base_revision: 0,
                    revision: 1,
                    length: body.len() as u32,
                    digest: digest.clone(),
                },
            ))
            .await
            .unwrap();
        commands
            .send(send(
                3,
                Command::CheckpointChunk {
                    epoch: 1,
                    room,
                    transfer: 41,
                    term,
                    base_revision: 0,
                    revision: 1,
                    offset: 0,
                    data: URL_SAFE_NO_PAD.encode(&body),
                },
            ))
            .await
            .unwrap();
        commands
            .send(send(
                4,
                Command::CheckpointEnd {
                    epoch: 1,
                    room,
                    transfer: 41,
                    term,
                    base_revision: 0,
                    revision: 1,
                    length: body.len() as u32,
                    digest: digest.clone(),
                },
            ))
            .await
            .unwrap();

        joining_control
            .sender
            .send(&ControlFrame {
                message_id: 2,
                payload: serde_json::to_vec(&CoordinationControl::Admission {
                    admission: joining_admission,
                })
                .unwrap(),
            })
            .await
            .unwrap();
        joining_control
            .sender
            .send(&ControlFrame {
                message_id: 3,
                payload: serde_json::to_vec(&NativeControlMessage {
                    kind: "native_control".into(),
                    message_id: 77,
                    payload: "lifecycle-after-held-admission".into(),
                })
                .unwrap(),
            })
            .await
            .unwrap();

        // An exact retry while the write waiter is held owns only a cursor
        // and still returns cumulative ACK credit through the actor loop.
        commands
            .send(send(
                5,
                Command::CheckpointBegin {
                    epoch: 1,
                    room,
                    transfer: 41,
                    term,
                    base_revision: 0,
                    revision: 1,
                    length: body.len() as u32,
                    digest: digest.clone(),
                },
            ))
            .await
            .unwrap();
        commands
            .send(send(
                6,
                Command::CheckpointChunk {
                    epoch: 1,
                    room,
                    transfer: 41,
                    term,
                    base_revision: 0,
                    revision: 1,
                    offset: 0,
                    data: URL_SAFE_NO_PAD.encode(&body),
                },
            ))
            .await
            .unwrap();
        commands
            .send(send(7, Command::Status))
            .await
            .unwrap();

        let deadline = tokio::time::Instant::now() + Duration::from_secs(6);
        let mut acknowledgements = 0;
        let mut saw_message = false;
        let mut saw_status = false;
        let mut saw_quorum_lost = false;
        while acknowledgements < 2 || !saw_message || !saw_status || !saw_quorum_lost {
            let event = tokio::select! {
                event = events.recv() => event.unwrap(),
                _ = tokio::time::sleep_until(deadline) => panic!("actor stalled behind coordination write"),
            };
            match event {
                Event::CheckpointAck { transfer: 41, offset, .. } => {
                    assert_eq!(offset as usize, body.len());
                    acknowledgements += 1;
                }
                Event::Message { message_id: 77, payload, .. } => {
                    assert_eq!(payload, "lifecycle-after-held-admission");
                    saw_message = true;
                }
                Event::Status { request_id: 7, .. } => saw_status = true,
                Event::CoordinationState { writable: false, .. } => {
                    saw_quorum_lost = true;
                }
                Event::Error { code, .. }
                    if code == "checkpoint_not_committed"
                        || code == "checkpoint_transfer_busy_proposal" => {}
                _ => {}
            }
        }

        commands
            .send(send(8, Command::Shutdown))
            .await
            .unwrap();
        let (mut actor, result) = timeout(Duration::from_secs(1), service)
            .await
            .expect("responsive shutdown")
            .unwrap();
        result.unwrap();
        let stale = CheckpointProposalKey {
            epoch: 1,
            room,
            incarnation: recovery.incarnation,
            transfer: transfer.transfer,
            term: transfer.term,
            base_revision: transfer.base_revision,
            revision: transfer.revision,
            length: transfer.bytes.len(),
            digest: transfer.digest,
        };
        actor.pending_checkpoint_proposal = Some(stale.clone());
        actor.epoch = 2;
        actor
            .completed(Completion::CheckpointProposal(
                stale,
                transfer,
                Ok(crate::coordination::Receipt {
                    accepted: true,
                    revision: 1,
                }),
            ))
            .await
            .unwrap();
        assert!(actor.outgoing_transfer.is_none());
        assert_eq!(actor.last_exported_revision, 0);
        actor.tasks.abort_all();
        drop(membership_gate);
        joining.stop().await;
        recovery.stop().await;
        joining_primary.close().await;
        voter_primary.close().await;
        host_primary.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn overlapping_admissions_are_serialized_and_reach_current_voter_default() {
    timeout(Duration::from_secs(25), async {
        let host_primary = endpoint().await;
        let first_primary = endpoint().await;
        let second_primary = endpoint().await;
        let relay = iroh::defaults::prod::default_relay_map()
            .urls::<Vec<_>>()
            .remove(0);
        let seed = Invite::create(
            host_primary.id(),
            relay,
            "test-build".into(),
            now().unwrap(),
            3600,
        )
        .unwrap();
        let room = seed.room();
        let (events, _events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(host_primary.clone(), events);
        let invite = actor.setup_host_recovery(seed).await.unwrap();
        actor.epoch = 1;
        actor.room = Some(room);
        actor.hosted = Some(invite.clone());
        actor.room_invite = Some(invite);
        let recovery = actor.recovery.clone().unwrap();
        let first = crate::recovery::RecoverySession::join(
            room,
            first_primary.id(),
            recovery.incarnation,
            recovery.coordination_address.clone(),
            false,
        )
        .await
        .unwrap();
        let second = crate::recovery::RecoverySession::join(
            room,
            second_primary.id(),
            recovery.incarnation,
            recovery.coordination_address.clone(),
            false,
        )
        .await
        .unwrap();
        let first_admission = first.advertise().await;
        let second_admission = second.advertise().await;

        actor
            .queue_admission_operation(first_primary.id(), vec![first_admission.clone()], true)
            .unwrap();
        actor
            .queue_admission_operation(first_primary.id(), vec![first_admission], true)
            .unwrap();
        assert!(actor.deferred_admissions.is_empty());
        actor
            .queue_admission_operation(second_primary.id(), vec![second_admission.clone()], true)
            .unwrap();
        assert_eq!(actor.deferred_admissions.len(), 1);

        while !actor.admissions.contains_key(&first.incarnation)
            || !actor.admissions.contains_key(&second.incarnation)
            || recovery.applied_voter_ids().await.len() != 3
        {
            let completion = timeout(Duration::from_secs(10), actor.tasks.join_next())
                .await
                .expect("serialized admission completion")
                .expect("admission worker exists")
                .unwrap();
            actor.completed(completion).await.unwrap();
        }
        assert_eq!(
            recovery.applied_voter_ids().await,
            BTreeSet::from([recovery.incarnation, first.incarnation, second.incarnation])
        );
        actor.tasks.abort_all();
        first.stop().await;
        second.stop().await;
        recovery.stop().await;
        host_primary.close().await;
        first_primary.close().await;
        second_primary.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn async_admission_completion_publishes_third_member_to_existing_follower() {
    timeout(Duration::from_secs(25), async {
        let host_primary = endpoint().await;
        let follower_primary = endpoint().await;
        let newcomer_primary = endpoint().await;
        let relay = iroh::defaults::prod::default_relay_map()
            .urls::<Vec<_>>()
            .remove(0);
        let seed = Invite::create(
            host_primary.id(),
            relay,
            "test-build".into(),
            now().unwrap(),
            3600,
        )
        .unwrap();
        let room = seed.room();
        let (events, _events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(host_primary.clone(), events);
        let invite = actor.setup_host_recovery(seed).await.unwrap();
        actor.epoch = 1;
        actor.room = Some(room);
        actor.hosted = Some(invite.clone());
        actor.room_invite = Some(invite.clone());
        let recovery = actor.recovery.clone().unwrap();

        let follower = crate::recovery::RecoverySession::join(
            room,
            follower_primary.id(),
            recovery.incarnation,
            recovery.coordination_address.clone(),
            false,
        )
        .await
        .unwrap();
        let follower_admission = follower.advertise().await;
        recovery.add_learner(&follower_admission).await.unwrap();
        recovery
            .promote_voters(BTreeSet::from([recovery.incarnation, follower.incarnation]))
            .await
            .unwrap();
        actor.remember_admission(follower_admission);

        // This established follower route predates the third admission.
        // It therefore receives the new binding only if the asynchronous
        // Admission completion publishes the post-commit roster.
        let connecting = async {
            let connection = follower_primary
                .connect(address(&host_primary), CONTROL_ALPN)
                .await
                .unwrap();
            transport::connect_control_on(connection, &invite)
                .await
                .unwrap()
        };
        let accepting = async {
            let connection = host_primary.accept().await.unwrap().await.unwrap();
            transport::accept_control(connection, &invite)
                .await
                .unwrap()
        };
        let (mut follower_control, host_control) = tokio::join!(connecting, accepting);
        actor
            .controls
            .insert(follower_primary.id(), ControlWorker::start(host_control));

        let newcomer = crate::recovery::RecoverySession::join(
            room,
            newcomer_primary.id(),
            recovery.incarnation,
            recovery.coordination_address.clone(),
            false,
        )
        .await
        .unwrap();
        let newcomer_admission = newcomer.advertise().await;
        actor
            .queue_admission_operation(
                newcomer_primary.id(),
                vec![newcomer_admission.clone()],
                true,
            )
            .unwrap();

        while !actor.admissions.contains_key(&newcomer.incarnation) {
            let completion = timeout(Duration::from_secs(10), actor.tasks.join_next())
                .await
                .expect("third admission completion")
                .expect("admission worker exists")
                .unwrap();
            actor.completed(completion).await.unwrap();
        }

        let frame = timeout(Duration::from_secs(5), follower_control.receiver.receive())
            .await
            .expect("post-admission membership publication")
            .unwrap();
        let published: CoordinationControl = serde_json::from_slice(&frame.payload).unwrap();
        let CoordinationControl::Membership {
            admissions,
            retired,
        } = published
        else {
            panic!("expected membership publication");
        };
        assert!(retired.is_empty());
        assert!(admissions.iter().any(|admission| {
            admission.incarnation == newcomer.incarnation
                && admission.primary_endpoint == newcomer_primary.id()
        }));
        assert!(admissions.iter().any(|admission| {
            admission.incarnation == follower.incarnation
                && admission.primary_endpoint == follower_primary.id()
        }));
        assert!(actor.pending_membership_publications.is_empty());

        actor.controls.clear();
        actor.tasks.abort_all();
        newcomer.stop().await;
        follower.stop().await;
        recovery.stop().await;
        newcomer_primary.close().await;
        follower_primary.close().await;
        host_primary.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn expired_probe_authorization_completion_never_installs_or_dials() {
    let host_primary = endpoint().await;
    let peer_primary = endpoint().await;
    let relay = iroh::defaults::prod::default_relay_map()
        .urls::<Vec<_>>()
        .remove(0);
    let seed = Invite::create(
        host_primary.id(),
        relay,
        "test-build".into(),
        now().unwrap(),
        3600,
    )
    .unwrap();
    let room = seed.room();
    let (events, mut events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(host_primary.clone(), events);
    let _invite = actor.setup_host_recovery(seed).await.unwrap();
    actor.epoch = 1;
    actor.room = Some(room);
    let recovery = actor.recovery.clone().unwrap();
    let target_incarnation = u64::MAX - 9;
    actor.remember_admission(Admission {
        room,
        incarnation: target_incarnation,
        authority_term: recovery.coordinator.current_term(),
        coordination_endpoint: peer_primary.id(),
        coordination_address: address(&peer_primary),
        primary_endpoint: peer_primary.id(),
    });
    let term = recovery.coordinator.current_term();
    let checkpoint = serde_json::to_vec(&serde_json::json!({
        "version": 1,
        "request": 50,
        "term": term,
        "base_revision": 0,
        "checkpoint": {
            "members": [
                {"member": 1, "incarnation": recovery.incarnation, "data": {"authenticatedEndpoint": host_primary.id().to_string()}},
                {"member": 2, "incarnation": target_incarnation, "data": {"authenticatedEndpoint": peer_primary.id().to_string()}}
            ],
            "room": {
                "version": 2,
                "snapshot": {
                    "members": [{"id": 1, "fighter": 3}, {"id": 2, "fighter": 7}],
                    "tables": [{"revision": 7, "p1": 1, "p2": 2}]
                }
            }
        },
        "effects": [],
        "effects_digest": "4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945"
    }))
    .unwrap();
    let transfer = CheckpointTransfer::new(room, 50, term, 0, 1, checkpoint).unwrap();
    let receipt = recovery
        .coordinator
        .propose(recovery.propose(&transfer).unwrap())
        .await
        .unwrap();
    assert!(receipt.accepted);
    assert!(
        recovery
            .probe_pair_bound(
                recovery.incarnation,
                target_incarnation,
                host_primary.id(),
                peer_primary.id(),
                7,
            )
            .await,
        "every completion fence except wall-clock expiry must be valid"
    );
    let key = ProbeAuthorizationKey {
        epoch: 1,
        room,
        incarnation: recovery.incarnation,
        peer: peer_primary.id(),
        target_incarnation,
        request: 51,
        pair_revision: 7,
        benchmark: false,
    };
    actor
        .pending_probe_authorizations
        .insert(peer_primary.id(), key.clone());
    actor.probe_peers.insert(peer_primary.id());
    let task_count = actor.tasks.len();
    actor
        .completed(Completion::ProbeAuthorization(
            key,
            Ok(ProbeAuthorization {
                term: recovery.coordinator.current_term(),
                leader: recovery.coordinator.current_leader(),
                revision: recovery.committed().await.revision,
                expires: now().unwrap().saturating_sub(1),
            }),
        ))
        .await
        .unwrap();
    assert!(!actor.probe_permissions.contains_key(&peer_primary.id()));
    assert!(!actor.probe_peers.contains(&peer_primary.id()));
    assert_eq!(actor.tasks.len(), task_count);
    assert!(matches!(
        events_rx.recv().await,
        Some(Event::Error { code, .. }) if code == "probe_unavailable"
    ));
    recovery.stop().await;
    host_primary.close().await;
    peer_primary.close().await;
}

#[tokio::test]
async fn service_replays_committed_checkpoint_to_each_native_owner() {
    timeout(Duration::from_secs(35), async {
        let host = endpoint().await;
        let guest = endpoint().await;
        let relay = iroh::defaults::prod::default_relay_map()
            .urls::<Vec<_>>()
            .remove(0);
        let seed = Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600)
            .unwrap();
        let room = seed.room();
        let (host_events_tx, mut host_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (host_commands, host_command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (guest_events_tx, mut guest_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (guest_commands, guest_command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_host_fault, host_failure) = watch::channel(false);
        let (_guest_fault, guest_failure) = watch::channel(false);
        let make_actor = |endpoint: Endpoint, events: mpsc::Sender<Event>| Actor {
            endpoint,
            relay_only: false,
            epoch: 0,
            opening: false,
            room: None,
            hosted: None,
            room_invite: None,
            host_address: None,
            controls: BTreeMap::new(),
            games: BTreeMap::new(),
            closed_generation: 0,
            tasks: JoinSet::new(),
            events: EventOutbox::new(events),
            recovery: None,
            admissions: BTreeMap::new(),
            admission_order: Vec::new(),
            applied_admission_members: BTreeSet::new(),
            incoming_transfer: None,
            pending_checkpoint_proposal: None,
            pending_checkpoint_retry: None,
            outgoing_transfer: None,
            committed_native_members: None,
            pending_retired_incarnations: BTreeSet::new(),
            retired_incarnations: BTreeSet::new(),
            last_exported_revision: 0,
            pending_checkpoint_ack: None,
            pending_checkpoint_committed: None,
            next_transport_message: TRANSPORT_MESSAGE_ID_BASE,
            next_coordination_operation: 1,
            pending_coordination_refresh: None,
            pending_membership_operation: None,
            pending_admission_operation: None,
        deferred_admissions: VecDeque::new(),
        pending_membership_publications: BTreeSet::new(),
            last_coordination_state: None,
            coordination_writable: false,
            last_control_rebound: None,
            unwritable_leader_since: None,
            reconnect_target: None,
            probe_reservations: BTreeMap::new(),
            pending_game_admissions: BTreeMap::new(),
            pending_probe_invalidations: BTreeMap::new(),
            probe_peers: BTreeSet::new(),
            probe_permissions: BTreeMap::new(),
            pending_probe_authorizations: BTreeMap::new(),
            retirement_started: None,
        };
        let mut host_actor = make_actor(host.clone(), host_events_tx);
        let invite = host_actor.setup_host_recovery(seed).await.unwrap();
        host_actor.epoch = 1;
        host_actor.room = Some(room);
        host_actor.hosted = Some(invite.clone());
        let host_service = tokio::spawn(async move {
            let result = host_actor.run(host_command_rx, host_failure).await;
            host_actor.clear_room();
            host_actor.tasks.shutdown().await;
            result
        });
        let mut guest_actor = make_actor(guest.clone(), guest_events_tx);
        // Use an explicit loopback connection for this deterministic
        // service fixture; production Join still uses the authenticated
        // invite address (relay or direct route).
        guest_actor.epoch = 1;
        guest_actor.opening = true;
        guest_actor.room = Some(room);
        guest_actor.host_address = Some(address(&host));
        let guest_connection = guest
            .connect(address(&host), CONTROL_ALPN)
            .await
            .unwrap();
        let guest_control = transport::connect_control_on(guest_connection, &invite)
            .await
            .unwrap();
        guest_actor
            .completed(Completion::GuestControl(1, invite.clone(), Ok(guest_control)))
            .await
            .unwrap();
        let guest_service = tokio::spawn(async move {
            let result = guest_actor.run(guest_command_rx, guest_failure).await;
            guest_actor.clear_room();
            guest_actor.tasks.shutdown().await;
            result
        });
        let _services = TaskScope(vec![
            host_service.abort_handle(),
            guest_service.abort_handle(),
        ]);
        // Wait until the host has both authenticated control routes before
        // submitting a proposal. This exercises the live admission path,
        // rather than a standalone Coordinator fixture.
        let mut term = 0;
        let mut host_has_guest_route = false;
        while !host_has_guest_route || term == 0 {
            match host_events.recv().await.unwrap() {
                Event::CoordinationState {
                    term: state_term,
                    writable: true,
                    leader_local: true,
                    ..
                } => term = term.max(state_term),
                Event::ControlRebound { members, .. } if members.len() >= 2 => {
                    host_has_guest_route = true;
                }
                Event::Error { code, .. } => panic!("host helper error: {code}"),
                _ => {}
            }
        }
        guest_commands
            .send(Request {
                id: 10,
                command: Command::Send {
                    epoch: 1,
                    peer: host.id(),
                    message_id: 71,
                    payload: "cpp hello alongside admission".into(),
                },
            })
            .await
            .unwrap();
        loop {
            match host_events.recv().await.unwrap() {
                Event::Message {
                    message_id,
                    payload,
                    ..
                } => {
                    assert_eq!(message_id, 71);
                    assert_eq!(payload, "cpp hello alongside admission");
                    break;
                }
                Event::Error { code, .. } => panic!("host helper error: {code}"),
                _ => {}
            }
        }
        tokio::time::sleep(Duration::from_millis(500)).await;
        let body = serde_json::json!({
            "version": 1,
            "request": 7,
            "term": term,
            "base_revision": 0,
            "checkpoint": {"room": "recovery-test"},
            "effects": [],
            "effects_digest": "4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945"
        });
        let bytes = serde_json::to_vec(&body).unwrap();
        let digest = recovery::hex_digest(&recovery::sha256(&bytes));
        host_commands
            .send(Request {
                id: 2,
                command: Command::CheckpointBegin {
                    epoch: 1,
                    room,
                    transfer: 7,
                    term,
                    base_revision: 0,
                    revision: 1,
                    length: bytes.len() as u32,
                    digest: digest.clone(),
                },
            })
            .await
            .unwrap();
        for (offset, chunk) in bytes.chunks(CHECKPOINT_CHUNK_BYTES).enumerate() {
            let offset = offset * CHECKPOINT_CHUNK_BYTES;
            host_commands
                .send(Request {
                    id: 3 + offset as u64,
                    command: Command::CheckpointChunk {
                        epoch: 1,
                        room,
                        transfer: 7,
                        term,
                        base_revision: 0,
                        revision: 1,
                        offset: offset as u32,
                        data: URL_SAFE_NO_PAD.encode(chunk),
                    },
                })
                .await
                .unwrap();
        }
        host_commands
            .send(Request {
                id: 4,
                command: Command::CheckpointEnd {
                    epoch: 1,
                    room,
                    transfer: 7,
                    term,
                    base_revision: 0,
                    revision: 1,
                    length: bytes.len() as u32,
                    digest,
                },
            })
            .await
            .unwrap();

        let deadline = tokio::time::Instant::now() + Duration::from_secs(15);
        let mut guest_committed = false;
        let mut guest_started = false;
        while !guest_committed {
            tokio::select! {
                event = host_events.recv() => match event.unwrap() {
                    Event::CheckpointBegin { transfer, .. } => {
                        host_commands.send(Request { id: 20, command: Command::CheckpointAck { epoch: 1, room, transfer, offset: 0 } }).await.unwrap();
                    }
                    Event::CheckpointChunk { transfer, offset, data, .. } => {
                        let next = offset + URL_SAFE_NO_PAD.decode(data).unwrap().len() as u32;
                        host_commands.send(Request { id: 21, command: Command::CheckpointAck { epoch: 1, room, transfer, offset: next } }).await.unwrap();
                    }
                    Event::CheckpointEnd { transfer, length, .. } => {
                        host_commands.send(Request { id: 22, command: Command::CheckpointAck { epoch: 1, room, transfer, offset: length } }).await.unwrap();
                    }
                    Event::Error { code, .. } => panic!("host helper error: {code}"),
                    _ => {},
                },
                event = guest_events.recv() => match event.unwrap() {
                    Event::CheckpointBegin { transfer, .. } => {
                        guest_started = true;
                        guest_commands.send(Request { id: 30, command: Command::CheckpointAck { epoch: 1, room, transfer, offset: 0 } }).await.unwrap();
                    }
                    Event::CheckpointChunk { transfer, offset, data, .. } => {
                        let next = offset + URL_SAFE_NO_PAD.decode(data).unwrap().len() as u32;
                        guest_commands.send(Request { id: 31, command: Command::CheckpointAck { epoch: 1, room, transfer, offset: next } }).await.unwrap();
                    }
                    Event::CheckpointEnd { transfer, length, .. } => {
                        guest_commands.send(Request { id: 32, command: Command::CheckpointAck { epoch: 1, room, transfer, offset: length } }).await.unwrap();
                    }
                    Event::CheckpointCommitted { transfer, revision, .. } => {
                        assert_eq!(transfer, 7);
                        assert_eq!(revision, 1);
                        guest_committed = true;
                    }
                    Event::Error { code, .. } => panic!("guest helper error: {code}"),
                    _ => {},
                },
                _ = tokio::time::sleep_until(deadline) => panic!("committed checkpoint was not replayed"),
            }
        }
        assert!(guest_started);
        host.close().await;
        guest.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn closing_room_does_not_poison_new_room_on_same_endpoint() {
    let host = endpoint().await;
    let relay = iroh::defaults::prod::default_relay_map()
        .urls::<Vec<_>>()
        .remove(0);
    let create = || {
        Invite::create(
            host.id(),
            relay.clone(),
            "test-build".into(),
            now().unwrap(),
            3600,
        )
        .unwrap()
    };
    let old = create();
    let replacement = create();
    assert_eq!(old.endpoint(), replacement.endpoint());
    assert_ne!(old.room(), replacement.room());
    assert!(replacement.admit(&old.proof(), now().unwrap()).is_err());
    replacement
        .admit(
            &Invite::parse(&replacement.encode().unwrap(), now().unwrap())
                .unwrap()
                .proof(),
            now().unwrap(),
        )
        .unwrap();
    let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = Actor {
        endpoint: host.clone(),
        relay_only: false,
        epoch: 1,
        opening: false,
        room: Some(old.room()),
        hosted: Some(old.clone()),
        room_invite: Some(old.clone()),
        host_address: None,
        controls: BTreeMap::new(),
        games: BTreeMap::new(),
        closed_generation: 99,
        tasks: JoinSet::new(),
        events: EventOutbox::new(events_tx),
        recovery: None,
        admissions: BTreeMap::new(),
        admission_order: Vec::new(),
        applied_admission_members: BTreeSet::new(),
        incoming_transfer: None,
        pending_checkpoint_proposal: None,
        pending_checkpoint_retry: None,
        outgoing_transfer: None,
        committed_native_members: None,
        pending_retired_incarnations: BTreeSet::new(),
        retired_incarnations: BTreeSet::new(),
        last_exported_revision: 0,
        pending_checkpoint_ack: None,
        pending_checkpoint_committed: None,
        next_transport_message: TRANSPORT_MESSAGE_ID_BASE,
        next_coordination_operation: 1,
        pending_coordination_refresh: None,
        pending_membership_operation: None,
        pending_admission_operation: None,
        deferred_admissions: VecDeque::new(),
        pending_membership_publications: BTreeSet::new(),
        last_coordination_state: None,
        coordination_writable: false,
        last_control_rebound: None,
        unwritable_leader_since: None,
        reconnect_target: None,
        probe_reservations: BTreeMap::new(),
        pending_game_admissions: BTreeMap::new(),
        pending_probe_invalidations: BTreeMap::new(),
        probe_peers: BTreeSet::new(),
        probe_permissions: BTreeMap::new(),
        pending_probe_authorizations: BTreeMap::new(),
        retirement_started: None,
    };
    actor
        .command(Request {
            id: 1,
            command: Command::Leave {
                epoch: 1,
                abandon: false,
            },
        })
        .unwrap();
    let _ = next(&mut events, "room_closed").await;
    assert!(actor.begin(2, "test-build"));
    actor
        .completed(Completion::Hosted(2, Ok(replacement.clone())))
        .await
        .unwrap();
    let _ = next(&mut events, "hosted").await;
    let _ = next(&mut events, "discord_invite").await;
    // An old leave request and a late old-host completion cannot close or
    // replace the new room, even though its endpoint is unchanged.
    actor
        .command(Request {
            id: 2,
            command: Command::Leave {
                epoch: 1,
                abandon: false,
            },
        })
        .unwrap();
    assert!(
        matches!(events.recv().await, Some(Event::Error { code, .. }) if code == "stale_epoch")
    );
    actor
        .completed(Completion::Hosted(1, Ok(old.clone())))
        .await
        .unwrap();
    assert_eq!(actor.room, Some(replacement.room()));
    assert_eq!(actor.closed_generation, 0);
    actor
        .hosted
        .as_ref()
        .unwrap()
        .admit(&replacement.proof(), now().unwrap())
        .unwrap();
    assert!(
        actor
            .hosted
            .as_ref()
            .unwrap()
            .admit(&old.proof(), now().unwrap())
            .is_err()
    );
    actor.clear_room();
    host.close().await;
}

#[tokio::test]
async fn actor_routes_cpp_control_and_keeps_gameplay_alive_when_control_closes() {
    timeout(Duration::from_secs(25), async {
        let host = endpoint().await;
        let remote = endpoint().await;
        let relay = iroh::defaults::prod::default_relay_map()
            .urls::<Vec<_>>()
            .remove(0);
        let invite =
            Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600).unwrap();
        let room = invite.room();
        let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_fault, failure) = watch::channel(false);
        let mut actor = Actor {
            endpoint: host.clone(),
            relay_only: false,
            epoch: 1,
            opening: false,
            room: Some(room),
            hosted: Some(invite.clone()),
            room_invite: Some(invite.clone()),
            host_address: None,
            controls: BTreeMap::new(),
            games: BTreeMap::new(),
            closed_generation: 0,
            tasks: JoinSet::new(),
            events: EventOutbox::new(events_tx),
            recovery: None,
            admissions: BTreeMap::new(),
            admission_order: Vec::new(),
            applied_admission_members: BTreeSet::new(),
            incoming_transfer: None,
            pending_checkpoint_proposal: None,
            pending_checkpoint_retry: None,
            outgoing_transfer: None,
            committed_native_members: None,
            pending_retired_incarnations: BTreeSet::new(),
            retired_incarnations: BTreeSet::new(),
            last_exported_revision: 0,
            pending_checkpoint_ack: None,
            pending_checkpoint_committed: None,
            next_transport_message: TRANSPORT_MESSAGE_ID_BASE,
            next_coordination_operation: 1,
            pending_coordination_refresh: None,
            pending_membership_operation: None,
            pending_admission_operation: None,
            deferred_admissions: VecDeque::new(),
            pending_membership_publications: BTreeSet::new(),
            last_coordination_state: None,
            coordination_writable: false,
            last_control_rebound: None,
            unwritable_leader_since: None,
            reconnect_target: None,
            probe_reservations: BTreeMap::new(),
            pending_game_admissions: BTreeMap::new(),
            pending_probe_invalidations: BTreeMap::new(),
            probe_peers: BTreeSet::new(),
            probe_permissions: BTreeMap::new(),
            pending_probe_authorizations: BTreeMap::new(),
            retirement_started: None,
        };
        let service = tokio::spawn(async move {
            let result = actor.run(command_rx, failure).await;
            actor.clear_room();
            actor.tasks.shutdown().await;
            result
        });
        let _service_scope = TaskScope(vec![service.abort_handle()]);
        let connection = remote.connect(address(&host), CONTROL_ALPN).await.unwrap();
        let mut control = transport::connect_control_on(connection, &invite)
            .await
            .unwrap();
        let _ = next(&mut events, "connected").await;
        control
            .sender
            .send(&ControlFrame {
                message_id: 42,
                payload: br#"{"type":"lobby_ready"}"#.to_vec(),
            })
            .await
            .unwrap();
        match next(&mut events, "message").await {
            Event::Message {
                epoch,
                peer,
                message_id,
                payload,
            } => {
                assert_eq!(epoch, 1);
                assert_eq!(peer, remote.id());
                assert_eq!(message_id, 42);
                assert_eq!(payload, r#"{"type":"lobby_ready"}"#);
            }
            _ => unreachable!(),
        }
        commands
            .send(Request {
                id: 2,
                command: Command::Send {
                    epoch: 1,
                    peer: remote.id(),
                    message_id: 51,
                    payload: "cpp authoritative reply".into(),
                },
            })
            .await
            .unwrap();
        let native_frame = control.receiver.receive().await.unwrap();
        let native: NativeControlMessage = serde_json::from_slice(&native_frame.payload).unwrap();
        assert_eq!(native.kind, "native_control");
        assert_eq!(native.message_id, 51);
        assert_eq!(native.payload, "cpp authoritative reply");
        let _ = next(&mut events, "sent").await;

        let local_host = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap();
        let local_remote = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap();
        commands
            .send(Request {
                id: 3,
                command: Command::PrepareGame {
                    epoch: 1,
                    peer: remote.id(),
                    room,
                    generation: 1,
                    capability: [8; 32],
                    local_port: local_host.local_addr().unwrap().port(),
                    max_packet: 1024,
                    dial: false,
                },
            })
            .await
            .unwrap();
        let _ = next(&mut events, "game_waiting").await;
        let auth = GameAuthorization {
            peer: host.id(),
            key: MatchKey {
                room,
                generation: 1,
            },
            capability: [8; 32],
            max_packet: 1024,
        };
        let game = transport::connect_game(&remote, address(&host), auth)
            .await
            .unwrap();
        let virtual_host = match next(&mut events, "game_ready").await {
            Event::GameReady { virtual_port, .. } => virtual_port,
            _ => unreachable!(),
        };
        let remote_bridge = Bridge::bind(game, local_remote.local_addr().unwrap())
            .await
            .unwrap();
        let (stop, stop_rx) = watch::channel(false);
        let bridge = tokio::spawn(remote_bridge.run(stop_rx));
        let _bridge_scope = TaskScope(vec![bridge.abort_handle()]);
        commands
            .send(Request {
                id: 4,
                command: Command::CloseControl {
                    epoch: 1,
                    peer: remote.id(),
                },
            })
            .await
            .unwrap();
        let _ = next(&mut events, "control_closed").await;
        control.connection.closed().await;
        local_host
            .send_to(b"fight continues", (Ipv4Addr::LOCALHOST, virtual_host))
            .await
            .unwrap();
        let mut buffer = [0; 64];
        let count = local_remote.recv(&mut buffer).await.unwrap();
        assert_eq!(&buffer[..count], b"fight continues");
        commands
            .send(Request {
                id: 5,
                command: Command::EndMatch {
                    epoch: 1,
                    generation: 1,
                },
            })
            .await
            .unwrap();
        let _ = next(&mut events, "game_closed").await;
        let _ = stop.send(true);
        let _ = bridge.await.unwrap();
        // A retired spectator/game edge is independently removable and
        // does not require ending the room's other control links.
        commands
            .send(Request {
                id: 6,
                command: Command::PrepareGame {
                    epoch: 1,
                    peer: remote.id(),
                    room,
                    generation: 2,
                    capability: [9; 32],
                    local_port: local_host.local_addr().unwrap().port(),
                    max_packet: 1024,
                    dial: false,
                },
            })
            .await
            .unwrap();
        let _ = next(&mut events, "game_waiting").await;
        commands
            .send(Request {
                id: 7,
                command: Command::EndPeer {
                    epoch: 1,
                    peer: remote.id(),
                    generation: 2,
                },
            })
            .await
            .unwrap();
        let _ = next(&mut events, "game_closed").await;
        commands
            .send(Request {
                id: 8,
                command: Command::PrepareGame {
                    epoch: 1,
                    peer: remote.id(),
                    room,
                    generation: 1,
                    capability: [8; 32],
                    local_port: local_host.local_addr().unwrap().port(),
                    max_packet: 1024,
                    dial: false,
                },
            })
            .await
            .unwrap();
        match next(&mut events, "error").await {
            Event::Error { code, .. } => assert_eq!(code, "invalid_game_registration"),
            _ => unreachable!(),
        }
        commands
            .send(Request {
                id: 9,
                command: Command::Leave {
                    epoch: 1,
                    abandon: false,
                },
            })
            .await
            .unwrap();
        let _ = next(&mut events, "room_closed").await;
        // Cancellation works while Host awaits relay readiness, with no
        // network work performed by the IPC reader or the game thread.
        commands
            .send(Request {
                id: 10,
                command: Command::Host {
                    epoch: 2,
                    build: "test-build".into(),
                },
            })
            .await
            .unwrap();
        commands
            .send(Request {
                id: 11,
                command: Command::Leave {
                    epoch: 2,
                    abandon: false,
                },
            })
            .await
            .unwrap();
        let _ = next(&mut events, "room_closed").await;
        commands
            .send(Request {
                id: 12,
                command: Command::Shutdown,
            })
            .await
            .unwrap();
        service.await.unwrap().unwrap();
        host.close().await;
        remote.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn actor_admits_full_sixteen_member_room_and_fifteen_game_links() {
    timeout(Duration::from_secs(60), async {
        // The host plus fifteen helpers is the transport shape of a
        // sixteen-member room. One gameplay endpoint may represent the
        // second fighter and fourteen admitted spectators for a table.
        assert_eq!(MAX_CONTROL_PEERS, 15);
        assert_eq!(MAX_GAME_LINKS, 15);
        const _: () = assert!(MAX_TASKS >= MAX_CONTROL_PEERS + MAX_GAME_LINKS);

        let host = endpoint().await;
        let relay = iroh::defaults::prod::default_relay_map()
            .urls::<Vec<_>>()
            .remove(0);
        let invite = Invite::create(
            host.id(),
            relay,
            "full-room-build".into(),
            now().unwrap(),
            3600,
        )
        .unwrap();
        let room = invite.room();
        let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_fault, failure) = watch::channel(false);
        let mut actor = Actor {
            endpoint: host.clone(),
            relay_only: false,
            epoch: 1,
            opening: false,
            room: Some(room),
            hosted: Some(invite.clone()),
            room_invite: Some(invite.clone()),
            host_address: None,
            controls: BTreeMap::new(),
            games: BTreeMap::new(),
            closed_generation: 0,
            tasks: JoinSet::new(),
            events: EventOutbox::new(events_tx),
            recovery: None,
            admissions: BTreeMap::new(),
            admission_order: Vec::new(),
            applied_admission_members: BTreeSet::new(),
            incoming_transfer: None,
            pending_checkpoint_proposal: None,
            pending_checkpoint_retry: None,
            outgoing_transfer: None,
            committed_native_members: None,
            pending_retired_incarnations: BTreeSet::new(),
            retired_incarnations: BTreeSet::new(),
            last_exported_revision: 0,
            pending_checkpoint_ack: None,
            pending_checkpoint_committed: None,
            next_transport_message: TRANSPORT_MESSAGE_ID_BASE,
            next_coordination_operation: 1,
            pending_coordination_refresh: None,
            pending_membership_operation: None,
            pending_admission_operation: None,
            deferred_admissions: VecDeque::new(),
            pending_membership_publications: BTreeSet::new(),
            last_coordination_state: None,
            coordination_writable: false,
            last_control_rebound: None,
            unwritable_leader_since: None,
            reconnect_target: None,
            probe_reservations: BTreeMap::new(),
            pending_game_admissions: BTreeMap::new(),
            pending_probe_invalidations: BTreeMap::new(),
            probe_peers: BTreeSet::new(),
            probe_permissions: BTreeMap::new(),
            pending_probe_authorizations: BTreeMap::new(),
            retirement_started: None,
        };
        let service = tokio::spawn(async move {
            let result = actor.run(command_rx, failure).await;
            actor.clear_room();
            actor.tasks.shutdown().await;
            result
        });
        let _service_scope = TaskScope(vec![service.abort_handle()]);

        let mut remotes = Vec::with_capacity(MAX_CONTROL_PEERS);
        let mut controls = Vec::with_capacity(MAX_CONTROL_PEERS);
        for _ in 0..MAX_CONTROL_PEERS {
            let remote = endpoint().await;
            let connection = remote.connect(address(&host), CONTROL_ALPN).await.unwrap();
            let control = transport::connect_control_on(connection, &invite)
                .await
                .unwrap();
            remotes.push(remote);
            controls.push(control);
            match next(&mut events, "connected").await {
                Event::Connected { .. } => (),
                _ => unreachable!(),
            }
        }
        assert_eq!(controls.len(), 15);

        // The seventeenth endpoint (host plus sixteen remotes) is
        // rejected without disturbing any admitted control stream.
        let overflow = endpoint().await;
        let overflow_connection = overflow
            .connect(address(&host), CONTROL_ALPN)
            .await
            .unwrap();
        assert!(
            transport::connect_control_on(overflow_connection, &invite)
                .await
                .is_err()
        );

        // Reserve unique local UDP ports for the host-side bridges. The
        // sockets are released before each bridge is authorized.
        let mut local_ports = Vec::with_capacity(MAX_GAME_LINKS);
        for _ in 0..MAX_GAME_LINKS {
            let socket = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap();
            local_ports.push(socket.local_addr().unwrap().port());
            drop(socket);
        }
        let mut games = Vec::with_capacity(MAX_GAME_LINKS);
        for (index, remote) in remotes.iter().enumerate() {
            let peer = remote.id();
            commands
                .send(Request {
                    id: 100 + index as u64,
                    command: Command::PrepareGame {
                        epoch: 1,
                        peer,
                        room,
                        generation: 1,
                        capability: [index as u8 + 1; 32],
                        local_port: local_ports[index],
                        max_packet: 1024,
                        dial: false,
                    },
                })
                .await
                .unwrap();
            let _ = next(&mut events, "game_waiting").await;
            let auth = GameAuthorization {
                peer: host.id(),
                key: MatchKey {
                    room,
                    generation: 1,
                },
                capability: [index as u8 + 1; 32],
                max_packet: 1024,
            };
            games.push(
                transport::connect_game(remote, address(&host), auth)
                    .await
                    .unwrap(),
            );
            let _ = next(&mut events, "game_ready").await;
        }
        assert_eq!(games.len(), MAX_GAME_LINKS);

        // The gameplay-link budget is independent and equally strict.
        commands
            .send(Request {
                id: 190,
                command: Command::PrepareGame {
                    epoch: 1,
                    peer: overflow.id(),
                    room,
                    generation: 1,
                    capability: [77; 32],
                    local_port: local_ports[0],
                    max_packet: 1024,
                    dial: false,
                },
            })
            .await
            .unwrap();
        match next(&mut events, "error").await {
            Event::Error { code, .. } => assert_eq!(code, "invalid_game_registration"),
            _ => unreachable!(),
        }

        // Retire links one at a time and keep all control channels alive;
        // this exercises the per-peer teardown path without ending the
        // room or aborting the remaining authorized links.
        for (index, remote) in remotes.iter().enumerate() {
            commands
                .send(Request {
                    id: 200 + index as u64,
                    command: Command::EndPeer {
                        epoch: 1,
                        peer: remote.id(),
                        generation: 1,
                    },
                })
                .await
                .unwrap();
            let _ = next(&mut events, "game_closed").await;
        }
        drop(games);
        assert_eq!(controls.len(), MAX_CONTROL_PEERS);

        commands
            .send(Request {
                id: 300,
                command: Command::Leave {
                    epoch: 1,
                    abandon: false,
                },
            })
            .await
            .unwrap();
        let _ = next(&mut events, "room_closed").await;
        commands
            .send(Request {
                id: 301,
                command: Command::Shutdown,
            })
            .await
            .unwrap();
        service.await.unwrap().unwrap();
        host.close().await;
        overflow.close().await;
        for remote in remotes {
            remote.close().await;
        }
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn ipc_service_rejects_malformed_and_replayed_commands() {
    for replay in [false, true] {
        timeout(Duration::from_secs(10), async {
            let endpoint = endpoint().await;
            let (server, mut client) = tokio::io::duplex(1024);
            let service = tokio::spawn(run(server, endpoint, false));
            if replay {
                let status = ControlFrame {
                    message_id: 2,
                    payload: br#"{"type":"status"}"#.to_vec(),
                };
                wire::write_ipc(&mut client, &status).await.unwrap();
                let _ = wire::read_ipc(&mut client).await.unwrap();
                wire::write_ipc(&mut client, &status).await.unwrap();
            } else {
                wire::write_ipc(
                    &mut client,
                    &ControlFrame {
                        message_id: 2,
                        payload: br#"{"type":"unknown"}"#.to_vec(),
                    },
                )
                .await
                .unwrap();
            }
            assert!(service.await.unwrap().is_err());
        })
        .await
        .unwrap();
    }
}
