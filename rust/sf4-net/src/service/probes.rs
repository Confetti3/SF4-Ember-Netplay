//! Connection probes: reservations, invalidation, and the probe tasks themselves.
use super::*;

impl Actor {
    pub(super) fn send_probe_reservation(
        &mut self,
        peer: EndpointId,
        room: [u8; 16],
        request: u64,
        pair_revision: u64,
        term: u64,
        expires: u64,
    ) {
        let Some((source_incarnation, target_incarnation)) = self
            .recovery
            .as_ref()
            .map(|recovery| recovery.incarnation)
            .zip(
                self.admissions
                    .values()
                    .find(|admission| admission.primary_endpoint == peer)
                    .map(|admission| admission.incarnation),
            )
        else {
            return;
        };
        let Ok(payload) = serde_json::to_string(&CoordinationControl::ProbeReservation {
            room,
            source: self.endpoint.id(),
            source_incarnation,
            target_incarnation,
            request,
            pair_revision,
            term,
            expires,
        }) else {
            return;
        };
        let id = self.next_transport_message;
        self.next_transport_message = self.next_transport_message.saturating_add(1);
        if let Some(control) = self.controls.get(&peer) {
            let _ = control.try_send(ControlFrame {
                message_id: id,
                payload: payload.into_bytes(),
            });
        }
    }

    pub(super) fn pump_probe_invalidations(&mut self) {
        let pending: Vec<_> = self
            .pending_probe_invalidations
            .iter()
            .map(|(peer, invalidation)| (*peer, invalidation.clone()))
            .collect();
        for (peer, invalidation) in pending {
            if !self.emit_bulk(Event::ProbeResult {
                epoch: self.epoch,
                room: self.room.unwrap_or([0; 16]),
                peer,
                request: invalidation.request,
                pair_revision: invalidation.pair_revision,
                route: invalidation.route,
                status: "invalidated".into(),
                sample_count: 0,
                loss_count: 100,
                p95_rtt_us: 0,
                recommended_delay: -1,
                metrics: crate::probe::Metrics::default(),
            }) {
                break;
            }
            self.pending_probe_invalidations.remove(&peer);
        }
    }

    pub(super) fn queue_probe_invalidation(
        &mut self,
        peer: EndpointId,
        request: u64,
        pair_revision: u64,
        route: String,
    ) {
        self.pending_probe_invalidations.insert(
            peer,
            PendingProbeInvalidation {
                request,
                pair_revision,
                route,
            },
        );
        self.pump_probe_invalidations();
    }

    pub(super) fn invalidate_changed_probe_routes(&mut self) {
        self.pump_probe_invalidations();
        let invalidated: Vec<_> = self
            .probe_reservations
            .iter()
            .filter_map(|(peer, reservation)| {
                let current = selected_probe_route(&reservation.connection);
                let closed = reservation.connection.close_reason().is_some();
                (closed || current != reservation.route || current == "unavailable").then_some((
                    *peer,
                    reservation.request,
                    reservation.pair_revision,
                    if closed {
                        "unavailable".into()
                    } else {
                        current
                    },
                ))
            })
            .collect();
        for (peer, request, pair_revision, route) in invalidated {
            if let Some(mut reservation) = self.probe_reservations.remove(&peer) {
                let reported = reservation.reported;
                if reservation.connection.close_reason().is_none() {
                    reservation.route = route.clone();
                    reservation.reported = false;
                    self.probe_reservations.insert(peer, reservation);
                }
                if reported {
                    self.queue_probe_invalidation(peer, request, pair_revision, route);
                }
            }
        }
    }

    pub(super) fn expire_probe_permissions(&mut self) {
        let now = tokio::time::Instant::now();
        let expired: Vec<_> = self
            .probe_permissions
            .iter()
            .filter_map(|(peer, permission)| (permission.expires <= now).then_some(*peer))
            .collect();
        for peer in expired {
            self.probe_permissions.remove(&peer);
            self.probe_peers.remove(&peer);
        }
    }

