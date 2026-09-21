//! Gameplay links: incoming connections, stream classification, bridges and their end.
use super::*;

impl Actor {
    pub(super) async fn completed_incoming(
        &mut self,
        epoch: u64,
        result: io::Result<Connection>,
    ) -> io::Result<()> {
        if let Ok(connection) = result {
            if epoch != self.epoch || self.tasks.len() >= MAX_TASKS {
                connection.close(1u32.into(), b"obsolete room");
                return Ok(());
            }
            let peer = connection.remote_id();
            if connection.alpn() == CONTROL_ALPN {
                self.remove_closed_control(peer);
                if let Some(invite) = self
                    .room_invite
                    .clone()
                    .or_else(|| self.hosted.clone())
                    .filter(|_| {
                        self.controls.len() < MAX_CONTROL_PEERS || self.controls.contains_key(&peer)
                    })
                {
                    let member = self.control_rejoin_member(peer);
                    let recovery = self.recovery.clone();
                    self.tasks.spawn(async move {
                        if let (Some(incarnation), Some(recovery)) = (member, recovery) {
                            let result =
                                if recovery.applied_member_ids().await.contains(&incarnation) {
                                    transport::accept_control_member(connection, &invite, peer)
                                        .await
                                } else {
                                    connection.close(1u32.into(), b"member retired");
                                    Err(failed("member retired"))
                                };
                            Completion::MemberControl(epoch, incarnation, result)
                        } else {
                            Completion::Control(
                                epoch,
                                transport::accept_control(connection, &invite).await,
                            )
                        }
                    });
                } else {
                    connection.close(1u32.into(), b"room unavailable");
                }
            } else if connection.alpn() == GAME_ALPN {
                let generation = self
                    .games
                    .get(&peer)
                    .filter(|slot| slot.waiting)
                    .map(|slot| slot.auth.key.generation);
                let probe = self
                    .probe_permissions
                    .get(&peer)
                    .filter(|p| p.expires > tokio::time::Instant::now())
                    .map(|p| (p.request, p.pair_revision));
                if (generation.is_none() && probe.is_none())
                    || self.pending_game_admissions.contains_key(&peer)
                {
                    connection.close(1u32.into(), b"gameplay not authorized");
                    return Ok(());
                }
                let admission_connection = connection.clone();
                let incoming = IncomingGame {
                    epoch,
                    peer,
                    connection,
                    generation,
                    probe,
                    deadline: tokio::time::Instant::now() + transport::HANDSHAKE_TIMEOUT,
                };
                let task = self.tasks.spawn(async move {
                    let result = transport::accept_game_stream_until(
                        &incoming.connection,
                        incoming.deadline,
                    )
                    .await;
                    Completion::ClassifiedGame(incoming, result)
                });
                self.pending_game_admissions
                    .insert(peer, (admission_connection, task));
            } else {
                connection.close(1u32.into(), b"unsupported protocol");
            }
        }
        Ok(())
    }

    pub(super) async fn completed_classified_game(
        &mut self,
        incoming: IncomingGame,
        result: io::Result<transport::GameStream>,
    ) -> io::Result<()> {
        let IncomingGame {
            epoch,
            peer,
            connection,
            generation,
            probe,
            deadline,
        } = incoming;
        if epoch != self.epoch {
            connection.close(1u32.into(), b"obsolete room");
            return Ok(());
        }
        if !self
            .pending_game_admissions
            .get(&peer)
            .is_some_and(|(pending, _)| pending.stable_id() == connection.stable_id())
        {
            connection.close(1u32.into(), b"superseded game admission");
            return Ok(());
        }
        self.pending_game_admissions.remove(&peer);
        let Ok(mode) = result else {
            return Ok(());
        };
        let authorized = tokio::time::Instant::now() < deadline
            && match &mode {
                transport::GameStream::Gameplay(_, _) => {
                    self.games.get(&peer).is_some_and(|slot| {
                        slot.waiting && Some(slot.auth.key.generation) == generation
                    })
                }
                transport::GameStream::Probe(_, _) => {
                    self.probe_permissions.get(&peer).is_some_and(|p| {
                        p.expires > tokio::time::Instant::now()
                            && Some((p.request, p.pair_revision)) == probe
                    })
                }
            };
        if !authorized {
            connection.close(1u32.into(), b"obsolete game admission");
            return Ok(());
        }
        match mode {
            transport::GameStream::Probe(send, recv) if self.probe_peers.contains(&peer) => {
                let room = self.room.unwrap_or([0; 16]);
                let (request, pair_revision) = self
                    .probe_permissions
                    .get(&peer)
                    .filter(|permission| permission.expires > tokio::time::Instant::now())
                    .map(|permission| (permission.request, permission.pair_revision))
                    .unwrap_or((0, 0));
                self.tasks.spawn(async move {
                    let result =
                        match serve_probe(connection, send, recv, room, request, pair_revision)
                            .await
                        {
                            Ok(connection) => Ok(ProbeCompletion {
                                peer,
                                request,
                                pair_revision,
                                samples_us: Vec::new(),
                                connection: Some(connection),
                                report: false,
                                route_changed: false,
                                metrics: crate::probe::Metrics::default(),
                            }),
                            Err(_) => Ok(ProbeCompletion {
                                peer,
                                request,
                                pair_revision,
                                samples_us: Vec::new(),
                                connection: None,
                                report: false,
                                route_changed: false,
                                metrics: crate::probe::Metrics::default(),
                            }),
                        };
                    Completion::Probe(epoch, result)
                });
            }
            transport::GameStream::Gameplay(send, recv) => {
                if let Some(slot) = self.games.get_mut(&peer).filter(|slot| slot.waiting) {
                    slot.waiting = false;
                    let auth = slot.auth.clone();
                    let generation = auth.key.generation;
                    slot.task = Some(self.tasks.spawn(async move {
                        Completion::Game(
                            epoch,
                            peer,
                            generation,
                            transport::accept_game_stream_with_until(
                                connection, auth, send, recv, deadline,
                            )
                            .await,
                        )
                    }));
                } else {
                    connection.close(1u32.into(), b"gameplay not authorized");
                }
            }
            transport::GameStream::Probe(send, recv) => {
                drop(send);
                drop(recv);
                connection.close(1u32.into(), b"probe not reserved");
            }
        }
        Ok(())
    }

