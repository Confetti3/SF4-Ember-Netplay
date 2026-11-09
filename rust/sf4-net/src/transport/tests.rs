use super::*;
use crate::bridge::Bridge;
use std::{net::Ipv4Addr, sync::atomic::Ordering};
use tokio::{net::UdpSocket, sync::watch};

async fn local_endpoint() -> Endpoint {
    Endpoint::builder(presets::Minimal)
        .clear_ip_transports()
        .bind_addr((Ipv4Addr::LOCALHOST, 0))
        .unwrap()
        .alpns(vec![CONTROL_ALPN.to_vec(), GAME_ALPN.to_vec()])
        .portmapper_config(PortmapperConfig::Disabled)
        .bind()
        .await
        .unwrap()
}

fn address(endpoint: &Endpoint) -> EndpointAddr {
    EndpointAddr::new(endpoint.id()).with_ip_addr(endpoint.bound_sockets()[0])
}

fn room(endpoint: &Endpoint) -> Invite {
    let relay = crate::invite::relay_map().urls::<Vec<_>>().remove(0);
    Invite::create(
        endpoint.id(),
        relay,
        "same-sidecar-build".into(),
        now().unwrap(),
        3600,
    )
    .unwrap()
}

async fn control_pair(a: &Endpoint, b: &Endpoint) -> (ControlChannel, ControlChannel) {
    let invite = room(b);
    let (client, server) = tokio::join!(
        async {
            let connection = a.connect(address(b), CONTROL_ALPN).await.unwrap();
            connect_control_on(connection, &invite).await.unwrap()
        },
        async {
            let connection = b.accept().await.unwrap().await.unwrap();
            accept_control(connection, &invite).await.unwrap()
        }
    );
    (client, server)
}

async fn game_pair(
    a: &Endpoint,
    b: &Endpoint,
    generation: u64,
) -> (GameConnection, GameConnection) {
    let key = MatchKey {
        room: [13; 16],
        generation,
    };
    let a_auth = GameAuthorization {
        peer: b.id(),
        key,
        capability: [91; 32],
        max_packet: 1024,
    };
    let b_auth = GameAuthorization {
        peer: a.id(),
        ..a_auth.clone()
    };
    let (client, server) = tokio::join!(connect_game(a, address(b), a_auth), async {
        let connection = b.accept().await.unwrap().await.unwrap();
        accept_game(connection, b_auth).await
    });
    (client.unwrap(), server.unwrap())
}

#[tokio::test]
async fn expired_invite_reauthenticates_only_the_admitted_endpoint_after_handoff() {
    let a = local_endpoint().await;
    let b = local_endpoint().await;
    let successor = local_endpoint().await;
    let invite = room(&b);
    let clock = now().unwrap() + 7200;
    for (server, member, allowed) in [
        (&b, None, false),
        (&b, Some(a.id()), true),
        (&successor, Some(a.id()), true),
        (&b, Some(b.id()), false),
    ] {
        let (client, accepted) = tokio::join!(
            async {
                let connection = a.connect(address(server), CONTROL_ALPN).await.unwrap();
                connect_control_on_proof(connection, &invite.proof(), Some(server.id())).await
            },
            async {
                let connection = server.accept().await.unwrap().await.unwrap();
                let accepting = Accepting {
                    member,
                    watchers: false,
                };
                accept_control_policy(connection, &invite, accepting, clock).await
            }
        );
        assert_eq!(client.is_ok(), allowed);
        assert_eq!(accepted.is_ok(), allowed);
    }
    a.close().await;
    b.close().await;
    successor.close().await;
}

#[tokio::test]
async fn partial_game_proof_uses_remaining_marker_deadline_and_releases_connection() {
    let a = local_endpoint().await;
    let b = local_endpoint().await;
    let (client, server) = tokio::join!(a.connect(address(&b), GAME_ALPN), async {
        b.accept().await.unwrap().await
    });
    let client = client.unwrap();
    let server = server.unwrap();
    let (mut send, _recv) = client.open_bi().await.unwrap();
    send.write_all(&GAME_PLAY_MAGIC).await.unwrap();
    let started = Instant::now();
    let deadline = started + Duration::from_millis(250);
    let GameStream::Gameplay(tx, rx) = accept_game_stream_until(&server, deadline).await.unwrap()
    else {
        panic!("wrong purpose")
    };
    tokio::time::sleep(Duration::from_millis(150)).await;
    let auth = GameAuthorization {
        peer: a.id(),
        key: MatchKey {
            room: [39; 16],
            generation: 1,
        },
        capability: [11; 32],
        max_packet: 1024,
    };
    assert!(
        timeout(
            Duration::from_millis(300),
            accept_game_stream_with_until(server, auth, tx, rx, deadline)
        )
        .await
        .unwrap()
        .is_err()
    );
    assert!(started.elapsed() < Duration::from_millis(500));
    timeout(Duration::from_secs(1), client.closed())
        .await
        .unwrap();
    let _new_generation = game_pair(&a, &b, 2).await;
    a.close().await;
    b.close().await;
}

