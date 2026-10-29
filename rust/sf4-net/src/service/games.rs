//! Gameplay links: incoming connections, stream classification, bridges and their end.
use super::*;

pub(super) struct GameSlot {
    pub(super) auth: GameAuthorization,
    pub(super) local_port: u16,
    pub(super) waiting: bool,
    pub(super) task: Option<AbortHandle>,
    pub(super) stats: Option<Arc<BridgeStats>>,
    pub(super) route_connection: Option<Connection>,
    pub(super) expires: tokio::time::Instant,
    /// Stable id of the accepted connection whose handshake is running as
    /// `task`. Candidates handshake one at a time; only this one's completion
    /// counts, and one that arrives after it was retired is ignored.
    pub(super) candidate: Option<usize>,
    pub(super) diagnostics: GameDiagnostics,
}

/// What a gameplay slot saw before it linked. It is reported with the slot's
/// close or failure so a link that never formed can be told from one that broke.
#[derive(Default)]
pub(super) struct GameDiagnostics {
    pub(super) refused_incoming: u32,
    /// Accepted connections that failed their handshake without ending the slot.
    pub(super) rejected_candidates: u32,
}

impl GameDiagnostics {
    pub(super) fn describe(&self, waiting: bool) -> String {
        format!(
            "waiting={} refused_incoming={} rejected_candidates={}",
            waiting, self.refused_incoming, self.rejected_candidates
        )
    }
}

impl GameSlot {
    /// Why a slot closed without a gameplay link, or `None` once it had one.
    pub(super) fn unlinked_reason(&self, cause: &str) -> Option<String> {
        self.stats
            .is_none()
            .then(|| format!("{cause}: {}", self.diagnostics.describe(self.waiting)))
    }
}

impl Drop for GameSlot {
    fn drop(&mut self) {
        if let Some(task) = &self.task {
            task.abort();
        }
    }
}