    pub(super) async fn completed_game(
        &mut self,
        epoch: u64,
        peer: EndpointId,
        generation: u64,
        result: io::Result<GameConnection>,
    ) -> io::Result<()> {
        let valid = epoch == self.epoch
            && self
                .games
                .get(&peer)
                .is_some_and(|slot| slot.auth.key.generation == generation && slot.stats.is_none());
        if !valid {
            return Ok(());
        } // GameConnection drop closes stale results.
        let local_port = self.games[&peer].local_port;
        let bridge = match result {
            Ok(game) => {
                let route = selected_probe_route(&game.connection);
                let route_connection = game.connection.clone();
                Bridge::bind(game, SocketAddr::from((Ipv4Addr::LOCALHOST, local_port)))
                    .await
                    .map(|bridge| (bridge, route, route_connection))
            }
            Err(error) => Err(error),
        };
        match bridge {
            Ok((bridge, route, route_connection)) => {
                let virtual_port = bridge.local_addr()?.port();
                // `valid` proved the slot; `&mut self` is held across the bind.
                let Some(slot) = self.games.get_mut(&peer) else {
                    return Ok(());
                };
                slot.prepare_deadline = None;
                slot.stats = Some(bridge.stats.clone());
                slot.route_connection = Some(route_connection);
                let max_packet = slot.auth.max_packet;
                slot.task = Some(self.tasks.spawn(async move {
                    let (_keep_running, stop) = watch::channel(false);
                    let failure = bridge.run(stop).await.err();
                    Completion::BridgeEnded(epoch, peer, generation, failure)
                }));
                self.emit(Event::GameReady {
                    epoch,
                    peer,
                    generation,
                    virtual_port,
                    max_packet,
                    route,
                })?;
            }
            Err(_) => {
                self.games.remove(&peer);
                self.error(0, "gameplay_prepare_failed")?;
                self.emit(Event::GameClosed {
                    epoch,
                    peer,
                    generation,
                })?;
            }
        }
        Ok(())
    }

    pub(super) async fn completed_bridge_ended(
        &mut self,
        epoch: u64,
        peer: EndpointId,
        generation: u64,
        failure: Option<crate::bridge::Failure>,
    ) -> io::Result<()> {
        if epoch == self.epoch
            && self
                .games
                .get(&peer)
                .is_some_and(|slot| slot.auth.key.generation == generation)
        {
            if let Some(slot) = self.games.remove(&peer) {
                let route = slot
                    .route_connection
                    .as_ref()
                    .map(selected_probe_route)
                    .unwrap_or_else(|| "unavailable".into());
                if let Some(stats) = slot.stats.as_ref() {
                    self.emit(Event::Statistics {
                        epoch,
                        peer,
                        generation,
                        sent_packets: stats.sent_packets.load(Ordering::Relaxed),
                        received_packets: stats.received_packets.load(Ordering::Relaxed),
                        sent_bytes: stats.sent_bytes.load(Ordering::Relaxed),
                        received_bytes: stats.received_bytes.load(Ordering::Relaxed),
                        rejected_packets: stats.rejected_packets.load(Ordering::Relaxed),
                        congestion_events: stats.congestion_events.load(Ordering::Relaxed),
                        local_drops: stats.local_drops.load(Ordering::Relaxed),
                        route: route.clone(),
                    })?;
                }
                if let Some(reason) = failure {
                    self.emit(Event::GameFailed {
                        epoch,
                        peer,
                        generation,
                        reason,
                        route,
                        max_packet: slot.auth.max_packet,
                        max_datagram: slot
                            .route_connection
                            .as_ref()
                            .and_then(Connection::max_datagram_size)
                            .unwrap_or(0),
                    })?;
                }
            }
            self.emit(Event::GameClosed {
                epoch,
                peer,
                generation,
            })?;
        }
        Ok(())
    }
}
