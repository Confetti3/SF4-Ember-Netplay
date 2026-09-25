//! Control-peer connections, the coordination handshake and inbound control frames.
use super::*;

impl Actor {
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
            || self.tasks.len() >= MAX_TASKS
            || self.room.is_none()
        {
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
        self.tasks.spawn(async move {
            tokio::time::sleep(Duration::from_millis(250)).await;
            Completion::Reconnect(
                epoch,
                target,
                transport::connect_control_to(&endpoint, address, &invite).await,
            )
        });
    }

    /// A replacement control from the same admitted endpoint is safe only
    /// after the previous transport has actually closed.  The bounded worker
    /// is removed on the next poll tick in the usual case; this helper closes
    /// the small race where a reconnect arrives between transport completion
    /// and that tick.  An open worker is never replaced, preserving the
    /// endpoint-to-incarnation binding and its message ordering.
    pub(super) fn remove_closed_control(&mut self, peer: EndpointId) {
        if self
            .controls
            .get(&peer)
            .is_some_and(|control| control.is_closed())
        {
            self.controls.remove(&peer);
        }
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
        if lost_leader == recovery.incarnation
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
        let admissions = self
            .admissions
            .values()
            .filter(|admission| !retired.contains(&admission.incarnation))
            .cloned()
            .collect();
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
                        || admission.incarnation == 0
                        || admission.authority_term == 0
                    {
                        return Err(failed("invalid coordination authority response"));
                    }
                    let session = crate::recovery::RecoverySession::join(
                        admission.room,
                        self.endpoint.id(),
                        admission.incarnation,
                        admission.coordination_address.clone(),
                        self.relay_only,
                    )
                    .await?;
                    self.remember_admission(admission.clone());
                    self.remember_admission(session.advertise().await);
                    self.recovery = Some(session);
                    self.last_coordination_state = None;
                    self.last_control_rebound = None;
                    // Complete the reciprocal authenticated binding now that
                    // the guest has a live coordination endpoint.
                    self.send_coordination_control(peer);
                    self.emit_coordination_state().await?;
                    return Ok(true);
                };
                if admission.room != recovery.room || admission.primary_endpoint != peer {
                    return Err(failed("invalid coordination member binding"));
                }
                if self.retired_incarnations.contains(&admission.incarnation)
                    || self
                        .pending_retired_incarnations
                        .contains(&admission.incarnation)
                    || (!self.admissions.contains_key(&admission.incarnation)
                        && self.retired_incarnations.len() >= MAX_RETIRED_INCARNATIONS)
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
                self.remember_admission(admission.clone());
                self.queue_admission_operation(peer, vec![admission], add_and_promote)?;
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
                        if self.retired_incarnations.len() < MAX_RETIRED_INCARNATIONS {
                            self.retired_incarnations.insert(incarnation);
                        }
                        self.pending_retired_incarnations.insert(incarnation);
                    }
                }
                let historical_retirements = applied_history.difference(&applied).count();
                let mut accepted = Vec::new();
                for admission in admissions {
                    if admission.room != recovery.room
                        || admission.primary_endpoint == self.endpoint.id()
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
                self.queue_admission_operation(peer, accepted, false)?;
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
                let Some(recovery) = self.recovery.clone() else {
                    return Ok(false);
                };
                let state = recovery.state().await;
                if room != recovery.room
                    || claimed_source != peer
                    || request == 0
                    || state.term != term
                    || !state.writable
                    || expires < now().unwrap_or(u64::MAX)
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
                    || !recovery
                        .probe_pair_bound(
                            source_incarnation,
                            target_incarnation,
                            peer,
                            self.endpoint.id(),
                            pair_revision,
                        )
                        .await
                {
                    return Err(failed("invalid probe reservation"));
                }
                let applied = timeout(Duration::from_secs(5), async {
                    loop {
                        if recovery
                            .probe_reserved(
                                source_incarnation,
                                target_incarnation,
                                request,
                                pair_revision,
                                term,
                                expires,
                            )
                            .await
                        {
                            break true;
                        }
                        tokio::time::sleep(Duration::from_millis(25)).await;
                    }
                })
                .await
                .unwrap_or(false);
                if !applied {
                    return Err(failed("probe reservation not applied"));
                }
                self.probe_permissions.insert(
                    peer,
                    ProbePermission {
                        request,
                        pair_revision,
                        expires: tokio::time::Instant::now()
                            + Duration::from_secs(expires.saturating_sub(now().unwrap_or(expires))),
                    },
                );
                self.probe_peers.insert(peer);
                Ok(true)
            }
        }
    }

    pub(super) async fn poll_controls(&mut self) -> io::Result<()> {
        let peers: Vec<_> = self.controls.keys().copied().collect();
        for peer in peers {
            for _ in 0..CONTROL_POLL_BUDGET {
                // Leave space for terminal/control lifecycle events. A remote
                // flood backpressures this peer's bounded worker instead of
                // exhausting the global queue and killing healthy gameplay.
                if !self.events.has_headroom() {
                    break;
                }
                let frame = self
                    .controls
                    .get_mut(&peer)
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
                                self.controls.remove(&peer);
                                if retry_join {
                                    self.reconnect_control(peer);
                                }
                                let _ = self.emit_bulk(Event::ControlClosed {
                                    epoch: self.epoch,
                                    peer,
                                });
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
                            self.controls.remove(&peer);
                            self.reconnect_control(peer);
                            self.emit(Event::ControlClosed {
                                epoch: self.epoch,
                                peer,
                            })?;
                            break;
                        }
                    }
                } else {
                    break;
                }
            }
            if self
                .controls
                .get(&peer)
                .is_some_and(|control| control.is_closed())
            {
                self.controls.remove(&peer);
                self.reconnect_control(peer);
                self.emit(Event::ControlClosed {
                    epoch: self.epoch,
                    peer,
                })?;
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
            for slot in self
                .games
                .values_mut()
                .filter(|slot| slot.waiting || slot.prepare_deadline.is_some())
            {
                if slot.expires < deadline {
                    slot.expires = deadline;
                    if let Some(sender) = slot.prepare_deadline.as_ref() {
                        sender.send_replace(deadline);
                    }
                }
            }
        }
        let expired: Vec<_> = self
            .games
            .iter()
            .filter(|(_, slot)| {
                (slot.waiting || slot.prepare_deadline.is_some())
                    && tokio::time::Instant::now() >= slot.expires
            })
            .map(|(peer, slot)| (*peer, slot.auth.key.generation))
            .collect();
        for (peer, generation) in expired {
            self.games.remove(&peer);
            self.emit(Event::GameClosed {
                epoch: self.epoch,
                peer,
                generation,
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
                self.remove_closed_control(peer);
                let replacing = self.controls.contains_key(&peer);
                if epoch != self.epoch
                    || self.room.is_none()
                    || (!replacing && self.controls.len() >= MAX_CONTROL_PEERS)
                {
                    channel.connection.close(1u32.into(), b"room unavailable");
                    return Ok(());
                }
                // The QUIC identity and room proof authenticate this
                // as a new connection from the same helper endpoint.
                // Supersede the old worker even if its remote close has
                // not propagated yet; coordination admission still
                // fences stale process incarnations independently.
                if replacing {
                    self.controls.remove(&peer);
                }
                self.opening = false;
                self.controls.insert(peer, ControlWorker::start(channel));
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
                        self.controls.remove(&peer);
                        let _ = self.emit_bulk(Event::ControlClosed { epoch, peer });
                        self.reconnect_control(peer);
                        self.error(0, "coordination_unavailable")?;
                        return Ok(());
                    }
                }
                // Checked above; `&mut self` is held across the await between.
                let Some(room) = self.room else {
                    return Ok(());
                };
                self.emit(Event::Connected { epoch, peer, room })?;
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
                self.emit_coordination_state().await?;
            }
            Err(_) if epoch == self.epoch && self.opening => {
                self.clear_room();
                self.error(0, "join_failed")?;
            }
            Err(_) => (), // Rejected inbound peer: keep the host's room alive.
        }
        Ok(())
    }
}
