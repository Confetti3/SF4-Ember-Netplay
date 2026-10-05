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
        let (applied, history) = recovery.applied_membership_provenance().await;
        let confirmed: Vec<u64> = self
            .pending_retired_incarnations
            .iter()
            .copied()
            .filter(|incarnation| !applied.contains(incarnation))
            .collect();
        for incarnation in &confirmed {
            recovery.rpc.retire(*incarnation).await;
        }
        self.apply_confirmed_retirements(confirmed.into_iter().collect(), &history);
        Ok(())
    }
}

#[test]
fn leaving_leader_hands_off_to_a_reachable_voter() {
    use super::departure::handoff_successor;
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
/// Every event that carries a route, built from paths with an address, a port
/// or a relay URL to leak, serializes to a class and a region and nothing else.
#[test]
fn route_events_serialize_without_addresses_ports_or_urls() {
    use crate::invite::public_route;
    let peer = iroh::SecretKey::generate().public();
    let paths = [
        Some(TransportAddr::Ip("203.0.113.7:45760".parse().unwrap())),
        Some(TransportAddr::Ip("[2001:db8::1]:57845".parse().unwrap())),
        Some(TransportAddr::Relay(
            "https://use1-1.relay.n0.iroh.link./".parse().unwrap(),
        )),
        Some(TransportAddr::Relay(
            "https://relay.example.com:4433/".parse().unwrap(),
        )),
        None,
    ];
    for path in paths {
        let route = public_route(path.as_ref());
        let events = [
            Event::ProbeResult {
                epoch: 1,
                room: [1; 16],
                peer,
                request: 2,
                pair_revision: 3,
                route: route.clone(),
                status: "ready".into(),
                sample_count: 100,
                loss_count: 0,
                p95_rtt_us: 2_000,
                recommended_delay: 2,
                metrics: crate::probe::Metrics::default(),
            },
            Event::GameReady {
                epoch: 1,
                peer,
                generation: 1,
                virtual_port: 41_000,
                max_packet: 1024,
                route: route.clone(),
                fixed_port: false,
            },
            Event::GameFailed {
                epoch: 1,
                peer,
                generation: 1,
                reason: crate::bridge::Failure::PeerClosed,
                route: route.clone(),
                max_packet: 1024,
                max_datagram: 1200,
            },
            Event::Statistics {
                epoch: 1,
                peer,
                generation: 1,
                sent_packets: 1,
                received_packets: 1,
                sent_bytes: 1,
                received_bytes: 1,
                rejected_packets: 0,
                congestion_events: 0,
                local_drops: 0,
                route,
            },
        ];
        for event in &events {
            assert_no_address(event);
            let json = serde_json::to_string(event).unwrap();
            for leaked in [
                "203.0.113",
                "2001:db8",
                "45760",
                "57845",
                "example.com",
                "4433",
            ] {
                assert!(!json.contains(leaked), "{leaked} in {json}");
            }
        }
    }
}

use iroh::TransportAddr;
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
/// The only route strings an event may carry: a class and a relay region.
fn assert_public_route(route: &str) {
    let region = route.strip_prefix("relay:");
    assert!(
        route == "direct"
            || route == "unavailable"
            || region.is_some_and(|code| ["use1", "usw1", "euc1", "aps1", "other"].contains(&code)),
        "route {route:?} is not a public route"
    );
}
/// A serialized event names no address, port or relay URL, whatever route it
/// carries.
fn assert_no_address(event: &Event) {
    let json = serde_json::to_string(event).unwrap();
    for address in ["ip:", "http", "iroh.link", "127.0.0.1", "[::"] {
        assert!(!json.contains(address), "{address} in {json}");
    }
    let value = serde_json::to_value(event).unwrap();
    if let Some(route) = value["route"].as_str() {
        assert_public_route(route);
    }
}
async fn next(events: &mut mpsc::Receiver<Event>, kind: &str) -> Event {
    loop {
        let event = events.recv().await.unwrap();
        let value = serde_json::to_value(&event).unwrap();
        if let Some(route) = value["route"].as_str() {
            assert_public_route(route);
        }
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
        parked_controls: BTreeMap::new(),
        games: BTreeMap::new(),
        closed_generation: 0,
        tasks: JoinSet::new(),
        events: EventOutbox::new(events),
        recovery: None,
        admissions: BTreeMap::new(),
        admission_order: Vec::new(),
        pending_admissions: BTreeMap::new(),
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
        pending_admission_bindings: Vec::new(),
        deferred_admissions: VecDeque::new(),
        pending_membership_publications: BTreeSet::new(),
        last_coordination_state: None,
        coordination_writable: false,
        last_control_rebound: None,
        unwritable_leader_since: None,
        reconnect_target: None,
        pending_game_admissions: BTreeMap::new(),
        pending_probe_invalidations: BTreeMap::new(),
        probe_peers: BTreeSet::new(),
        probe_permissions: BTreeMap::new(),
        own_probes: BTreeMap::new(),
        pending_probe_authorizations: BTreeMap::new(),
        pending_probe_reservations: BTreeMap::new(),
        join_settled: false,
        join_control_losses: 0,
        join_first_loss: None,
        retirement_started: None,
        departure_failed: false,
        short: ShortLinks::default(),
        public: None,
        coordination_port: None,
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
async fn a_join_through_an_invitation_to_this_helper_says_so() {
    let own = endpoint().await;
    let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(own.clone(), events_tx);
    // A former leader holding a copy taken while it still led the room.
    let invitation = Invite::create(own.id(), test_relay(), "test-build".into(), now().unwrap(), 3600)
        .unwrap()
        .encode()
        .unwrap();
    assert!(
        actor
            .command(Request {
                id: 6,
                command: Command::Join {
                    epoch: 1,
                    invitation,
                    build: "test-build".into(),
                },
            })
            .unwrap()
    );
    assert!(matches!(
        events.recv().await.unwrap(),
        Event::Error { epoch: 1, request_id: 6, ref code, ref reason, .. }
            if code == "invalid_or_incompatible_invitation" && reason.as_deref() == Some("own_room")
    ));
    assert_eq!(actor.room, None);
    own.close().await;
}

#[test]
fn a_copied_invitation_lasts_a_week() {
    let id = iroh::SecretKey::generate().public();
    let start = now().unwrap();
    let invite = Invite::create(id, test_relay(), "test-build".into(), start, INVITE_LIFETIME).unwrap();
    let text = invite.encode().unwrap();
    // A link copied today still opens the room six days from now, and not
    // after its week.
    let day = 24 * 60 * 60;
    assert!(Invite::parse_for_build(&text, start + 6 * day, "test-build").is_ok());
    assert!(Invite::parse_for_build(&text, start + 7 * day + 1, "test-build").is_err());
}

#[tokio::test]
async fn an_open_room_renews_its_invitation_before_it_expires() {
    let own = endpoint().await;
    let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(own.clone(), events_tx);
    actor.epoch = 4;
    let start = now().unwrap();
    let invite = Invite::create(own.id(), test_relay(), "test-build".into(), start, INVITE_LIFETIME).unwrap();
    actor.hosted = Some(invite.clone());
    actor.room_invite = Some(invite.clone());
    // Less than a tenth of its lifetime gone: nothing to do.
    actor.renew_invitation(start + INVITE_LIFETIME / 20);
    assert!(events.try_recv().is_err());
    assert_eq!(actor.room_invite.as_ref().unwrap().expires(), invite.expires());
    // Past a tenth: both copies move on and the native side hears of it.
    let later = start + INVITE_LIFETIME / 4;
    actor.renew_invitation(later);
    let Event::DiscordInvite { epoch, invitation, .. } = events.recv().await.unwrap() else {
        panic!("expected a refreshed invitation");
    };
    assert_eq!(epoch, 4);
    assert_eq!(actor.room_invite.as_ref().unwrap().expires(), later + INVITE_LIFETIME);
    assert_eq!(actor.hosted.as_ref().unwrap().expires(), later + INVITE_LIFETIME);
    let parsed = Invite::parse_for_build(&invitation, invite.expires() + 1, "test-build").unwrap();
    assert_eq!(parsed.room(), invite.room());
    // Out of a room there is nothing to renew.
    actor.hosted = None;
    actor.room_invite = None;
    actor.renew_invitation(later + INVITE_LIFETIME);
    assert!(events.try_recv().is_err());
    own.close().await;
}

#[tokio::test]
async fn a_refused_join_names_the_epoch_it_asked_for() {
    let host = endpoint().await;
    let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (_commands, mut command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (_fault, mut failure) = watch::channel(false);
    let mut actor = test_actor(host.clone(), events_tx);
    actor.epoch = 1;
    actor.room = Some([7; 16]);
    // The native client filters events by its current epoch, so a refusal
    // tagged with the helper's epoch would leave that attempt joining forever.
    let join = |epoch| Command::Join {
        epoch,
        invitation: "not-an-invitation".into(),
        build: "test-build".into(),
    };
    assert!(
        actor
            .command(Request {
                id: 5,
                command: join(2),
            })
            .unwrap()
    );
    assert!(matches!(
        next(&mut events, "error").await,
        Event::Error { epoch: 2, request_id: 5, ref code, .. } if code == "invalid_room_state"
    ));
    assert_eq!(actor.room, Some([7; 16]));
    // A stale Leave is a rejection, not a failed departure: it must not let
    // a later epoch release a healthy room.
    assert!(
        actor
            .leave_command(9, false, &mut command_rx, &mut failure)
            .await
            .unwrap()
    );
    assert!(matches!(
        next(&mut events, "error").await,
        Event::Error { epoch: 1, ref code, .. } if code == "stale_epoch"
    ));
    assert!(!actor.departure_failed);
    assert!(!actor.begin(3, "test-build"));
    assert_eq!(actor.room, Some([7; 16]));
    // An invalid request never releases retained state either.
    actor.departure_failed = true;
    assert!(!actor.begin(1, "test-build"));
    assert_eq!(actor.room, Some([7; 16]));
    host.close().await;
}

#[tokio::test]
async fn an_unconfirmed_departure_is_released_by_the_next_epoch() {
    timeout(Duration::from_secs(60), async {
        let host_primary = endpoint().await;
        let follower_primary = endpoint().await;
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
        let (host_events, _host_events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut host = test_actor(host_primary.clone(), host_events);
        let invite = host.setup_host_recovery(seed).await.unwrap();
        let host_recovery = host.recovery.clone().unwrap();
        while host_recovery.coordinator.current_leader() != Some(host_recovery.incarnation) {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
        let voter = crate::recovery::RecoverySession::join(
            room,
            follower_primary.id(),
            host_recovery.incarnation,
            host_recovery.coordination_address.clone(),
            false,
        )
        .await
        .unwrap();
        let voter_admission = voter.advertise().await;
        host_recovery.add_learner(&voter_admission).await.unwrap();
        host_recovery
            .promote_voters(BTreeSet::from([
                host_recovery.incarnation,
                voter.incarnation,
            ]))
            .await
            .unwrap();
        host.remember_admission(voter_admission.clone());

        // The follower is a committed native member and voter, but its host
        // never applies the departure: no native Leave commits and no
        // membership proof arrives, exactly the case the native client gives
        // up on after its own bound.
        let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_commands, mut command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_fault, mut failure) = watch::channel(false);
        let mut follower = test_actor(follower_primary.clone(), events_tx);
        follower.epoch = 1;
        follower.room = Some(room);
        follower.room_invite = Some(invite.clone());
        follower.remember_admission(voter_admission);
        follower.remember_admission(host_recovery.advertise().await);
        follower.committed_native_members =
            Some(BTreeSet::from([host_primary.id(), follower_primary.id()]));
        follower.recovery = Some(voter.clone());
        while voter.coordinator.current_leader() != Some(host_recovery.incarnation) {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
        assert!(
            follower
                .leave_command(1, false, &mut command_rx, &mut failure)
                .await
                .unwrap()
        );
        assert!(matches!(
            next(&mut events, "error").await,
            Event::Error { epoch: 1, ref code, .. } if code == "leave_successor_unconfirmed"
        ));
        assert!(follower.departure_failed);
        assert_eq!(follower.room, Some(room));
        // The native client has left locally; its next room releases this one,
        // but the departed vote lingers for the grace: the host can still
        // commit the removal that the departure never got.
        assert!(follower.begin(2, "test-build"));
        assert_eq!(follower.room, None);
        assert!(!follower.departure_failed);
        assert!(follower.recovery.is_none());
        host_recovery
            .remove_members(BTreeSet::from([host_recovery.incarnation]))
            .await
            .unwrap();
        assert_eq!(
            host_recovery.applied_voter_ids().await,
            BTreeSet::from([host_recovery.incarnation])
        );
        host_recovery.stop().await;
        host_primary.close().await;
        follower_primary.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn an_abandoned_coordinated_room_keeps_its_vote_for_the_grace() {
    timeout(Duration::from_secs(30), async {
        let host = endpoint().await;
        let relay = iroh::defaults::prod::default_relay_map()
            .urls::<Vec<_>>()
            .remove(0);
        let seed =
            Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600).unwrap();
        let room = seed.room();
        let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_commands, mut command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_fault, mut failure) = watch::channel(false);
        let mut actor = test_actor(host.clone(), events_tx);
        let invite = actor.setup_host_recovery(seed).await.unwrap();
        let recovery = actor.recovery.clone().unwrap();
        actor.epoch = 1;
        actor.room = Some(room);
        actor.hosted = Some(invite.clone());
        actor.room_invite = Some(invite);
        // An abandon releases the native room at once but keeps answering
        // the old room's authority RPCs, so the leader can still commit the
        // departure with this vote.
        assert!(
            actor
                .leave_command(1, true, &mut command_rx, &mut failure)
                .await
                .unwrap()
        );
        assert!(matches!(
            next(&mut events, "room_closed").await,
            Event::RoomClosed { epoch: 1 }
        ));
        assert!(actor.retirement_started.is_some());
        assert!(actor.recovery.is_some());
        assert!(actor.controls.is_empty() && actor.hosted.is_none());
        // The next room begins at once; the old route lingers on its own.
        assert!(actor.begin(2, "test-build"));
        assert!(actor.recovery.is_none() && actor.room.is_none());
        assert!(actor.retirement_started.is_none());
        assert!(recovery.coordinator.current_leader().is_some());
        recovery.stop().await;
        host.close().await;
    })
    .await
    .unwrap();
}

/// One host actor running its loop, one remote endpoint with a control to it.
struct BoundHost {
    host: Endpoint,
    remote: Endpoint,
    room: [u8; 16],
    invite: Invite,
    recovery: crate::recovery::RecoverySession,
    events: mpsc::Receiver<Event>,
    // Dropping either sender ends the actor's loop as an IPC loss.
    _commands: mpsc::Sender<Request>,
    _fault: watch::Sender<bool>,
    _scope: TaskScope,
}

async fn bound_host(prepare: impl FnOnce(&mut Actor)) -> BoundHost {
    let host = endpoint().await;
    let remote = endpoint().await;
    let relay = iroh::defaults::prod::default_relay_map()
        .urls::<Vec<_>>()
        .remove(0);
    let seed = Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600).unwrap();
    let room = seed.room();
    let (events_tx, events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (fault, failure) = watch::channel(false);
    let mut actor = test_actor(host.clone(), events_tx);
    let invite = actor.setup_host_recovery(seed).await.unwrap();
    let recovery = actor.recovery.clone().unwrap();
    while recovery.coordinator.current_leader() != Some(recovery.incarnation) {
        tokio::time::sleep(Duration::from_millis(25)).await;
    }
    actor.epoch = 1;
    actor.room = Some(room);
    actor.hosted = Some(invite.clone());
    actor.room_invite = Some(invite.clone());
    prepare(&mut actor);
    let service = tokio::spawn(async move {
        let result = actor.run(command_rx, failure).await;
        actor.clear_room();
        actor.tasks.shutdown().await;
        result
    });
    BoundHost {
        host,
        remote,
        room,
        invite,
        recovery,
        events,
        _commands: commands,
        _fault: fault,
        _scope: TaskScope(vec![service.abort_handle()]),
    }
}

/// Drain events until the control closes; neither a session nor a native
/// message may have been published for it.
async fn expect_closed_without_session(events: &mut mpsc::Receiver<Event>) {
    loop {
        match events.recv().await.unwrap() {
            Event::PeerSession { .. } => panic!("session published for a rejected binding"),
            Event::Message { .. } => panic!("native message delivered before its session"),
            Event::Error { code, .. } => panic!("unexpected helper error: {code}"),
            Event::ControlClosed { .. } => break,
            _ => (),
        }
    }
}

async fn send_admission(control: &mut transport::ControlChannel, id: u64, admission: Admission) {
    let payload = serde_json::to_vec(&CoordinationControl::Admission { admission }).unwrap();
    control
        .sender
        .send(&ControlFrame {
            message_id: id,
            payload,
        })
        .await
        .unwrap();
}

#[tokio::test]
async fn a_refused_admission_queue_closes_the_control_without_a_session() {
    timeout(Duration::from_secs(30), async {
        let mut fixture = bound_host(|actor| {
            // An operation that never completes keeps the queue from draining.
            actor.pending_admission_operation = Some(AdmissionOperationKey {
                sequence: 0,
                epoch: 1,
                room: actor.room.unwrap(),
                incarnation: 0,
                term: 0,
                leader: None,
                peer: actor.endpoint.id(),
                fingerprint: Vec::new(),
                add_member_if_leader: false,
            });
            for index in 0..MAX_CONTROL_PEERS {
                actor.deferred_admissions.push_back(DeferredAdmission {
                    peer: actor.endpoint.id(),
                    admissions: Vec::new(),
                    add_member_if_leader: false,
                    fingerprint: vec![index as u8],
                    bindings: Vec::new(),
                });
            }
        })
        .await;
        let connection = fixture
            .remote
            .connect(address(&fixture.host), CONTROL_ALPN)
            .await
            .unwrap();
        let mut control = transport::connect_control_on(connection, &fixture.invite)
            .await
            .unwrap();
        let _ = next(&mut fixture.events, "connected").await;
        let admission = Admission {
            room: fixture.room,
            incarnation: 0x4242,
            authority_term: 1,
            coordination_endpoint: fixture.remote.id(),
            coordination_address: address(&fixture.remote),
            primary_endpoint: fixture.remote.id(),
        };
        send_admission(&mut control, 2, admission).await;
        expect_closed_without_session(&mut fixture.events).await;
        fixture.recovery.stop().await;
        fixture.host.close().await;
        fixture.remote.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn a_binding_the_operation_rejects_closes_the_control_without_a_session() {
    timeout(Duration::from_secs(60), async {
        let mut fixture = bound_host(|_| {}).await;
        let connection = fixture
            .remote
            .connect(address(&fixture.host), CONTROL_ALPN)
            .await
            .unwrap();
        let mut control = transport::connect_control_on(connection, &fixture.invite)
            .await
            .unwrap();
        let _ = next(&mut fixture.events, "connected").await;
        // The host's coordination route refuses this incarnation, so the
        // learner can never be added; the bounded operation fails and the
        // control closes with nothing native read from it. (An unreachable
        // learner is not enough: OpenRaft counts a fresh learner as caught up
        // while the log is short, and a room of two promotes nobody.)
        let nobody = endpoint().await;
        let admission = Admission {
            room: fixture.room,
            incarnation: 0x4343,
            authority_term: 1,
            coordination_endpoint: nobody.id(),
            coordination_address: address(&nobody),
            primary_endpoint: fixture.remote.id(),
        };
        nobody.close().await;
        fixture.recovery.rpc.retire(admission.incarnation).await;
        send_admission(&mut control, 2, admission).await;
        control
            .sender
            .send(&ControlFrame {
                message_id: 3,
                payload: b"native message before any session".to_vec(),
            })
            .await
            .unwrap();
        expect_closed_without_session(&mut fixture.events).await;
        fixture.recovery.stop().await;
        fixture.host.close().await;
        fixture.remote.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn an_accepted_admission_publishes_its_session_even_when_roster_known() {
    timeout(Duration::from_secs(60), async {
        let mut fixture = bound_host(|_| {}).await;
        let member = crate::recovery::RecoverySession::join(
            fixture.room,
            fixture.remote.id(),
            fixture.recovery.incarnation,
            fixture.recovery.coordination_address.clone(),
            false,
        )
        .await
        .unwrap();
        let admission = member.advertise().await;
        let connection = fixture
            .remote
            .connect(address(&fixture.host), CONTROL_ALPN)
            .await
            .unwrap();
        let mut control = transport::connect_control_on(connection, &fixture.invite)
            .await
            .unwrap();
        let _ = next(&mut fixture.events, "connected").await;
        // The session is published when the binding completes, and the
        // message sent right behind the Admission is delivered only after it.
        send_admission(&mut control, 2, admission.clone()).await;
        control
            .sender
            .send(&ControlFrame {
                message_id: 3,
                payload: b"first native message".to_vec(),
            })
            .await
            .unwrap();
        let mut session_seen = false;
        loop {
            let event = fixture.events.recv().await.unwrap();
            match event {
                Event::PeerSession { incarnation, .. } => {
                    assert_eq!(incarnation, member.incarnation);
                    session_seen = true;
                }
                Event::Message { payload, .. } => {
                    assert_eq!(payload, "first native message");
                    assert!(session_seen, "native message delivered before its session");
                    break;
                }
                Event::Error { code, .. } => panic!("unexpected helper error: {code}"),
                _ => (),
            }
        }
        // A reconnect re-presenting the same, now roster-known, incarnation
        // is published again; the native record treats the repeat as a no-op.
        let again = fixture
            .remote
            .connect(address(&fixture.host), CONTROL_ALPN)
            .await
            .unwrap();
        let mut control = transport::connect_control_on(again, &fixture.invite)
            .await
            .unwrap();
        let _ = next(&mut fixture.events, "connected").await;
        send_admission(&mut control, 2, admission).await;
        assert!(matches!(
            next(&mut fixture.events, "peer_session").await,
            Event::PeerSession { incarnation, .. } if incarnation == member.incarnation
        ));
        member.stop().await;
        fixture.recovery.stop().await;
        fixture.host.close().await;
        fixture.remote.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn a_completion_settles_only_the_control_pending_for_its_incarnation() {
    timeout(Duration::from_secs(30), async {
        let host = endpoint().await;
        let remote = endpoint().await;
        let relay = iroh::defaults::prod::default_relay_map()
            .urls::<Vec<_>>()
            .remove(0);
        let seed =
            Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600).unwrap();
        let room = seed.room();
        let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(host.clone(), events_tx);
        let invite = actor.setup_host_recovery(seed).await.unwrap();
        let recovery = actor.recovery.clone().unwrap();
        while recovery.coordinator.current_leader() != Some(recovery.incarnation) {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
        actor.epoch = 1;
        actor.room = Some(room);
        actor.hosted = Some(invite.clone());
        actor.room_invite = Some(invite.clone());
        // The remote's current control; its predecessor's operation is still
        // in flight when it presents its own Admission.
        let accept = |invite: Invite| {
            let host = host.clone();
            async move {
                let connection = host.accept().await.unwrap().await.unwrap();
                transport::accept_control(connection, &invite)
                    .await
                    .unwrap()
            }
        };
        let connect = |invite: Invite| {
            let remote = remote.clone();
            let host = host.clone();
            async move {
                let connection = remote.connect(address(&host), CONTROL_ALPN).await.unwrap();
                transport::connect_control_on(connection, &invite)
                    .await
                    .unwrap()
            }
        };
        let (_old_side, old_control) =
            tokio::join!(connect(invite.clone()), accept(invite.clone()));
        let old_control = ControlWorker::start(old_control);
        let old_id = old_control.id();
        drop(old_control); // replaced before its operation completes
        let (_remote_side, current) = tokio::join!(connect(invite.clone()), accept(invite.clone()));
        let mut current = ControlWorker::start(current);
        // Every process incarnation binds its own coordination key.
        let old_coordination = endpoint().await;
        let new_coordination = endpoint().await;
        let (old, new) = (0x1111, 0x2222);
        let admission = |incarnation| {
            let coordination = if incarnation == old {
                &old_coordination
            } else {
                &new_coordination
            };
            Admission {
                room,
                incarnation,
                authority_term: 1,
                coordination_endpoint: coordination.id(),
                coordination_address: address(coordination),
                primary_endpoint: remote.id(),
            }
        };
        current.set_session(Session::Pending(new));
        let control_id = current.id();
        actor.controls.insert(remote.id(), current);
        let key = |sequence| AdmissionOperationKey {
            sequence,
            epoch: 1,
            room,
            incarnation: recovery.incarnation,
            term: recovery.coordinator.current_term(),
            leader: recovery.coordinator.current_leader(),
            peer: remote.id(),
            fingerprint: vec![sequence as u8],
            add_member_if_leader: false,
        };
        let binding = |control, incarnation| ControlBinding {
            peer: remote.id(),
            control,
            incarnation,
        };
        let untouched = |actor: &Actor, events: &mut mpsc::Receiver<Event>| {
            assert_eq!(
                actor
                    .controls
                    .get(&remote.id())
                    .map(|control| control.session()),
                Some(Session::Pending(new))
            );
            while let Ok(event) = events.try_recv() {
                assert!(
                    !matches!(
                        event,
                        Event::PeerSession { .. } | Event::ControlClosed { .. }
                    ),
                    "a replaced control's completion touched its replacement"
                );
            }
        };
        // The old control's operation succeeds, then one fails: neither
        // binds, closes or releases the replacement, even though it is the
        // same endpoint and the replacement is pending.
        actor.pending_admission_operation = Some(key(1));
        actor.pending_admission_bindings = vec![binding(old_id, old)];
        actor
            .completed_admission(
                key(1),
                Ok(AdmissionOperationResult {
                    admissions: vec![admission(old)],
                }),
            )
            .await
            .unwrap();
        untouched(&actor, &mut events);
        actor.pending_admission_operation = Some(key(2));
        actor.pending_admission_bindings = vec![binding(old_id, old)];
        actor
            .completed_admission(key(2), Err(failed("binding rejected")))
            .await
            .unwrap();
        untouched(&actor, &mut events);
        // Roster work for the replacement's own incarnation owns no control.
        actor.pending_admission_operation = Some(key(3));
        actor.pending_admission_bindings = Vec::new();
        actor
            .completed_admission(key(3), Err(failed("roster write lost")))
            .await
            .unwrap();
        untouched(&actor, &mut events);
        // Its own operation binds it and publishes exactly its incarnation.
        actor.pending_admission_operation = Some(key(4));
        actor.pending_admission_bindings = vec![binding(control_id, new)];
        actor
            .completed_admission(
                key(4),
                Ok(AdmissionOperationResult {
                    admissions: vec![admission(new)],
                }),
            )
            .await
            .unwrap();
        assert_eq!(
            actor
                .controls
                .get(&remote.id())
                .map(|control| control.session()),
            Some(Session::Bound(new))
        );
        assert!(matches!(
            next(&mut events, "peer_session").await,
            Event::PeerSession { incarnation, .. } if incarnation == new
        ));
        // A later control whose own operation fails is closed, by its id.
        let (_remote_side_again, later) =
            tokio::join!(connect(invite.clone()), accept(invite.clone()));
        let mut later = ControlWorker::start(later);
        later.set_session(Session::Pending(0x3333));
        let later_id = later.id();
        assert_ne!(later_id, control_id);
        actor.controls.insert(remote.id(), later);
        actor.pending_admission_operation = Some(key(5));
        actor.pending_admission_bindings = vec![binding(later_id, 0x3333)];
        actor
            .completed_admission(key(5), Err(failed("binding rejected")))
            .await
            .unwrap();
        assert!(!actor.controls.contains_key(&remote.id()));
        assert!(matches!(
            next(&mut events, "control_closed").await,
            Event::ControlClosed { control, .. } if control == later_id
        ));
        actor.tasks.abort_all();
        recovery.stop().await;
        host.close().await;
        remote.close().await;
        old_coordination.close().await;
        new_coordination.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn a_rejected_admission_publishes_no_session() {
    timeout(Duration::from_secs(30), async {
        let host = endpoint().await;
        let remote = endpoint().await;
        let relay = iroh::defaults::prod::default_relay_map()
            .urls::<Vec<_>>()
            .remove(0);
        let seed =
            Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600).unwrap();
        let room = seed.room();
        let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_fault, failure) = watch::channel(false);
        let mut actor = test_actor(host.clone(), events_tx);
        let invite = actor.setup_host_recovery(seed).await.unwrap();
        let recovery = actor.recovery.clone().unwrap();
        while recovery.coordinator.current_leader() != Some(recovery.incarnation) {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
        actor.epoch = 1;
        actor.room = Some(room);
        actor.hosted = Some(invite.clone());
        actor.room_invite = Some(invite.clone());
        let retired = 0x7777;
        actor.retired_incarnations.insert(retired);
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
        // A replayed Admission for a retired incarnation is rejected and its
        // control closed. The native room must not have been told about a
        // new session first: that instruction could not be retracted.
        let payload = serde_json::to_vec(&CoordinationControl::Admission {
            admission: Admission {
                room,
                incarnation: retired,
                authority_term: 1,
                coordination_endpoint: remote.id(),
                coordination_address: address(&remote),
                primary_endpoint: remote.id(),
            },
        })
        .unwrap();
        control
            .sender
            .send(&ControlFrame {
                message_id: 2,
                payload,
            })
            .await
            .unwrap();
        expect_closed_without_session(&mut events).await;
        recovery.stop().await;
        host.close().await;
        remote.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn departure_notice_retires_the_seat_at_the_leader() {
    timeout(Duration::from_secs(30), async {
        let host = endpoint().await;
        let remote = endpoint().await;
        let relay = iroh::defaults::prod::default_relay_map()
            .urls::<Vec<_>>()
            .remove(0);
        let seed =
            Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600).unwrap();
        let room = seed.room();
        let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_fault, failure) = watch::channel(false);
        let mut actor = test_actor(host.clone(), events_tx);
        let invite = actor.setup_host_recovery(seed).await.unwrap();
        let recovery = actor.recovery.clone().unwrap();
        while recovery.coordinator.current_leader() != Some(recovery.incarnation) {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
        actor.epoch = 1;
        actor.room = Some(room);
        actor.hosted = Some(invite.clone());
        actor.room_invite = Some(invite.clone());
        let incarnation = 0x5151;
        actor.remember_admission(Admission {
            room,
            incarnation,
            authority_term: 1,
            coordination_endpoint: remote.id(),
            coordination_address: address(&remote),
            primary_endpoint: remote.id(),
        });
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
        // A notice for an incarnation this leader never admitted is ignored;
        // the admitted member's notice hands its seat to the native room
        // while its control, and its vote, stay open.
        for (id, notice) in [(2, 0x9999), (3, incarnation)] {
            let payload = serde_json::to_vec(&CoordinationControl::Departure {
                room,
                incarnation: notice,
            })
            .unwrap();
            control
                .sender
                .send(&ControlFrame {
                    message_id: id,
                    payload,
                })
                .await
                .unwrap();
        }
        assert!(matches!(
            next(&mut events, "peer_departed").await,
            Event::PeerDeparted { epoch: 1, peer } if peer == remote.id()
        ));
        recovery.stop().await;
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
            candidate: None,
            diagnostics: GameDiagnostics::default(),
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
async fn completed_probe_records_the_recommendation_and_closes_the_connection() {
    timeout(Duration::from_secs(20), async {
        let host = endpoint().await;
        let guest = endpoint().await;
        let room = [47; 16];
        let (request, pair_revision) = (9, 23);
        // One exchange: the responder answers, then either closes at once, as
        // the actor does once it has recorded the probe, or keeps its side
        // open and hands the connection back.
        let exchange = |close_after_answer: bool| {
            let (host, guest) = (host.clone(), guest.clone());
            async move {
                tokio::join!(
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
                                let connection = serve_probe(
                                    connection,
                                    send,
                                    recv,
                                    room,
                                    request,
                                    pair_revision,
                                )
                                .await?;
                                if close_after_answer {
                                    connection.close(0u32.into(), b"probe complete");
                                    Ok(None)
                                } else {
                                    Ok(Some(connection))
                                }
                            }
                            transport::GameStream::Gameplay(_, _) => {
                                Err(failed("expected probe stream"))
                            }
                        }
                    }
                )
            }
        };

        // The prober still receives the whole answer when the responder
        // closes straight after it; the close may already be visible.
        let (closed_probe, closed_guest) = exchange(true).await;
        let closed_probe = closed_probe.unwrap();
        closed_guest.unwrap();
        assert!(
            !closed_probe.route_changed,
            "the responder's close after the stream end is not a route change"
        );
        assert!(!closed_probe.samples_us.is_empty());
        if let Some(connection) = closed_probe.connection {
            connection.close(0u32.into(), b"test done");
        }

        // With the responder's side held open, only completed_probe can close
        // the connection, so the check below is about the actor.
        let (host_probe, guest_side) = exchange(false).await;
        let host_probe = host_probe.unwrap();
        let _guest_side = guest_side.unwrap();
        let connection = host_probe.connection.clone().unwrap();
        assert!(connection.close_reason().is_none());

        let (events, mut receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(host.clone(), events);
        actor.epoch = 5;
        actor.room = Some(room);
        let permit = |request, pair_revision| ProbePermission {
            request,
            pair_revision,
            expires: tokio::time::Instant::now() + Duration::from_secs(5),
        };
        actor.own_probes.insert(
            guest.id(),
            OwnProbe {
                request,
                pair_revision,
            },
        );
        actor
            .completed(Completion::Probe(5, Ok(host_probe)))
            .await
            .unwrap();
        assert!(
            connection.close_reason().is_some(),
            "nothing reuses a probe connection, so it must not be kept open"
        );
        assert!(actor.own_probes.is_empty());
        assert!(matches!(
            receiver.recv().await,
            Some(Event::ProbeResult { request: 9, status, .. }) if status == "ready"
        ));

        // A check the peer runs against us reports nothing and leaves the
        // recommendation alone.
        actor.probe_peers.insert(guest.id());
        actor.probe_permissions.insert(guest.id(), permit(10, 23));
        actor
            .completed(Completion::Probe(
                5,
                Ok(ProbeCompletion {
                    peer: guest.id(),
                    request: 10,
                    pair_revision: 23,
                    samples_us: Vec::new(),
                    connection: None,
                    report: false,
                    route_changed: false,
                    route: None,
                    metrics: crate::probe::Metrics::default(),
                }),
            ))
            .await
            .unwrap();
        assert!(actor.probe_permissions.is_empty() && actor.probe_peers.is_empty());
        assert!(actor.pending_probe_invalidations.is_empty());
        assert!(receiver.try_recv().is_err(), "the peer's check reported");
        host.close().await;
        guest.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn probe_invalidation_retries_after_bulk_backpressure() {
    timeout(Duration::from_secs(10), async {
        let local = endpoint().await;
        let remote = endpoint().await;
        let (events, mut receiver) = mpsc::channel(LIFECYCLE_EVENT_RESERVE + 1);
        let mut actor = test_actor(local.clone(), events);
        actor.epoch = 7;
        actor.room = Some([59; 16]);
        assert!(actor.emit_bulk(Event::CheckpointAck {
            epoch: 7,
            room: [59; 16],
            transfer: 1,
            offset: 0,
        }));

        actor.queue_probe_invalidation(remote.id(), 23, 37, "unavailable".into());
        assert!(actor.pending_probe_invalidations.contains_key(&remote.id()));
        assert!(matches!(
            receiver.recv().await,
            Some(Event::CheckpointAck { .. })
        ));

        actor.pump_probe_invalidations();
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
        let (old_connection, old_remote) =
            tokio::join!(local.connect(address(&remote), GAME_ALPN), async {
                remote.accept().await.unwrap().await
            });
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
                    route: None,
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
                .pending_probe_invalidations
                .get(&remote.id())
                .is_some_and(
                    |invalidation| invalidation.request == 29 && invalidation.pair_revision == 41
                )
        );
        old_observer.closed().await;
        assert!(old_observer.close_reason().is_some());
        drop(old_remote);
        local.close().await;
        remote.close().await;
    })
    .await
    .unwrap();
}

/// A completed probe of `request` and `pair_revision`, as a worker reports it:
/// a clean hundred-sample measurement for the prober, nothing for the responder.
fn probe_completion(
    peer: EndpointId,
    request: u64,
    pair_revision: u64,
    report: bool,
) -> Completion {
    let samples: Vec<u64> = if report { vec![300; 100] } else { Vec::new() };
    Completion::Probe(
        5,
        Ok(ProbeCompletion {
            peer,
            request,
            pair_revision,
            metrics: crate::probe::Metrics {
                expected: if report { 100 } else { 0 },
                sent: if report { 100 } else { 0 },
                replies: samples.len() as u32,
                ..crate::probe::Metrics::default()
            },
            samples_us: samples,
            connection: None,
            report,
            route_changed: false,
            route: Some("direct".into()),
        }),
    )
}

/// The peer checks us while we check it, and both checks carry the same request
/// id and pair revision. Each completion settles only the probe of its own role,
/// in either order.
#[tokio::test]
async fn a_probe_completion_settles_only_the_probe_of_its_own_role() {
    timeout(Duration::from_secs(10), async {
        let local = endpoint().await;
        let remote = endpoint().await;
        let peer = remote.id();
        let (events, mut receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(local.clone(), events);
        actor.epoch = 5;
        actor.room = Some([62; 16]);
        let stage = |actor: &mut Actor| {
            actor.own_probes.insert(
                peer,
                OwnProbe {
                    request: 1,
                    pair_revision: 8,
                },
            );
            actor.probe_peers.insert(peer);
            actor.probe_permissions.insert(
                peer,
                ProbePermission {
                    request: 1,
                    pair_revision: 8,
                    expires: tokio::time::Instant::now() + Duration::from_secs(5),
                },
            );
        };

        // The answered check finishes first. It reports nothing and leaves
        // our own check running.
        stage(&mut actor);
        actor
            .completed(probe_completion(peer, 1, 8, false))
            .await
            .unwrap();
        assert!(actor.own_probes.contains_key(&peer));
        assert!(actor.probe_permissions.is_empty() && actor.probe_peers.is_empty());
        assert!(receiver.try_recv().is_err(), "the answered check reported");
        // Our own check then completes with its own samples.
        actor
            .completed(probe_completion(peer, 1, 8, true))
            .await
            .unwrap();
        assert!(actor.own_probes.is_empty());
        assert!(matches!(
            receiver.recv().await,
            Some(Event::ProbeResult { request: 1, pair_revision: 8, status, sample_count: 100, .. })
                if status == "ready"
        ));

        // Our own check finishes first. The answered check still being
        // served is untouched by it, and finishing later reports nothing.
        stage(&mut actor);
        actor
            .completed(probe_completion(peer, 1, 8, true))
            .await
            .unwrap();
        assert!(actor.own_probes.is_empty());
        assert!(actor.probe_permissions.contains_key(&peer));
        assert!(actor.probe_peers.contains(&peer));
        assert!(matches!(
            receiver.recv().await,
            Some(Event::ProbeResult { status, sample_count: 100, .. }) if status == "ready"
        ));
        actor
            .completed(probe_completion(peer, 1, 8, false))
            .await
            .unwrap();
        assert!(actor.probe_permissions.is_empty() && actor.probe_peers.is_empty());
        assert!(receiver.try_recv().is_err(), "the answered check reported");
        assert!(actor.pending_probe_invalidations.is_empty());
        local.close().await;
        remote.close().await;
    })
    .await
    .unwrap();
}

/// A helper actor staged for one probe exchange and started, as the real
/// helper runs it.
struct ProbeSide {
    events: mpsc::Receiver<Event>,
    _commands: mpsc::Sender<Request>,
    _fault: watch::Sender<bool>,
    _service: TaskScope,
}

impl ProbeSide {
    const REVISION: u64 = 4;

    fn actor(endpoint: &Endpoint) -> (Actor, mpsc::Receiver<Event>) {
        let (events_tx, events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(endpoint.clone(), events_tx);
        actor.epoch = 1;
        actor.room = Some([94; 16]);
        (actor, events)
    }

    /// The worker the probe command spawns to check `peer`, released once
    /// `gate` opens.
    fn stage_check(
        actor: &mut Actor,
        peer: &Endpoint,
        gate: Option<tokio::sync::oneshot::Receiver<()>>,
    ) {
        let (endpoint, address, peer_id) = (actor.endpoint.clone(), address(peer), peer.id());
        actor.own_probes.insert(
            peer_id,
            OwnProbe {
                request: 1,
                pair_revision: Self::REVISION,
            },
        );
        actor.tasks.spawn(async move {
            if let Some(gate) = gate {
                let _ = gate.await;
            }
            let completion = run_probe(
                endpoint,
                address,
                [94; 16],
                peer_id,
                1,
                Self::REVISION,
                false,
            )
            .await
            .expect("check of the peer");
            Completion::Probe(1, Ok(completion))
        });
    }

    /// The reservation the peer's check needs, as its control frame installs it.
    fn permit(actor: &mut Actor, peer: &Endpoint) {
        actor.install_probe_permission(peer.id(), 1, Self::REVISION);
    }

    fn start(mut actor: Actor, events: mpsc::Receiver<Event>) -> Self {
        let (commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (fault, failure) = watch::channel(false);
        let service = tokio::spawn(async move {
            let result = actor.run(command_rx, failure).await;
            actor.tasks.shutdown().await;
            result
        });
        Self {
            events,
            _commands: commands,
            _fault: fault,
            _service: TaskScope(vec![service.abort_handle()]),
        }
    }

    async fn probe_result(&mut self) -> Event {
        timeout(Duration::from_secs(40), async {
            loop {
                match self.events.recv().await.expect("helper events") {
                    event @ Event::ProbeResult { .. } => return event,
                    Event::Error { code, .. } => panic!("helper reported {code}"),
                    _ => (),
                }
            }
        })
        .await
        .expect("a probe result")
    }
}

/// A checks B and B checks A the moment A has its result, with the same request
/// id and pair revision both ways, while A is still serving B's check. Both
/// checks must finish with their own samples and neither helper may retire the
/// other's recommendation, however the two helpers' closes and completions
/// interleave.
async fn reverse_check_after_check() {
    let (a, b) = (endpoint().await, endpoint().await);
    let ((mut a_actor, a_events), (mut b_actor, b_events)) =
        (ProbeSide::actor(&a), ProbeSide::actor(&b));
    let (release, gate) = tokio::sync::oneshot::channel();
    ProbeSide::stage_check(&mut a_actor, &b, None);
    ProbeSide::permit(&mut b_actor, &a);
    ProbeSide::stage_check(&mut b_actor, &a, Some(gate));
    ProbeSide::permit(&mut a_actor, &b);
    let mut a_side = ProbeSide::start(a_actor, a_events);
    let mut b_side = ProbeSide::start(b_actor, b_events);

    let first = a_side.probe_result().await;
    assert_no_address(&first);
    match first {
        Event::ProbeResult {
            peer,
            request: 1,
            status,
            sample_count,
            ..
        } => assert!(peer == b.id() && status == "ready" && sample_count >= 80),
        other => panic!("A's check: {}", serde_json::to_string(&other).unwrap()),
    }
    release.send(()).unwrap();
    let second = b_side.probe_result().await;
    assert_no_address(&second);
    match second {
        Event::ProbeResult {
            peer,
            request: 1,
            pair_revision: ProbeSide::REVISION,
            route,
            status,
            sample_count,
            ..
        } => {
            assert_eq!(peer, a.id());
            assert_eq!(status, "ready", "B's check must not be retired: {route}");
            assert!(sample_count >= 80, "{sample_count} samples");
            assert_eq!(route, "direct");
        }
        other => panic!("B's check: {}", serde_json::to_string(&other).unwrap()),
    }
    // Serving B's check retires nothing of A's, and nothing of A's check
    // reaches B afterwards.
    assert!(
        timeout(Duration::from_millis(500), a_side.probe_result())
            .await
            .is_err(),
        "A's recommendation was retired or replaced"
    );
    assert!(
        timeout(Duration::from_millis(300), b_side.probe_result())
            .await
            .is_err()
    );
    drop((a_side, b_side));
    a.close().await;
    b.close().await;
}

/// A and B check each other at the same moment, so each helper serves the
/// other's check while its own is in flight and the two finish in any order.
/// Each side ends with exactly one result of its own, with samples, and no
/// retirement.
async fn simultaneous_checks_both_ways() {
    let (a, b) = (endpoint().await, endpoint().await);
    let ((mut a_actor, a_events), (mut b_actor, b_events)) =
        (ProbeSide::actor(&a), ProbeSide::actor(&b));
    ProbeSide::permit(&mut a_actor, &b);
    ProbeSide::permit(&mut b_actor, &a);
    ProbeSide::stage_check(&mut a_actor, &b, None);
    ProbeSide::stage_check(&mut b_actor, &a, None);
    let mut a_side = ProbeSide::start(a_actor, a_events);
    let mut b_side = ProbeSide::start(b_actor, b_events);

    let (a_result, b_result) = tokio::join!(a_side.probe_result(), b_side.probe_result());
    for (result, peer) in [(a_result, b.id()), (b_result, a.id())] {
        match result {
            Event::ProbeResult {
                peer: reported,
                request: 1,
                pair_revision: ProbeSide::REVISION,
                status,
                sample_count,
                ..
            } => assert!(reported == peer && status == "ready" && sample_count >= 80),
            other => panic!("check: {}", serde_json::to_string(&other).unwrap()),
        }
    }
    for side in [&mut a_side, &mut b_side] {
        assert!(
            timeout(Duration::from_millis(500), side.probe_result())
                .await
                .is_err(),
            "a recommendation was retired or replaced"
        );
    }
    drop((a_side, b_side));
    a.close().await;
    b.close().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_peers_new_check_always_completes_with_its_own_samples() {
    // Each exchange already overlaps two checks between the same pair; a few
    // rounds cover both completion orders. Running them one after another
    // keeps the sample counts about the checks, not about machine load.
    for _ in 0..2 {
        reverse_check_after_check().await;
        simultaneous_checks_both_ways().await;
    }
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
async fn an_accepted_admission_keeps_its_identity() {
    timeout(Duration::from_secs(20), async {
        let host = endpoint().await;
        let peer = endpoint().await;
        let member_primary = endpoint().await;
        let member_coordination = endpoint().await;
        let peer_coordination = endpoint().await;
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
        actor.room_invite = Some(invite);
        let recovery = actor.recovery.clone().unwrap();
        let own = actor.admissions[&recovery.incarnation].clone();
        let member = Admission {
            room,
            incarnation: 0x5150,
            authority_term: 1,
            coordination_endpoint: member_coordination.id(),
            coordination_address: address(&member_coordination),
            primary_endpoint: member_primary.id(),
        };
        actor.remember_admission(member.clone());
        let present = |admission: Admission| {
            serde_json::to_string(&CoordinationControl::Admission { admission }).unwrap()
        };

        // The peer's authenticated primary endpoint paired with an
        // identity another admission already holds: the host's own
        // incarnation, a member's incarnation, or the host's coordination key
        // under a fresh incarnation. Also a coordination key that disagrees
        // with the address that would route it.
        let conflicting = [
            Admission {
                primary_endpoint: peer.id(),
                ..own.clone()
            },
            Admission {
                primary_endpoint: peer.id(),
                ..member.clone()
            },
            Admission {
                incarnation: 0x5151,
                primary_endpoint: peer.id(),
                ..own.clone()
            },
            Admission {
                incarnation: 0x5152,
                primary_endpoint: peer.id(),
                ..member.clone()
            },
            Admission {
                room,
                incarnation: 0x5152,
                authority_term: 1,
                coordination_endpoint: member_coordination.id(),
                coordination_address: address(&peer_coordination),
                primary_endpoint: peer.id(),
            },
        ];
        for admission in conflicting {
            assert!(
                actor
                    .accept_coordination_control(peer.id(), &present(admission))
                    .await
                    .is_err()
            );
        }
        assert_eq!(
            actor.admissions[&recovery.incarnation].primary_endpoint,
            host.id()
        );
        assert_eq!(
            actor.admissions[&0x5150].primary_endpoint,
            member_primary.id()
        );
        assert!(actor.pending_admissions.is_empty());
        assert!(actor.pending_admission_operation.is_none());

        // An admission with its own identities is held apart until its
        // operation accepts it; nothing resolves a route through it before.
        let fresh = Admission {
            room,
            incarnation: 0x5153,
            authority_term: 1,
            coordination_endpoint: peer_coordination.id(),
            coordination_address: address(&peer_coordination),
            primary_endpoint: peer.id(),
        };
        assert!(
            actor
                .accept_coordination_control(peer.id(), &present(fresh.clone()))
                .await
                .unwrap()
        );
        assert!(!actor.admissions.contains_key(&fresh.incarnation));
        assert_eq!(
            actor.pending_admissions[&peer.id()].incarnation,
            fresh.incarnation
        );

        // Even a completion that reports a conflicting binding as validated
        // leaves the accepted record in place.
        let key = actor.pending_admission_operation.clone().unwrap();
        actor
            .completed_admission(
                key,
                Ok(AdmissionOperationResult {
                    admissions: vec![Admission {
                        primary_endpoint: peer.id(),
                        ..member.clone()
                    }],
                }),
            )
            .await
            .unwrap();
        assert_eq!(
            actor.admissions[&0x5150].primary_endpoint,
            member_primary.id()
        );
        assert!(!actor.admissions.contains_key(&fresh.incarnation));

        actor.tasks.abort_all();
        recovery.stop().await;
        for endpoint in [
            host,
            peer,
            member_primary,
            member_coordination,
            peer_coordination,
        ] {
            endpoint.close().await;
        }
    })
    .await
    .unwrap();
}

/// Runs the actor's next finished worker through its completion handler.
async fn complete_next(actor: &mut Actor) {
    let completion = timeout(Duration::from_secs(10), actor.tasks.join_next())
        .await
        .expect("worker completion")
        .expect("worker exists")
        .unwrap();
    actor.completed(completion).await.unwrap();
}

#[tokio::test]
async fn a_confirmed_departure_is_not_reconciled_on_every_refresh() {
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
        actor.room_invite = Some(invite);
        // The committed native roster no longer lists the member that left.
        actor.committed_native_members = Some(BTreeSet::from([host.id()]));
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
        let incarnation = admission.incarnation;
        recovery.add_learner(&admission).await.unwrap();
        actor.remember_admission(admission);
        recovery
            .remove_nodes(BTreeSet::from([incarnation]))
            .await
            .unwrap();

        // The first refresh sees the departure in applied history and fences
        // it; the reconciliation it starts confirms the retirement.
        actor.emit_coordination_state().await.unwrap();
        complete_next(&mut actor).await;
        assert!(actor.pending_retired_incarnations.contains(&incarnation));
        assert!(actor.pending_membership_operation.is_some());
        complete_next(&mut actor).await;
        assert!(actor.retired_incarnations.contains(&incarnation));
        assert!(actor.pending_retired_incarnations.is_empty());
        assert!(!actor.admissions.contains_key(&incarnation));

        // That completion refreshes once more. The departure stays in the
        // Raft history for good, but a confirmed retirement must not be
        // fenced and reconciled again: each round refreshes, which used to
        // start the next round, and every leader round published a
        // Membership frame to every member.
        complete_next(&mut actor).await;
        assert!(
            actor.pending_retired_incarnations.is_empty(),
            "a refresh fenced a confirmed retirement again"
        );
        assert!(
            actor.pending_membership_operation.is_none(),
            "a confirmed retirement started another reconciliation"
        );
        assert!(actor.tasks.is_empty());

        departed.stop().await;
        recovery.stop().await;
        host.close().await;
        departed_primary.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn a_full_retirement_set_makes_room_by_dropping_an_aged_out_tombstone() {
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
    // The 128 tombstones are unknown to the replicated history, so they have
    // aged out and the newest departure takes the place of the lowest one.
    assert!(actor.pending_retired_incarnations.is_empty());
    assert!(actor.retired_incarnations.contains(&incarnation));
    assert!(!actor.retired_incarnations.contains(&1));
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
            .queue_admission_operation(
                first_primary.id(),
                vec![first_admission.clone()],
                true,
                None,
            )
            .unwrap();
        actor
            .queue_admission_operation(first_primary.id(), vec![first_admission], true, None)
            .unwrap();
        assert!(actor.deferred_admissions.is_empty());
        actor
            .queue_admission_operation(
                second_primary.id(),
                vec![second_admission.clone()],
                true,
                None,
            )
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
                None,
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
    actor.own_probes.insert(
        peer_primary.id(),
        OwnProbe {
            request: 51,
            pair_revision: 7,
        },
    );
    let task_count = actor.tasks.len();
    actor
        .completed(Completion::ProbeAuthorization(
            key,
            Ok(ProbeAuthorization {
                term: recovery.coordinator.current_term(),
                leader: recovery.coordinator.current_leader(),
                revision: recovery.committed().await.revision,
                expires: now().unwrap() + 60,
                // Its window has run out on the monotonic clock, whatever the wall
                // clock says.
                deadline: tokio::time::Instant::now(),
            }),
        ))
        .await
        .unwrap();
    assert!(!actor.probe_permissions.contains_key(&peer_primary.id()));
    assert!(!actor.own_probes.contains_key(&peer_primary.id()));
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
            parked_controls: BTreeMap::new(),
            games: BTreeMap::new(),
            closed_generation: 0,
            tasks: JoinSet::new(),
            events: EventOutbox::new(events),
            recovery: None,
            admissions: BTreeMap::new(),
            admission_order: Vec::new(),
            pending_admissions: BTreeMap::new(),
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
            pending_admission_bindings: Vec::new(),
        deferred_admissions: VecDeque::new(),
        pending_membership_publications: BTreeSet::new(),
            last_coordination_state: None,
            coordination_writable: false,
            last_control_rebound: None,
            unwritable_leader_since: None,
            reconnect_target: None,
            pending_game_admissions: BTreeMap::new(),
            pending_probe_invalidations: BTreeMap::new(),
            probe_peers: BTreeSet::new(),
            probe_permissions: BTreeMap::new(),
            own_probes: BTreeMap::new(),
            pending_probe_authorizations: BTreeMap::new(),
            pending_probe_reservations: BTreeMap::new(),
            join_settled: false,
            join_control_losses: 0,
            join_first_loss: None,
            retirement_started: None,
            departure_failed: false,
            short: ShortLinks::default(),
            public: None,
            coordination_port: None,
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
                    control: 0,
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
        parked_controls: BTreeMap::new(),
        games: BTreeMap::new(),
        closed_generation: 99,
        tasks: JoinSet::new(),
        events: EventOutbox::new(events_tx),
        recovery: None,
        admissions: BTreeMap::new(),
        admission_order: Vec::new(),
        pending_admissions: BTreeMap::new(),
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
        pending_admission_bindings: Vec::new(),
        deferred_admissions: VecDeque::new(),
        pending_membership_publications: BTreeSet::new(),
        last_coordination_state: None,
        coordination_writable: false,
        last_control_rebound: None,
        unwritable_leader_since: None,
        reconnect_target: None,
        pending_game_admissions: BTreeMap::new(),
        pending_probe_invalidations: BTreeMap::new(),
        probe_peers: BTreeSet::new(),
        probe_permissions: BTreeMap::new(),
        own_probes: BTreeMap::new(),
        pending_probe_authorizations: BTreeMap::new(),
        pending_probe_reservations: BTreeMap::new(),
        join_settled: false,
        join_control_losses: 0,
        join_first_loss: None,
        retirement_started: None,
        departure_failed: false,
        short: ShortLinks::default(),
        public: None,
        coordination_port: None,
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
            parked_controls: BTreeMap::new(),
            games: BTreeMap::new(),
            closed_generation: 0,
            tasks: JoinSet::new(),
            events: EventOutbox::new(events_tx),
            recovery: None,
            admissions: BTreeMap::new(),
            admission_order: Vec::new(),
            pending_admissions: BTreeMap::new(),
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
            pending_admission_bindings: Vec::new(),
            deferred_admissions: VecDeque::new(),
            pending_membership_publications: BTreeSet::new(),
            last_coordination_state: None,
            coordination_writable: false,
            last_control_rebound: None,
            unwritable_leader_since: None,
            reconnect_target: None,
            pending_game_admissions: BTreeMap::new(),
            pending_probe_invalidations: BTreeMap::new(),
            probe_peers: BTreeSet::new(),
            probe_permissions: BTreeMap::new(),
            own_probes: BTreeMap::new(),
            pending_probe_authorizations: BTreeMap::new(),
            pending_probe_reservations: BTreeMap::new(),
            join_settled: false,
            join_control_losses: 0,
            join_first_loss: None,
            retirement_started: None,
            departure_failed: false,
            short: ShortLinks::default(),
            public: None,
            coordination_port: None,
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
        let Event::Connected {
            control: control_id,
            ..
        } = next(&mut events, "connected").await
        else {
            unreachable!()
        };
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
                    control: 0,
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

        // A send named for a control this endpoint no longer holds is
        // refused and says why; one named for the current control is sent.
        for (id, named, message_id) in [(90, control_id.wrapping_add(1), 52), (91, control_id, 53)]
        {
            commands
                .send(Request {
                    id,
                    command: Command::Send {
                        epoch: 1,
                        peer: remote.id(),
                        message_id,
                        payload: "named reply".into(),
                        control: named,
                    },
                })
                .await
                .unwrap();
        }
        match next(&mut events, "error").await {
            Event::Error {
                code, peer, reason, ..
            } => {
                assert_eq!(code, "control_send_failed");
                assert_eq!(peer, Some(remote.id()));
                assert_eq!(reason.as_deref(), Some("replaced"));
            }
            _ => unreachable!(),
        }
        let named_frame = control.receiver.receive().await.unwrap();
        let named: NativeControlMessage = serde_json::from_slice(&named_frame.payload).unwrap();
        assert_eq!(named.message_id, 53);
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
        let ready = next(&mut events, "game_ready").await;
        assert_no_address(&ready);
        let virtual_host = match ready {
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
            parked_controls: BTreeMap::new(),
            games: BTreeMap::new(),
            closed_generation: 0,
            tasks: JoinSet::new(),
            events: EventOutbox::new(events_tx),
            recovery: None,
            admissions: BTreeMap::new(),
            admission_order: Vec::new(),
            pending_admissions: BTreeMap::new(),
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
            pending_admission_bindings: Vec::new(),
            deferred_admissions: VecDeque::new(),
            pending_membership_publications: BTreeSet::new(),
            last_coordination_state: None,
            coordination_writable: false,
            last_control_rebound: None,
            unwritable_leader_since: None,
            reconnect_target: None,
            pending_game_admissions: BTreeMap::new(),
            pending_probe_invalidations: BTreeMap::new(),
            probe_peers: BTreeSet::new(),
            probe_permissions: BTreeMap::new(),
            own_probes: BTreeMap::new(),
            pending_probe_authorizations: BTreeMap::new(),
            pending_probe_reservations: BTreeMap::new(),
            join_settled: false,
            join_control_losses: 0,
            join_first_loss: None,
            retirement_started: None,
            departure_failed: false,
            short: ShortLinks::default(),
            public: None,
            coordination_port: None,
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

fn test_relay() -> iroh::RelayUrl {
    iroh::defaults::prod::default_relay_map()
        .urls::<Vec<_>>()
        .remove(0)
}

#[tokio::test]
async fn a_rejoining_incarnation_supersedes_the_older_one_of_its_endpoint() {
    timeout(Duration::from_secs(60), async {
        let host = endpoint().await;
        let remote = endpoint().await;
        let seed = Invite::create(
            host.id(),
            test_relay(),
            "test-build".into(),
            now().unwrap(),
            3600,
        )
        .unwrap();
        let room = seed.room();
        let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_fault, failure) = watch::channel(false);
        let mut actor = test_actor(host.clone(), events_tx);
        let invite = actor.setup_host_recovery(seed).await.unwrap();
        let recovery = actor.recovery.clone().unwrap();
        while recovery.coordinator.current_leader() != Some(recovery.incarnation) {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
        // The endpoint's previous process is an admitted member whose exit
        // was abandoned, so the leader still holds its route binding.
        let old = crate::recovery::RecoverySession::join(
            room,
            remote.id(),
            recovery.incarnation,
            recovery.coordination_address.clone(),
            false,
        )
        .await
        .unwrap();
        let old_admission = old.advertise().await;
        recovery.add_learner(&old_admission).await.unwrap();
        actor.epoch = 1;
        actor.room = Some(room);
        actor.hosted = Some(invite.clone());
        actor.room_invite = Some(invite.clone());
        actor.remember_admission(old_admission);
        let new = crate::recovery::RecoverySession::join(
            room,
            remote.id(),
            recovery.incarnation,
            recovery.coordination_address.clone(),
            false,
        )
        .await
        .unwrap();
        let new_admission = new.advertise().await;
        let service = tokio::spawn(async move {
            let result = actor.run(command_rx, failure).await;
            actor.clear_room();
            actor.tasks.shutdown().await;
            result
        });
        let _scope = TaskScope(vec![service.abort_handle()]);

        let connection = remote.connect(address(&host), CONTROL_ALPN).await.unwrap();
        let mut control = transport::connect_control_on(connection, &invite)
            .await
            .unwrap();
        let _ = next(&mut events, "connected").await;
        send_admission(&mut control, 2, new_admission).await;
        // The new process is bound although the old one still held the
        // endpoint, and the old one leaves the coordination membership.
        let session = timeout(Duration::from_secs(20), next(&mut events, "peer_session"))
            .await
            .expect("the rejoining process was never bound");
        assert!(matches!(
            session,
            Event::PeerSession { incarnation, .. } if incarnation == new.incarnation
        ));
        timeout(Duration::from_secs(30), async {
            loop {
                let members = recovery.applied_member_ids().await;
                if members.contains(&new.incarnation) && !members.contains(&old.incarnation) {
                    break;
                }
                tokio::time::sleep(Duration::from_millis(50)).await;
            }
        })
        .await
        .expect("the superseded incarnation was never removed");
        assert!(recovery.rpc.is_retired(old.incarnation).await);
        old.stop().await;
        new.stop().await;
        recovery.stop().await;
        host.close().await;
        remote.close().await;
    })
    .await
    .unwrap();
}

fn joined_invite(authority: &Endpoint) -> Invite {
    Invite::create(
        authority.id(),
        test_relay(),
        "test-build".into(),
        now().unwrap(),
        3600,
    )
    .unwrap()
}

async fn next_error(events: &mut mpsc::Receiver<Event>) -> Event {
    loop {
        let event = events.recv().await.unwrap();
        if matches!(event, Event::Error { .. }) {
            return event;
        }
    }
}

#[tokio::test]
async fn failed_attempts_say_at_which_stage_they_failed() {
    let local = endpoint().await;
    let (events, mut receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(local.clone(), events);
    let unreachable = || io::Error::new(io::ErrorKind::ConnectionAborted, transport::HOST_UNREACHABLE);
    // A join whose connection never opened, with no home relay connected here
    // (the test endpoint has none): the relay is what is missing.
    actor.epoch = 1;
    actor.opening = true;
    actor
        .completed_control(1, Err(unreachable()), None)
        .await
        .unwrap();
    assert!(matches!(
        next_error(&mut receiver).await,
        Event::Error { epoch: 1, ref code, ref reason, .. }
            if code == "join_failed" && reason.as_deref() == Some("relay_unreachable")
    ));
    // A connection that opened and was refused carries no stage.
    actor.epoch = 2;
    actor.opening = true;
    actor
        .completed_control(2, Err(io::Error::other("refused")), None)
        .await
        .unwrap();
    assert!(matches!(
        next_error(&mut receiver).await,
        Event::Error { epoch: 2, ref code, reason: None, .. } if code == "join_failed"
    ));
    // A public host's refusal, and a handshake that ran out of time, each have
    // their own reason, apart from the generic close above and from each other.
    let invite = Invite::create(local.id(), test_relay(), "stage".into(), now().unwrap(), 3600)
        .unwrap();
    let ticket = public_support::ticket_for(&invite, &public_support::identity(1), &local);
    for (epoch, error, expected) in [
        (
            4,
            transport::admission_refused(),
            "refused",
        ),
        (
            5,
            io::Error::new(io::ErrorKind::TimedOut, transport::HANDSHAKE_TIMED_OUT),
            "timeout",
        ),
    ] {
        actor.epoch = epoch;
        actor.opening = true;
        // A failed join clears the room, and its public marker with it.
        actor.public = Some(crate::public_room::PublicRoom::Member {
            ticket: ticket.clone(),
        });
        actor
            .completed_control(epoch, Err(error), None)
            .await
            .unwrap();
        assert!(matches!(
            next_error(&mut receiver).await,
            Event::Error { epoch: seen, ref code, ref reason, .. }
                if seen == epoch && code == "join_failed" && reason.as_deref() == Some(expected)
        ));
    }
    // A private join never reports either: the same errors are the generic one.
    actor.public = None;
    for (epoch, error) in [
        (6, transport::admission_refused()),
        (
            7,
            io::Error::new(io::ErrorKind::TimedOut, transport::HANDSHAKE_TIMED_OUT),
        ),
    ] {
        actor.epoch = epoch;
        actor.opening = true;
        actor
            .completed_control(epoch, Err(error), None)
            .await
            .unwrap();
        assert!(matches!(
            next_error(&mut receiver).await,
            Event::Error { epoch: seen, ref code, reason: None, .. }
                if seen == epoch && code == "join_failed"
        ));
    }
    // A host attempt whose relay never came online.
    actor.epoch = 3;
    actor.opening = true;
    actor
        .completed_hosted(3, Err(failed("relay_unavailable")))
        .await
        .unwrap();
    assert!(matches!(
        next_error(&mut receiver).await,
        Event::Error { epoch: 3, ref code, ref reason, .. }
            if code == "host_unavailable" && reason.as_deref() == Some("relay_unreachable")
    ));
    local.close().await;
}

#[tokio::test]
async fn the_actor_reports_its_network_without_a_room() {
    timeout(Duration::from_secs(15), async {
        let local = endpoint().await;
        let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_fault, failure) = watch::channel(false);
        let mut actor = test_actor(local.clone(), events_tx);
        let service = tokio::spawn(async move { actor.run(command_rx, failure).await });
        let _service_scope = TaskScope(vec![service.abort_handle()]);
        // The first tick reports before any net report exists.
        let Event::NetworkReport { relay, nat, .. } = next(&mut events, "network_report").await
        else {
            unreachable!()
        };
        assert!(relay.is_empty() || ["use1", "usw1", "euc1", "aps1", "other"].contains(&relay.as_str()));
        assert!(["checking", "open", "strict", "no_udp"].contains(&nat.as_str()));
        local.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn a_join_that_is_never_admitted_ends_in_join_failed() {
    let joiner = endpoint().await;
    let authority = endpoint().await;
    let other = endpoint().await;
    let invite = joined_invite(&authority);
    let (events, mut receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(joiner.clone(), events);
    actor.epoch = 1;
    actor.room = Some(invite.room());
    actor.room_invite = Some(invite);
    // Losing a control to some other member says nothing about the join.
    for _ in 0..=MAX_JOIN_CONTROL_LOSSES {
        assert!(!actor.join_control_lost(other.id()).await.unwrap());
    }
    for _ in 1..MAX_JOIN_CONTROL_LOSSES {
        assert!(!actor.join_control_lost(authority.id()).await.unwrap());
    }
    // Quick refusals alone do not end the join, as on a slow relay; the same
    // count spread over the give-up window does.
    assert!(!actor.join_control_lost(authority.id()).await.unwrap());
    assert!(actor.room.is_some());
    actor.join_first_loss = actor.join_first_loss.map(|first| first - JOIN_GIVE_UP_AFTER);
    assert!(actor.join_control_lost(authority.id()).await.unwrap());
    assert!(actor.room.is_none());
    assert!(matches!(
        next_error(&mut receiver).await,
        Event::Error { epoch: 1, ref code, ref reason, .. }
            if code == "join_failed" && reason.as_deref() == Some("control_lost")
    ));

    // A join whose member was admitted redials without limit.
    let mut settled = test_actor(joiner.clone(), mpsc::channel(IPC_QUEUE_CAPACITY).0);
    settled.join_settled = true;
    settled.room_invite = Some(joined_invite(&authority));
    for _ in 0..=MAX_JOIN_CONTROL_LOSSES {
        assert!(!settled.join_control_lost(authority.id()).await.unwrap());
    }
    joiner.close().await;
    authority.close().await;
    other.close().await;
}

#[tokio::test]
async fn a_retirement_set_holding_only_recent_departures_fails_closed() {
    let local = endpoint().await;
    let (events, _receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(local.clone(), events);
    let mut history: BTreeSet<u64> = (1..=MAX_RETIRED_INCARNATIONS as u64).collect();
    actor
        .retired_incarnations
        .extend(1..=MAX_RETIRED_INCARNATIONS as u64);
    // Every tombstone is still inside the replicated window: none may go.
    assert!(!actor.record_retirement(u64::MAX, &history));
    assert_eq!(actor.retired_incarnations.len(), MAX_RETIRED_INCARNATIONS);
    assert!(!actor.retired_incarnations.contains(&u64::MAX));
    // Once one has aged out of the window it makes room.
    history.remove(&5);
    assert!(actor.record_retirement(u64::MAX, &history));
    assert!(actor.retired_incarnations.contains(&u64::MAX));
    assert!(!actor.retired_incarnations.contains(&5));
    assert_eq!(actor.retired_incarnations.len(), MAX_RETIRED_INCARNATIONS);
    local.close().await;
}

/// A learner follower with a control to a running leader that has admitted it.
struct LearnerFollower {
    fixture: BoundHost,
    learner: crate::recovery::RecoverySession,
    follower: Actor,
    events: mpsc::Receiver<Event>,
    commands: mpsc::Receiver<Request>,
    failure: watch::Receiver<bool>,
    _command_sender: mpsc::Sender<Request>,
    _fault: watch::Sender<bool>,
}

async fn learner_follower() -> LearnerFollower {
    let mut fixture = bound_host(|_| {}).await;
    let learner = crate::recovery::RecoverySession::join(
        fixture.room,
        fixture.remote.id(),
        fixture.recovery.incarnation,
        fixture.recovery.coordination_address.clone(),
        false,
    )
    .await
    .unwrap();
    let admission = learner.advertise().await;
    let connection = fixture
        .remote
        .connect(address(&fixture.host), CONTROL_ALPN)
        .await
        .unwrap();
    let channel = transport::connect_control_on(connection, &fixture.invite)
        .await
        .unwrap();
    let _ = next(&mut fixture.events, "connected").await;
    let worker = ControlWorker::start(channel);
    worker
        .try_send(ControlFrame {
            message_id: TRANSPORT_MESSAGE_ID_BASE,
            payload: serde_json::to_vec(&CoordinationControl::Admission {
                admission: admission.clone(),
            })
            .unwrap(),
        })
        .unwrap();
    // The leader binds the session once the learner is a Raft member. It
    // also promotes the member, so hand the vote back: a learner is what a
    // room of four or more members holds.
    let _ = next(&mut fixture.events, "peer_session").await;
    fixture
        .recovery
        .promote_voters(BTreeSet::from([fixture.recovery.incarnation]))
        .await
        .unwrap();
    let (events_tx, events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (command_sender, commands) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (fault, failure) = watch::channel(false);
    let mut follower = test_actor(fixture.remote.clone(), events_tx);
    follower.epoch = 1;
    follower.room = Some(fixture.room);
    follower.room_invite = Some(fixture.invite.clone());
    follower.remember_admission(admission);
    follower.committed_native_members =
        Some(BTreeSet::from([fixture.host.id(), fixture.remote.id()]));
    follower.recovery = Some(learner.clone());
    follower.next_transport_message = TRANSPORT_MESSAGE_ID_BASE + 1;
    follower.controls.insert(fixture.host.id(), worker);
    while learner.coordinator.current_leader() != Some(fixture.recovery.incarnation)
        || learner.applied_voter_ids().await != BTreeSet::from([fixture.recovery.incarnation])
    {
        tokio::time::sleep(Duration::from_millis(25)).await;
    }
    LearnerFollower {
        fixture,
        learner,
        follower,
        events,
        commands,
        failure,
        _command_sender: command_sender,
        _fault: fault,
    }
}

/// The leader must hear the departure before it sees the control close.
async fn expect_departure_before_close(events: &mut mpsc::Receiver<Event>, peer: EndpointId) {
    loop {
        match events.recv().await.unwrap() {
            Event::PeerDeparted { peer: departed, .. } if departed == peer => return,
            Event::ControlClosed { .. } => panic!("control closed before the departure notice"),
            _ => (),
        }
    }
}

#[tokio::test]
async fn a_learner_departure_notice_reaches_the_leader() {
    timeout(Duration::from_secs(60), async {
        let mut fixture = learner_follower().await;
        assert!(
            fixture
                .follower
                .leave_command(1, false, &mut fixture.commands, &mut fixture.failure)
                .await
                .unwrap()
        );
        assert!(matches!(
            next(&mut fixture.events, "room_closed").await,
            Event::RoomClosed { epoch: 1 }
        ));
        expect_departure_before_close(&mut fixture.fixture.events, fixture.fixture.remote.id())
            .await;
        fixture.learner.stop().await;
        fixture.fixture.recovery.stop().await;
        fixture.fixture.host.close().await;
        fixture.fixture.remote.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn an_abandoning_follower_announces_its_departure() {
    timeout(Duration::from_secs(60), async {
        let mut fixture = learner_follower().await;
        assert!(
            fixture
                .follower
                .leave_command(1, true, &mut fixture.commands, &mut fixture.failure)
                .await
                .unwrap()
        );
        assert!(matches!(
            next(&mut fixture.events, "room_closed").await,
            Event::RoomClosed { epoch: 1 }
        ));
        assert!(fixture.follower.controls.is_empty());
        expect_departure_before_close(&mut fixture.fixture.events, fixture.fixture.remote.id())
            .await;
        fixture.learner.stop().await;
        fixture.fixture.recovery.stop().await;
        fixture.fixture.host.close().await;
        fixture.fixture.remote.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn removing_a_voter_does_not_wait_for_an_unreachable_voter() {
    timeout(Duration::from_secs(60), async {
        let host = endpoint().await;
        let leaving_primary = endpoint().await;
        let dead_primary = endpoint().await;
        let seed = Invite::create(
            host.id(),
            test_relay(),
            "test-build".into(),
            now().unwrap(),
            3600,
        )
        .unwrap();
        let room = seed.room();
        let (events, _events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(host.clone(), events);
        let invite = actor.setup_host_recovery(seed).await.unwrap();
        actor.epoch = 1;
        actor.room = Some(room);
        actor.hosted = Some(invite.clone());
        actor.room_invite = Some(invite);
        let recovery = actor.recovery.clone().unwrap();
        while recovery.coordinator.current_leader() != Some(recovery.incarnation) {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
        let mut sessions = Vec::new();
        for primary in [&leaving_primary, &dead_primary] {
            let session = crate::recovery::RecoverySession::join(
                room,
                primary.id(),
                recovery.incarnation,
                recovery.coordination_address.clone(),
                false,
            )
            .await
            .unwrap();
            let admission = session.advertise().await;
            recovery.add_learner(&admission).await.unwrap();
            actor.remember_admission(admission);
            sessions.push(session);
        }
        let (leaving, dead) = (sessions.remove(0), sessions.remove(0));
        recovery
            .promote_voters(BTreeSet::from([
                recovery.incarnation,
                leaving.incarnation,
                dead.incarnation,
            ]))
            .await
            .unwrap();
        // One voter has gone for good, and the roster has dropped the other.
        dead.stop().await;
        actor
            .pending_retired_incarnations
            .insert(leaving.incarnation);
        let retained = BTreeSet::from([host.id(), dead_primary.id()]);
        actor.committed_native_members = Some(retained.clone());
        let term = recovery.state().await.term;
        let revision = recovery.committed().await.revision;
        actor.spawn_membership_operation(retained, term, revision);
        // The replacement voter set leaves out the voter that cannot answer,
        // so the change commits instead of waiting on it.
        complete_next(&mut actor).await;
        assert_eq!(
            recovery.applied_voter_ids().await,
            BTreeSet::from([recovery.incarnation])
        );
        assert!(actor.retired_incarnations.contains(&leaving.incarnation));
        // The unreachable voter keeps its membership as a learner.
        assert!(
            recovery
                .applied_member_ids()
                .await
                .contains(&dead.incarnation)
        );
        leaving.stop().await;
        recovery.stop().await;
        host.close().await;
        leaving_primary.close().await;
        dead_primary.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn a_probe_reservation_wait_does_not_block_the_actor() {
    timeout(Duration::from_secs(60), async {
        let host = endpoint().await;
        let remote = endpoint().await;
        let seed = Invite::create(
            host.id(),
            test_relay(),
            "test-build".into(),
            now().unwrap(),
            3600,
        )
        .unwrap();
        let room = seed.room();
        let (events, _events_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(host.clone(), events);
        let invite = actor.setup_host_recovery(seed).await.unwrap();
        let recovery = actor.recovery.clone().unwrap();
        while recovery.coordinator.current_leader() != Some(recovery.incarnation) {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
        actor.epoch = 1;
        actor.room = Some(room);
        actor.hosted = Some(invite.clone());
        actor.room_invite = Some(invite);
        let source = crate::recovery::RecoverySession::join(
            room,
            remote.id(),
            recovery.incarnation,
            recovery.coordination_address.clone(),
            false,
        )
        .await
        .unwrap();
        let source_admission = source.advertise().await;
        recovery.add_learner(&source_admission).await.unwrap();
        actor.remember_admission(source_admission);
        actor.remember_admission(recovery.advertise().await);
        let state = recovery.state().await;
        // The native roster seats both members at table revision 5.
        let roster = serde_json::json!({
            "version": 1,
            "request": 1,
            "term": state.term,
            "base_revision": 0,
            "checkpoint": {
                "members": [
                    {"member": 1, "incarnation": source.incarnation,
                     "data": {"authenticatedEndpoint": remote.id().to_string()}},
                    {"member": 2, "incarnation": recovery.incarnation,
                     "data": {"authenticatedEndpoint": host.id().to_string()}}
                ],
                "room": {"version": 2, "snapshot": {
                    "members": [{"id": 1}, {"id": 2}],
                    "tables": [{"revision": 5, "p1": 1, "p2": 2}]
                }}
            }
        });
        let receipt = recovery
            .coordinator
            .propose(crate::coordination::Proposal {
                request: "roster".into(),
                dedup_id: "roster".into(),
                term: state.term,
                base: recovery.committed().await.revision,
                checkpoint: roster.to_string(),
                admin: None,
            })
            .await
            .unwrap();
        assert!(receipt.accepted);

        let expires = now().unwrap() + 60;
        let payload = serde_json::to_string(&CoordinationControl::ProbeReservation {
            room,
            source: remote.id(),
            source_incarnation: source.incarnation,
            target_incarnation: recovery.incarnation,
            request: 9,
            pair_revision: 5,
            term: state.term,
            expires,
        })
        .unwrap();
        // The reservation's Raft entry does not exist yet. The frame is
        // accepted at once instead of holding the actor tick for the wait.
        let started = std::time::Instant::now();
        assert!(
            actor
                .accept_coordination_control(remote.id(), &payload)
                .await
                .unwrap()
        );
        assert!(started.elapsed() < Duration::from_secs(2));
        assert!(actor.probe_permissions.is_empty());
        assert!(actor.pending_probe_reservations.contains_key(&remote.id()));

        recovery
            .reserve_probe(
                source.incarnation,
                recovery.incarnation,
                9,
                5,
                state.term,
                expires,
            )
            .await
            .unwrap();
        complete_next(&mut actor).await;
        assert!(actor.pending_probe_reservations.is_empty());
        assert!(actor.probe_permissions.contains_key(&remote.id()));
        assert!(actor.probe_peers.contains(&remote.id()));
        source.stop().await;
        recovery.stop().await;
        host.close().await;
        remote.close().await;
    })
    .await
    .unwrap();
}

/// One helper actor of a two-player room, driven through its command queue.
struct GameSide {
    commands: mpsc::Sender<Request>,
    events: mpsc::Receiver<Event>,
    socket: UdpSocket,
    generation: u64,
    _fault: watch::Sender<bool>,
    _service: TaskScope,
}

impl GameSide {
    /// `peer` is dialed through its bound address.
    async fn start(endpoint: &Endpoint, room: [u8; 16], peer: &Endpoint) -> Self {
        let (events_tx, events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (fault, failure) = watch::channel(false);
        let mut actor = test_actor(endpoint.clone(), events_tx);
        actor.epoch = 1;
        actor.room = Some(room);
        actor.host_address = Some(address(peer));
        let service = tokio::spawn(async move {
            let result = actor.run(command_rx, failure).await;
            actor.tasks.shutdown().await;
            result
        });
        Self {
            commands,
            events,
            socket: UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap(),
            generation: 1,
            _fault: fault,
            _service: TaskScope(vec![service.abort_handle()]),
        }
    }

    async fn prepare(&mut self, room: [u8; 16], peer: &Endpoint, capability: u8, dial: bool) {
        self.commands
            .send(Request {
                id: 1,
                command: Command::PrepareGame {
                    epoch: 1,
                    peer: peer.id(),
                    room,
                    generation: self.generation,
                    capability: [capability; 32],
                    local_port: self.socket.local_addr().unwrap().port(),
                    max_packet: 1024,
                    dial,
                },
            })
            .await
            .unwrap();
        let _ = next(&mut self.events, "game_waiting").await;
    }

    async fn ready(&mut self) -> u16 {
        match next(&mut self.events, "game_ready").await {
            Event::GameReady { virtual_port, .. } => virtual_port,
            _ => unreachable!(),
        }
    }

    /// End the current match. The next `prepare` is a rematch.
    async fn end_match(&mut self) {
        self.commands
            .send(Request {
                id: 2,
                command: Command::EndMatch {
                    epoch: 1,
                    generation: self.generation,
                },
            })
            .await
            .unwrap();
        self.generation += 1;
    }

    /// The `gameplay_prepare_failed` error, and the reason on the `game_closed` after it.
    async fn failure(&mut self) -> (Option<EndpointId>, String, Option<String>) {
        let mut error = None;
        loop {
            match self.events.recv().await.unwrap() {
                Event::Error {
                    code, peer, reason, ..
                } => {
                    assert_eq!(code, "gameplay_prepare_failed");
                    error = Some((peer, reason.unwrap_or_default()));
                }
                Event::GameClosed { reason, .. } if error.is_some() => {
                    let (peer, text) = error.unwrap();
                    return (peer, text, reason);
                }
                _ => (),
            }
        }
    }

    /// Nothing that ends or completes the slot arrives within `wait`.
    async fn stays_waiting(&mut self, wait: Duration, why: &str) {
        let _ = timeout(wait, async {
            while let Some(event) = self.events.recv().await {
                assert!(
                    !matches!(
                        event,
                        Event::GameClosed { .. } | Event::Error { .. } | Event::GameReady { .. }
                    ),
                    "{why}: {}",
                    serde_json::to_string(&event).unwrap()
                );
            }
        })
        .await;
    }
}

async fn game_connection_pair(from: &Endpoint, to: &Endpoint) -> (Connection, Connection) {
    let (dialed, accepted) = tokio::join!(from.connect(address(to), GAME_ALPN), async {
        to.accept().await.unwrap().await
    });
    (dialed.unwrap(), accepted.unwrap())
}

/// Both helpers prepare the current generation, the listener first so the dial
/// finds it, and datagrams must cross the link that forms.
async fn form_game_link(
    listener: &mut GameSide,
    dialer: &mut GameSide,
    room: [u8; 16],
    listener_endpoint: &Endpoint,
    dialer_endpoint: &Endpoint,
) {
    let started = tokio::time::Instant::now();
    listener.prepare(room, dialer_endpoint, 5, false).await;
    dialer.prepare(room, listener_endpoint, 5, true).await;
    let listener_port = listener.ready().await;
    dialer.ready().await;
    assert!(started.elapsed() < Duration::from_secs(5));
    // The bridge relays what the local game socket sends to its virtual port.
    let message = format!("link {}", listener.generation);
    listener
        .socket
        .send_to(message.as_bytes(), (Ipv4Addr::LOCALHOST, listener_port))
        .await
        .unwrap();
    let mut buffer = [0; 64];
    let count = dialer.socket.recv(&mut buffer).await.unwrap();
    assert_eq!(&buffer[..count], message.as_bytes());
}

/// The field bug: rematches failed after a connection check. A connection left
/// over from a check stays open beside the games, and neither the first game
/// nor the rematch depends on it.
#[tokio::test]
async fn a_stale_probe_connection_never_blocks_the_next_game() {
    timeout(Duration::from_secs(30), async {
        let (listener_endpoint, dialer_endpoint) = (endpoint().await, endpoint().await);
        let room = [81; 16];
        let (_dialer_end, _listener_end) =
            game_connection_pair(&dialer_endpoint, &listener_endpoint).await;
        let mut listener = GameSide::start(&listener_endpoint, room, &dialer_endpoint).await;
        let mut dialer = GameSide::start(&dialer_endpoint, room, &listener_endpoint).await;
        for _ in 0..2 {
            form_game_link(
                &mut listener,
                &mut dialer,
                room,
                &listener_endpoint,
                &dialer_endpoint,
            )
            .await;
            listener.end_match().await;
            dialer.end_match().await;
        }
        listener_endpoint.close().await;
        dialer_endpoint.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn failed_gameplay_link_reports_its_cause_and_what_the_listener_saw() {
    timeout(Duration::from_secs(20), async {
        let (listener_endpoint, dialer_endpoint) = (endpoint().await, endpoint().await);
        let room = [85; 16];
        let mut listener = GameSide::start(&listener_endpoint, room, &dialer_endpoint).await;
        let mut dialer = GameSide::start(&dialer_endpoint, room, &listener_endpoint).await;
        // The two sides disagree on the capability, so no link can form.
        listener.prepare(room, &dialer_endpoint, 5, false).await;
        dialer.prepare(room, &listener_endpoint, 6, true).await;
        let (peer, reason, closed) = dialer.failure().await;
        assert_eq!(peer, Some(listener_endpoint.id()));
        assert!(reason.contains("link failed"), "{reason}");
        assert_eq!(closed.as_deref(), Some(reason.as_str()));
        // The listener's rejected candidate does not end its slot, which keeps
        // waiting for a valid dial until its window or the match ends.
        listener
            .commands
            .send(Request {
                id: 2,
                command: Command::EndMatch {
                    epoch: 1,
                    generation: 1,
                },
            })
            .await
            .unwrap();
        let reason = loop {
            match listener.events.recv().await.unwrap() {
                Event::GameClosed { peer, reason, .. } => {
                    assert_eq!(peer, dialer_endpoint.id());
                    break reason.unwrap_or_default();
                }
                Event::Error { code, .. } => panic!("listener reported {code}"),
                _ => (),
            }
        };
        assert!(reason.contains("ended before gameplay link"), "{reason}");
        assert!(reason.contains("waiting=true"), "{reason}");
        assert!(reason.contains("refused_incoming="), "{reason}");
        assert!(reason.contains("rejected_candidates=1"), "{reason}");
        listener_endpoint.close().await;
        dialer_endpoint.close().await;
    })
    .await
    .unwrap();
}

/// A hosting actor with one remote member seated at table revision 5, the
/// setup a probe reservation from that member needs. The remote's controls are
/// real workers so their ids differ.
struct ReservationFixture {
    host: Endpoint,
    remote: Endpoint,
    actor: Actor,
    events: mpsc::Receiver<Event>,
    recovery: crate::recovery::RecoverySession,
    source: crate::recovery::RecoverySession,
    invite: Invite,
    room: [u8; 16],
    term: u64,
    expires: u64,
}

impl ReservationFixture {
    async fn start() -> Self {
        let host = endpoint().await;
        let remote = endpoint().await;
        let seed = Invite::create(
            host.id(),
            test_relay(),
            "test-build".into(),
            now().unwrap(),
            3600,
        )
        .unwrap();
        let room = seed.room();
        let (events_tx, events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut actor = test_actor(host.clone(), events_tx);
        let invite = actor.setup_host_recovery(seed).await.unwrap();
        let recovery = actor.recovery.clone().unwrap();
        while recovery.coordinator.current_leader() != Some(recovery.incarnation) {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
        actor.epoch = 1;
        actor.room = Some(room);
        actor.hosted = Some(invite.clone());
        actor.room_invite = Some(invite.clone());
        let source = crate::recovery::RecoverySession::join(
            room,
            remote.id(),
            recovery.incarnation,
            recovery.coordination_address.clone(),
            false,
        )
        .await
        .unwrap();
        let source_admission = source.advertise().await;
        recovery.add_learner(&source_admission).await.unwrap();
        actor.remember_admission(source_admission);
        actor.remember_admission(recovery.advertise().await);
        let state = recovery.state().await;
        let roster = serde_json::json!({
            "version": 1,
            "request": 1,
            "term": state.term,
            "base_revision": 0,
            "checkpoint": {
                "members": [
                    {"member": 1, "incarnation": source.incarnation,
                     "data": {"authenticatedEndpoint": remote.id().to_string()}},
                    {"member": 2, "incarnation": recovery.incarnation,
                     "data": {"authenticatedEndpoint": host.id().to_string()}}
                ],
                "room": {"version": 2, "snapshot": {
                    "members": [{"id": 1}, {"id": 2}],
                    "tables": [{"revision": 5, "p1": 1, "p2": 2}]
                }}
            }
        });
        let receipt = recovery
            .coordinator
            .propose(crate::coordination::Proposal {
                request: "roster".into(),
                dedup_id: "roster".into(),
                term: state.term,
                base: recovery.committed().await.revision,
                checkpoint: roster.to_string(),
                admin: None,
            })
            .await
            .unwrap();
        assert!(receipt.accepted);
        Self {
            host,
            remote,
            actor,
            events,
            recovery,
            source,
            invite,
            room,
            term: state.term,
            expires: now().unwrap() + 60,
        }
    }

    /// Install a fresh control from the remote, replacing any current one.
    /// Returns its id.
    async fn connect_control(&mut self) -> u64 {
        let (_remote_side, accepted) = tokio::join!(
            async {
                let connection = self
                    .remote
                    .connect(address(&self.host), CONTROL_ALPN)
                    .await
                    .unwrap();
                transport::connect_control_on(connection, &self.invite)
                    .await
                    .unwrap()
            },
            async {
                let connection = self.host.accept().await.unwrap().await.unwrap();
                transport::accept_control(connection, &self.invite)
                    .await
                    .unwrap()
            }
        );
        let worker = ControlWorker::start(accepted);
        let id = worker.id();
        self.actor.remove_control(self.remote.id());
        self.actor.controls.insert(self.remote.id(), worker);
        id
    }

    fn frame(&self, request: u64) -> String {
        serde_json::to_string(&CoordinationControl::ProbeReservation {
            room: self.room,
            source: self.remote.id(),
            source_incarnation: self.source.incarnation,
            target_incarnation: self.recovery.incarnation,
            request,
            pair_revision: 5,
            term: self.term,
            expires: self.expires,
        })
        .unwrap()
    }

    async fn reserve(&self, request: u64) {
        self.recovery
            .reserve_probe(
                self.source.incarnation,
                self.recovery.incarnation,
                request,
                5,
                self.term,
                self.expires,
            )
            .await
            .unwrap();
    }

    fn no_control_closed(&mut self) {
        while let Ok(event) = self.events.try_recv() {
            assert!(
                !matches!(event, Event::ControlClosed { .. }),
                "an expired reservation closed a control it was not presented on"
            );
        }
    }

    async fn stop(mut self) {
        self.actor.tasks.abort_all();
        self.source.stop().await;
        self.recovery.stop().await;
        self.host.close().await;
        self.remote.close().await;
    }
}

/// Seconds the checked PC's wall clock is set ahead of the checking peer's
/// (negative: behind). Each is a different PC pair.
const PEER_CLOCK_OFFSETS: [i64; 11] = [0, 5, 15, 19, 21, 30, 60, 300, 3600, -60, -3600];

/// The field report: two PCs whose clocks differ could not check the
/// connection between them. The checking peer stamps its reservation on its
/// own clock; the checked PC, its clock set ahead or behind, accepts the
/// frame whatever the difference, and the permission lasts the same span on
/// its monotonic clock.
#[tokio::test]
async fn a_reservation_is_accepted_whatever_the_checked_pcs_clock_says() {
    timeout(Duration::from_secs(90), async {
        let mut fixture = ReservationFixture::start().await;
        let remote = fixture.remote.id();
        let mut refused = Vec::new();
        let mut lifetimes = Vec::new();
        for (index, offset) in PEER_CLOCK_OFFSETS.into_iter().enumerate() {
            let request = 20 + index as u64;
            fixture.expires = now().unwrap() + crate::probe::window(false).as_secs();
            fixture.reserve(request).await;
            let frame = fixture.frame(request);
            let accepted = WALL_CLOCK_OFFSET
                .scope(
                    offset,
                    fixture.actor.accept_coordination_control(remote, &frame),
                )
                .await;
            if !matches!(accepted, Ok(true)) {
                refused.push(offset);
                continue;
            }
            let permission = fixture
                .actor
                .probe_permissions
                .get(&remote)
                .expect("an accepted reservation installs its permission");
            assert_eq!(permission.request, request);
            let lifetime = permission
                .expires
                .saturating_duration_since(tokio::time::Instant::now());
            let full = crate::probe::window(true);
            if lifetime > full || lifetime + Duration::from_secs(2) < full {
                lifetimes.push((offset, lifetime.as_secs()));
            }
        }
        assert!(
            refused.is_empty() && lifetimes.is_empty(),
            "refused with the clock ahead by {refused:?} s; \
             permission lifetimes off the window (offset, s): {lifetimes:?}"
        );
        fixture.stop().await;
    })
    .await
    .unwrap();
}

/// The check itself, end to end on the checked side: its clock is half a
/// minute ahead of the checking peer's, and the peer's measurement completes
/// with samples instead of timing out unanswered.
#[tokio::test]
async fn a_check_of_a_pc_whose_clock_is_ahead_completes() {
    timeout(Duration::from_secs(60), async {
        let mut fixture = ReservationFixture::start().await;
        let remote = fixture.remote.id();
        fixture.expires = now().unwrap() + crate::probe::window(false).as_secs();
        fixture.reserve(7).await;
        let frame = fixture.frame(7);
        let accepted = WALL_CLOCK_OFFSET
            .scope(
                30,
                fixture.actor.accept_coordination_control(remote, &frame),
            )
            .await;
        let ReservationFixture {
            host,
            remote: checker,
            actor,
            events,
            recovery,
            source,
            room,
            ..
        } = fixture;
        let (_commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let (_fault, failure) = watch::channel(false);
        let mut actor = actor;
        let service = tokio::spawn(WALL_CLOCK_OFFSET.scope(30, async move {
            let result = actor.run(command_rx, failure).await;
            actor.tasks.shutdown().await;
            result
        }));
        let checked = run_probe(
            checker.clone(),
            address(&host),
            room,
            host.id(),
            7,
            5,
            false,
        )
        .await;
        service.abort();
        drop(events);
        let completion = checked.unwrap_or_else(|error| {
            panic!("the check failed ({error}); the reservation was answered {accepted:?}")
        });
        let summary = crate::recovery::summarize_datagram_probe(
            &completion.samples_us,
            completion.metrics.expected,
            completion.metrics.sent,
        );
        assert_eq!(summary.status, "ready");
        assert!(
            summary.sample_count >= 80,
            "{} samples",
            summary.sample_count
        );
        if let Some(connection) = completion.connection {
            connection.close(0u32.into(), b"done");
        }
        source.stop().await;
        recovery.stop().await;
        host.close().await;
        checker.close().await;
    })
    .await
    .unwrap();
}

/// This PC's wall clock is corrected (or set by hand) while its own check
/// waits for the room to commit the reservation. Whichever way it jumps, the
/// check goes ahead: how long the authorization stays usable is counted on
/// the monotonic clock from when it was requested.
#[tokio::test]
async fn a_wall_clock_jump_during_a_check_changes_nothing() {
    timeout(Duration::from_secs(90), async {
        for (request, jump) in [(3, 3600i64), (4, 30), (5, -3600)] {
            let mut fixture = ReservationFixture::start().await;
            fixture.connect_control().await;
            let (room, remote) = (fixture.room, fixture.remote.id());
            fixture
                .actor
                .spawn_probe(1, room, remote, request, 5, false)
                .await
                .unwrap();
            WALL_CLOCK_OFFSET
                .scope(jump, complete_next(&mut fixture.actor))
                .await;
            while let Ok(event) = fixture.events.try_recv() {
                assert!(
                    !matches!(event, Event::Error { .. }),
                    "a {jump} s jump refused the check: {}",
                    serde_json::to_string(&event).unwrap()
                );
            }
            assert!(
                fixture.actor.own_probes.contains_key(&remote),
                "a {jump} s jump dropped the check"
            );
            fixture.stop().await;
        }
    })
    .await
    .unwrap();
}

/// The monotonic deadline still holds: an authorization that completes after
/// its window, with the wall clock saying it is fresh, is refused.
#[tokio::test]
async fn a_late_probe_authorization_is_refused_by_the_monotonic_clock() {
    timeout(Duration::from_secs(60), async {
        let mut fixture = ReservationFixture::start().await;
        fixture.connect_control().await;
        let (room, remote) = (fixture.room, fixture.remote.id());
        fixture
            .actor
            .spawn_probe(1, room, remote, 3, 5, false)
            .await
            .unwrap();
        let completion = timeout(Duration::from_secs(10), fixture.actor.tasks.join_next())
            .await
            .expect("worker completion")
            .expect("a worker")
            .expect("worker result");
        let Completion::ProbeAuthorization(key, Ok(mut authorization)) = completion else {
            panic!("the authorization was not committed");
        };
        authorization.deadline = tokio::time::Instant::now();
        let task_count = fixture.actor.tasks.len();
        WALL_CLOCK_OFFSET
            .scope(
                -3600,
                fixture
                    .actor
                    .completed(Completion::ProbeAuthorization(key, Ok(authorization))),
            )
            .await
            .unwrap();
        assert!(!fixture.actor.own_probes.contains_key(&remote));
        assert_eq!(fixture.actor.tasks.len(), task_count);
        fixture.stop().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn an_expired_reservation_wait_leaves_a_replacement_control_open() {
    timeout(Duration::from_secs(60), async {
        let mut fixture = ReservationFixture::start().await;
        let original = fixture.connect_control().await;
        // The reservation's Raft entry never arrives, so its wait is still
        // pending when the control that presented it is replaced.
        let payload = fixture.frame(9);
        let remote = fixture.remote.id();
        assert!(
            fixture
                .actor
                .accept_coordination_control(remote, &payload)
                .await
                .unwrap()
        );
        assert_eq!(
            fixture
                .actor
                .pending_probe_reservations
                .get(&remote)
                .map(|key| key.control),
            Some(original)
        );
        let replacement = fixture.connect_control().await;
        assert_ne!(replacement, original);
        // The wait expires unapplied and settles against the old control's id.
        complete_next(&mut fixture.actor).await;
        assert!(fixture.actor.pending_probe_reservations.is_empty());
        assert_eq!(
            fixture.actor.controls.get(&remote).map(ControlWorker::id),
            Some(replacement)
        );
        assert!(fixture.actor.probe_permissions.is_empty());
        fixture.no_control_closed();
        fixture.stop().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn an_expired_reservation_wait_still_closes_the_control_that_presented_it() {
    timeout(Duration::from_secs(60), async {
        let mut fixture = ReservationFixture::start().await;
        let control = fixture.connect_control().await;
        let payload = fixture.frame(9);
        let remote = fixture.remote.id();
        assert!(
            fixture
                .actor
                .accept_coordination_control(remote, &payload)
                .await
                .unwrap()
        );
        complete_next(&mut fixture.actor).await;
        assert!(!fixture.actor.controls.contains_key(&remote));
        assert!(matches!(
            next(&mut fixture.events, "control_closed").await,
            Event::ControlClosed { control: closed, .. } if closed == control
        ));
        fixture.stop().await;
    })
    .await
    .unwrap();
}

impl ReservationFixture {
    /// Commits the native roster with both fighters seated at table
    /// `revision`, as the room's next checkpoint.
    async fn seat_pair(&self, revision: u64) {
        let roster = serde_json::json!({
            "version": 1,
            "request": revision,
            "term": self.term,
            "base_revision": 0,
            "checkpoint": {
                "members": [
                    {"member": 1, "incarnation": self.source.incarnation,
                     "data": {"authenticatedEndpoint": self.remote.id().to_string()}},
                    {"member": 2, "incarnation": self.recovery.incarnation,
                     "data": {"authenticatedEndpoint": self.host.id().to_string()}}
                ],
                "room": {"version": 2, "snapshot": {
                    "members": [{"id": 1}, {"id": 2}],
                    "tables": [{"revision": revision, "p1": 1, "p2": 2}]
                }}
            }
        });
        let receipt = self
            .recovery
            .coordinator
            .propose(crate::coordination::Proposal {
                request: format!("roster:{revision}"),
                dedup_id: format!("roster:{revision}"),
                term: self.term,
                base: self.recovery.committed().await.revision,
                checkpoint: roster.to_string(),
                admin: None,
            })
            .await
            .unwrap();
        assert!(receipt.accepted);
    }

    /// A reservation for the pair as seated at table `revision`, committed.
    async fn reserve_at(&self, request: u64, revision: u64) -> String {
        self.recovery
            .reserve_probe(
                self.source.incarnation,
                self.recovery.incarnation,
                request,
                revision,
                self.term,
                self.expires,
            )
            .await
            .unwrap();
        serde_json::to_string(&CoordinationControl::ProbeReservation {
            room: self.room,
            source: self.remote.id(),
            source_incarnation: self.source.incarnation,
            target_incarnation: self.recovery.incarnation,
            request,
            pair_revision: revision,
            term: self.term,
            expires: self.expires,
        })
        .unwrap()
    }
}

/// A check asked for the moment both seats fill: the source saw the seats
/// committed on the leader, but this replica has not applied them when the
/// reservation arrives. The frame waits for them instead of closing the room
/// control, and the permission follows once the seats apply.
#[tokio::test]
async fn a_reservation_ahead_of_the_seat_commit_waits_for_it() {
    timeout(Duration::from_secs(60), async {
        let mut fixture = ReservationFixture::start().await;
        let control = fixture.connect_control().await;
        let remote = fixture.remote.id();
        // The fixture seats the pair at table revision 5; the source checks
        // the pair as seated at revision 6, which this replica lacks.
        let frame = fixture.reserve_at(11, 6).await;
        assert!(
            fixture
                .actor
                .accept_coordination_control(remote, &frame)
                .await
                .unwrap()
        );
        assert!(fixture.actor.probe_permissions.is_empty());
        assert!(
            fixture
                .actor
                .pending_probe_reservations
                .contains_key(&remote)
        );
        fixture.seat_pair(6).await;
        complete_next(&mut fixture.actor).await;
        assert!(fixture.actor.pending_probe_reservations.is_empty());
        assert_eq!(
            fixture
                .actor
                .probe_permissions
                .get(&remote)
                .map(|permission| permission.request),
            Some(11)
        );
        assert_eq!(
            fixture.actor.controls.get(&remote).map(ControlWorker::id),
            Some(control)
        );
        fixture.no_control_closed();
        fixture.stop().await;
    })
    .await
    .unwrap();
}

/// A pair that is still not seated when the wait runs out is refused as
/// before: the control that presented the reservation closes.
#[tokio::test]
async fn a_reservation_for_a_pair_never_seated_closes_its_control() {
    timeout(Duration::from_secs(60), async {
        let mut fixture = ReservationFixture::start().await;
        let control = fixture.connect_control().await;
        let remote = fixture.remote.id();
        let frame = fixture.reserve_at(12, 7).await;
        assert!(
            fixture
                .actor
                .accept_coordination_control(remote, &frame)
                .await
                .unwrap()
        );
        complete_next(&mut fixture.actor).await;
        assert!(fixture.actor.probe_permissions.is_empty());
        assert!(!fixture.actor.controls.contains_key(&remote));
        assert!(matches!(
            next(&mut fixture.events, "control_closed").await,
            Event::ControlClosed { control: closed, .. } if closed == control
        ));
        fixture.stop().await;
    })
    .await
    .unwrap();
}

/// In a server-owned room the host relays the reservation. One for a pair
/// this member never sees seated is dropped when the wait runs out: no
/// control closes, the host's least of all.
#[tokio::test]
async fn a_relayed_reservation_for_a_pair_never_seated_is_dropped() {
    timeout(Duration::from_secs(60), async {
        let mut fixture = ReservationFixture::start().await;
        let source_control = fixture.connect_control().await;
        let remote = fixture.remote.id();
        let relay_host = endpoint().await;
        fixture.actor.public = Some(crate::public_room::PublicRoom::Member {
            ticket: public_support::ticket_for(
                &fixture.invite,
                &public_support::identity(1),
                &fixture.host,
            ),
        });
        fixture.actor.host_address = Some(address(&relay_host));
        let frame = fixture.reserve_at(13, 7).await;
        assert!(
            fixture
                .actor
                .accept_coordination_control(relay_host.id(), &frame)
                .await
                .unwrap()
        );
        assert!(
            fixture
                .actor
                .pending_probe_reservations
                .contains_key(&remote)
        );
        complete_next(&mut fixture.actor).await;
        assert!(fixture.actor.pending_probe_reservations.is_empty());
        assert!(fixture.actor.probe_permissions.is_empty());
        assert_eq!(
            fixture.actor.controls.get(&remote).map(ControlWorker::id),
            Some(source_control)
        );
        fixture.no_control_closed();
        relay_host.close().await;
        fixture.stop().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn a_newer_applied_reservation_supersedes_an_older_pending_wait() {
    timeout(Duration::from_secs(60), async {
        let mut fixture = ReservationFixture::start().await;
        let control = fixture.connect_control().await;
        let remote = fixture.remote.id();
        // Request 9 is not applied and waits on a task.
        let older = fixture.frame(9);
        assert!(
            fixture
                .actor
                .accept_coordination_control(remote, &older)
                .await
                .unwrap()
        );
        assert!(
            fixture
                .actor
                .pending_probe_reservations
                .contains_key(&remote)
        );
        // Request 10 is already applied and takes the fast path.
        fixture.reserve(10).await;
        let newer = fixture.frame(10);
        assert!(
            fixture
                .actor
                .accept_coordination_control(remote, &newer)
                .await
                .unwrap()
        );
        assert!(fixture.actor.pending_probe_reservations.is_empty());
        assert_eq!(
            fixture
                .actor
                .probe_permissions
                .get(&remote)
                .map(|permission| permission.request),
            Some(10)
        );
        // The older wait expires unapplied and changes nothing.
        complete_next(&mut fixture.actor).await;
        assert_eq!(
            fixture.actor.controls.get(&remote).map(ControlWorker::id),
            Some(control)
        );
        assert_eq!(
            fixture
                .actor
                .probe_permissions
                .get(&remote)
                .map(|permission| permission.request),
            Some(10)
        );
        fixture.no_control_closed();
        fixture.stop().await;
    })
    .await
    .unwrap();
}

/// The dialer's half of a handshake on `dialer_end`, held open without confirming.
async fn unconfirmed_dial(
    dialer_end: &Connection,
    listener: &Endpoint,
    room: [u8; 16],
) -> (transport::GameDialSend, GameAuthorization) {
    let auth = GameAuthorization {
        peer: listener.id(),
        key: MatchKey {
            room,
            generation: 1,
        },
        capability: [5; 32],
        max_packet: 1024,
    };
    let send = transport::connect_game_unconfirmed(dialer_end, &auth)
        .await
        .unwrap();
    (send, auth)
}

#[tokio::test]
async fn accept_completes_only_after_the_dialer_confirms() {
    timeout(Duration::from_secs(20), async {
        let (listener_endpoint, dialer_endpoint) = (endpoint().await, endpoint().await);
        let auth = |peer: &Endpoint| GameAuthorization {
            peer: peer.id(),
            key: MatchKey {
                room: [87; 16],
                generation: 1,
            },
            capability: [5; 32],
            max_packet: 1024,
        };

        // The reply reaches the dialer, which then finishes its stream.
        let (dialer_end, listener_end) =
            game_connection_pair(&dialer_endpoint, &listener_endpoint).await;
        let mut accepted =
            tokio::spawn(transport::accept_game(listener_end, auth(&dialer_endpoint)));
        let (mut send, _) = unconfirmed_dial(&dialer_end, &listener_endpoint, [87; 16]).await;
        assert!(
            timeout(Duration::from_millis(300), &mut accepted)
                .await
                .is_err(),
            "the listener committed before the dialer confirmed"
        );
        send.confirm().await.unwrap();
        assert!(accepted.await.unwrap().is_ok());

        // The dialer stops waiting and closes instead of confirming.
        let (dialer_end, listener_end) =
            game_connection_pair(&dialer_endpoint, &listener_endpoint).await;
        let accepted = tokio::spawn(transport::accept_game(listener_end, auth(&dialer_endpoint)));
        let (send, _) = unconfirmed_dial(&dialer_end, &listener_endpoint, [87; 16]).await;
        dialer_end.close(1u32.into(), b"dial abandoned");
        drop(send);
        assert!(accepted.await.unwrap().is_err());
        listener_endpoint.close().await;
        dialer_endpoint.close().await;
    })
    .await
    .unwrap();
}

/// A dial that has read the listener's reply and then goes away never
/// confirms. Dropping its send stream must reset it rather than finish it, or
/// the listener would take the abandoned connection for the link. The slot
/// keeps waiting, and a later valid dial forms the link.
#[tokio::test]
async fn an_abandoned_dial_is_never_confirmed_and_the_slot_keeps_waiting() {
    timeout(Duration::from_secs(20), async {
        let (listener_endpoint, dialer_endpoint) = (endpoint().await, endpoint().await);
        let room = [88; 16];
        let mut listener = GameSide::start(&listener_endpoint, room, &dialer_endpoint).await;
        listener.prepare(room, &dialer_endpoint, 5, false).await;
        let abandoned = dialer_endpoint
            .connect(address(&listener_endpoint), GAME_ALPN)
            .await
            .unwrap();
        let (send, auth) = unconfirmed_dial(&abandoned, &listener_endpoint, room).await;
        drop(send);
        listener
            .stays_waiting(
                Duration::from_millis(700),
                "an abandoned dial ended the slot",
            )
            .await;
        let game = transport::connect_game(&dialer_endpoint, address(&listener_endpoint), auth)
            .await
            .expect("the valid dial must still be accepted");
        listener.ready().await;
        assert!(game.connection.close_reason().is_none());
        listener_endpoint.close().await;
        dialer_endpoint.close().await;
    })
    .await
    .unwrap();
}

/// A departure the peer flushed and closed behind, queued after more frames
/// than one poll delivers. The worker's transport is closed when the first
/// poll ends, but the departure was accepted by the reader and must still be
/// handed to the native room, ahead of the control's close. With
/// `replacement`, a new control from the same endpoint arrives between poll
/// budgets: the old frames still come first, under the old control, and the
/// replacement's traffic follows its own `Connected`.
async fn flushed_departure_behind_more_than_one_poll_budget(replacement: bool) {
    fn drain(events: &mut mpsc::Receiver<Event>, order: &mut Vec<String>) {
        while let Ok(event) = events.try_recv() {
            match event {
                Event::Message { payload, .. } => order.push(format!("message {payload}")),
                Event::PeerDeparted { .. } => order.push("departed".into()),
                Event::ControlClosed { control, .. } => order.push(format!("closed {control}")),
                Event::Connected { control, .. } => order.push(format!("connected {control}")),
                _ => (),
            }
        }
    }
    timeout(Duration::from_secs(60), async {
        let mut fixture = ReservationFixture::start().await;
        let remote_id = fixture.remote.id();
        let (remote_side, accepted) = tokio::join!(
            async {
                let connection = fixture
                    .remote
                    .connect(address(&fixture.host), CONTROL_ALPN)
                    .await
                    .unwrap();
                transport::connect_control_on(connection, &fixture.invite)
                    .await
                    .unwrap()
            },
            async {
                let connection = fixture.host.accept().await.unwrap().await.unwrap();
                transport::accept_control(connection, &fixture.invite)
                    .await
                    .unwrap()
            }
        );
        fixture
            .actor
            .controls
            .insert(remote_id, ControlWorker::start(accepted));

        let ordinary = CONTROL_POLL_BUDGET * 2 + 5;
        let mut remote_worker = ControlWorker::start(remote_side);
        for id in 0..ordinary {
            remote_worker
                .try_send(ControlFrame {
                    message_id: TRANSPORT_MESSAGE_ID_BASE + id as u64,
                    payload: format!("ordinary {id}").into_bytes(),
                })
                .unwrap();
        }
        remote_worker
            .try_send(ControlFrame {
                message_id: TRANSPORT_MESSAGE_ID_BASE + ordinary as u64,
                payload: serde_json::to_vec(&CoordinationControl::Departure {
                    room: fixture.room,
                    incarnation: fixture.source.incarnation,
                })
                .unwrap(),
            })
            .unwrap();
        // Flush and close as a leaving peer does. The actor has not polled.
        remote_worker.finish();
        remote_worker
            .finished(tokio::time::Instant::now() + Duration::from_secs(10))
            .await;
        drop(remote_worker);
        timeout(Duration::from_secs(10), async {
            while !fixture.actor.controls[&remote_id].is_closed() {
                tokio::time::sleep(Duration::from_millis(10)).await;
            }
        })
        .await
        .unwrap();
        // Let the reader queue what the closed stream still holds.
        tokio::time::sleep(Duration::from_millis(300)).await;

        let mut order: Vec<String> = Vec::new();
        // One poll delivers one budget of the queued frames.
        fixture.actor.poll_controls().await.unwrap();
        drain(&mut fixture.events, &mut order);
        assert!(!order.iter().any(|entry| entry == "departed"));

        let (mut replacement_worker, mut replacement_id) = (None, 0);
        if replacement {
            let (replacement_side, accepted) = tokio::join!(
                async {
                    let connection = fixture
                        .remote
                        .connect(address(&fixture.host), CONTROL_ALPN)
                        .await
                        .unwrap();
                    transport::connect_control_on(connection, &fixture.invite)
                        .await
                        .unwrap()
                },
                async {
                    let connection = fixture.host.accept().await.unwrap().await.unwrap();
                    transport::accept_control(connection, &fixture.invite)
                        .await
                        .unwrap()
                }
            );
            replacement_id = accepted.connection.stable_id() as u64;
            let epoch = fixture.actor.epoch;
            fixture
                .actor
                .completed_control(epoch, Ok(accepted), None)
                .await
                .unwrap();
            assert!(
                fixture.actor.parked_controls.contains_key(&remote_id),
                "the replacement started while the old control still held frames"
            );
            drain(&mut fixture.events, &mut order);
            assert!(!order.iter().any(|entry| entry.starts_with("connected")));
            let worker = ControlWorker::start(replacement_side);
            worker
                .try_send(ControlFrame {
                    message_id: TRANSPORT_MESSAGE_ID_BASE + 1000,
                    payload: b"replacement frame".to_vec(),
                })
                .unwrap();
            replacement_worker = Some(worker);
        }

        let last = if replacement {
            "message replacement frame".to_string()
        } else {
            format!("closed {}", fixture.actor.controls[&remote_id].id())
        };
        for _ in 0..400 {
            fixture.actor.poll_controls().await.unwrap();
            drain(&mut fixture.events, &mut order);
            if order.contains(&last) {
                break;
            }
            tokio::time::sleep(Duration::from_millis(5)).await;
        }
        let position = |wanted: &str| order.iter().position(|entry| entry == wanted);
        let departed = position("departed").expect("the queued departure was discarded");
        let ordinary_delivered = order
            .iter()
            .filter(|entry| entry.starts_with("message ordinary"))
            .count();
        assert_eq!(ordinary_delivered, ordinary);
        assert!(
            order
                .iter()
                .rposition(|entry| entry.starts_with("message ordinary"))
                .unwrap()
                < departed,
            "{order:?}"
        );
        let end = position(&last).expect("nothing followed the departure");
        assert!(end > departed, "{order:?}");
        if replacement {
            assert_eq!(
                position(&format!("connected {replacement_id}")).expect("replacement connected"),
                end - 1,
                "{order:?}"
            );
            assert!(
                !order.iter().any(|entry| entry.starts_with("closed")),
                "a superseded control is replaced without a close event: {order:?}"
            );
            assert!(fixture.actor.parked_controls.is_empty());
            assert_eq!(
                fixture.actor.controls[&remote_id].id(),
                replacement_id,
                "the replacement is the control now"
            );
        }
        drop(replacement_worker);
        fixture.stop().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn a_flushed_departure_behind_more_than_one_poll_budget_is_delivered() {
    flushed_departure_behind_more_than_one_poll_budget(false).await;
}

#[tokio::test]
async fn a_replacement_control_waits_for_the_frames_the_old_one_received() {
    flushed_departure_behind_more_than_one_poll_budget(true).await;
}

/// A parked replacement never outlives the worker it waited behind. Worker A
/// holds queued frames, replacement B parks behind it, A is drained without an
/// actor poll, and replacement C then completes. C is installed and B is
/// closed, so B's drain deadline cannot later replace the newer C.
#[tokio::test]
async fn a_parked_replacement_cannot_evict_a_newer_installed_control() {
    timeout(Duration::from_secs(60), async {
        let mut fixture = ReservationFixture::start().await;
        let remote_id = fixture.remote.id();
        let epoch = fixture.actor.epoch;

        let (remote_a, accepted_a) = tokio::join!(
            async {
                let connection = fixture
                    .remote
                    .connect(address(&fixture.host), CONTROL_ALPN)
                    .await
                    .unwrap();
                transport::connect_control_on(connection, &fixture.invite)
                    .await
                    .unwrap()
            },
            async {
                let connection = fixture.host.accept().await.unwrap().await.unwrap();
                transport::accept_control(connection, &fixture.invite)
                    .await
                    .unwrap()
            }
        );
        fixture
            .actor
            .controls
            .insert(remote_id, ControlWorker::start(accepted_a));
        let a_id = fixture.actor.controls[&remote_id].id();

        // A queues a few frames, then its peer closes. The actor has not polled.
        let mut remote_a_worker = ControlWorker::start(remote_a);
        for id in 0..3 {
            remote_a_worker
                .try_send(ControlFrame {
                    message_id: TRANSPORT_MESSAGE_ID_BASE + id,
                    payload: format!("old {id}").into_bytes(),
                })
                .unwrap();
        }
        remote_a_worker.finish();
        remote_a_worker
            .finished(tokio::time::Instant::now() + Duration::from_secs(10))
            .await;
        drop(remote_a_worker);
        timeout(Duration::from_secs(10), async {
            while !fixture.actor.controls[&remote_id].is_closed() {
                tokio::time::sleep(Duration::from_millis(10)).await;
            }
        })
        .await
        .unwrap();
        tokio::time::sleep(Duration::from_millis(300)).await;
        assert!(
            !fixture.actor.controls[&remote_id].is_drained(),
            "A still holds the frames it received"
        );

        // B arrives while A holds frames and parks behind it.
        let (remote_b, accepted_b) = tokio::join!(
            async {
                let connection = fixture
                    .remote
                    .connect(address(&fixture.host), CONTROL_ALPN)
                    .await
                    .unwrap();
                transport::connect_control_on(connection, &fixture.invite)
                    .await
                    .unwrap()
            },
            async {
                let connection = fixture.host.accept().await.unwrap().await.unwrap();
                transport::accept_control(connection, &fixture.invite)
                    .await
                    .unwrap()
            }
        );
        let b_connection = accepted_b.connection.clone();
        fixture
            .actor
            .completed_control(epoch, Ok(accepted_b), None)
            .await
            .unwrap();
        assert!(fixture.actor.parked_controls.contains_key(&remote_id));

        // A finishes draining without the actor polling.
        while fixture
            .actor
            .controls
            .get_mut(&remote_id)
            .unwrap()
            .try_receive()
            .is_some()
        {}
        timeout(Duration::from_secs(10), async {
            while !fixture.actor.controls[&remote_id].is_drained() {
                tokio::time::sleep(Duration::from_millis(10)).await;
                while fixture
                    .actor
                    .controls
                    .get_mut(&remote_id)
                    .unwrap()
                    .try_receive()
                    .is_some()
                {}
            }
        })
        .await
        .unwrap();

        // C completes before the next control poll.
        let (remote_c, accepted_c) = tokio::join!(
            async {
                let connection = fixture
                    .remote
                    .connect(address(&fixture.host), CONTROL_ALPN)
                    .await
                    .unwrap();
                transport::connect_control_on(connection, &fixture.invite)
                    .await
                    .unwrap()
            },
            async {
                let connection = fixture.host.accept().await.unwrap().await.unwrap();
                transport::accept_control(connection, &fixture.invite)
                    .await
                    .unwrap()
            }
        );
        let c_id = accepted_c.connection.stable_id() as u64;
        fixture
            .actor
            .completed_control(epoch, Ok(accepted_c), None)
            .await
            .unwrap();
        assert_ne!(c_id, a_id);
        assert_eq!(
            fixture.actor.controls[&remote_id].id(),
            c_id,
            "C replaced the drained A"
        );
        assert!(
            fixture.actor.parked_controls.is_empty(),
            "the older parked B was superseded by C"
        );
        timeout(Duration::from_secs(10), async {
            while b_connection.close_reason().is_none() {
                tokio::time::sleep(Duration::from_millis(10)).await;
            }
        })
        .await
        .expect("B was closed when C superseded it");

        // With B gone there is nothing left to time out and replace C.
        tokio::time::sleep(Duration::from_millis(50)).await;
        fixture.actor.poll_controls().await.unwrap();
        assert_eq!(fixture.actor.controls[&remote_id].id(), c_id);
        assert!(fixture.actor.parked_controls.is_empty());

        drop((remote_b, remote_c));
        fixture.stop().await;
    })
    .await
    .unwrap();
}

/// Settling fences a parked candidate by the worker it waits behind: a worker
/// installed for the peer after the candidate parked is never replaced by it.
#[tokio::test]
async fn a_parked_candidate_is_dropped_when_a_newer_worker_stands() {
    timeout(Duration::from_secs(60), async {
        let mut fixture = ReservationFixture::start().await;
        let remote_id = fixture.remote.id();
        let epoch = fixture.actor.epoch;
        let mut pair = Vec::new();
        for _ in 0..3 {
            let (remote_side, accepted) = tokio::join!(
                async {
                    let connection = fixture
                        .remote
                        .connect(address(&fixture.host), CONTROL_ALPN)
                        .await
                        .unwrap();
                    transport::connect_control_on(connection, &fixture.invite)
                        .await
                        .unwrap()
                },
                async {
                    let connection = fixture.host.accept().await.unwrap().await.unwrap();
                    transport::accept_control(connection, &fixture.invite)
                        .await
                        .unwrap()
                }
            );
            pair.push((remote_side, accepted));
        }
        let (remote_c, accepted_c) = pair.pop().unwrap();
        let (remote_b, accepted_b) = pair.pop().unwrap();
        let (remote_a, accepted_a) = pair.pop().unwrap();
        fixture
            .actor
            .controls
            .insert(remote_id, ControlWorker::start(accepted_a));
        let a_id = fixture.actor.controls[&remote_id].id();
        // B is parked behind A, then a different worker is put in place
        // without going through the replacement path.
        let b_connection = accepted_b.connection.clone();
        fixture.actor.parked_controls.insert(
            remote_id,
            ParkedControl {
                epoch,
                channel: accepted_b,
                joined_invite: None,
                behind: Some(a_id),
                since: tokio::time::Instant::now() - CONTROL_REPLACE_DRAIN_LIMIT * 2,
            },
        );
        let c_id = accepted_c.connection.stable_id() as u64;
        fixture
            .actor
            .controls
            .insert(remote_id, ControlWorker::start(accepted_c));
        assert_eq!(fixture.actor.controls[&remote_id].id(), c_id);
        fixture.actor.poll_controls().await.unwrap();
        assert_eq!(
            fixture.actor.controls[&remote_id].id(),
            c_id,
            "an overdue candidate must not replace a worker installed after it parked"
        );
        assert!(fixture.actor.parked_controls.is_empty());
        timeout(Duration::from_secs(10), async {
            while b_connection.close_reason().is_none() {
                tokio::time::sleep(Duration::from_millis(10)).await;
            }
        })
        .await
        .expect("the stale candidate was closed");
        drop((remote_a, remote_b, remote_c));
        fixture.stop().await;
    })
    .await
    .unwrap();
}

/// Connections that fail validation do not end a listener's slot: the wrong
/// generation, then the wrong capability. The window is still open for a valid
/// dial, which forms the link.
#[tokio::test]
async fn a_rejected_candidate_leaves_the_slot_waiting_for_a_valid_dial() {
    timeout(Duration::from_secs(30), async {
        let (listener_endpoint, dialer_endpoint) = (endpoint().await, endpoint().await);
        let room = [91; 16];
        let mut listener = GameSide::start(&listener_endpoint, room, &dialer_endpoint).await;
        listener.prepare(room, &dialer_endpoint, 5, false).await;
        let auth = GameAuthorization {
            peer: listener_endpoint.id(),
            key: MatchKey {
                room,
                generation: 1,
            },
            capability: [5; 32],
            max_packet: 1024,
        };
        let mut wrong_generation = auth.clone();
        wrong_generation.key.generation = 2;
        let mut wrong_capability = auth.clone();
        wrong_capability.capability = [9; 32];
        for bad in [wrong_generation, wrong_capability] {
            let candidate = dialer_endpoint
                .connect(address(&listener_endpoint), GAME_ALPN)
                .await
                .unwrap();
            assert!(
                transport::connect_game_unconfirmed(&candidate, &bad)
                    .await
                    .is_err()
            );
            // Candidates run one at a time; let the listener retire this one.
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
        listener
            .stays_waiting(
                Duration::from_millis(700),
                "a rejected candidate ended the slot",
            )
            .await;
        let game = transport::connect_game(&dialer_endpoint, address(&listener_endpoint), auth)
            .await
            .expect("the valid dial must still be accepted");
        listener.ready().await;
        assert!(game.connection.close_reason().is_none());
        listener_endpoint.close().await;
        dialer_endpoint.close().await;
    })
    .await
    .unwrap();
}

/// A link service on loopback, the same code as the one deployed.
async fn short_service() -> (String, ember_short::AppState) {
    let state = ember_short::AppState::new(
        ember_short::Limits::default(),
        ember_short::system_clock(),
    );
    let listener = tokio::net::TcpListener::bind((Ipv4Addr::LOCALHOST, 0))
        .await
        .unwrap();
    let port = listener.local_addr().unwrap().port();
    let served = state.clone();
    tokio::spawn(async move {
        let _ = ember_short::serve_until(listener, served, std::future::pending()).await;
    });
    (format!("http://127.0.0.1:{port}/s/v1/"), state)
}

/// Runs finished tasks through `completed` until a short-link task has
/// been handled. Tasks a cleared room aborted are skipped.
async fn complete_short(actor: &mut Actor) {
    loop {
        let finished = timeout(Duration::from_secs(20), actor.tasks.join_next())
            .await
            .expect("a task finishes")
            .expect("a task was running");
        let completion = match finished {
            Ok(completion) => completion,
            Err(error) if error.is_cancelled() => continue,
            Err(error) => panic!("task failed: {error}"),
        };
        let short = matches!(
            completion,
            Completion::ShortPublished(..)
                | Completion::ShortResolved(..)
                | Completion::ShortAdopted(..)
        );
        actor.completed(completion).await.unwrap();
        if short {
            return;
        }
    }
}

fn short_join(epoch: u64, invitation: &str, build: &str) -> Request {
    Request {
        id: 40 + epoch,
        command: Command::Join {
            epoch,
            invitation: invitation.into(),
            build: build.into(),
        },
    }
}

async fn joined_reason(guest: &mut Actor, events: &mut mpsc::Receiver<Event>, epoch: u64, text: &str, build: &str) -> Option<String> {
    assert!(guest.command(short_join(epoch, text, build)).unwrap());
    complete_short(guest).await;
    match timeout(Duration::from_secs(5), events.recv()).await.unwrap().unwrap() {
        Event::Error { epoch: refused, ref code, reason, .. } => {
            assert_eq!(refused, epoch);
            assert_eq!(code, "invalid_or_incompatible_invitation");
            assert_eq!(guest.room, None);
            assert!(!guest.opening);
            reason
        }
        _ => panic!("expected a refused join"),
    }
}

#[test]
fn short_invite_ipc_round_trips() {
    let command: Command = serde_json::from_str(r#"{"type":"short_invite","epoch":3}"#).unwrap();
    assert!(matches!(command, Command::ShortInvite { epoch: 3 }));
    assert!(serde_json::from_str::<Command>(r#"{"type":"short_invite","epoch":3,"extra":1}"#).is_err());
    let event = serde_json::to_value(Event::ShortInvite {
        epoch: 3,
        link: "https://embernetplay.link/j#0000-0000-0000".into(),
        status: "ready".into(),
    })
    .unwrap();
    assert_eq!(event["type"], "short_invite");
    assert_eq!(event["status"], "ready");
}

#[tokio::test]
async fn a_short_link_opens_the_room_it_was_made_for() {
    let (service, store) = short_service().await;
    let host_endpoint = endpoint().await;
    let guest_endpoint = endpoint().await;
    let (host_events_tx, mut host_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (guest_events_tx, mut guest_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut host = test_actor(host_endpoint.clone(), host_events_tx);
    let mut guest = test_actor(guest_endpoint.clone(), guest_events_tx);
    host.short.service = service.clone();
    guest.short.service = service.clone();
    host.epoch = 4;
    let start = now().unwrap();
    let invite = Invite::create(host_endpoint.id(), test_relay(), "test-build".into(), start, INVITE_LIFETIME).unwrap();
    host.hosted = Some(invite.clone());
    host.room_invite = Some(invite.clone());

    // A stale epoch, or no room, is answered without a link.
    host.short_invite_command(3).unwrap();
    assert!(matches!(host_events.recv().await.unwrap(), Event::ShortInvite { epoch: 3, ref link, ref status } if link.is_empty() && status == "unavailable"));
    assert!(host.tasks.is_empty());

    host.short_invite_command(4).unwrap();
    complete_short(&mut host).await;
    let Event::ShortInvite { epoch, link, status } = host_events.recv().await.unwrap() else {
        panic!("expected the short link");
    };
    assert_eq!((epoch, status.as_str()), (4, "ready"));
    assert!(link.starts_with(crate::short_invite::LINK_PREFIX));
    assert_eq!(link.len(), 42);
    assert_eq!(crate::short_invite::parse(&link), Some(invite.short_code()));
    assert_eq!(store.len(), 1);
    // Asked again, the same link comes back at once.
    host.short_invite_command(4).unwrap();
    assert!(host.tasks.is_empty());
    assert!(matches!(host_events.recv().await.unwrap(), Event::ShortInvite { link: ref again, .. } if *again == link));

    // The guest opens it, in any of the forms a player might paste, and
    // goes on to dial the host the record names.
    let code = crate::short_invite::display_code(&invite.short_code());
    for (epoch, pasted) in [(1, link.clone()), (2, code.to_lowercase()), (3, format!(" {code}\r\n"))] {
        assert!(guest.command(short_join(epoch, &pasted, "test-build")).unwrap());
        assert_eq!(guest.room, None);
        complete_short(&mut guest).await;
        assert_eq!(guest.room, Some(invite.room()));
        assert_eq!(guest.host_address.as_ref().map(|address| address.id), Some(host_endpoint.id()));
        assert!(guest.opening);
        assert!(guest_events.try_recv().is_err());
        guest.clear_room();
    }

    // The joiner still checks its own build against the record.
    assert_eq!(joined_reason(&mut guest, &mut guest_events, 5, &link, "other-build").await.as_deref(), Some("other_build"));

    // A renewed invitation is stored again under the same link.
    let renewed_at = start + INVITE_LIFETIME * 3 / 4;
    host.renew_invitation(renewed_at);
    let _ = host_events.recv().await.unwrap();
    host.pump_short_link(renewed_at);
    assert!(!host.tasks.is_empty());
    complete_short(&mut host).await;
    assert!(host_events.try_recv().is_err());
    let keys = crate::short_invite::derive(&invite.short_code()).unwrap();
    let stored = crate::short_invite::fetch(&service, &keys).await.unwrap();
    assert_eq!(stored, host.room_invite.as_ref().unwrap().encode().unwrap());
    // Unchanged and recently stored: nothing to do.
    host.pump_short_link(now().unwrap());
    assert!(host.tasks.is_empty());
    // Leaving the room forgets the link.
    host.clear_room();
    assert!(host.short.room.is_none());
    host_endpoint.close().await;
    guest_endpoint.close().await;
}

#[tokio::test]
async fn a_new_leader_keeps_a_shared_short_link_current() {
    let (service, store) = short_service().await;
    let host_endpoint = endpoint().await;
    let guest_endpoint = endpoint().await;
    let (host_events_tx, mut host_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (guest_events_tx, mut guest_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut host = test_actor(host_endpoint.clone(), host_events_tx);
    let mut guest = test_actor(guest_endpoint.clone(), guest_events_tx);
    host.short.service = service.clone();
    guest.short.service = service.clone();
    host.epoch = 4;
    guest.epoch = 7;
    let start = now().unwrap();
    let invite = Invite::create(host_endpoint.id(), test_relay(), "test-build".into(), start, INVITE_LIFETIME).unwrap();
    // The guest leads the room now: its invitation names its own endpoint.
    let coordination = iroh::SecretKey::generate().public();
    let led = invite
        .clone()
        .with_authority_route(guest_endpoint.id(), coordination, 2, 9)
        .unwrap();
    guest.room_invite = Some(led.clone());

    // Nobody shared a link yet: the new leader finds no record and publishes
    // nothing.
    guest.adopt_short_link();
    complete_short(&mut guest).await;
    assert!(guest.short.room.is_none() && guest.short.adopt_at == 0);
    guest.pump_short_link(now().unwrap());
    assert!(guest.tasks.is_empty());
    assert_eq!(store.len(), 0);

    // The host shares one, then leaves.
    host.hosted = Some(invite.clone());
    host.room_invite = Some(invite.clone());
    host.short_invite_command(4).unwrap();
    complete_short(&mut host).await;
    assert!(matches!(host_events.recv().await.unwrap(), Event::ShortInvite { ref status, .. } if status == "ready"));
    host.clear_room();

    // The new leader takes the link over and stores its own invitation under
    // it, without telling the native side anything new.
    guest.adopt_short_link();
    complete_short(&mut guest).await;
    assert!(guest.short.room.is_some());
    guest.pump_short_link(now().unwrap());
    assert!(!guest.tasks.is_empty());
    complete_short(&mut guest).await;
    assert!(guest_events.try_recv().is_err());
    let keys = crate::short_invite::derive(&invite.short_code()).unwrap();
    let stored = crate::short_invite::fetch(&service, &keys).await.unwrap();
    assert_eq!(stored, led.encode().unwrap());
    // Asked for the link, the leader gives the same one at once.
    guest.short_invite_command(7).unwrap();
    assert!(guest.tasks.is_empty());
    assert!(matches!(guest_events.recv().await.unwrap(), Event::ShortInvite { ref link, .. } if crate::short_invite::parse(link) == Some(invite.short_code())));
    // Leaving forgets it.
    guest.clear_room();
    assert!(guest.short.room.is_none() && !guest.short.adopting);

    // A lookup the service does not answer is tried again later, a few
    // times at most.
    let mut lone = test_actor(endpoint().await, mpsc::channel(IPC_QUEUE_CAPACITY).0);
    lone.short.service = "http://127.0.0.1:9/s/v1/".into();
    lone.room_invite = Some(led.clone());
    for _ in 0..3 {
        lone.adopt_short_link();
        complete_short(&mut lone).await;
        assert!(lone.short.room.is_none());
    }
    assert_eq!(lone.short.adopt_at, 0);
    lone.adopt_short_link();
    assert!(lone.tasks.is_empty());
    host_endpoint.close().await;
    guest_endpoint.close().await;
}

/// Marks `actor` as the room's coordination leader in `term`, as a refresh
/// that saw it lead would.
fn leads(actor: &mut Actor, term: u64) {
    actor.last_coordination_state = Some((term, 1, true, true, 1, 0));
}

/// This member's copy of the room's invitation once it leads: the same room
/// and capability, its own route.
fn led_by(invite: &Invite, leader: &Endpoint, term: u64) -> Invite {
    invite
        .clone()
        .with_authority_route(leader.id(), iroh::SecretKey::generate().public(), term, term)
        .unwrap()
}

/// Runs the short-link task the pump starts (a lookup that was asked for, or
/// a store of the invitation), if any. Returns whether one ran.
async fn pump_adopt(actor: &mut Actor) -> bool {
    actor.pump_short_link(now().unwrap());
    if actor.tasks.is_empty() {
        return false;
    }
    complete_short(actor).await;
    true
}

/// The link the service holds for the room now.
async fn stored_link(service: &str, invite: &Invite) -> String {
    let keys = crate::short_invite::derive(&invite.short_code()).unwrap();
    crate::short_invite::fetch(service, &keys).await.unwrap()
}

#[test]
fn only_a_change_of_leader_looks_for_a_shared_link() {
    let mut short = ShortLinks::default();
    // The game that opened the room leads it from the start: nothing to take over.
    short.follow_leader(true, 1, false, true, 100);
    assert_eq!(short.adopt_at, 0);
    // Refreshes under the same lead never ask again.
    short.follow_leader(true, 1, false, true, 200);
    assert_eq!(short.adopt_at, 0);
    // An election with no leader yet is not another member leading.
    short.follow_leader(false, 2, false, true, 300);
    short.follow_leader(true, 2, false, true, 400);
    assert_eq!(short.adopt_at, 0);
    // Another member leads, then the lead comes back: look once.
    short.follow_leader(false, 3, true, true, 500);
    assert_eq!(short.adopt_at, 0);
    short.follow_leader(true, 4, false, true, 600);
    assert_eq!(short.adopt_at, 600);
    short.adopt_at = 0;
    short.follow_leader(true, 4, false, true, 700);
    assert_eq!(short.adopt_at, 0);
    // A member that joined looks as soon as it leads.
    let mut joined = ShortLinks::default();
    joined.follow_leader(true, 2, false, false, 0);
    assert_eq!(joined.adopt_at, 1);
    // Leaving forgets all of it, the service excepted.
    joined.service = "http://127.0.0.1:9/s/v1/".into();
    joined.clear();
    assert!(joined.room.is_none() && joined.led_term == 0 && !joined.followed);
    assert_eq!(joined.service, "http://127.0.0.1:9/s/v1/");
}

#[tokio::test]
async fn the_host_keeps_a_link_a_departed_member_shared() {
    let (service, _store) = short_service().await;
    let host_endpoint = endpoint().await;
    let guest_endpoint = endpoint().await;
    let (host_events_tx, _host_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut host = test_actor(host_endpoint.clone(), host_events_tx);
    let (guest_events_tx, mut guest_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut guest = test_actor(guest_endpoint.clone(), guest_events_tx);
    host.short.service = service.clone();
    guest.short.service = service.clone();
    let invite = Invite::create(host_endpoint.id(), test_relay(), "test-build".into(), now().unwrap(), INVITE_LIFETIME).unwrap();
    host.epoch = 4;
    host.hosted = Some(invite.clone());
    host.room_invite = Some(invite.clone());
    leads(&mut host, 1);
    host.short.follow_leader(true, 1, false, true, now().unwrap());
    guest.epoch = 7;
    guest.room_invite = Some(invite.clone());

    // A member leaves before anyone shared a link: one lookup finds nothing.
    let history = BTreeSet::new();
    host.apply_confirmed_retirements(BTreeSet::from([21]), &history);
    assert!(pump_adopt(&mut host).await);
    assert!(host.short.room.is_none() && host.short.adopt_at == 0);
    assert!(!pump_adopt(&mut host).await);
    // The same departure confirmed again asks nothing.
    host.apply_confirmed_retirements(BTreeSet::from([21]), &history);
    assert_eq!(host.short.adopt_at, 0);
    // With every worker busy, a lookup waits for one instead of being lost.
    while host.tasks.len() < MAX_TASKS {
        host.tasks.spawn(std::future::pending::<Completion>());
    }
    host.apply_confirmed_retirements(BTreeSet::from([20]), &history);
    host.pump_short_link(now().unwrap());
    assert!(host.tasks.len() == MAX_TASKS && host.short.adopt_at != 0);
    host.tasks.abort_all();
    while host.tasks.join_next().await.is_some() {}
    assert!(pump_adopt(&mut host).await);
    assert_eq!(host.short.adopt_at, 0);

    // The guest shares a link, then leaves while the host leads on.
    guest.short_invite_command(7).unwrap();
    complete_short(&mut guest).await;
    assert!(matches!(guest_events.recv().await.unwrap(), Event::ShortInvite { ref status, .. } if status == "ready"));
    guest.clear_room();
    host.apply_confirmed_retirements(BTreeSet::from([22]), &history);
    assert!(pump_adopt(&mut host).await);
    assert!(host.short.room.is_some());
    // The host stores its own invitation under the guest's link from now on.
    host.renew_invitation(now().unwrap() + INVITE_LIFETIME / 2);
    assert!(pump_adopt(&mut host).await);
    let renewed = host.room_invite.clone().unwrap();
    assert_ne!(renewed.encode().unwrap(), invite.encode().unwrap());
    assert_eq!(stored_link(&service, &invite).await, renewed.encode().unwrap());
    // Holding the link, the host looks nothing up for later departures.
    host.apply_confirmed_retirements(BTreeSet::from([23]), &history);
    assert_eq!(host.short.adopt_at, 0);
    // A member that does not lead never looks.
    let mut follower = test_actor(endpoint().await, mpsc::channel(IPC_QUEUE_CAPACITY).0);
    follower.short.service = service.clone();
    follower.room_invite = Some(invite.clone());
    follower.apply_confirmed_retirements(BTreeSet::from([24]), &history);
    assert!(!pump_adopt(&mut follower).await);
    host_endpoint.close().await;
    guest_endpoint.close().await;
}

#[tokio::test]
async fn the_host_takes_a_shared_link_back_with_the_lead() {
    let (service, _store) = short_service().await;
    let host_endpoint = endpoint().await;
    let guest_endpoint = endpoint().await;
    let (host_events_tx, _host_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (guest_events_tx, _guest_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut host = test_actor(host_endpoint.clone(), host_events_tx);
    let mut guest = test_actor(guest_endpoint.clone(), guest_events_tx);
    host.short.service = service.clone();
    guest.short.service = service.clone();
    let invite = Invite::create(host_endpoint.id(), test_relay(), "test-build".into(), now().unwrap(), INVITE_LIFETIME).unwrap();
    host.hosted = Some(invite.clone());
    host.room_invite = Some(invite.clone());
    host.short.follow_leader(true, 1, false, true, now().unwrap());

    // The guest leads for a while and shares a link under its own route.
    host.short.follow_leader(false, 2, true, true, now().unwrap());
    let guest_route = led_by(&invite, &guest_endpoint, 2);
    guest.epoch = 7;
    guest.room_invite = Some(guest_route.clone());
    guest.short_invite_command(7).unwrap();
    complete_short(&mut guest).await;
    assert_eq!(stored_link(&service, &invite).await, guest_route.encode().unwrap());
    guest.clear_room();

    // The lead comes back to the host, which takes the link over.
    let host_route = led_by(&invite, &host_endpoint, 3);
    host.room_invite = Some(host_route.clone());
    leads(&mut host, 3);
    host.short.follow_leader(true, 3, false, true, now().unwrap());
    assert!(pump_adopt(&mut host).await);
    assert!(host.short.room.is_some());
    assert!(pump_adopt(&mut host).await);
    assert_eq!(stored_link(&service, &invite).await, host_route.encode().unwrap());
    host_endpoint.close().await;
    guest_endpoint.close().await;
}

#[tokio::test]
async fn a_shared_link_follows_the_lead_through_every_handoff() {
    let (service, _store) = short_service().await;
    let endpoints = [endpoint().await, endpoint().await, endpoint().await, endpoint().await];
    let (mut members, mut _events) = (Vec::new(), Vec::new());
    for member in &endpoints {
        let (events_tx, events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        _events.push(events);
        let mut actor = test_actor(member.clone(), events_tx);
        actor.short.service = service.clone();
        actor.epoch = 5;
        members.push(actor);
    }
    let invite = Invite::create(endpoints[0].id(), test_relay(), "test-build".into(), now().unwrap(), INVITE_LIFETIME).unwrap();
    members[0].hosted = Some(invite.clone());
    for member in &mut members {
        member.room_invite = Some(invite.clone());
    }
    // The host leads and a member that joined shares the link, then the host
    // leaves.
    members[0].short.follow_leader(true, 1, false, true, now().unwrap());
    members[1].short_invite_command(5).unwrap();
    complete_short(&mut members[1]).await;
    members[0].clear_room();
    // The lead passes 1, 2, 3, each leaving after its term: every new leader
    // holds the link and stores its own route under it.
    for (term, index) in [(2u64, 1usize), (3, 2), (4, 3)] {
        for (other, member) in members.iter_mut().enumerate().skip(1) {
            if other != index {
                member.short.follow_leader(false, term, true, false, now().unwrap());
            }
        }
        let route = led_by(&invite, &endpoints[index], term);
        let leader = &mut members[index];
        leader.room_invite = Some(route.clone());
        leads(leader, term);
        leader.short.follow_leader(true, term, false, false, now().unwrap());
        // The member that published already holds the link; the others take it over.
        while pump_adopt(leader).await {}
        assert!(leader.short.room.is_some());
        assert_eq!(stored_link(&service, &invite).await, route.encode().unwrap());
        leader.clear_room();
    }
    for member in endpoints {
        member.close().await;
    }
}

#[tokio::test]
async fn a_short_link_that_cannot_be_opened_says_why() {
    let (service, _store) = short_service().await;
    let own = endpoint().await;
    let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut guest = test_actor(own.clone(), events_tx);
    guest.short.service = service.clone();
    let host_id = iroh::SecretKey::generate().public();
    let start = now().unwrap();

    // Nothing stored under this code: mistyped, or its room has closed.
    let missing = Invite::create(host_id, test_relay(), "test-build".into(), start, 3600).unwrap();
    let missing_link = crate::short_invite::link(&missing.short_code());
    assert_eq!(joined_reason(&mut guest, &mut events, 1, &missing_link, "test-build").await.as_deref(), Some("short_unknown"));

    // An invitation that has expired since it was stored.
    let expired = Invite::create(host_id, test_relay(), "test-build".into(), start - 4000, 3600).unwrap();
    let keys = crate::short_invite::derive(&expired.short_code()).unwrap();
    crate::short_invite::publish(&service, &keys, &expired.encode().unwrap(), 600).await.unwrap();
    let expired_link = crate::short_invite::link(&expired.short_code());
    assert_eq!(joined_reason(&mut guest, &mut events, 2, &expired_link, "test-build").await.as_deref(), Some("expired"));

    // A record whose invitation belongs to another room is refused, even
    // though it opens with the code.
    let other = Invite::create(host_id, test_relay(), "test-build".into(), start, 3600).unwrap();
    let wanted = Invite::create(host_id, test_relay(), "test-build".into(), start, 3600).unwrap();
    let keys = crate::short_invite::derive(&wanted.short_code()).unwrap();
    crate::short_invite::publish(&service, &keys, &other.encode().unwrap(), 600).await.unwrap();
    let wanted_link = crate::short_invite::link(&wanted.short_code());
    assert_eq!(joined_reason(&mut guest, &mut events, 3, &wanted_link, "test-build").await.as_deref(), Some("malformed"));

    // The service is down: the player is told to use the full invitation.
    guest.short.service = "http://127.0.0.1:9/s/v1/".into();
    assert_eq!(joined_reason(&mut guest, &mut events, 4, &missing_link, "test-build").await.as_deref(), Some("short_unavailable"));

    // A host whose service is down hears so once and keeps no link.
    let (host_events_tx, mut host_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut host = test_actor(own.clone(), host_events_tx);
    host.short.service = "http://127.0.0.1:9/s/v1/".into();
    host.epoch = 6;
    host.hosted = Some(Invite::create(own.id(), test_relay(), "test-build".into(), start, 3600).unwrap());
    host.short_invite_command(6).unwrap();
    complete_short(&mut host).await;
    assert!(matches!(host_events.recv().await.unwrap(), Event::ShortInvite { epoch: 6, ref link, ref status } if link.is_empty() && status == "unavailable"));
    assert!(host.short.room.is_none());

    // A Leave while the lookup runs drops its answer.
    guest.short.service = service;
    assert!(guest.command(short_join(7, &missing_link, "test-build")).unwrap());
    guest.clear_room();
    while let Some(finished) = guest.tasks.join_next().await {
        if let Ok(completion) = finished {
            guest.completed(completion).await.unwrap();
        }
    }
    assert!(events.try_recv().is_err());
    assert_eq!(guest.room, None);
    own.close().await;
}

mod join_deadline;
mod public_admission;
mod public_rooms;
mod public_support;
mod server_owned;