    pub(super) async fn spawn_probe(
        &mut self,
        epoch: u64,
        room: [u8; 16],
        peer: EndpointId,
        request: u64,
        pair_revision: u64,
        benchmark: bool,
    ) -> io::Result<()> {
        if !self.matches(epoch) || self.room != Some(room) || request == 0 {
            return self.probe_error(1);
        }
        if self.tasks.len() >= MAX_TASKS {
            return self.probe_error(2);
        }
        if !self.controls.contains_key(&peer) {
            return self.probe_error(3);
        }
        if self.probe_peers.contains(&peer) {
            return self.probe_error(4);
        }
        let Some(recovery) = self.recovery.clone() else {
            return self.probe_error(5);
        };
        let Some(target_incarnation) = self
            .admissions
            .values()
            .find(|admission| admission.primary_endpoint == peer && admission.room == room)
            .map(|admission| admission.incarnation)
        else {
            return self.probe_error(6);
        };
        let key = ProbeAuthorizationKey {
            epoch,
            room,
            incarnation: recovery.incarnation,
            peer,
            target_incarnation,
            request,
            pair_revision,
            benchmark,
        };
        self.probe_peers.insert(peer);
        self.pending_probe_authorizations.insert(peer, key.clone());
        let source = self.endpoint.id();
        self.tasks.spawn(async move {
            let result = async {
                let state = recovery.state().await;
                if !state.writable
                    || recovery.coordinator.current_term() != state.term
                    || recovery.coordinator.current_leader().is_none()
                {
                    return Err(failed("probe authority unavailable"));
                }
                if !recovery
                    .probe_pair_bound(
                        recovery.incarnation,
                        target_incarnation,
                        source,
                        peer,
                        pair_revision,
                    )
                    .await
                {
                    return Err(failed("probe pair unavailable"));
                }
                let expires = now()
                    .unwrap_or_default()
                    .saturating_add(crate::probe::duration(benchmark).as_secs() + 5)
                    .saturating_add(transport::HANDSHAKE_TIMEOUT.as_secs());
                recovery
                    .reserve_probe(
                        recovery.incarnation,
                        target_incarnation,
                        request,
                        pair_revision,
                        state.term,
                        expires,
                    )
                    .await?;
                let committed = recovery.committed().await;
                Ok(ProbeAuthorization {
                    term: state.term,
                    leader: recovery.coordinator.current_leader(),
                    revision: committed.revision,
                    expires,
                })
            }
            .await;
            Completion::ProbeAuthorization(key, result)
        });
        Ok(())
    }

    pub(super) async fn completed_probe_authorization(
        &mut self,
        key: ProbeAuthorizationKey,
        result: io::Result<ProbeAuthorization>,
    ) -> io::Result<()> {
        if self.pending_probe_authorizations.get(&key.peer) != Some(&key) {
            return Ok(());
        }
        self.pending_probe_authorizations.remove(&key.peer);
        let Some(recovery) = self.recovery.clone() else {
            self.probe_peers.remove(&key.peer);
            return Ok(());
        };
        let wall_now = now().unwrap_or_default();
        let valid = if let Ok(authorization) = &result {
            let committed = recovery.committed().await;
            key.epoch == self.epoch
                && self.room == Some(key.room)
                && recovery.room == key.room
                && recovery.incarnation == key.incarnation
                && recovery.coordinator.current_term() == authorization.term
                && recovery.coordinator.current_leader() == authorization.leader
                && authorization.leader.is_some()
                && authorization.expires > wall_now
                && committed.revision == authorization.revision
                && !self.games.contains_key(&key.peer)
                && self.admissions.values().any(|admission| {
                    admission.room == key.room
                        && admission.primary_endpoint == key.peer
                        && admission.incarnation == key.target_incarnation
                })
                && recovery
                    .probe_pair_bound(
                        key.incarnation,
                        key.target_incarnation,
                        self.endpoint.id(),
                        key.peer,
                        key.pair_revision,
                    )
                    .await
        } else {
            false
        };
        let authorization = match result {
            Ok(authorization) => authorization,
            Err(error) => {
                let reason = match error.to_string().as_str() {
                    "probe authority unavailable" => 7,
                    "probe pair unavailable" => 8,
                    "room proposal busy" => 11,
                    _ => 9,
                };
                self.probe_peers.remove(&key.peer);
                self.probe_error(reason)?;
                return Ok(());
            }
        };
        if !valid {
            self.probe_peers.remove(&key.peer);
            self.probe_error(10)?;
            return Ok(());
        }
        self.pending_probe_invalidations.remove(&key.peer);
        let address = self
            .host_address
            .as_ref()
            .filter(|address| address.id == key.peer)
            .cloned()
            .unwrap_or_else(|| EndpointAddr::new(key.peer));
        let remaining = Duration::from_secs(authorization.expires.saturating_sub(wall_now));
        self.probe_permissions.insert(
            key.peer,
            ProbePermission {
                request: key.request,
                pair_revision: key.pair_revision,
                expires: tokio::time::Instant::now() + remaining,
            },
        );
        self.send_probe_reservation(
            key.peer,
            key.room,
            key.request,
            key.pair_revision,
            authorization.term,
            authorization.expires,
        );
        let endpoint = self.endpoint.clone();
        self.tasks.spawn(async move {
            let result = run_probe(
                endpoint,
                address,
                key.room,
                key.peer,
                key.request,
                key.pair_revision,
                key.benchmark,
            )
            .await
            .unwrap_or(ProbeCompletion {
                peer: key.peer,
                request: key.request,
                pair_revision: key.pair_revision,
                samples_us: Vec::new(),
                connection: None,
                report: true,
                route_changed: false,
                metrics: crate::probe::Metrics::default(),
            });
            Completion::Probe(key.epoch, Ok(result))
        });
        Ok(())
    }