#[tokio::test]
async fn game_dial_retries_until_matching_listener_is_installed() {
    timeout(Duration::from_secs(15), async {
        let a = local_endpoint().await;
        let b = local_endpoint().await;
        let key = MatchKey {
            room: [27; 16],
            generation: 4,
        };
        let a_auth = GameAuthorization {
            peer: b.id(),
            key,
            capability: [71; 32],
            max_packet: 1024,
        };
        let b_auth = GameAuthorization {
            peer: a.id(),
            ..a_auth.clone()
        };
        let (client, server) = tokio::join!(connect_game(&a, address(&b), a_auth), async {
            // Model the remote helper receiving the GAME_ALPN dial
            // before its prepare_game command. The next bounded dial
            // must use the listener installed immediately afterward.
            let premature = b.accept().await.unwrap().await.unwrap();
            premature.close(1u32.into(), b"gameplay not authorized");
            let prepared = b.accept().await.unwrap().await.unwrap();
            accept_game(prepared, b_auth).await
        });
        assert!(client.is_ok());
        assert!(server.is_ok());
        a.close().await;
        b.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn actual_quic_control_preserves_ready_ids_and_rejects_replays() {
    timeout(Duration::from_secs(15), async {
        let a = local_endpoint().await;
        let b = local_endpoint().await;
        let (mut client, mut server) = control_pair(&a, &b).await;
        let frame = ControlFrame {
            message_id: 1234,
            payload: br#"{"type":"lobby_ready"}"#.to_vec(),
        };
        client.sender.send(&frame).await.unwrap();
        assert_eq!(server.receiver.receive().await.unwrap(), frame);
        assert!(client.sender.send(&frame).await.is_err());
        client.connection.close(0u32.into(), b"test finished");
        assert!(server.receiver.receive().await.is_err());
        a.close().await;
        b.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn wrong_room_capability_is_rejected_before_control_delivery() {
    timeout(Duration::from_secs(15), async {
        let a = local_endpoint().await;
        let b = local_endpoint().await;
        let expected = room(&b);
        let wrong = room(&b);
        let (client, server) = tokio::join!(
            async {
                let connection = a.connect(address(&b), CONTROL_ALPN).await.unwrap();
                connect_control_on(connection, &wrong).await
            },
            async {
                let connection = b.accept().await.unwrap().await.unwrap();
                accept_control(connection, &expected).await
            }
        );
        assert!(client.is_err());
        assert!(server.is_err());
        a.close().await;
        b.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn control_workers_bound_both_directions_and_backpressure_the_consumer() {
    use crate::control::{CONTROL_QUEUE_CAPACITY, ControlWorker, QueueError};
    timeout(Duration::from_secs(15), async {
        let a = local_endpoint().await;
        let b = local_endpoint().await;
        let (client, server) = control_pair(&a, &b).await;
        let sender = ControlWorker::start(client);
        let mut receiver = ControlWorker::start(server);
        // This current-thread executor cannot drain the queue until yield.
        for id in 2..(CONTROL_QUEUE_CAPACITY as u64 + 2) {
            sender
                .try_send(ControlFrame {
                    message_id: id,
                    payload: vec![1],
                })
                .unwrap();
        }
        assert_eq!(
            sender.try_send(ControlFrame {
                message_id: 999,
                payload: vec![1]
            }),
            Err(QueueError::Full)
        );
        // The full queue is the peak. Reading it starts the count over, but the
        // frames still waiting behind the blocked writer are in the next count.
        assert_eq!(sender.take_queue_peak(), CONTROL_QUEUE_CAPACITY);
        assert_eq!(sender.take_queue_peak(), CONTROL_QUEUE_CAPACITY);
        // The writer drains the queue with nothing more sent: the window that
        // began full still reads full, and the one after it reads empty.
        let mut expected = 2;
        while expected < CONTROL_QUEUE_CAPACITY as u64 + 2 {
            if let Some(frame) = receiver.try_receive() {
                assert_eq!(frame.message_id, expected);
                expected += 1;
            } else {
                tokio::task::yield_now().await;
            }
        }
        assert_eq!(sender.take_queue_peak(), CONTROL_QUEUE_CAPACITY);
        assert_eq!(sender.take_queue_peak(), 0);
        assert_eq!(
            sender.try_send(ControlFrame {
                message_id: 1000,
                payload: vec![1; wire::MAX_CONTROL_PAYLOAD + 1]
            }),
            Err(QueueError::Invalid)
        );
        let mut id = CONTROL_QUEUE_CAPACITY as u64 + 2;
        let end = CONTROL_QUEUE_CAPACITY as u64 * 3 + 2;
        while id < end {
            match sender.try_send(ControlFrame {
                message_id: id,
                payload: vec![1],
            }) {
                Ok(()) => id += 1,
                Err(QueueError::Full) => (),
                Err(QueueError::Closed) => panic!("backpressure closed the control route"),
                Err(QueueError::Invalid) => panic!("valid frame rejected"),
            }
            tokio::task::yield_now().await;
        }
        assert!(!receiver.is_closed());
        while expected < end {
            if let Some(frame) = receiver.try_receive() {
                assert_eq!(frame.message_id, expected);
                expected += 1;
            } else {
                tokio::task::yield_now().await;
            }
        }
        assert!(!receiver.is_closed());
        assert_eq!(receiver.rejected(), 0);
        // Every frame arrived, so the queue has drained: once the window that
        // saw it busy is read, the next one is empty.
        assert!(sender.take_queue_peak() > 0);
        assert_eq!(sender.take_queue_peak(), 0);
        drop(sender);
        drop(receiver);
        a.close().await;
        b.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn interrupted_control_read_cannot_resume_at_a_corrupted_boundary() {
    timeout(Duration::from_secs(15), async {
        let a = local_endpoint().await;
        let b = local_endpoint().await;
        let (client, mut server) = control_pair(&a, &b).await;
        assert!(
            timeout(Duration::from_millis(10), server.receiver.receive())
                .await
                .is_err()
        );
        assert!(server.receiver.receive().await.is_err());
        client.connection.close(0u32.into(), b"test ended");
        a.close().await;
        b.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn wrong_match_capability_and_peer_identity_are_rejected() {
    timeout(Duration::from_secs(15), async {
        let a = local_endpoint().await;
        let b = local_endpoint().await;
        for wrong_identity in [false, true] {
            let key = MatchKey {
                room: [13; 16],
                generation: 1,
            };
            let a_auth = GameAuthorization {
                peer: b.id(),
                key,
                capability: [1; 32],
                max_packet: 1024,
            };
            let b_auth = GameAuthorization {
                peer: if wrong_identity { b.id() } else { a.id() },
                capability: if wrong_identity { [1; 32] } else { [2; 32] },
                ..a_auth.clone()
            };
            let (client, server) = tokio::join!(connect_game(&a, address(&b), a_auth), async {
                accept_game(b.accept().await.unwrap().await.unwrap(), b_auth).await
            });
            assert!(client.is_err());
            assert!(server.is_err());
        }
        a.close().await;
        b.close().await;
    })
    .await
    .unwrap();
}

/// Both sides of one match: a loopback socket standing in for each GGPO,
/// bridged over the game connection until `stop` is sent.
struct BridgedPair {
    local_a: UdpSocket,
    local_b: UdpSocket,
    virtual_a: std::net::SocketAddr,
    virtual_b: std::net::SocketAddr,
    stats_b: std::sync::Arc<crate::bridge::BridgeStats>,
    stop: watch::Sender<bool>,
    a_task: tokio::task::JoinHandle<Result<(), crate::bridge::Failure>>,
    b_task: tokio::task::JoinHandle<Result<(), crate::bridge::Failure>>,
}

async fn bridged_pair(game_a: GameConnection, game_b: GameConnection) -> BridgedPair {
    let local_a = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap();
    let local_b = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap();
    let bridge_a = Bridge::bind(game_a, local_a.local_addr().unwrap())
        .await
        .unwrap();
    let bridge_b = Bridge::bind(game_b, local_b.local_addr().unwrap())
        .await
        .unwrap();
    let virtual_a = bridge_a.local_addr().unwrap();
    let virtual_b = bridge_b.local_addr().unwrap();
    let stats_b = bridge_b.stats.clone();
    let (stop, stop_rx) = watch::channel(false);
    BridgedPair {
        local_a,
        local_b,
        virtual_a,
        virtual_b,
        stats_b,
        stop,
        a_task: tokio::spawn(bridge_a.run(stop_rx.clone())),
        b_task: tokio::spawn(bridge_b.run(stop_rx)),
    }
}

#[tokio::test]
async fn raw_udp_survives_control_close_and_filters_stale_remote_and_local_packets() {
    timeout(Duration::from_secs(20), async {
        let a = local_endpoint().await;
        let b = local_endpoint().await;
        let (control_a, control_b) = control_pair(&a, &b).await;
        let (game_a, game_b) = game_pair(&a, &b, 1).await;
        let raw_sender = game_a.connection.clone();
        let BridgedPair {
            local_a,
            local_b,
            virtual_a,
            virtual_b,
            stats_b,
            stop,
            a_task,
            b_task,
        } = bridged_pair(game_a, game_b).await;
        control_a
            .connection
            .close(0u32.into(), b"room channel test");
        control_b.connection.closed().await;

        let stale = MatchKey {
            room: [13; 16],
            generation: 2,
        }
        .encode(&[88; 16], 1200)
        .unwrap();
        raw_sender.send_datagram(stale.into()).unwrap();
        let intruder = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap();
        intruder
            .send_to(b"wrong local port", virtual_a)
            .await
            .unwrap();

        // With no rendering or game-frame tick, packets still flow in both
        // directions and arrive from the registered virtual endpoint.
        let mut buffer = [0; 2048];
        for index in 0..100u32 {
            let mut payload = vec![0xA5; 1024];
            payload[..4].copy_from_slice(&index.to_be_bytes());
            local_a.send_to(&payload, virtual_a).await.unwrap();
            let (size, source) = local_b.recv_from(&mut buffer).await.unwrap();
            assert_eq!(source, virtual_b);
            assert_eq!(buffer[..size], payload);
            local_b.send_to(&payload, virtual_b).await.unwrap();
            let (size, source) = local_a.recv_from(&mut buffer).await.unwrap();
            assert_eq!(source, virtual_a);
            assert_eq!(buffer[..size], payload);
        }
        assert_eq!(stats_b.received_packets.load(Ordering::Relaxed), 100);
        assert!(stats_b.rejected_packets.load(Ordering::Relaxed) >= 1);
        stop.send(true).unwrap();
        a_task.await.unwrap().unwrap();
        b_task.await.unwrap().unwrap();
        assert!(raw_sender.close_reason().is_some());
        a.close().await;
        b.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn fifty_match_generations_reuse_endpoints_and_close_each_mapping() {
    timeout(Duration::from_secs(45), async {
        let a = local_endpoint().await;
        let b = local_endpoint().await;
        let (mut control_a, mut control_b) = control_pair(&a, &b).await;
        for generation in 1..=50 {
            let (game_a, game_b) = game_pair(&a, &b, generation).await;
            let BridgedPair {
                local_a,
                local_b,
                virtual_a,
                stop,
                a_task,
                b_task,
                ..
            } = bridged_pair(game_a, game_b).await;
            local_a
                .send_to(&generation.to_be_bytes(), virtual_a)
                .await
                .unwrap();
            let mut bytes = [0; 64];
            let count = local_b.recv(&mut bytes).await.unwrap();
            assert_eq!(bytes[..count], generation.to_be_bytes());
            stop.send(true).unwrap();
            // Either side can observe its peer's close before local stop.
            let _ = a_task.await.unwrap();
            let _ = b_task.await.unwrap();
        }
        let frame = ControlFrame {
            message_id: 500,
            payload: b"room still alive".to_vec(),
        };
        control_a.sender.send(&frame).await.unwrap();
        assert_eq!(control_b.receiver.receive().await.unwrap(), frame);
        a.close().await;
        b.close().await;
    })
    .await
    .unwrap();
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn closed_ggpo_socket_with_peer_traffic_keeps_the_runtime_responsive() {
    // F-008. Native GGPO closes its loopback socket at match end while the
    // other fighter is still sending. Under Wine a connected bridge socket then
    // stalled every worker, so no timer below could fire. A plain thread
    // bounds the test.
    // Set on every exit, including a panic, so the watchdog never ends the
    // whole test binary for a failure the harness already reported.
    struct Finished(std::sync::Arc<std::sync::atomic::AtomicBool>);
    impl Drop for Finished {
        fn drop(&mut self) {
            self.0.store(true, Ordering::Relaxed);
        }
    }
    let finished = Finished(std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false)));
    let watchdog = finished.0.clone();
    std::thread::spawn(move || {
        std::thread::sleep(std::time::Duration::from_secs(30));
        if !watchdog.load(Ordering::Relaxed) {
            eprintln!("helper runtime stalled after the GGPO socket closed");
            std::process::exit(101);
        }
    });
    let a = local_endpoint().await;
    let b = local_endpoint().await;
    let (game_a, game_b) = game_pair(&a, &b, 1).await;
    let BridgedPair {
        local_a,
        local_b,
        virtual_a,
        virtual_b,
        stop,
        a_task,
        b_task,
        ..
    } = bridged_pair(game_a, game_b).await;
    let (peer_stop, peer_stop_rx) = watch::channel(false);
    let peer = tokio::spawn(async move {
        let mut buffer = [0; 2048];
        while !*peer_stop_rx.borrow() {
            local_b.send_to(&[0x42; 200], virtual_b).await.unwrap();
            let _ = timeout(Duration::from_millis(16), local_b.recv_from(&mut buffer)).await;
        }
    });
    let mut buffer = [0; 2048];
    for _ in 0..30 {
        local_a.send_to(&[0x41; 200], virtual_a).await.unwrap();
        timeout(Duration::from_secs(5), local_a.recv_from(&mut buffer))
            .await
            .unwrap()
            .unwrap();
    }
    drop(local_a);
    // Timers keep firing while the peer's traffic meets the closed socket.
    // 20 wakes of 50 ms take about 1 s even where each sleep rounds up to the
    // 15.6 ms Windows timer tick; a stalled runtime misses the bound.
    let started = Instant::now();
    for _ in 0..20 {
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    assert!(started.elapsed() < Duration::from_secs(5));
    // The peer stops first, so it never sends to a bridge that has closed.
    peer_stop.send(true).unwrap();
    timeout(Duration::from_secs(5), peer)
        .await
        .unwrap()
        .unwrap();
    stop.send(true).unwrap();
    timeout(Duration::from_secs(5), a_task)
        .await
        .unwrap()
        .unwrap()
        .unwrap();
    timeout(Duration::from_secs(5), b_task)
        .await
        .unwrap()
        .unwrap()
        .unwrap();
    drop(finished);
    a.close().await;
    b.close().await;
}

/// Against the live relays, by hand only (needs the internet):
/// `cargo test --lib live_relays -- --ignored --nocapture`. For each relay
/// this build uses: an endpoint with that relay alone gets it as its home
/// relay, learns its public address from the relay's QUIC address discovery
/// and sees no captive portal; then two relay-only endpoints connect through
/// it and exchange a stream.
#[tokio::test]
#[ignore = "needs the internet and the live relays"]
async fn live_relays_answer_probes_and_carry_a_relay_only_connection() {
    use iroh::{RelayMap, RelayUrl, Watcher as _};
    use std::time::Duration;
    const ALPN: &[u8] = b"sf4e/live-relay-check/1";
    for relay in crate::invite::relay_map().urls::<Vec<RelayUrl>>() {
        let only = || {
            Endpoint::builder(presets::N0)
                .relay_mode(RelayMode::Custom(RelayMap::from_iter([
                    iroh::RelayConfig::from(relay.clone()),
                ])))
                .alpns(vec![ALPN.to_vec()])
                .portmapper_config(PortmapperConfig::Disabled)
        };
        let probe = only().bind().await.unwrap();
        let mut reports = probe.net_report();
        let report = timeout(Duration::from_secs(30), async {
            loop {
                if let Some(report) = reports.get()
                    && report.preferred_relay.is_some()
                    && report.global_v4.is_some()
                {
                    return report;
                }
                reports.updated().await.unwrap();
            }
        })
        .await
        .unwrap_or_else(|_| panic!("{relay}: no complete net report: {:?}", reports.get()));
        println!(
            "{relay}: preferred {:?}, global v4 {}, captive portal {:?}",
            report.preferred_relay,
            report.global_v4.is_some(),
            report.captive_portal
        );
        assert_eq!(report.preferred_relay.as_ref(), Some(&relay), "{relay}");
        assert!(report.global_v4.is_some(), "{relay}: no QUIC address discovery");
        // iroh checks for a captive portal only when its probes are slow, so
        // an unset answer is fine; a portal seen on our own relay is not.
        assert_ne!(report.captive_portal, Some(true), "{relay}");
        probe.close().await;

        let a = only().clear_ip_transports().bind().await.unwrap();
        let b = only().clear_ip_transports().bind().await.unwrap();
        timeout(Duration::from_secs(20), async {
            tokio::join!(a.online(), b.online());
        })
        .await
        .unwrap_or_else(|_| panic!("{relay}: endpoints did not come online"));
        let target = EndpointAddr::new(b.id()).with_relay_url(relay.clone());
        let accept = tokio::spawn({
            let b = b.clone();
            async move {
                let connection = b.accept().await.unwrap().await.unwrap();
                let mut stream = connection.accept_uni().await.unwrap();
                let bytes = stream.read_to_end(64).await.unwrap();
                connection.close(0u32.into(), b"done");
                bytes
            }
        });
        let connection = timeout(Duration::from_secs(20), a.connect(target, ALPN))
            .await
            .unwrap_or_else(|_| panic!("{relay}: connect timed out"))
            .unwrap();
        let mut stream = connection.open_uni().await.unwrap();
        stream.write_all(b"ember").await.unwrap();
        stream.finish().unwrap();
        let received = timeout(Duration::from_secs(20), accept)
            .await
            .unwrap_or_else(|_| panic!("{relay}: nothing arrived"))
            .unwrap();
        assert_eq!(received, b"ember", "{relay}");
        connection.closed().await;
        a.close().await;
        b.close().await;
    }
}

#[tokio::test]
async fn active_relays_answer_any_id_with_every_relay() {
    use iroh::address_lookup::AddressLookup as _;
    use n0_future::StreamExt as _;
    let id = iroh::SecretKey::generate().public();
    let mut items = ActiveRelays.resolve(id).expect("an answer for any id");
    let item = items.next().await.expect("one item").expect("no error");
    assert_eq!(item.endpoint_id(), id);
    let relays: std::collections::BTreeSet<_> =
        item.endpoint_info().relay_urls().cloned().collect();
    let active: std::collections::BTreeSet<_> =
        crate::invite::relay_map().urls::<Vec<_>>().into_iter().collect();
    assert_eq!(relays, active);
    assert_eq!(item.endpoint_info().ip_addrs().count(), 0);
    assert!(items.next().await.is_none());
}

/// By hand only (needs the internet): a dialer with `ActiveRelays` and no
/// n0 lookup at all, as a player whose router filter blocks dns.iroh.link,
/// finds a peer by its id alone, as a match dials an opponent who is not the
/// room host. Without `ActiveRelays` that dial fails with "No addressing
/// information available" (the 1.5.5 match-start failure).
#[tokio::test]
#[ignore = "needs the internet and the live relays"]
async fn live_a_dialer_without_n0_lookup_finds_a_peer_by_id() {
    use std::time::Duration;
    const ALPN: &[u8] = b"sf4e/live-id-dial/1";
    let filtered = || {
        Endpoint::builder(presets::Minimal)
            .relay_mode(RelayMode::Custom(crate::invite::relay_map()))
            .alpns(vec![ALPN.to_vec()])
    };
    let bare = filtered().bind().await.unwrap();
    let a = filtered().address_lookup(ActiveRelays).bind().await.unwrap();
    let b = filtered().bind().await.unwrap();
    timeout(Duration::from_secs(20), async {
        tokio::join!(bare.online(), a.online(), b.online());
    })
    .await
    .expect("online");
    let failed = timeout(Duration::from_secs(10), bare.connect(EndpointAddr::new(b.id()), ALPN)).await;
    assert!(matches!(failed, Ok(Err(_))), "without ActiveRelays: {failed:?}");
    let accept = tokio::spawn({
        let b = b.clone();
        async move { b.accept().await.unwrap().await.map(|c| c.remote_id()) }
    });
    let connection = timeout(Duration::from_secs(20), a.connect(EndpointAddr::new(b.id()), ALPN))
        .await
        .expect("dial timed out")
        .expect("dial by id with ActiveRelays");
    assert_eq!(connection.remote_id(), b.id());
    let accepted = timeout(Duration::from_secs(5), accept).await.unwrap().unwrap().unwrap();
    assert_eq!(accepted, a.id());
}