impl Actor {
    pub(super) async fn completed_incoming(
        &mut self,
        epoch: u64,
        result: io::Result<Connection>,
    ) -> io::Result<()> {
        if let Ok(connection) = result {
            if epoch != self.epoch {
                connection.close(1u32.into(), b"obsolete room");
                return Ok(());
            }
            let peer = connection.remote_id();
            if connection.alpn() == crate::lan::ALPN {
                self.lan.accept(connection, self.room_invite.as_ref().or(self.hosted.as_ref()));
                return Ok(());
            }
            if self.tasks.len() >= MAX_TASKS {
                connection.close(1u32.into(), b"room busy");
                return Ok(());
            }
            if connection.alpn() == CONTROL_ALPN && self.server_owned() {
                self.accept_public_incoming(epoch, connection);
            } else if connection.alpn() == CONTROL_ALPN && !self.admits_control(&peer) {
                connection.close(1u32.into(), b"not admitted");
            } else if connection.alpn() == CONTROL_ALPN {
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
                    let watchers = self.takes_observers();
                    self.tasks.spawn(async move {
                        if let (Some(incarnation), Some(recovery)) = (member, recovery) {
                            let result = if recovery
                                .coordinator
                                .store()
                                .applied_members()
                                .await
                                .contains(&incarnation)
                            {
                                let accepting = transport::Accepting {
                                    member: Some(peer),
                                    watchers,
                                };
                                transport::accept_control_for(connection, &invite, accepting).await
                            } else {
                                connection.close(1u32.into(), b"member retired");
                                Err(failed("member retired"))
                            };
                            Completion::MemberControl(epoch, incarnation, result)
                        } else {
                            let accepting = transport::Accepting {
                                member: None,
                                watchers,
                            };
                            Completion::Control(
                                epoch,
                                transport::accept_control_for(connection, &invite, accepting).await,
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
                    .filter(|slot| slot.waiting && slot.candidate.is_none())
                    .map(|slot| slot.auth.key.generation);
                let probe = self
                    .probe_permissions
                    .get(&peer)
                    .filter(|p| p.expires > tokio::time::Instant::now())
                    .map(|p| (p.request, p.pair_revision));
                if (generation.is_none() && probe.is_none())
                    || self.pending_game_admissions.contains_key(&peer)
                {
                    if let Some(slot) = self.games.get_mut(&peer) {
                        slot.diagnostics.refused_incoming =
                            slot.diagnostics.refused_incoming.saturating_add(1);
                    }
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
                        slot.waiting
                            && slot.candidate.is_none()
                            && Some(slot.auth.key.generation) == generation
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
                    let connection =
                        serve_probe(connection, send, recv, room, request, pair_revision)
                            .await
                            .ok();
                    let result = Ok(ProbeCompletion {
                        peer,
                        request,
                        pair_revision,
                        samples_us: Vec::new(),
                        connection,
                        report: false,
                        route_changed: false,
                        route: None,
                        metrics: crate::probe::Metrics::default(),
                    });
                    Completion::Probe(epoch, result)
                });
            }
            transport::GameStream::Gameplay(send, recv) => {
                if let Some(slot) = self
                    .games
                    .get_mut(&peer)
                    .filter(|slot| slot.waiting && slot.candidate.is_none())
                {
                    // Only a marker has arrived. The proof, generation,
                    // capability and confirmation are validated by this
                    // handshake, so the slot keeps waiting until the candidate
                    // finishes; a rejected candidate never ends it.
                    let candidate = connection.stable_id();
                    slot.candidate = Some(candidate);
                    let auth = slot.auth.clone();
                    let generation = auth.key.generation;
                    slot.task = Some(self.tasks.spawn(async move {
                        Completion::GameCandidate(
                            epoch,
                            peer,
                            generation,
                            candidate,
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
                let fixed_port =
                    transport::fixed_port::selected_path_uses_fixed_port(&game.connection);
                let route_connection = game.connection.clone();
                Bridge::bind(game, SocketAddr::from((Ipv4Addr::LOCALHOST, local_port)))
                    .await
                    .map(|bridge| (bridge, route, fixed_port, route_connection))
            }
            Err(error) => Err(error),
        };
        match bridge {
            Ok((bridge, route, fixed_port, route_connection)) => {
                let virtual_port = bridge.local_addr()?.port();
                // `valid` proved the slot; `&mut self` is held across the bind.
                let Some(slot) = self.games.get_mut(&peer) else {
                    return Ok(());
                };
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
                    fixed_port,
                })?;
            }
            Err(error) => {
                let detail = self.games.remove(&peer).map(|slot| {
                    format!(
                        "link failed: {error}; {}",
                        slot.diagnostics.describe(slot.waiting)
                    )
                });
                let reason = detail.unwrap_or_else(|| error.to_string());
                self.emit(Event::Error {
                    request_id: 0,
                    epoch: self.epoch,
                    probe_failure: None,
                    peer: Some(peer),
                    code: "gameplay_prepare_failed".into(),
                    reason: Some(reason.clone()),
                })?;
                self.emit(Event::GameClosed {
                    epoch,
                    peer,
                    generation,
                    reason: Some(reason),
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
            && let Some(slot) = self.games.remove(&peer)
        {
            let route = slot
                .route_connection
                .as_ref()
                .map(selected_probe_route)
                .unwrap_or_else(|| "unavailable".into());
            if let Some(stats) = slot.stats.as_ref() {
                self.emit(statistics_event(
                    epoch,
                    peer,
                    generation,
                    stats,
                    route.clone(),
                ))?;
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
            self.emit(Event::GameClosed {
                epoch,
                peer,
                generation,
                reason: slot
                    .route_connection
                    .as_ref()
                    .and_then(Connection::close_reason)
                    .map(|error| error.to_string()),
            })?;
        }
        Ok(())
    }

    /// An accepted connection's handshake ended. Only the candidate the slot is
    /// running counts. A win becomes the generation's link. A rejection ends only
    /// that candidate: the slot keeps waiting until its own deadline, so an
    /// invalid or obsolete attempt cannot close a valid pending link.
    pub(super) async fn completed_game_candidate(
        &mut self,
        epoch: u64,
        peer: EndpointId,
        generation: u64,
        candidate: usize,
        result: io::Result<GameConnection>,
    ) -> io::Result<()> {
        let Some(slot) = self.games.get_mut(&peer).filter(|slot| {
            epoch == self.epoch
                && slot.auth.key.generation == generation
                && slot.stats.is_none()
                && slot.candidate == Some(candidate)
        }) else {
            return Ok(()); // GameConnection drop closes stale results.
        };
        slot.candidate = None;
        slot.task = None;
        match result {
            Ok(game) => {
                slot.waiting = false;
                self.completed_game(epoch, peer, generation, Ok(game)).await
            }
            Err(_) => {
                slot.diagnostics.rejected_candidates =
                    slot.diagnostics.rejected_candidates.saturating_add(1);
                Ok(())
            }
        }
    }
}

impl Actor {
    /// `Command::PrepareGame`: a gameplay slot for `peer`, dialed or
    /// waited for.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn prepare_game(
        &mut self,
        id: u64,
        epoch: u64,
        peer: EndpointId,
        room: [u8; 16],
        generation: u64,
        capability: [u8; 32],
        local_port: u16,
        max_packet: usize,
        dial: bool,
    ) -> io::Result<()> {
        if !self.matches(epoch)
            || self.room != Some(room)
            || generation <= self.closed_generation
            || generation > i64::MAX as u64
            || room == [0; 16]
            || capability == [0; 32]
            || local_port == 0
            || max_packet == 0
            || max_packet > wire::MAX_UDP_PAYLOAD
            || peer == self.endpoint.id()
            || self.games.contains_key(&peer)
            || self.games.len() >= MAX_GAME_LINKS
            || self
                .games
                .values()
                .any(|game| game.auth.key.generation != generation)
            || self.tasks.len() >= MAX_TASKS
        {
            self.error(id, "invalid_game_registration")?;
            return Ok(());
        }
        let auth = GameAuthorization {
            peer,
            key: MatchKey { room, generation },
            capability,
            max_packet,
        };
        let mut slot = GameSlot {
            auth: auth.clone(),
            local_port,
            // The listener accepts an authorized connection from the
            // dialer, which always dials a new one.
            waiting: !dial,
            task: None,
            stats: None,
            route_connection: None,
            expires: tokio::time::Instant::now() + transport::PREPARED_GAME_TIMEOUT,
            candidate: None,
            diagnostics: GameDiagnostics::default(),
        };
        if dial {
            let endpoint = self.endpoint.clone();
            let address = self
                .host_address
                .as_ref()
                .filter(|a| a.id == peer)
                .cloned()
                .unwrap_or_else(|| EndpointAddr::new(peer));
            slot.task = Some(self.tasks.spawn(async move {
                let result = transport::connect_game(&endpoint, address, auth).await;
                Completion::Game(epoch, peer, generation, result)
            }));
        }
        self.games.insert(peer, slot);
        // Preparation already suspends latency (`Unavailable(InMatch)`).
        self.latency_games_changed();
        self.emit(Event::GameWaiting {
            epoch,
            peer,
            generation,
        })?;
        Ok(())
    }

    /// `Command::EndMatch`: every slot of `generation` closes.
    pub(super) fn end_match(&mut self, id: u64, epoch: u64, generation: u64) -> io::Result<()> {
        if !self.matches(epoch)
            || generation == 0
            || generation > i64::MAX as u64
            || self
                .games
                .values()
                .any(|game| game.auth.key.generation != generation)
        {
            self.error(id, "stale_match")?;
            return Ok(());
        }
        self.closed_generation = self.closed_generation.max(generation);
        let slots = std::mem::take(&mut self.games);
        for (peer, slot) in slots {
            if let Some((connection, task)) = self.pending_game_admissions.remove(&peer) {
                connection.close(0u32.into(), b"match ended during admission");
                task.abort();
            }
            self.emit(Event::GameClosed {
                epoch,
                peer,
                generation,
                reason: slot.unlinked_reason("ended before gameplay link"),
            })?;
        }
        Ok(())
    }

    /// `Command::EndPeer`: `peer`'s slot of `generation` closes.
    pub(super) fn end_peer(
        &mut self,
        id: u64,
        epoch: u64,
        peer: EndpointId,
        generation: u64,
    ) -> io::Result<()> {
        if !self.matches(epoch)
            || generation == 0
            || generation > i64::MAX as u64
            || generation <= self.closed_generation
        {
            self.error(id, "stale_match")?;
            return Ok(());
        }
        // Per-link teardown is intentionally idempotent.  A bridge
        // may have already observed the remote close before the
        // authority's game_peer_end message reaches this helper.
        let Some(slot) = self.games.get(&peer) else {
            return Ok(());
        };
        if slot.auth.key.generation != generation {
            self.error(id, "stale_match")?;
            return Ok(());
        }
        let slot = self.games.remove(&peer);
        if let Some((connection, task)) = self.pending_game_admissions.remove(&peer) {
            connection.close(0u32.into(), b"peer ended during admission");
            task.abort();
        }
        self.emit(Event::GameClosed {
            epoch,
            peer,
            generation,
            reason: slot.and_then(|slot| slot.unlinked_reason("peer ended before gameplay link")),
        })?;
        Ok(())
    }
}