    pub(super) async fn completed_probe(
        &mut self,
        epoch: u64,
        result: io::Result<ProbeCompletion>,
    ) -> io::Result<()> {
        if epoch != self.epoch {
            return Ok(());
        }
        if let Ok(probe) = result {
            let current = self
                .probe_permissions
                .get(&probe.peer)
                .is_some_and(|permission| {
                    permission.request == probe.request
                        && permission.pair_revision == probe.pair_revision
                });
            if !current {
                if let Some(connection) = probe.connection {
                    connection.close(1u32.into(), b"obsolete probe completion");
                }
                return Ok(());
            }
            self.probe_peers.remove(&probe.peer);
            self.probe_permissions.remove(&probe.peer);
            self.pending_probe_invalidations.remove(&probe.peer);
            // A check initiated by the other player may replace our
            // measured connection. Retire its recommendation explicitly.
            if let Some(previous) = self.probe_reservations.remove(&probe.peer)
                && previous.reported
                && !probe.report
            {
                self.queue_probe_invalidation(
                    probe.peer,
                    previous.request,
                    previous.pair_revision,
                    probe
                        .connection
                        .as_ref()
                        .map(selected_probe_route)
                        .unwrap_or_default(),
                );
            }
            if probe.report {
                let route = probe
                    .connection
                    .as_ref()
                    .map(selected_probe_route)
                    .unwrap_or_else(|| "unavailable".into());
                if probe.route_changed {
                    if let Some(connection) = probe.connection
                        && connection.close_reason().is_none()
                    {
                        self.probe_reservations.insert(
                            probe.peer,
                            ProbeReservation {
                                reported: false,
                                connection,
                                request: probe.request,
                                pair_revision: probe.pair_revision,
                                route: route.clone(),
                            },
                        );
                    }
                    self.queue_probe_invalidation(
                        probe.peer,
                        probe.request,
                        probe.pair_revision,
                        route,
                    );
                    return Ok(());
                }
                let summary = recovery::summarize_datagram_probe(
                    &probe.samples_us,
                    probe.metrics.expected,
                    probe.metrics.sent,
                );
                if summary.status == "ready" {
                    if let Some(connection) = probe.connection {
                        self.probe_reservations.insert(
                            probe.peer,
                            ProbeReservation {
                                reported: true,
                                connection,
                                request: probe.request,
                                pair_revision: probe.pair_revision,
                                route: route.clone(),
                            },
                        );
                    }
                } else if let Some(connection) = probe.connection {
                    connection.close(1u32.into(), b"probe unavailable");
                }
                self.emit(Event::ProbeResult {
                    epoch,
                    room: self.room.unwrap_or([0; 16]),
                    peer: probe.peer,
                    request: probe.request,
                    pair_revision: probe.pair_revision,
                    route,
                    status: summary.status.clone(),
                    sample_count: summary.sample_count,
                    loss_count: summary.loss_count,
                    p95_rtt_us: summary.p95_rtt_us,
                    recommended_delay: if summary.status == "ready" {
                        i16::from(summary.recommended_delay)
                    } else {
                        -1
                    },
                    metrics: probe.metrics,
                })?;
            } else if let Some(connection) = probe.connection {
                self.probe_reservations.insert(
                    probe.peer,
                    ProbeReservation {
                        reported: false,
                        route: selected_probe_route(&connection),
                        connection,
                        request: probe.request,
                        pair_revision: probe.pair_revision,
                    },
                );
            }
        }
        Ok(())
    }
}

