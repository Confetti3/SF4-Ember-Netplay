//! Handshake dials, parked controls and flushed departures.

use super::*;

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
    phase(
        "accept_completes_only_after_the_dialer_confirms",
        Duration::from_secs(20),
        async {
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
            let accepted =
                tokio::spawn(transport::accept_game(listener_end, auth(&dialer_endpoint)));
            let (send, _) = unconfirmed_dial(&dialer_end, &listener_endpoint, [87; 16]).await;
            dialer_end.close(1u32.into(), b"dial abandoned");
            drop(send);
            assert!(accepted.await.unwrap().is_err());
            listener_endpoint.close().await;
            dialer_endpoint.close().await;
        },
    )
    .await;
}

/// A dial that has read the listener's reply and then goes away never
/// confirms. Dropping its send stream must reset it rather than finish it, or
/// the listener would take the abandoned connection for the link. The slot
/// keeps waiting, and a later valid dial forms the link.
#[tokio::test]
async fn an_abandoned_dial_is_never_confirmed_and_the_slot_keeps_waiting() {
    phase(
        "an_abandoned_dial_is_never_confirmed_and_the_slot_keeps_waiting",
        Duration::from_secs(20),
        async {
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
        },
    )
    .await;
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
    phase(
        "flushed_departure_behind_more_than_one_poll_budget",
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
            phase(
                "the remote control closes",
                Duration::from_secs(10),
                async {
                    while !fixture.actor.controls[&remote_id].is_closed() {
                        tokio::time::sleep(Duration::from_millis(10)).await;
                    }
                },
            )
            .await;
            // Let the reader queue what the closed stream still holds.
            tokio::time::sleep(Duration::from_millis(300)).await;

            let mut order: Vec<String> = Vec::new();
            // One poll delivers one budget of the queued frames.
            fixture.actor.poll_controls().await.unwrap();
            drain(&mut fixture.events, &mut order);
            assert!(!order.iter().any(|entry| entry == "departed"));
            // Nothing signals the frames past the budget again, so the next tick
            // comes soon rather than at the room's slow period.
            assert!(fixture.actor.controls[&remote_id].has_queued());
            assert_eq!(fixture.actor.tick_period(), FAST_TICK);

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
                    position(&format!("connected {replacement_id}"))
                        .expect("replacement connected"),
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
        },
    )
    .await;
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
    phase(
        "a_parked_replacement_cannot_evict_a_newer_installed_control",
        Duration::from_secs(60),
        async {
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
            phase(
                "the remote control closes",
                Duration::from_secs(10),
                async {
                    while !fixture.actor.controls[&remote_id].is_closed() {
                        tokio::time::sleep(Duration::from_millis(10)).await;
                    }
                },
            )
            .await;
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
            phase(
                "the remote control drains",
                Duration::from_secs(10),
                async {
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
                },
            )
            .await;

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
            phase(
                "B closes when C supersedes it",
                Duration::from_secs(10),
                async {
                    while b_connection.close_reason().is_none() {
                        tokio::time::sleep(Duration::from_millis(10)).await;
                    }
                },
            )
            .await;

            // With B gone there is nothing left to time out and replace C.
            tokio::time::sleep(Duration::from_millis(50)).await;
            fixture.actor.poll_controls().await.unwrap();
            assert_eq!(fixture.actor.controls[&remote_id].id(), c_id);
            assert!(fixture.actor.parked_controls.is_empty());

            drop((remote_b, remote_c));
            fixture.stop().await;
        },
    )
    .await;
}

/// Settling fences a parked candidate by the worker it waits behind: a worker
/// installed for the peer after the candidate parked is never replaced by it.
#[tokio::test]
async fn a_parked_candidate_is_dropped_when_a_newer_worker_stands() {
    phase(
        "a_parked_candidate_is_dropped_when_a_newer_worker_stands",
        Duration::from_secs(60),
        async {
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
            phase(
                "the stale candidate closes",
                Duration::from_secs(10),
                async {
                    while b_connection.close_reason().is_none() {
                        tokio::time::sleep(Duration::from_millis(10)).await;
                    }
                },
            )
            .await;
            drop((remote_a, remote_b, remote_c));
            fixture.stop().await;
        },
    )
    .await;
}

/// Connections that fail validation do not end a listener's slot: the wrong
/// generation, then the wrong capability. The window is still open for a valid
/// dial, which forms the link.
#[tokio::test]
async fn a_rejected_candidate_leaves_the_slot_waiting_for_a_valid_dial() {
    phase(
        "a_rejected_candidate_leaves_the_slot_waiting_for_a_valid_dial",
        Duration::from_secs(30),
        async {
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
        },
    )
    .await;
}
