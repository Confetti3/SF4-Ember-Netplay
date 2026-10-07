//! Control-peer connections, the coordination handshake and inbound control frames.
use super::*;

impl Actor {
    /// Announce this member's departure on the controls still named in
    /// `pending`. The committed leader retires the native seat immediately;
    /// the departing helper keeps its control and vote alive until that
    /// exclusion is committed. Returns true once every open control has
    /// accepted the frame; a refused queue leaves its peer pending for the
    /// caller's next attempt.
    pub(super) fn send_departure_control(&mut self, pending: &mut BTreeSet<EndpointId>) -> bool {
        let Some(recovery) = self.recovery.as_ref() else {
            return true;
        };
        let message = CoordinationControl::Departure {
            room: recovery.room,
            incarnation: recovery.incarnation,
        };
        let Ok(payload) = serde_json::to_string(&message) else {
            return true;
        };
        for (peer, control) in &self.controls {
            if !pending.contains(peer) {
                continue;
            }
            let id = self.next_transport_message;
            self.next_transport_message = self.next_transport_message.saturating_add(1);
            let frame = ControlFrame {
                message_id: id,
                payload: payload.clone().into_bytes(),
            };
            if control.try_send(frame).is_ok() {
                pending.remove(peer);
            }
        }
        pending.retain(|peer| self.controls.contains_key(peer));
        pending.is_empty()
    }

    /// Remove a control and name it, for the close event that follows.
    pub(super) fn remove_control(&mut self, peer: EndpointId) -> u64 {
        self.controls
            .remove(&peer)
            .map(|control| control.id())
            .unwrap_or(0)
    }

    /// Bind a control to its accepted incarnation and tell the native side.
    /// Every accepted Admission is published; the native record ignores a
    /// repeat for the incarnation it already holds.
    fn bind_control_session(&mut self, peer: EndpointId, incarnation: u64) -> io::Result<()> {
        let Some(control) = self.controls.get_mut(&peer) else {
            return Ok(());
        };
        control.set_session(Session::Bound(incarnation));
        self.emit(Event::PeerSession {
            epoch: self.epoch,
            peer,
            incarnation,
        })
    }

    /// The admission operation a control presented has completed. Only that
    /// exact control, still pending for that incarnation, is settled:
    /// accepted binds it, anything else closes it, with nothing native
    /// delivered meanwhile. A control that replaced it waits for its own
    /// operation; roster work never owns a control.
    pub(super) fn settle_control_session(
        &mut self,
        binding: ControlBinding,
        accepted: bool,
    ) -> io::Result<()> {
        let ControlBinding {
            peer,
            control,
            incarnation,
        } = binding;
        if self.controls.get(&peer).is_none_or(|current| {
            current.id() != control || current.session() != Session::Pending(incarnation)
        }) {
            return Ok(());
        }
        if accepted {
            return self.bind_control_session(peer, incarnation);
        }
        let control = self.remove_control(peer);
        self.emit(Event::ControlClosed {
            epoch: self.epoch,
            peer,
            control,
        })
    }