pub(super) async fn run_probe(
    endpoint: Endpoint,
    address: EndpointAddr,
    room: [u8; 16],
    peer: EndpointId,
    request: u64,
    pair_revision: u64,
    benchmark: bool,
) -> io::Result<ProbeCompletion> {
    let (connection, send, recv) = connect_probe_stream(&endpoint, &address).await?;
    let measurement = crate::probe::measure(
        &connection,
        send,
        recv,
        room,
        request,
        pair_revision,
        benchmark,
    )
    .await?;
    Ok(ProbeCompletion {
        peer,
        request,
        pair_revision,
        samples_us: measurement.samples,
        connection: Some(connection),
        report: true,
        route_changed: measurement.route_changed,
        metrics: measurement.metrics,
    })
}

async fn connect_probe_stream(
    endpoint: &Endpoint,
    address: &EndpointAddr,
) -> io::Result<(Connection, SendStream, RecvStream)> {
    let deadline = tokio::time::Instant::now() + transport::HANDSHAKE_TIMEOUT;
    loop {
        let now = tokio::time::Instant::now();
        if now >= deadline {
            return Err(failed("probe timeout"));
        }
        let remaining = deadline - now;
        let connection =
            match timeout(remaining, endpoint.connect(address.clone(), GAME_ALPN)).await {
                Ok(Ok(connection)) => connection,
                Ok(Err(_)) => {
                    tokio::time::sleep(Duration::from_millis(25)).await;
                    continue;
                }
                Err(_) => return Err(failed("probe timeout")),
            };
        let stream = timeout(
            deadline.saturating_duration_since(tokio::time::Instant::now()),
            connection.open_bi(),
        )
        .await;
        let Ok(Ok((mut send, mut recv))) = stream else {
            connection.close(1u32.into(), b"probe stream unavailable");
            continue;
        };
        let ready = async {
            send.write_all(&transport::GAME_PROBE_MAGIC).await?;
            send.flush().await?;
            let mut acknowledgment = [0u8; transport::GAME_PROBE_MAGIC.len()];
            recv.read_exact(&mut acknowledgment)
                .await
                .map_err(|_| failed("probe acknowledgment"))?;
            (acknowledgment == transport::GAME_PROBE_MAGIC)
                .then_some(())
                .ok_or_else(|| failed("invalid probe acknowledgment"))
        };
        let acknowledgment_timeout = deadline
            .saturating_duration_since(tokio::time::Instant::now())
            .min(Duration::from_millis(500));
        if matches!(timeout(acknowledgment_timeout, ready).await, Ok(Ok(()))) {
            return Ok((connection, send, recv));
        }
        // A committed reservation and its control notification use a different
        // connection from GAME_ALPN. The first gameplay connection may win that
        // race; retry only until the existing handshake deadline proves the
        // recipient has applied the reservation and acknowledged this stream.
        connection.close(1u32.into(), b"probe reservation pending");
        tokio::time::sleep(Duration::from_millis(25)).await;
    }
}

pub(super) async fn serve_probe(
    connection: Connection,
    mut send: SendStream,
    recv: RecvStream,
    room: [u8; 16],
    request: u64,
    pair_revision: u64,
) -> io::Result<Connection> {
    send.write_all(&transport::GAME_PROBE_MAGIC)
        .await
        .map_err(|_| failed("probe acknowledgment"))?;
    crate::probe::respond(&connection, send, recv, room, request, pair_revision).await?;
    Ok(connection)
}

pub(super) fn selected_probe_route(connection: &Connection) -> String {
    let paths = connection.paths();
    paths
        .iter()
        .find(|path| path.is_selected())
        .map(|path| path.remote_addr().to_string())
        .unwrap_or_else(|| "unavailable".into())
}
