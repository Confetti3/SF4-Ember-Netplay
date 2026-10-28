//! Connection probes: the permission for a peer to probe, invalidation of a
//! check whose route changed, and the probe tasks themselves. A probe
//! connection is closed once its measurement is recorded; gameplay never
//! reuses it, and a check the peer runs never touches ours.
use super::*;

#[derive(Clone, PartialEq, Eq)]
pub(super) struct ProbeAuthorizationKey {
    pub(super) epoch: u64,
    pub(super) room: [u8; 16],
    pub(super) incarnation: u64,
    pub(super) peer: EndpointId,
    pub(super) target_incarnation: u64,
    pub(super) request: u64,
    pub(super) pair_revision: u64,
    pub(super) benchmark: bool,
}

/// A probe reservation whose Raft entry this replica had not applied yet when
/// the control frame arrived.
#[derive(Clone, PartialEq, Eq)]
pub(super) struct ProbeReservationKey {
    pub(super) epoch: u64,
    pub(super) peer: EndpointId,
    /// The control worker that presented the frame, or 0 when the peer had
    /// none. A wait that ends unapplied may close only this exact control.
    pub(super) control: u64,
    pub(super) request: u64,
    pub(super) pair_revision: u64,
    pub(super) expires: u64,
}

pub(super) struct ProbeAuthorization {
    pub(super) term: u64,
    pub(super) leader: Option<u64>,
    pub(super) revision: u64,
    pub(super) expires: u64,
    /// When the authorization stops being usable, on this PC's monotonic
    /// clock, counted from when it was requested.
    pub(super) deadline: tokio::time::Instant,
}

#[derive(Clone)]
pub(super) struct PendingProbeInvalidation {
    pub(super) request: u64,
    pub(super) pair_revision: u64,
    pub(super) route: String,
}

/// Permission for a peer to open a probe connection to this helper, installed
/// by the peer's reservation. It covers only probes this helper answers.
pub(super) struct ProbePermission {
    pub(super) request: u64,
    pub(super) pair_revision: u64,
    pub(super) expires: tokio::time::Instant,
}

/// A check this helper runs against a peer, from its command until its
/// completion. The peer may check us at the same time, with the same request
/// id and pair revision, so this is kept apart from `ProbePermission`.
pub(super) struct OwnProbe {
    pub(super) request: u64,
    pub(super) pair_revision: u64,
}

pub(super) struct ProbeCompletion {
    pub(super) peer: EndpointId,
    pub(super) request: u64,
    pub(super) pair_revision: u64,
    pub(super) samples_us: Vec<u64>,
    pub(super) connection: Option<Connection>,
    pub(super) report: bool,
    pub(super) route_changed: bool,
    /// The route the measurement observed; unset when it must be read from
    /// `connection`.
    pub(super) route: Option<String>,
    pub(super) metrics: crate::probe::Metrics,
}

/// Why a check this helper was asked to run did not start: the
/// `probe_failure` code of its `probe_unavailable` error, which the native
/// side reads.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(super) enum ProbeFailure {
    /// Another epoch or room, or no request id.
    StaleRequest = 1,
    NoTaskSlot = 2,
    /// No control the reservation could go out on.
    NoRoute = 3,
    /// A check of the same peer is already running.
    AlreadyChecking = 4,
    NoCoordination = 5,
    /// The peer has no admission in this room.
    NotAdmitted = 6,
    /// The room was not writable under a current leader.
    AuthorityUnavailable = 7,
    /// The committed seats do not hold the pair.
    PairUnavailable = 8,
    /// The reservation was not committed.
    ReservationFailed = 9,
    /// The pair or the peer's admission changed while it was reserved.
    Superseded = 10,
    /// Another proposal held the leader.
    ProposalBusy = 11,
}

impl ProbeFailure {
    /// The error a reservation task ends with for this failure, read back
    /// with `of`.
    fn error(self) -> io::Error {
        io::Error::other(ProbeRefused(self))
    }

    fn of(error: &io::Error) -> Option<Self> {
        error
            .get_ref()?
            .downcast_ref::<ProbeRefused>()
            .map(|refused| refused.0)
    }
}

/// `ProbeFailure::error`'s payload, written as the text it always had.
#[derive(Debug)]
struct ProbeRefused(ProbeFailure);

impl std::fmt::Display for ProbeRefused {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self.0 {
            ProbeFailure::AuthorityUnavailable => "probe authority unavailable",
            ProbeFailure::PairUnavailable => "probe pair unavailable",
            _ => "probe unavailable",
        })
    }
}

