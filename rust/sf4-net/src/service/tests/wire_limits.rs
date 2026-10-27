//! Event privacy, frame bounds and the native nesting limit.

use super::*;

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

#[tokio::test]
async fn ipc_service_rejects_malformed_and_replayed_commands() {
    for replay in [false, true] {
        phase(
            "ipc_service_rejects_malformed_and_replayed_commands",
            Duration::from_secs(10),
            async {
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
            },
        )
        .await;
    }
}

fn nested_json(depth: usize) -> String {
    format!("{}{}", "[".repeat(depth), "]".repeat(depth))
}

#[test]
fn json_nested_past_the_native_limit_is_not_handed_on() {
    let ok = |text: &str| native_json_depth_ok(text.as_bytes());
    assert!(ok(&nested_json(MAX_NATIVE_JSON_DEPTH)));
    assert!(!ok(&nested_json(MAX_NATIVE_JSON_DEPTH + 1)));
    assert!(!ok(&format!("{{\"a\":{}", "{".repeat(64 * 1024))));
    // Brackets inside strings, escaped quotes included, are text.
    let quoted = format!("{{\"name\":\"\\\"{}\"}}", "[".repeat(1000));
    assert!(ok(&quoted));
    assert!(ok(
        r#"{"kind":"room","members":[{"data":{"name":"a]]]"}}]}"#
    ));
}

/// Both paths that find a committed checkpoint, the watcher and the
/// proposal's completion, export it through `start_outgoing_checkpoint`.
#[tokio::test]
async fn a_checkpoint_nested_past_the_native_limit_is_passed_over_once() {
    let endpoint = endpoint().await;
    let (events, mut receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(endpoint.clone(), events);
    let room = [46; 16];
    actor.epoch = 1;
    actor.room = Some(room);
    let deep = nested_json(MAX_NATIVE_JSON_DEPTH + 1).into_bytes();
    let deep = CheckpointTransfer::new(room, 17, 3, 7, 8, deep).unwrap();

    actor.start_outgoing_checkpoint(deep);

    assert!(actor.outgoing_transfer.is_none());
    assert_eq!(actor.last_exported_revision, 8);
    assert!(receiver.try_recv().is_err());
    // The next revision exports as usual.
    let next = CheckpointTransfer::new(room, 18, 3, 8, 9, b"{\"checkpoint\":{}}".to_vec()).unwrap();
    actor.start_outgoing_checkpoint(next);
    assert!(matches!(
        receiver.recv().await,
        Some(Event::CheckpointBegin {
            transfer: 18,
            revision: 9,
            ..
        })
    ));
    endpoint.close().await;
}

/// A peer's message nested past the limit, bare or as a native control's
/// payload, is dropped; the route stays open and later messages arrive.
#[tokio::test]
async fn a_control_message_nested_past_the_native_limit_is_dropped() {
    phase(
        "a_control_message_nested_past_the_native_limit_is_dropped",
        Duration::from_secs(60),
        async {
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
            let remote_worker = ControlWorker::start(remote_side);
            let deep = nested_json(MAX_NATIVE_JSON_DEPTH + 1);
            // A native control carries its payload as a JSON string or as raw JSON.
            let quoted = serde_json::json!({
                "type": "native_control",
                "message_id": 7,
                "payload": deep,
            })
            .to_string();
            let raw = format!(r#"{{"type":"native_control","message_id":8,"payload":{deep}}}"#);
            for (offset, payload) in [deep.clone(), quoted, raw, "[\"after\"]".to_string()]
                .into_iter()
                .enumerate()
            {
                remote_worker
                    .try_send(ControlFrame {
                        message_id: TRANSPORT_MESSAGE_ID_BASE + offset as u64,
                        payload: payload.into_bytes(),
                    })
                    .unwrap();
            }
            let mut delivered = Vec::new();
            while !delivered
                .iter()
                .any(|payload: &String| payload == "[\"after\"]")
            {
                fixture.actor.poll_controls().await.unwrap();
                while let Ok(event) = fixture.events.try_recv() {
                    match event {
                        Event::Message { payload, .. } => delivered.push(payload),
                        Event::ControlClosed { .. } => panic!("the route closed"),
                        _ => (),
                    }
                }
                tokio::time::sleep(Duration::from_millis(10)).await;
            }
            assert_eq!(delivered, vec!["[\"after\"]".to_string()]);
            assert!(fixture.actor.controls.contains_key(&remote_id));
            drop(remote_worker);
        },
    )
    .await;
}
