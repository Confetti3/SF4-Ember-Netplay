//! Game links and control routing.

use super::*;

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
            .command_now(Request {
                id: 17,
                command: Command::Status
            })
            .await
            .unwrap()
    );
    assert!(matches!(
        receiver.recv().await,
        Some(Event::Status { request_id: 17, .. })
    ));
    assert!(
        actor
            .command_now(Request {
                id: 19,
                command: Command::EndMatch {
                    epoch: 7,
                    generation: 3
                }
            })
            .await
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
            .command_now(Request {
                id: 18,
                command: Command::Shutdown
            })
            .await
            .unwrap()
    );
    drop(client);
    local.close().await;
    remote.close().await;
}

#[tokio::test]
async fn closing_room_does_not_poison_new_room_on_same_endpoint() {
    let host = endpoint().await;
    let relay = test_relay();
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
        epoch: 1,
        room: Some(old.room()),
        hosted: Some(old.clone()),
        room_invite: Some(old.clone()),
        closed_generation: 99,
        ..test_actor(host.clone(), events_tx)
    };
    actor
        .command_now(Request {
            id: 1,
            command: Command::Leave {
                epoch: 1,
                abandon: false,
            },
        })
        .await
        .unwrap();
    let _ = next(&mut events, "room_closed").await;
    assert!(actor.begin(2, "test-build", crate::transport::Transport::Online).await);
    actor
        .completed(Completion::Hosted(2, Ok(replacement.clone()), None))
        .await
        .unwrap();
    let _ = next(&mut events, "hosted").await;
    let _ = next(&mut events, "discord_invite").await;
    // An old leave request and a late old-host completion cannot close or
    // replace the new room, even though its endpoint is unchanged.
    actor
        .command_now(Request {
            id: 2,
            command: Command::Leave {
                epoch: 1,
                abandon: false,
            },
        })
        .await
        .unwrap();
    assert!(
        matches!(events.recv().await, Some(Event::Error { code, .. }) if code == "stale_epoch")
    );
    actor
        .completed(Completion::Hosted(1, Ok(old.clone()), None))
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
    phase(
        "actor_routes_cpp_control_and_keeps_gameplay_alive_when_control_closes",
        Duration::from_secs(25),
        async {
            let host = endpoint().await;
            let remote = endpoint().await;
            let relay = test_relay();
            let invite =
                Invite::create(host.id(), relay, "test-build".into(), now().unwrap(), 3600)
                    .unwrap();
            let room = invite.room();
            let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (_fault, failure) = watch::channel(false);
            let mut actor = Actor {
                epoch: 1,
                room: Some(room),
                hosted: Some(invite.clone()),
                room_invite: Some(invite.clone()),
                ..test_actor(host.clone(), events_tx)
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
            let native: NativeControlMessage =
                serde_json::from_slice(&native_frame.payload).unwrap();
            assert_eq!(native.kind, "native_control");
            assert_eq!(native.message_id, 51);
            assert_eq!(native.payload, "cpp authoritative reply");
            let _ = next(&mut events, "sent").await;

            // A send named for a control this endpoint no longer holds is
            // refused and says why; one named for the current control is sent.
            for (id, named, message_id) in
                [(90, control_id.wrapping_add(1), 52), (91, control_id, 53)]
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
            // A link the peer closes names the peer's own close reason.
            commands
                .send(Request {
                    id: 9,
                    command: Command::PrepareGame {
                        epoch: 1,
                        peer: remote.id(),
                        room,
                        generation: 3,
                        capability: [10; 32],
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
                    generation: 3,
                },
                capability: [10; 32],
                max_packet: 1024,
            };
            let game = transport::connect_game(&remote, address(&host), auth)
                .await
                .unwrap();
            let _ = next(&mut events, "game_ready").await;
            drop(game);
            match next(&mut events, "game_closed").await {
                Event::GameClosed { reason, .. } => {
                    let reason = reason.unwrap();
                    assert!(
                        reason.starts_with("closed by peer") && reason.contains("match ended"),
                        "{reason}"
                    );
                }
                _ => unreachable!(),
            }
            commands
                .send(Request {
                    id: 10,
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
                    id: 11,
                    command: Command::Host {
                        transport: crate::transport::RoomKind::Online,
                        epoch: 2,
                        build: "test-build".into(),
                    },
                })
                .await
                .unwrap();
            commands
                .send(Request {
                    id: 12,
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
                    id: 13,
                    command: Command::Shutdown,
                })
                .await
                .unwrap();
            service.await.unwrap().unwrap();
            host.close().await;
            remote.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn actor_admits_full_sixteen_member_room_and_fifteen_game_links() {
    phase(
        "actor_admits_full_sixteen_member_room_and_fifteen_game_links",
        Duration::from_secs(60),
        async {
            // The host plus fifteen helpers is the transport shape of a
            // sixteen-member room. One gameplay endpoint may represent the
            // second fighter and fourteen admitted spectators for a table.
            assert_eq!(MAX_CONTROL_PEERS, 15);
            assert_eq!(MAX_GAME_LINKS, 15);
            const _: () = assert!(MAX_TASKS >= MAX_CONTROL_PEERS + MAX_GAME_LINKS);

            let host = endpoint().await;
            let relay = test_relay();
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
                epoch: 1,
                room: Some(room),
                hosted: Some(invite.clone()),
                room_invite: Some(invite.clone()),
                ..test_actor(host.clone(), events_tx)
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
        },
    )
    .await;
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
    phase(
        "a_stale_probe_connection_never_blocks_the_next_game",
        Duration::from_secs(30),
        async {
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
        },
    )
    .await;
}

#[tokio::test]
async fn failed_gameplay_link_reports_its_cause_and_what_the_listener_saw() {
    phase(
        "failed_gameplay_link_reports_its_cause_and_what_the_listener_saw",
        Duration::from_secs(20),
        async {
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
        },
    )
    .await;
}
