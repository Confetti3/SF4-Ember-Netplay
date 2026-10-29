//! Checkpoint export, credit windows and replay to native owners.

use super::*;

#[tokio::test]
async fn near_limit_checkpoint_obeys_four_credit_window_and_cumulative_acks() {
    phase(
        "near_limit_checkpoint_obeys_four_credit_window_and_cumulative_acks",
        Duration::from_secs(10),
        async {
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
                let chunks = CHECKPOINT_WINDOW
                    .min((bytes.len() - received).div_ceil(CHECKPOINT_CHUNK_BYTES));
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
                            received += data.0.len();
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
                    .bytes
                    .to_vec(),
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
        },
    )
    .await;
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
    phase("completed_receiver_accepts_exact_retry_before_delayed_commit_marker", Duration::from_secs(5), async {
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
        // Every chunk was acknowledged; only the final marker is missing.
        let progress = format!(
            "revision={} begun=true acked={len}/{len} in_flight=0",
            transfer.revision,
            len = transfer.bytes.len()
        );
        assert!(matches!(
            source_rx.recv().await,
            Some(Event::Error { code, reason, .. })
                if code == "checkpoint_send_timeout" && reason.as_deref() == Some(progress.as_str())
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
    .await;
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

/// The pump takes the store lock only for a revision past the last export,
/// read from the revision the store publishes; a timed-out export is retried.
#[tokio::test]
async fn the_checkpoint_pump_waits_on_the_store_only_for_a_new_revision() {
    phase(
        "the_checkpoint_pump_waits_on_the_store_only_for_a_new_revision",
        Duration::from_secs(30),
        async {
            let host = endpoint().await;
            let relay = test_relay();
            let seed = Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600)
                .unwrap();
            let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let mut actor = test_actor(host.clone(), events_tx);
            let (_invite, recovery) = led_host(&mut actor, seed).await;
            commit_native(&recovery, 17, "{\"checkpoint\":{}}").await;
            actor.last_exported_revision = 1;
            {
                // Nothing new: the pump returns while the store is locked.
                let _held = recovery.coordinator.store().hold_lock().await;
                timeout(
                    Duration::from_millis(500),
                    actor.pump_committed_checkpoint(),
                )
                .await
                .expect("the pump waited on the store lock")
                .unwrap();
            }
            assert!(actor.outgoing_transfer.is_none());
            // A timed-out export lowers the last export with no new commit, and
            // the next pump replays the commit.
            actor.last_exported_revision = 0;
            actor.pump_committed_checkpoint().await.unwrap();
            assert!(matches!(
                next(&mut events, "checkpoint_begin").await,
                Event::CheckpointBegin {
                    transfer: 17,
                    revision: 1,
                    ..
                }
            ));
            recovery.stop().await;
            host.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn held_checkpoint_and_admission_writes_do_not_block_actor_or_import_stale_completion() {
    phase("held_checkpoint_and_admission_writes_do_not_block_actor_or_import_stale_completion", Duration::from_secs(20), async {
        let host_primary = endpoint().await;
        let voter_primary = endpoint().await;
        let joining_primary = endpoint().await;
        let relay = test_relay();
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
            crate::transport::Transport::Online,
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
            crate::transport::Transport::Online,
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
                    data: crate::wire::IpcBytes(body.clone()),
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
                    data: crate::wire::IpcBytes(body.clone()),
                },
            ))
            .await
            .unwrap();
        commands
            .send(send(7, Command::Status))
            .await
            .unwrap();

        // The actor keeps serving checkpoint credit, status and the quorum
        // watch behind the held write. The native message behind the held
        // Admission is not delivered: its session is bound only when the
        // admission completes, and the native side must hear of the session
        // before any message from it.
        let deadline = tokio::time::Instant::now() + Duration::from_secs(6);
        let mut acknowledgements = 0;
        let mut saw_status = false;
        let mut saw_quorum_lost = false;
        while acknowledgements < 2 || !saw_status || !saw_quorum_lost {
            let event = tokio::select! {
                event = events.recv() => event.unwrap(),
                _ = tokio::time::sleep_until(deadline) => panic!("actor stalled behind coordination write"),
            };
            match event {
                Event::CheckpointAck { transfer: 41, offset, .. } => {
                    assert_eq!(offset as usize, body.len());
                    acknowledgements += 1;
                }
                Event::Message { message_id: 77, .. } => {
                    panic!("native message delivered before its session was bound");
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
    .await;
}

#[tokio::test]
async fn service_replays_committed_checkpoint_to_each_native_owner() {
    // Each wait below has its own deadline and names itself when it runs
    // out, so a loaded machine is not charged one budget for the whole
    // fixture and a hang still says where it is. Idle, the fixture takes
    // about 7 s, 6 s of it the two coordination endpoints' relay waits
    // (ADVERTISE_READY_TIMEOUT).
    let setup = Duration::from_secs(20);
    let host = endpoint().await;
    let guest = endpoint().await;
    let relay = test_relay();
    let seed = Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600).unwrap();
    let room = seed.room();
    let (host_events_tx, mut host_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (host_commands, host_command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (guest_events_tx, mut guest_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (guest_commands, guest_command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (_host_fault, host_failure) = watch::channel(false);
    let (_guest_fault, guest_failure) = watch::channel(false);
    let mut host_actor = test_actor(host.clone(), host_events_tx);
    let invite = phase(
        "host recovery setup",
        setup,
        host_actor.setup_host_recovery(seed),
    )
    .await
    .unwrap();
    host_actor.epoch = 1;
    host_actor.room = Some(room);
    host_actor.hosted = Some(invite.clone());
    let host_service = tokio::spawn(async move {
        let result = host_actor.run(host_command_rx, host_failure).await;
        host_actor.clear_room();
        host_actor.tasks.shutdown().await;
        result
    });
    let mut guest_actor = test_actor(guest.clone(), guest_events_tx);
    // Use an explicit loopback connection for this deterministic
    // service fixture; production Join still uses the authenticated
    // invite address (relay or direct route).
    guest_actor.epoch = 1;
    guest_actor.opening = true;
    guest_actor.room = Some(room);
    guest_actor.host_address = Some(address(&host));
    let guest_control = phase("guest control stream", setup, async {
        let connection = guest.connect(address(&host), CONTROL_ALPN).await.unwrap();
        transport::connect_control_on(connection, &invite)
            .await
            .unwrap()
    })
    .await;
    phase(
        "guest join recovery",
        setup,
        guest_actor.completed(Completion::GuestControl(
            1,
            invite.clone(),
            Ok(guest_control),
            None,
        )),
    )
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
    let term = phase("host admits the guest's route", setup, async {
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
        term
    })
    .await;
    guest_commands
        .send(Request {
            id: 10,
            command: Command::Send {
                epoch: 1,
                peer: host.id(),
                message_id: 71,
                payload: "cpp hello alongside admission".into(),
                control: 0,
            },
        })
        .await
        .unwrap();
    let message = phase(
        "guest message reaches the host",
        Duration::from_secs(15),
        next(&mut host_events, "message"),
    )
    .await;
    let Event::Message {
        message_id,
        payload,
        ..
    } = message
    else {
        unreachable!()
    };
    assert_eq!(message_id, 71);
    assert_eq!(payload, "cpp hello alongside admission");
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
    let digest_hex = digest.clone();
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
                    data: crate::wire::IpcBytes(chunk.to_vec()),
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

    let replayed = "committed checkpoint replayed to the guest";
    let guest_started = phase(replayed, Duration::from_secs(15), async {
        let mut guest_started = false;
        let mut guest_committed = false;
        let mut host_marked = false;
        while !guest_committed || !host_marked {
            tokio::select! {
                event = host_events.recv() => match event.unwrap() {
                    // The host proposed this commit itself: it gets one marker
                    // in place of the transfer and answers it at full length.
                    Event::CheckpointCommittedLocal { transfer, revision, length, digest: marked, .. } => {
                        assert_eq!(transfer, 7);
                        assert_eq!(revision, 1);
                        assert_eq!(length as usize, bytes.len());
                        assert_eq!(marked, digest_hex);
                        host_marked = true;
                        host_commands.send(Request { id: 23, command: Command::CheckpointAck { epoch: 1, room, transfer, offset: length } }).await.unwrap();
                    }
                    Event::CheckpointBegin { transfer: 7, .. } => {
                        panic!("the host's own commit came back as a full transfer");
                    }
                    Event::CheckpointBegin { transfer, .. } => {
                        host_commands.send(Request { id: 20, command: Command::CheckpointAck { epoch: 1, room, transfer, offset: 0 } }).await.unwrap();
                    }
                    Event::CheckpointChunk { transfer, offset, data, .. } => {
                        let next = offset + data.0.len() as u32;
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
                        let next = offset + data.0.len() as u32;
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
            }
        }
        guest_started
    })
    .await;
    assert!(guest_started);
    host.close().await;
    guest.close().await;
}