    pub(super) fn send_coordination_control(&mut self, peer: EndpointId) {
        let Some(recovery) = self.recovery.clone() else {
            return;
        };
        let leader_local = recovery.coordinator.current_leader() == Some(recovery.incarnation);
        let own = self.admissions.values().find(|admission| {
            admission.primary_endpoint == self.endpoint.id()
                || admission.coordination_endpoint == recovery.coordination_endpoint
        });
        let Some(own) = own else {
            return;
        };
        let message = CoordinationControl::Admission {
            admission: own.clone(),
        };
        let Ok(payload) = serde_json::to_string(&message) else {
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
        // A fresh control route may have missed an earlier retirement
        // broadcast.  Replay the durable tombstone set only from the
        // committed leader; followers must not impersonate membership
        // authority while they establish their reciprocal Admission.
        if leader_local {
            self.pending_membership_publications.insert(peer);
            self.pump_membership_publications();
        }
    }

    pub(super) fn control_rejoin_member(&self, peer: EndpointId) -> Option<u64> {
        if !self
            .committed_native_members
            .as_ref()
            .is_some_and(|members| members.contains(&peer))
        {
            return None;
        }
        self.admissions
            .values()
            .find(|admission| {
                self.room == Some(admission.room)
                    && admission.primary_endpoint == peer
                    && admission.incarnation != 0
                    && !self.retired_incarnations.contains(&admission.incarnation)
                    && !self
                        .pending_retired_incarnations
                        .contains(&admission.incarnation)
            })
            .map(|admission| admission.incarnation)
    }

    pub(super) fn reconnect_control(&mut self, peer: EndpointId) {
        if self.reconnect_target.is_some()
            || self.controls.contains_key(&peer)
            || self.parked_controls.contains_key(&peer)
            || self.tasks.len() >= MAX_TASKS
            || self.room.is_none()
        {
            return;
        }
        // A public host has no leader to follow and never dials a member.
        if self.is_public_host() {
            return;
        }
        let Some(invite) = self.room_invite.clone() else {
            return;
        };
        let target = self.current_leader_primary().unwrap_or(peer);
        if target == self.endpoint.id() || self.controls.contains_key(&target) {
            return;
        }
        let endpoint = self.endpoint.clone();
        let epoch = self.epoch;
        let address = invite
            .address()
            .relay_urls()
            .next()
            .cloned()
            .map(|relay| EndpointAddr::new(target).with_relay_url(relay))
            .unwrap_or_else(|| EndpointAddr::new(target));
        self.reconnect_target = Some(target);
        // A server-owned member redials with the ticket it joined with. Past
        // its minute the host still takes it from an endpoint it holds as a
        // member (`AdmissionPolicy::check`).
        let ticket = self.member_ticket();
        self.tasks.spawn(async move {
            tokio::time::sleep(Duration::from_millis(250)).await;
            let result = match &ticket {
                Some(ticket) => {
                    crate::public_room::connect_public_to(&endpoint, address, &invite, ticket).await
                }
                None => transport::connect_control_to(&endpoint, address, &invite).await,
            };
            Completion::Reconnect(epoch, target, result)
        });
    }

    /// Close and forget every parked replacement control.
    pub(super) fn clear_parked_controls(&mut self) {
        for parked in std::mem::take(&mut self.parked_controls).into_values() {
            parked.channel.connection.close(1u32.into(), b"room unavailable");
        }
    }

    /// Install the replacement parked behind the peer's worker once that
    /// worker has nothing left to deliver: its transport is closed, its reader
    /// has ended and the actor has taken every frame it queued. The old worker
    /// is then dropped without a close event, as a superseded control always
    /// was; the replacement announces itself with its own Connected. A worker
    /// already removed by another path leaves the replacement free at once,
    /// and one that cannot drain within CONTROL_REPLACE_DRAIN_LIMIT is dropped
    /// with its frames, as every replaced worker was before they were drained.
    async fn settle_parked_control(&mut self, peer: EndpointId) -> io::Result<()> {
        let Some(parked) = self.parked_controls.get(&peer) else {
            return Ok(());
        };
        // A candidate only ever replaces the worker it was parked behind. A
        // different worker for the peer was installed after it was parked,
        // is newer, and is left alone while the stale candidate is closed.
        let current = self.controls.get(&peer).map(ControlWorker::id);
        if current.is_some() && current != parked.behind {
            if let Some(stale) = self.parked_controls.remove(&peer) {
                stale
                    .channel
                    .connection
                    .close(1u32.into(), b"control replaced");
            }
            return Ok(());
        }
        let drained = self
            .controls
            .get(&peer)
            .is_none_or(|old| old.is_closed() && old.is_drained());
        if !drained && parked.since.elapsed() < CONTROL_REPLACE_DRAIN_LIMIT {
            return Ok(());
        }
        let Some(parked) = self.parked_controls.remove(&peer) else {
            return Ok(());
        };
        let replaced = self.controls.remove(&peer);
        if parked.epoch != self.epoch || self.room.is_none() {
            parked
                .channel
                .connection
                .close(1u32.into(), b"room unavailable");
            return Ok(());
        }
        // The install is the admission boundary: the candidate is decided on
        // the room as it stands now, with a fresh clock and with neither its
        // own reservation nor the worker it replaces counted, and becomes a
        // member only here. Nothing is awaited between this and the install.
        if self.refuse_public_control(&parked.channel) {
            parked.channel.connection.close(1u32.into(), b"admission ended");
            // The replaced worker was announced and will not be replaced.
            if let Some(old) = replaced {
                self.emit(Event::ControlClosed {
                    epoch: self.epoch,
                    peer,
                    control: old.id(),
                })?;
            }
            return Ok(());
        }
        if let Some(account) = parked.channel.account.as_ref() {
            self.remember_public_member(peer, account);
        }
        self.install_control(parked.epoch, parked.channel, parked.joined_invite)
            .await
    }

    pub(super) fn current_leader_primary(&self) -> Option<EndpointId> {
        let recovery = self.recovery.as_ref()?;
        let leader = recovery.coordinator.current_leader()?;
        self.admissions
            .get(&leader)
            .map(|admission| admission.primary_endpoint)
    }

    /// Ask one deterministic surviving applied voter to campaign after a
    /// sustained failure of the committed leader. OpenRaft still requires the
    /// existing committed quorum before authority can change.
    pub(super) async fn trigger_recovery_election_for_leader(&self, lost_leader: u64) {
        let Some(recovery) = self.recovery.as_ref() else {
            return;
        };
        // A server-owned room has no voter to campaign.
        if self.server_owned()
            || lost_leader == recovery.incarnation
            || recovery.coordinator.current_leader() != Some(lost_leader)
        {
            return;
        }
        let voters = recovery.applied_voter_ids().await;
        let candidate = voters.into_iter().find(|incarnation| {
            *incarnation != lost_leader
                && !self.pending_retired_incarnations.contains(incarnation)
                && !self.retired_incarnations.contains(incarnation)
        });
        if candidate == Some(recovery.incarnation) {
            let _ = recovery.coordinator.raft().trigger().elect().await;
        }
    }

    pub(super) fn send_membership_control(
        &mut self,
        peer: EndpointId,
        retired_before_retry: &BTreeSet<u64>,
    ) -> bool {
        let mut retired = retired_before_retry.clone();
        retired.extend(self.pending_retired_incarnations.iter().copied());
        retired.extend(self.retired_incarnations.iter().copied());
        let admissions = self.membership_view(
            self.admissions
                .values()
                .filter(|admission| !retired.contains(&admission.incarnation))
                .cloned()
                .collect(),
        );
        let Ok(payload) = serde_json::to_string(&CoordinationControl::Membership {
            admissions,
            retired: retired.into_iter().collect(),
        }) else {
            return false;
        };
        let id = self.next_transport_message;
        let sent = self.controls.get(&peer).is_some_and(|control| {
            control
                .try_send(ControlFrame {
                    message_id: id,
                    payload: payload.into_bytes(),
                })
                .is_ok()
        });
        if sent {
            self.next_transport_message = self.next_transport_message.saturating_add(1);
        }
        sent
    }

    pub(super) fn queue_membership_publication(&mut self) {
        self.pending_membership_publications
            .extend(self.controls.keys().copied());
        self.pump_membership_publications();
    }

    pub(super) fn pump_membership_publications(&mut self) {
        let leader_local = self.recovery.as_ref().is_some_and(|recovery| {
            recovery.coordinator.current_leader() == Some(recovery.incarnation)
        });
        if !leader_local {
            // A retained send belongs to the authority that queued it. After
            // an election, drop that work rather than impersonating the new
            // leader; a local-leader refresh queues a current full snapshot.
            self.pending_membership_publications.clear();
            return;
        }
        let peers = self
            .pending_membership_publications
            .iter()
            .copied()
            .collect::<Vec<_>>();
        for peer in peers {
            if !self.controls.contains_key(&peer) {
                self.pending_membership_publications.remove(&peer);
                continue;
            }
            if self.send_membership_control(peer, &BTreeSet::new()) {
                self.pending_membership_publications.remove(&peer);
            }
        }
    }

    pub(super) async fn accept_coordination_control(
        &mut self,
        peer: EndpointId,
        payload: &str,
    ) -> io::Result<bool> {
        let Ok(message) = serde_json::from_str::<CoordinationControl>(payload) else {
            return Ok(false);
        };
        match message {
            CoordinationControl::Departure { room, incarnation } => {
                // Only an authenticated control for this room may retire its
                // own seat, and never the committed leader's: a leader hands
                // off through the successor proof instead.
                let Some(recovery) = self.recovery.as_ref() else {
                    return Ok(false);
                };
                if room != recovery.room
                    || incarnation == 0
                    || !self.controls.contains_key(&peer)
                    || recovery.coordinator.current_leader() != Some(recovery.incarnation)
                {
                    return Ok(true);
                }
                let admitted = self.admissions.values().any(|admission| {
                    admission.primary_endpoint == peer && admission.incarnation == incarnation
                });
                if !admitted {
                    return Ok(true);
                }
                self.emit(Event::PeerDeparted {
                    epoch: self.epoch,
                    peer,
                })?;
                Ok(true)
            }
            CoordinationControl::Admission { admission } => {
                let Some(recovery) = self.recovery.clone() else {
                    // sf4e2/emd2 carries only the authenticated primary room
                    // ticket. The host's Admission response on that same
                    // control stream is the authority lookup: it supplies the
                    // committed coordination endpoint and process incarnation
                    // without asking C++ to know an Iroh identity.
                    let Some(invite) = self.room_invite.clone() else {
                        return Err(failed("coordination admission unavailable"));
                    };
                    if admission.room != invite.room()
                        || admission.room != self.room.unwrap_or([0; 16])
                        || admission.primary_endpoint != peer
                        || admission.coordination_endpoint == self.endpoint.id()
                        || !admission.identity_consistent()
                        || admission.authority_term == 0
                    {
                        return Err(failed("invalid coordination authority response"));
                    }
                    let session = crate::recovery::RecoverySession::join_in(
                        admission.room,
                        self.endpoint.id(),
                        admission.incarnation,
                        admission.coordination_address.clone(),
                        self.relay_only,
                        self.server_owned(),
                    )
                    .await?;
                    self.remember_admission(admission.clone());
                    self.remember_admission(session.advertise().await);
                    self.recovery = Some(session);
                    // Accepted here and now: the authority's own binding.
                    self.bind_control_session(peer, admission.incarnation)?;
                    self.last_coordination_state = None;
                    self.last_control_rebound = None;
                    // Complete the reciprocal authenticated binding now that
                    // the guest has a live coordination endpoint.
                    self.send_coordination_control(peer);
                    self.emit_coordination_state().await?;
                    return Ok(true);
                };
                // An Admission describes the presenting peer's own process:
                // never this process's identity, and never an incarnation or
                // coordination key that another accepted admission holds. The
                // leader route and the refreshed invitation are resolved
                // through those records.
                if admission.room != recovery.room
                    || admission.primary_endpoint != peer
                    || admission.incarnation == recovery.incarnation
                    || admission.coordination_endpoint == recovery.coordination_endpoint
                    || self.admission_conflicts(&admission)
                {
                    return Err(failed("invalid coordination member binding"));
                }
                if self.retired_incarnations.contains(&admission.incarnation)
                    || self
                        .pending_retired_incarnations
                        .contains(&admission.incarnation)
                {
                    return Err(failed("retired coordination incarnation"));
                }
                let (applied, applied_history) = recovery.applied_membership_provenance().await;
                if admission.incarnation != recovery.incarnation
                    && !applied.contains(&admission.incarnation)
                    && (self
                        .applied_admission_members
                        .contains(&admission.incarnation)
                        || applied_history.contains(&admission.incarnation))
                {
                    // The actor has previously observed this incarnation in
                    // applied membership, so an Admission after its applied
                    // exclusion is a replay even if the former leader died
                    // before its local tombstone broadcast.
                    self.pending_retired_incarnations
                        .insert(admission.incarnation);
                    return Err(failed("retired coordination incarnation"));
                }
                let historical_retirements = applied_history.difference(&applied).count();
                if !applied_history.contains(&admission.incarnation)
                    && (applied_history.len() >= crate::coordination::MAX_MEMBER_HISTORY
                        || historical_retirements >= MAX_RETIRED_INCARNATIONS)
                {
                    return Err(failed("coordination membership history full"));
                }
                let add_and_promote =
                    recovery.coordinator.current_leader() == Some(recovery.incarnation);
                if add_and_promote {
                    self.supersede_endpoint_incarnations(&recovery, &admission)
                        .await;
                }
                // Held apart until the operation below validates it; only its
                // completion installs the record.
                self.hold_pending_admission(admission.clone());
                let incarnation = admission.incarnation;
                // The binding is accepted by the asynchronous operation this
                // control presents. Until then nothing native is read from it,
                // so the native side learns of the session before any message
                // from it. A reconnect re-presenting the bound incarnation
                // stays bound and presents nothing to settle.
                let binding = self.controls.get_mut(&peer).and_then(|control| {
                    if control.session() == Session::Bound(incarnation) {
                        return None;
                    }
                    control.set_session(Session::Pending(incarnation));
                    Some(ControlBinding {
                        peer,
                        control: control.id(),
                        incarnation,
                    })
                });
                self.queue_admission_operation(peer, vec![admission], add_and_promote, binding)?;
                Ok(true)
            }
            CoordinationControl::Membership {
                admissions,
                retired,
            } => {
                let Some(recovery) = self.recovery.clone() else {
                    return Ok(false);
                };
                let (applied, applied_history) = recovery.applied_membership_provenance().await;
                let leader_primary = if let Some(leader) = recovery.coordinator.current_leader() {
                    self.admissions
                        .get(&leader)
                        .map(|admission| admission.primary_endpoint)
                } else if applied.is_empty() && applied_history.is_empty() {
                    // A fresh learner has no local leader until its first Raft
                    // membership arrives. During only that empty bootstrap
                    // state, trust the authority cryptographically bound by
                    // the room invitation and authenticated primary control.
                    self.room_invite
                        .as_ref()
                        .and_then(|invite| {
                            self.admissions.values().find(|admission| {
                                admission.room == recovery.room
                                    && admission.primary_endpoint == invite.endpoint()
                                    && (invite.authority_incarnation() == 0
                                        || admission.incarnation == invite.authority_incarnation())
                            })
                        })
                        .map(|admission| admission.primary_endpoint)
                } else {
                    None
                };
                if leader_primary != Some(peer) {
                    return Err(failed("membership authority is not current leader"));
                }
                for incarnation in retired {
                    if incarnation != recovery.incarnation {
                        self.record_retirement(incarnation, &applied_history);
                        self.pending_retired_incarnations.insert(incarnation);
                    }
                }
                let historical_retirements = applied_history.difference(&applied).count();
                let mut accepted = Vec::new();
                for admission in admissions {
                    if admission.room != recovery.room
                        || admission.primary_endpoint == self.endpoint.id()
                        || admission.incarnation == recovery.incarnation
                        || admission.coordination_endpoint == recovery.coordination_endpoint
                        || self.admission_conflicts(&admission)
                        || self.retired_incarnations.contains(&admission.incarnation)
                        || self
                            .pending_retired_incarnations
                            .contains(&admission.incarnation)
                    {
                        continue;
                    }
                    if applied_history.contains(&admission.incarnation)
                        && !applied.contains(&admission.incarnation)
                    {
                        self.pending_retired_incarnations
                            .insert(admission.incarnation);
                        continue;
                    }
                    if !applied_history.contains(&admission.incarnation)
                        && (applied_history.len() >= crate::coordination::MAX_MEMBER_HISTORY
                            || historical_retirements >= MAX_RETIRED_INCARNATIONS)
                    {
                        return Err(failed("coordination membership history full"));
                    }
                    accepted.push(admission);
                }
                self.last_coordination_state = None;
                self.last_control_rebound = None;
                self.emit_coordination_state().await?;
                self.queue_admission_operation(peer, accepted, false, None)?;
                Ok(true)
            }
            CoordinationControl::ProbeReservation {
                room,
                source: claimed_source,
                source_incarnation,
                target_incarnation,
                request,
                pair_revision,
                term,
                expires,
            } => {
                self.accept_probe_reservation(
                    peer,
                    room,
                    claimed_source,
                    source_incarnation,
                    target_incarnation,
                    request,
                    pair_revision,
                    term,
                    expires,
                )
                .await
            }
        }
    }

    /// A joiner's control to the room's authority closed. The authority closes
    /// it to refuse an Admission it cannot take, for example a retired
    /// incarnation or a full membership history, and the joiner then redials
    /// with the same Admission. Once its coordination member has appeared in
    /// the applied membership a lost control is an ordinary reconnect; before
    /// that, `MAX_JOIN_CONTROL_LOSSES` in a row end the join with `join_failed`.
    /// Returns true when the room was cleared.
    pub(super) async fn join_control_lost(&mut self, peer: EndpointId) -> io::Result<bool> {
        if self.join_settled
            || self.hosted.is_some()
            || self
                .room_invite
                .as_ref()
                .is_none_or(|invite| invite.endpoint() != peer)
        {
            return Ok(false);
        }
        if let Some(recovery) = &self.recovery
            && recovery
                .applied_member_ids()
                .await
                .contains(&recovery.incarnation)
        {
            self.join_settled = true;
            return Ok(false);
        }
        self.join_control_losses = self.join_control_losses.saturating_add(1);
        let first_loss = *self
            .join_first_loss
            .get_or_insert_with(tokio::time::Instant::now);
        if self.join_control_losses < MAX_JOIN_CONTROL_LOSSES
            || first_loss.elapsed() < JOIN_GIVE_UP_AFTER
        {
            return Ok(false);
        }
        self.clear_room();
        self.error_because(0, "join_failed", Some("control_lost"))?;
        Ok(true)
    }

    pub(super) async fn poll_controls(&mut self) -> io::Result<()> {
        // A replacement waiting behind a worker that has since been removed,
        // or that ran out of time, is installed before anything is polled.
        let parked: Vec<_> = self.parked_controls.keys().copied().collect();
        for peer in parked {
            self.settle_parked_control(peer).await?;
        }
        let peers: Vec<_> = self.controls.keys().copied().collect();
        for peer in peers {
            for _ in 0..CONTROL_POLL_BUDGET {
                // Leave space for terminal/control lifecycle events. A remote
                // flood backpressures this peer's bounded worker instead of
                // exhausting the global queue and killing healthy gameplay.
                if !self.events.has_headroom() {
                    break;
                }
                // A control whose session binding is still being applied
                // keeps its frames queued: the native side must hear of the
                // session before any message from it, and a failed binding
                // closes the control with nothing delivered.
                let frame = self
                    .controls
                    .get_mut(&peer)
                    .filter(|control| !matches!(control.session(), Session::Pending(_)))
                    .and_then(|control| control.try_receive());
                if let Some(frame) = frame {
                    match String::from_utf8(frame.payload) {
                        Ok(payload) => {
                            let coordination =
                                self.accept_coordination_control(peer, &payload).await;
                            if coordination.is_err() {
                                // A stale or unauthenticated coordination
                                // control is scoped to this peer. Close that
                                // route while retaining independent gameplay
                                // bridges and the rest of the room.
                                let retry_join = self.recovery.is_none()
                                    && self
                                        .room_invite
                                        .as_ref()
                                        .is_some_and(|invite| invite.endpoint() == peer);
                                let control = self.remove_control(peer);
                                if retry_join {
                                    self.reconnect_control(peer);
                                }
                                let _ = self.emit_bulk(Event::ControlClosed {
                                    epoch: self.epoch,
                                    peer,
                                    control,
                                });
                                if self.join_control_lost(peer).await? {
                                    return Ok(());
                                }
                                break;
                            }
                            if coordination.unwrap_or(false) {
                                // Any roster mutation is published by its
                                // authenticated asynchronous completion.
                            } else if let Ok(native) =
                                serde_json::from_str::<NativeControlMessage>(&payload)
                                && native.kind == "native_control"
                                && native.message_id > 1
                                && !native.payload.is_empty()
                            {
                                self.emit(Event::Message {
                                    epoch: self.epoch,
                                    peer,
                                    message_id: native.message_id,
                                    payload: native.payload,
                                })?;
                            } else {
                                self.emit(Event::Message {
                                    epoch: self.epoch,
                                    peer,
                                    message_id: frame.message_id,
                                    payload,
                                })?;
                            }
                        }
                        Err(_) => {
                            let control = self.remove_control(peer);
                            self.reconnect_control(peer);
                            self.emit(Event::ControlClosed {
                                epoch: self.epoch,
                                peer,
                                control,
                            })?;
                            break;
                        }
                    }
                } else {
                    break;
                }
            }
            // A closed transport is not the end of its inbound frames: the
            // peer may have finished a departure and closed while more than
            // one poll budget of frames was still queued. The worker stays,
            // and keeps being drained under the same budget, until its
            // reader has ended and the queue is empty.
            if self
                .controls
                .get(&peer)
                .is_some_and(|control| control.is_closed() && control.is_drained())
            {
                // A parked replacement takes over from the drained worker
                // without a close event for it.
                if self.parked_controls.contains_key(&peer) {
                    self.settle_parked_control(peer).await?;
                    continue;
                }
                let control = self.remove_control(peer);
                self.reconnect_control(peer);
                self.emit(Event::ControlClosed {
                    epoch: self.epoch,
                    peer,
                    control,
                })?;
                if self.join_control_lost(peer).await? {
                    return Ok(());
                }
            }
        }
        // A pending native game authorization belongs to the committed room
        // state. During a coordinated quorum outage keep its bounded listener
        // alive and let the new authority either authorize it or commit its
        // cancellation. Prepared listeners retain a renewable 60-second
        // pre-stream wait; once a gameplay stream arrives, the transport
        // keeps its separate ten-second marker/proof handshake deadline.
        if self.recovery.is_some() && !self.coordination_writable {
            let deadline = tokio::time::Instant::now() + transport::PREPARED_GAME_TIMEOUT;
            for slot in self.games.values_mut().filter(|slot| slot.waiting) {
                if slot.expires < deadline {
                    slot.expires = deadline;
                }
            }
        }
        let expired: Vec<_> = self
            .games
            .iter()
            .filter(|(_, slot)| {
                // A candidate in its handshake keeps the slot until that
                // handshake's own deadline; a rejection then leaves the slot
                // to expire here.
                slot.waiting
                    && slot.candidate.is_none()
                    && tokio::time::Instant::now() >= slot.expires
            })
            .map(|(peer, slot)| (*peer, slot.auth.key.generation))
            .collect();
        for (peer, generation) in expired {
            let slot = self.games.remove(&peer);
            self.emit(Event::GameClosed {
                epoch: self.epoch,
                peer,
                generation,
                reason: slot.and_then(|slot| slot.unlinked_reason("prepare window expired")),
            })?;
        }
        Ok(())
    }

    pub(super) async fn completed_control(
        &mut self,
        epoch: u64,
        result: io::Result<ControlChannel>,
        joined_invite: Option<Invite>,
    ) -> io::Result<()> {
        match result {
            Ok(channel) => {
                let peer = channel.connection.remote_id();
                let replacing = self.controls.contains_key(&peer);
                if epoch != self.epoch
                    || self.room.is_none()
                    || (!replacing && self.controls.len() >= self.max_control_peers())
                {
                    channel.connection.close(1u32.into(), b"room unavailable");
                    return Ok(());
                }
                // An early check, for a prompt refusal. The candidate is a
                // reservation while it waits; the install decides again.
                // `Accepted` has gone out, so the joiner sees a refusal here
                // (and at the install) as its control closing, not as
                // `refused`.
                if self.refuse_public_control(&channel) {
                    channel.connection.close(1u32.into(), b"admission ended");
                    return Ok(());
                }
                // The QUIC identity and room proof authenticate this
                // as a new connection from the same helper endpoint.
                // Supersede the old worker even if its remote close has
                // not propagated yet; coordination admission still
                // fences stale process incarnations independently. The
                // old worker is closed, and the replacement waits behind
                // it until the frames it already received are delivered.
                // Every replacement takes this one path: a candidate still
                // parked for the peer is superseded first, the new one
                // records the worker it waits behind, and the common
                // settle routine installs it at once when that worker has
                // nothing left to deliver.
                if let Some(previous) = self.parked_controls.remove(&peer) {
                    previous
                        .channel
                        .connection
                        .close(1u32.into(), b"control replaced");
                }
                let behind = self.controls.get(&peer).map(|old| {
                    old.close();
                    old.id()
                });
                self.parked_controls.insert(
                    peer,
                    ParkedControl {
                        epoch,
                        channel,
                        joined_invite,
                        behind,
                        since: tokio::time::Instant::now(),
                    },
                );
                self.settle_parked_control(peer).await
            }
            Err(error) if epoch == self.epoch && self.opening => {
                let reason = network::join_error_reason(
                    &error,
                    network::home_relay_connected(&self.endpoint),
                    self.server_owned(),
                );
                self.clear_room();
                self.error_because(0, "join_failed", reason)
            }
            Err(_) => Ok(()), // Rejected inbound peer: keep the host's room alive.
        }
    }

    /// Start the worker for an accepted control connection and announce it.
    /// The peer has no worker when this runs.
    async fn install_control(
        &mut self,
        epoch: u64,
        channel: ControlChannel,
        joined_invite: Option<Invite>,
    ) -> io::Result<()> {
        let peer = channel.connection.remote_id();
        self.opening = false;
        self.controls.insert(
            peer,
            ControlWorker::start_waking(channel, self.control_wake.clone()),
        );
        if let Some(invite) = joined_invite.as_ref() {
            self.room_invite = Some(invite.clone());
            // sf4e2/emd2 intentionally omits the private
            // coordination endpoint. The authenticated host
            // Admission frame on this primary control stream
            // performs the authority lookup before recovery
            // starts. sf4e3/full invites can bootstrap
            // directly from their committed route.
            let setup = invite.coordination_address().is_some();
            if setup && self.setup_join_recovery(invite).await.is_err() {
                let control = self.remove_control(peer);
                let _ = self.emit_bulk(Event::ControlClosed {
                    epoch,
                    peer,
                    control,
                });
                self.reconnect_control(peer);
                self.error(0, "coordination_unavailable")?;
                return Ok(());
            }
        }
        // The caller checked this; `&mut self` is held across the await between.
        let Some(room) = self.room else {
            return Ok(());
        };
        let control = self.controls.get(&peer).map(ControlWorker::id).unwrap_or(0);
        let account = self
            .controls
            .get(&peer)
            .and_then(ControlWorker::account)
            .map(ToString::to_string);
        self.emit(Event::Connected {
            epoch,
            peer,
            room,
            control,
            account,
        })?;
        if let Some(invite) = joined_invite {
            self.emit(Event::DiscordInvite {
                epoch,
                invitation: invite.encode()?,
                secret: invite.encode_discord()?,
            })?;
        }
        self.send_coordination_control(peer);
        self.last_coordination_state = None;
        self.last_control_rebound = None;
        self.emit_coordination_state().await
    }
}