impl std::error::Error for ProbeRefused {}

/// How a reservation that arrived ahead of its committed entry ended.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum ReservationWait {
    /// Committed, for the pair as this replica holds it seated.
    Bound,
    /// Committed, but the seats it names have since been replaced.
    Superseded,
    /// Its entry never reached this replica in time.
    Unapplied,
}

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
        // A member of a server-owned room has no control to another member;
        // its host relays the reservation (`relay_probe_reservation`).
        let route = self.probe_reservation_route(peer);
        let _ = self.send_frame(route, payload.into_bytes());
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
            return self.probe_error(ProbeFailure::StaleRequest);
        }
        if self.tasks.len() >= MAX_TASKS {
            return self.probe_error(ProbeFailure::NoTaskSlot);
        }
        if self.probe_reservation_route(peer).is_none() {
            return self.probe_error(ProbeFailure::NoRoute);
        }
        if self.own_probes.contains_key(&peer) {
            return self.probe_error(ProbeFailure::AlreadyChecking);
        }
        let Some(recovery) = self.recovery.clone() else {
            return self.probe_error(ProbeFailure::NoCoordination);
        };
        let Some(target_incarnation) = self
            .admissions
            .values()
            .find(|admission| admission.primary_endpoint == peer && admission.room == room)
            .map(|admission| admission.incarnation)
        else {
            return self.probe_error(ProbeFailure::NotAdmitted);
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
        self.own_probes.insert(
            peer,
            OwnProbe {
                request,
                pair_revision,
            },
        );
        self.pending_probe_authorizations.insert(peer, key.clone());
        let source = self.endpoint.id();
        // The authorization is usable for the check's window from now, on
        // this PC's monotonic clock: a wall clock corrected or set by hand
        // meanwhile changes nothing.
        let deadline = tokio::time::Instant::now() + crate::probe::window(benchmark);
        self.tasks.spawn(async move {
            let result = async {
                let state = recovery.state().await;
                if !state.writable
                    || recovery.coordinator.current_term() != state.term
                    || recovery.coordinator.current_leader().is_none()
                {
                    return Err(ProbeFailure::AuthorityUnavailable.error());
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
                    return Err(ProbeFailure::PairUnavailable.error());
                }
                // Stamped on this PC's wall clock, as 1.1.0 builds expect, but
                // only ever matched: the room's entry and the peer's frame
                // must carry the same value. No PC compares it with a clock.
                let expires = now()
                    .unwrap_or_default()
                    .saturating_add(crate::probe::window(benchmark).as_secs());
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
                Ok(ProbeAuthorization {
                    term: state.term,
                    leader: recovery.coordinator.current_leader(),
                    revision: recovery.coordinator.store().committed().await.revision(),
                    expires,
                    deadline,
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
            self.own_probes.remove(&key.peer);
            return Ok(());
        };
        let valid = if let Ok(authorization) = &result {
            let committed_revision = recovery.coordinator.store().committed().await.revision();
            key.epoch == self.epoch
                && self.room == Some(key.room)
                && recovery.room == key.room
                && recovery.incarnation == key.incarnation
                && recovery.coordinator.current_term() == authorization.term
                && recovery.coordinator.current_leader() == authorization.leader
                && authorization.leader.is_some()
                && tokio::time::Instant::now() < authorization.deadline
                && committed_revision == authorization.revision
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
                let reason = ProbeFailure::of(&error).unwrap_or(
                    if crate::coordination::is_proposal_busy(&error) {
                        ProbeFailure::ProposalBusy
                    } else {
                        ProbeFailure::ReservationFailed
                    },
                );
                self.own_probes.remove(&key.peer);
                self.probe_error(reason)?;
                return Ok(());
            }
        };
        if !valid {
            self.own_probes.remove(&key.peer);
            self.probe_error(ProbeFailure::Superseded)?;
            return Ok(());
        }
        // Native code discards its recommendation when it requests a check, so
        // a retained route-change invalidation of the earlier one is stale.
        self.pending_probe_invalidations.remove(&key.peer);
        let address = self
            .host_address
            .as_ref()
            .filter(|address| address.id == key.peer)
            .cloned()
            .unwrap_or_else(|| EndpointAddr::new(key.peer));
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
                route: None,
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
            // A completion settles only the probe of its own role: a check we
            // ran, or a check we answered. The peer may check us while we
            // check it, with the same request id and pair revision, so the
            // request and revision alone cannot tell the two apart.
            let current = if probe.report {
                self.own_probes.get(&probe.peer).is_some_and(|own| {
                    own.request == probe.request && own.pair_revision == probe.pair_revision
                })
            } else {
                self.probe_permissions
                    .get(&probe.peer)
                    .is_some_and(|permission| {
                        permission.request == probe.request
                            && permission.pair_revision == probe.pair_revision
                    })
            };
            if !current {
                if let Some(connection) = probe.connection {
                    connection.close(1u32.into(), b"obsolete probe completion");
                }
                return Ok(());
            }
            if probe.report {
                self.own_probes.remove(&probe.peer);
                self.pending_probe_invalidations.remove(&probe.peer);
            } else {
                self.probe_peers.remove(&probe.peer);
                self.probe_permissions.remove(&probe.peer);
            }
            // The measurement is recorded and nothing reuses its connection.
            // Gameplay dials its own. Answering the peer's check reports
            // nothing: our own recommendation is not theirs to retire.
            if !probe.report {
                if let Some(connection) = &probe.connection {
                    connection.close(0u32.into(), b"probe complete");
                }
                return Ok(());
            }
            let observed = probe
                .route
                .clone()
                .or_else(|| probe.connection.as_ref().map(selected_probe_route));
            if let Some(connection) = &probe.connection {
                connection.close(0u32.into(), b"probe complete");
            }
            let route = observed.unwrap_or_else(|| "unavailable".into());
            if probe.route_changed {
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
        }
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    pub(super) async fn accept_probe_reservation(
        &mut self,
        peer: EndpointId,
        room: [u8; 16],
        claimed_source: EndpointId,
        source_incarnation: u64,
        target_incarnation: u64,
        request: u64,
        pair_revision: u64,
        term: u64,
        expires: u64,
    ) -> io::Result<bool> {
        if self.is_public_host() {
            let frame = CoordinationControl::ProbeReservation {
                room,
                source: claimed_source,
                source_incarnation,
                target_incarnation,
                request,
                pair_revision,
                term,
                expires,
            };
            return if self.relay_probe_reservation(peer, frame) {
                Ok(true)
            } else {
                Err(failed("invalid probe reservation"))
            };
        }
        let Some(recovery) = self.recovery.clone() else {
            return Ok(false);
        };
        let state = recovery.state().await;
        // In a server-owned room the frame arrives from the host, which
        // relayed it from the source's own control; anywhere else it must
        // arrive on the source's control. Either way the reservation counts
        // only once this replica holds its committed entry. Its `expires` is
        // on the source's wall clock, which need not agree with this PC's, so
        // it is matched against that entry and never compared with a time.
        let relayed = self.server_owned() && self.host_endpoint() == Some(peer);
        let via = peer;
        let peer = claimed_source;
        if room != recovery.room
            || (!relayed && claimed_source != via)
            || request == 0
            || state.term != term
            || !state.writable
            || !self.admissions.values().any(|admission| {
                admission.room == room
                    && admission.primary_endpoint == peer
                    && admission.incarnation == source_incarnation
            })
            || !self.admissions.values().any(|admission| {
                admission.room == room
                    && admission.primary_endpoint == self.endpoint.id()
                    && admission.incarnation == target_incarnation
            })
        {
            // A refused frame closes the control it came on. A relayed one came
            // on the host's, which did not write it: it is dropped instead.
            if relayed {
                return Ok(true);
            }
            return Err(failed("invalid probe reservation"));
        }
        // The reservation needs its own committed entry and the committed
        // seats that bind the pair at `pair_revision`. The source checked
        // both on the leader; this replica may not have applied either yet,
        // for example a check asked for the moment both seats fill.
        let target = self.endpoint.id();
        //
        // The source checked the seats before it proposed the reservation, so
        // the seat commit it names comes before the reservation's entry in
        // the room's log: a replica holding the entry holds those seats or a
        // newer commit. The entry is what a frame waits for. Once it has
        // applied, an unbound pair means a later commit (a Watch, a Ready)
        // replaced the seats: the check is stale, not invalid, and is dropped
        // with no permission and no closed control.
        let reserved = move |recovery: crate::recovery::RecoverySession| async move {
            let reservation = crate::coordination::ProbeReservation {
                room: recovery.room,
                source_incarnation,
                target_incarnation,
                request,
                pair_revision,
                term,
                expires,
            };
            recovery
                .coordinator
                .store()
                .probe_reserved(&reservation)
                .await
        };
        let settled = move |recovery: crate::recovery::RecoverySession| async move {
            if recovery
                .probe_pair_bound(
                    source_incarnation,
                    target_incarnation,
                    peer,
                    target,
                    pair_revision,
                )
                .await
            {
                ReservationWait::Bound
            } else {
                ReservationWait::Superseded
            }
        };
        if reserved(recovery.clone()).await {
            // This frame supersedes any older reservation of the peer
            // still waiting on a task; that completion is ignored.
            self.pending_probe_reservations.remove(&peer);
            if settled(recovery).await == ReservationWait::Bound {
                self.install_probe_permission(peer, request, pair_revision);
            }
            return Ok(true);
        }
        // The reservation's Raft entry has not reached this replica yet.
        // Wait for it on a task instead of in the actor tick; its completion
        // installs the permission, drops a stale check, or, still unapplied
        // at PROBE_RESERVATION_TIMEOUT, closes the control.
        if self.tasks.len() >= MAX_TASKS {
            if relayed {
                return Ok(true);
            }
            return Err(failed("probe reservation busy"));
        }
        let key = ProbeReservationKey {
            epoch: self.epoch,
            peer,
            // A relayed reservation that never applies closes nothing: the
            // host's control is not the route that vouched for it.
            control: if relayed {
                0
            } else {
                self.controls.get(&peer).map(ControlWorker::id).unwrap_or(0)
            },
            request,
            pair_revision,
            expires,
        };
        self.pending_probe_reservations.insert(peer, key.clone());
        self.tasks.spawn(async move {
            let applied = timeout(PROBE_RESERVATION_TIMEOUT, async {
                loop {
                    if reserved(recovery.clone()).await {
                        break;
                    }
                    tokio::time::sleep(Duration::from_millis(25)).await;
                }
            })
            .await
            .is_ok();
            let outcome = if applied {
                settled(recovery).await
            } else {
                ReservationWait::Unapplied
            };
            Completion::ProbeReservation(key, outcome)
        });
        Ok(true)
    }

    /// Let `peer` open its probe connection for a check's window, counted
    /// from now on this PC's monotonic clock. The frame does not say whether
    /// the check is a benchmark, so the window is the benchmark's, the longer
    /// one; a permission ends sooner when its check completes or the peer's
    /// next reservation replaces it.
    pub(super) fn install_probe_permission(
        &mut self,
        peer: EndpointId,
        request: u64,
        pair_revision: u64,
    ) {
        self.probe_permissions.insert(
            peer,
            ProbePermission {
                request,
                pair_revision,
                expires: tokio::time::Instant::now() + crate::probe::window(true),
            },
        );
        self.probe_peers.insert(peer);
    }

    pub(super) async fn completed_probe_reservation(
        &mut self,
        key: ProbeReservationKey,
        outcome: ReservationWait,
    ) -> io::Result<()> {
        if self.pending_probe_reservations.get(&key.peer) != Some(&key) {
            return Ok(());
        }
        self.pending_probe_reservations.remove(&key.peer);
        if key.epoch != self.epoch || self.room.is_none() {
            return Ok(());
        }
        if outcome == ReservationWait::Bound {
            self.install_probe_permission(key.peer, key.request, key.pair_revision);
        } else if outcome == ReservationWait::Unapplied
            && self
                .controls
                .get(&key.peer)
                .is_some_and(|control| control.id() == key.control)
        {
            // Same outcome as a refused coordination frame: the route that
            // presented the unapplied reservation closes. A control that
            // replaced it after the frame arrived is not that route and is
            // left alone.
            let control = self.remove_control(key.peer);
            self.emit(Event::ControlClosed {
                epoch: self.epoch,
                peer: key.peer,
                control,
            })?;
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
        route: Some(measurement.route),
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
        // A committed reservation and its control notification travel on the
        // coordination control, not on GAME_ALPN. The first probe connection
        // may win that race; retry only until the existing handshake deadline
        // proves the recipient has applied the reservation and acknowledged
        // this stream.
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

/// The connection's selected route as an event may carry it: `direct`,
/// `relay:<region>` or `unavailable`, never an address.
pub(super) fn selected_probe_route(connection: &Connection) -> String {
    crate::probe::route(connection)
}
