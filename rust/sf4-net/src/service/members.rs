//! Admission, retirement and departure of room members.
use super::*;

/// Each proof a departing helper waits for (removal, promotion, successor
/// confirmation) gets this long before Leave reports the step as failed.
const LEAVE_STEP_TIMEOUT: Duration = Duration::from_secs(5);
const LEAVE_POLL_INTERVAL: Duration = Duration::from_millis(25);

impl Actor {
    pub(super) fn remember_admission(&mut self, admission: Admission) {
        let incarnation = admission.incarnation;
        if self.retired_incarnations.contains(&incarnation)
            || self.pending_retired_incarnations.contains(&incarnation)
        {
            return;
        }
        self.admissions.insert(incarnation, admission);
        if !self.admission_order.contains(&incarnation) {
            self.admission_order.push(incarnation);
        }
    }

    pub(super) fn apply_confirmed_retirements(&mut self, confirmed: BTreeSet<u64>) {
        for incarnation in confirmed {
            // Keep the exact retirement fence bounded. Once the room has
            // accumulated the configured number of historical incarnations,
            // leave this departure pending and fail closed: the RPC binding
            // is already read-only, and evicting the ID from the pending set
            // would let a lagging peer replay the old Admission after a later
            // control reconnect.
            self.admissions.remove(&incarnation);
            self.admission_order.retain(|id| *id != incarnation);
            if !self.retired_incarnations.contains(&incarnation)
                && self.retired_incarnations.len() >= MAX_RETIRED_INCARNATIONS
            {
                continue;
            }
            self.retired_incarnations.insert(incarnation);
            self.pending_retired_incarnations.remove(&incarnation);
        }
    }

    pub(super) fn schedule_membership_reconciliation(
        &mut self,
        retained: BTreeSet<EndpointId>,
        term: u64,
        revision: u64,
    ) {
        if retained.is_empty() || !retained.contains(&self.endpoint.id()) {
            return;
        }
        let roster_changed = self.committed_native_members.as_ref() != Some(&retained);
        if roster_changed {
            let previous = self.committed_native_members.clone().unwrap_or_default();
            let removed = self
                .admissions
                .values()
                .filter(|admission| {
                    previous.contains(&admission.primary_endpoint)
                        && !retained.contains(&admission.primary_endpoint)
                })
                .map(|admission| admission.incarnation);
            // The roster is already committed. Fence every omitted identity
            // before any asynchronous membership write can stall or lose its
            // waiter during a leadership change.
            self.pending_retired_incarnations.extend(removed);
            self.committed_native_members = Some(retained.clone());
        }
        if !roster_changed && self.pending_retired_incarnations.is_empty() {
            return;
        }
        self.spawn_membership_operation(retained, term, revision);
    }

    pub(super) fn schedule_pending_membership_reconciliation(&mut self, term: u64, revision: u64) {
        if self.pending_retired_incarnations.is_empty() {
            return;
        }
        let retained = self.committed_native_members.clone().unwrap_or_else(|| {
            self.admissions
                .values()
                .map(|admission| admission.primary_endpoint)
                .chain(std::iter::once(self.endpoint.id()))
                .collect()
        });
        self.spawn_membership_operation(retained, term, revision);
    }

    pub(super) fn spawn_membership_operation(
        &mut self,
        retained: BTreeSet<EndpointId>,
        term: u64,
        revision: u64,
    ) {
        if self.pending_membership_operation.is_some() || self.tasks.len() >= MAX_TASKS {
            return;
        }
        let Some(recovery) = self.recovery.clone() else {
            return;
        };
        let sequence = self.next_coordination_operation;
        self.next_coordination_operation = self.next_coordination_operation.saturating_add(1);
        let key = MembershipOperationKey {
            sequence,
            epoch: self.epoch,
            room: recovery.room,
            incarnation: recovery.incarnation,
            term,
            revision,
        };
        let pending = self.pending_retired_incarnations.clone();
        let admissions = self.admissions.values().cloned().collect::<Vec<_>>();
        self.pending_membership_operation = Some(key.clone());
        self.tasks.spawn(async move {
            let result = async {
                if recovery.coordinator.current_term() != term {
                    return Err(failed("obsolete membership operation"));
                }
                if recovery.coordinator.current_leader() == Some(recovery.incarnation) {
                    let applied_voters = recovery.applied_voter_ids().await;
                    let voter_removals = pending
                        .intersection(&applied_voters)
                        .copied()
                        .collect::<BTreeSet<_>>();
                    if !voter_removals.is_empty() {
                        let replacement = applied_voters
                            .difference(&voter_removals)
                            .copied()
                            .collect::<BTreeSet<_>>();
                        if replacement.is_empty() {
                            return Err(failed("empty replacement membership"));
                        }
                        recovery.remove_members(replacement).await?;
                    }
                    if recovery.coordinator.current_term() != term {
                        return Err(failed("obsolete membership operation"));
                    }
                    let voters_after = recovery.applied_voter_ids().await;
                    for admission in admissions.iter().filter(|admission| {
                        retained.contains(&admission.primary_endpoint)
                            && !voters_after.contains(&admission.incarnation)
                    }) {
                        recovery.add_learner(admission).await?;
                    }
                    let members = recovery.applied_member_ids().await;
                    let learner_removals = pending
                        .intersection(&members)
                        .filter(|incarnation| !voters_after.contains(incarnation))
                        .copied()
                        .collect::<BTreeSet<_>>();
                    recovery.remove_nodes(learner_removals).await?;
                }
                let members = recovery.applied_member_ids().await;
                let confirmed_retirements = pending
                    .difference(&members)
                    .copied()
                    .collect::<BTreeSet<_>>();
                for incarnation in &confirmed_retirements {
                    recovery.rpc.retire(*incarnation).await;
                }
                Ok(MembershipOperationResult {
                    confirmed_retirements,
                })
            }
            .await;
            Completion::MembershipReconciliation(key, result)
        });
    }

    pub(super) fn queue_admission_operation(
        &mut self,
        peer: EndpointId,
        admissions: Vec<Admission>,
        add_member_if_leader: bool,
    ) -> io::Result<()> {
        if admissions.is_empty() {
            return Ok(());
        }
        let fingerprint = serde_json::to_vec(&admissions)
            .map_err(|_| failed("coordination admission encoding failed"))?;
        let duplicate_active = self
            .pending_admission_operation
            .as_ref()
            .is_some_and(|key| {
                key.peer == peer
                    && key.fingerprint == fingerprint
                    && key.add_member_if_leader == add_member_if_leader
            });
        let duplicate_deferred = self.deferred_admissions.iter().any(|deferred| {
            deferred.peer == peer
                && deferred.fingerprint == fingerprint
                && deferred.add_member_if_leader == add_member_if_leader
        });
        if duplicate_active || duplicate_deferred {
            return Ok(());
        }
        if self.deferred_admissions.len() >= MAX_CONTROL_PEERS {
            return Err(failed("coordination admission busy"));
        }
        self.deferred_admissions.push_back(DeferredAdmission {
            peer,
            admissions,
            add_member_if_leader,
            fingerprint,
        });
        self.start_next_admission_operation();
        Ok(())
    }

    pub(super) fn start_next_admission_operation(&mut self) {
        if self.pending_admission_operation.is_some() || self.tasks.len() >= MAX_TASKS {
            return;
        }
        let Some(deferred) = self.deferred_admissions.pop_front() else {
            return;
        };
        let Some(recovery) = self.recovery.clone() else {
            self.deferred_admissions.clear();
            return;
        };
        let sequence = self.next_coordination_operation;
        self.next_coordination_operation = self.next_coordination_operation.saturating_add(1);
        let key = AdmissionOperationKey {
            sequence,
            epoch: self.epoch,
            room: recovery.room,
            incarnation: recovery.incarnation,
            term: recovery.coordinator.current_term(),
            leader: recovery.coordinator.current_leader(),
            peer: deferred.peer,
            fingerprint: deferred.fingerprint,
            add_member_if_leader: deferred.add_member_if_leader,
        };
        let admissions = deferred.admissions;
        let add_and_promote =
            deferred.add_member_if_leader && key.leader == Some(recovery.incarnation);
        let pending_retired = self.pending_retired_incarnations.clone();
        let retired = self.retired_incarnations.clone();
        let mut candidates = self.admission_order.clone();
        for admission in self.admissions.values().chain(admissions.iter()) {
            if !candidates.contains(&admission.incarnation) {
                candidates.push(admission.incarnation);
            }
        }
        let desired = crate::recovery::stable_voter_count(
            self.admissions
                .keys()
                .copied()
                .chain(admissions.iter().map(|admission| admission.incarnation))
                .collect::<BTreeSet<_>>()
                .len(),
        );
        self.pending_admission_operation = Some(key.clone());
        self.tasks.spawn(async move {
            let operation = async {
                if recovery.coordinator.current_term() != key.term
                    || recovery.coordinator.current_leader() != key.leader
                {
                    return Err(failed("obsolete coordination admission"));
                }
                if add_and_promote {
                    for admission in &admissions {
                        recovery.add_learner(admission).await?;
                    }
                    if recovery.coordinator.current_term() == key.term
                        && recovery.coordinator.current_leader() == key.leader
                    {
                        let current = recovery.applied_voter_ids().await;
                        let mut voters = current
                            .into_iter()
                            .filter(|id| !pending_retired.contains(id) && !retired.contains(id))
                            .collect::<BTreeSet<_>>();
                        for id in candidates {
                            if voters.len() >= desired {
                                break;
                            }
                            if !pending_retired.contains(&id) && !retired.contains(&id) {
                                voters.insert(id);
                            }
                        }
                        if !voters.is_empty() && voters.len() <= crate::recovery::MAX_VOTERS {
                            // Admission is already durable once AddLearner
                            // succeeds. Preserve that authenticated binding if
                            // a later voter promotion loses authority or fails;
                            // the serialized next refresh retries promotion.
                            let _ = recovery.promote_voters(voters).await;
                        }
                    }
                } else {
                    for admission in &admissions {
                        recovery.admit(admission).await?;
                    }
                }
                let (members, history) = recovery.applied_membership_provenance().await;
                if admissions.iter().any(|admission| {
                    history.contains(&admission.incarnation)
                        && !members.contains(&admission.incarnation)
                }) {
                    return Err(failed("retired coordination incarnation"));
                }
                Ok(AdmissionOperationResult { admissions })
            };
            let result = timeout(COORDINATION_ADMISSION_TIMEOUT, operation)
                .await
                .unwrap_or_else(|_| Err(failed("coordination admission timeout")));
            Completion::Admission(key, result)
        });
    }

    pub(super) async fn graceful_leave(&mut self, epoch: u64, abandon: bool) -> io::Result<()> {
        if !self.matches(epoch) {
            return self.error(0, "stale_epoch");
        }
        // Leave can be retried while the native adapter drains its closing
        // event. Preserve the original authority proof window instead of
        // clearing the coordination route on the duplicate command.
        if self.retirement_started.is_some() {
            return self.emit(Event::RoomClosed { epoch });
        }
        if abandon {
            self.clear_room();
            return self.emit(Event::RoomClosed { epoch });
        }
        let retain_departure_authority = self.recovery.as_ref().is_some_and(|recovery| {
            recovery.coordinator.current_leader() == Some(recovery.incarnation)
        });
        if let Some(recovery) = self.recovery.clone() {
            let state = recovery.state().await;
            let voters = recovery.applied_voter_ids().await;
            let applied_members = recovery.applied_member_ids().await;
            let coordination_only_departure = self.hosted.is_none()
                && self.committed_native_members.is_none()
                && applied_members.contains(&recovery.incarnation)
                && !state.leader_local;
            let coordination_only_learner =
                coordination_only_departure && !voters.contains(&recovery.incarnation);
            if coordination_only_departure {
                // The primary/native Join may be rejected after the helper
                // has already been admitted to Raft. There is no native
                // checkpoint roster to reconcile in that case, so ask the
                // committed leader to remove this authenticated voter before
                // running the normal successor proof below.
                let (removed_or_promoted, committed_self_removal) =
                    timeout(LEAVE_STEP_TIMEOUT, async {
                        loop {
                            if recovery.coordinator.current_leader().is_none()
                                || recovery.coordinator.current_leader()
                                    == Some(recovery.incarnation)
                            {
                                break (true, false);
                            }
                            if recovery.request_self_removal().await.is_ok() {
                                // The leader's authenticated remove RPC does not
                                // reply until both the voter transition and exact
                                // RemoveNodes entry are committed. A removed
                                // follower may never receive that final entry, so
                                // this response is its durable exclusion proof.
                                break (true, true);
                            }
                            tokio::time::sleep(LEAVE_POLL_INTERVAL).await;
                        }
                    })
                    .await
                    .unwrap_or((false, false));
                if !removed_or_promoted {
                    self.error(0, "leave_membership_failed")?;
                    return Ok(());
                }
                if committed_self_removal {
                    self.clear_room();
                    return self.emit(Event::RoomClosed { epoch });
                }
                // A learner does not participate in the successor election,
                // but it must remain live until the explicit RemoveNodes
                // entry has applied locally. Closing earlier leaves its old
                // authenticated route active on lagging replicas.
                if coordination_only_learner {
                    let coordination_member_removed = timeout(LEAVE_STEP_TIMEOUT, async {
                        loop {
                            if !recovery
                                .applied_member_ids()
                                .await
                                .contains(&recovery.incarnation)
                            {
                                break true;
                            }
                            // OpenRaft may commit RemoveNodes on the leader
                            // and stop replicating to the learner before the
                            // departing process applies that final entry.
                            // The authenticated leader claim includes its
                            // complete applied membership, so it is the
                            // equivalent committed exclusion proof for this
                            // one-way departure route.
                            if recovery
                                .confirm_departure(state.term, recovery.committed().await.revision)
                                .await
                                .is_ok()
                            {
                                break true;
                            }
                            tokio::time::sleep(LEAVE_POLL_INTERVAL).await;
                        }
                    })
                    .await
                    .unwrap_or(false);
                    if !coordination_member_removed {
                        self.error(0, "leave_membership_unconfirmed")?;
                        return Ok(());
                    }
                }
            }
            if voters.contains(&recovery.incarnation) && voters.len() > 1 {
                if state.leader_local {
                    let mut successor_voters = voters.clone();
                    successor_voters.remove(&recovery.incarnation);
                    // OpenRaft commits joint old/new membership through the
                    // old quorum before the helper retires its route.
                    let successor = successor_voters.iter().next().copied();
                    let promoted = match timeout(
                        LEAVE_STEP_TIMEOUT,
                        recovery.promote_voters(successor_voters),
                    )
                    .await
                    {
                        Ok(Ok(())) => true,
                        Ok(Err(_)) | Err(_) => {
                            // A simultaneous non-native learner Leave may
                            // have committed the complementary singleton
                            // transition first. Treat that durable applied
                            // membership as the handoff proof instead of
                            // tearing down the actor with a race-specific
                            // failure.
                            let singleton = timeout(Duration::from_secs(2), async {
                                loop {
                                    let applied = recovery.applied_voter_ids().await;
                                    if applied.len() == 1 && applied.contains(&recovery.incarnation)
                                    {
                                        break true;
                                    }
                                    tokio::time::sleep(LEAVE_POLL_INTERVAL).await;
                                }
                            })
                            .await
                            .unwrap_or(false);
                            if singleton {
                                true
                            } else {
                                self.error(0, "leave_membership_failed")?;
                                false
                            }
                        }
                    };
                    if !promoted {
                        return Ok(());
                    }
                    if let Some(successor) = successor {
                        // OpenRaft may retain the old leader as a learner
                        // after the joint transition. Trigger the election
                        // on the committed successor so the retiring leader
                        // can obtain a real successor authority claim before
                        // it tears down its control route.
                        let _ = recovery.rpc.request_election(successor).await;
                    }
                }
                // A non-leader departure is committed by the native room
                // authority first. In both cases wait until this incarnation
                // is absent from the committed voter set instead of tearing
                // down a still-authoritative helper. A retain=false change
                // can stop replicating to the departing follower before its
                // local metrics learn that fact, so followers also query the
                // current leader's authenticated voter claim.
                let minimum_revision = recovery.committed().await.revision;
                let mut leader_confirmed_departure = false;
                let removed = timeout(LEAVE_STEP_TIMEOUT, async {
                    loop {
                        if !recovery
                            .applied_voter_ids()
                            .await
                            .contains(&recovery.incarnation)
                        {
                            break true;
                        }
                        // A simultaneous two-helper Leave can reach the
                        // successor after the old leader has committed the
                        // singleton membership but before the successor's
                        // command is handled.  There is then no remote
                        // successor left to query: the local committed
                        // membership and leader proof are the handoff.
                        if recovery.applied_voter_ids().await.len() == 1
                            && recovery.coordinator.current_leader() == Some(recovery.incarnation)
                        {
                            leader_confirmed_departure = true;
                            break true;
                        }
                        if recovery
                            .confirm_singleton_departure(state.term, minimum_revision)
                            .await
                            .is_ok()
                        {
                            leader_confirmed_departure = true;
                            break true;
                        }
                        if !state.leader_local
                            && recovery
                                .confirm_departure(state.term, minimum_revision)
                                .await
                                .is_ok()
                        {
                            leader_confirmed_departure = true;
                            break true;
                        }
                        tokio::time::sleep(LEAVE_POLL_INTERVAL).await;
                    }
                })
                .await
                .unwrap_or(false);
                if !removed {
                    return self.error(0, "leave_successor_unconfirmed");
                }
                let minimum_term = state.term;
                let confirmed = if leader_confirmed_departure {
                    true
                } else {
                    timeout(LEAVE_STEP_TIMEOUT, async {
                        loop {
                            if recovery
                                .confirm_available_successor(minimum_term, minimum_revision)
                                .await
                                .is_ok()
                            {
                                break true;
                            }
                            tokio::time::sleep(Duration::from_millis(50)).await;
                        }
                    })
                    .await
                    .unwrap_or(false)
                };
                if !confirmed {
                    // Keep the endpoint and gameplay links alive if no
                    // successor has a committed authority claim yet.
                    return self.error(0, "leave_successor_unconfirmed");
                }
                self.last_coordination_state = None;
                self.last_control_rebound = None;
                let _ = self.emit_coordination_state().await;
            }
            // A native RoomAction Leave may have committed the membership
            // removal before this helper receives the explicit Leave.  The
            // coordination-only learner path above has its own applied-member
            // exclusion proof; a native departure is confirmed by the
            // successor proof (or the already-established retirement grace).
            // Do not turn an unused local flag into a second, weaker proof.
        }
        // A simultaneous follower Leave can become the committed singleton
        // successor while this future is running. Keep that newly acquired
        // authority route alive for the same proof window as the old leader,
        // so the retiring peer can authenticate the handoff before shutdown.
        let became_successor = self.recovery.as_ref().is_some_and(|recovery| {
            recovery.coordinator.current_leader() == Some(recovery.incarnation)
        });
        if retain_departure_authority || became_successor {
            self.enter_departure_grace();
            self.emit(Event::RoomClosed { epoch })
        } else {
            self.clear_room();
            self.emit(Event::RoomClosed { epoch })
        }
    }

    /// Run the potentially slow membership handoff without starving the IPC
    /// command loop.  A partition can keep OpenRaft's membership commit
    /// pending until the five-second bounded operation timeout; status,
    /// shutdown, and an explicit abandon must still be serviceable during
    /// that interval.  The leave future owns the actor borrow, so these
    /// lifecycle responses use the captured event sender and immutable
    /// endpoint snapshot until the operation is cancelled.
    pub(super) async fn leave_command(
        &mut self,
        epoch: u64,
        abandon: bool,
        commands: &mut mpsc::Receiver<Request>,
        failed_ipc: &mut watch::Receiver<bool>,
    ) -> io::Result<bool> {
        let events = self.events.clone();
        let endpoint = self.endpoint.clone();
        let status_epoch = self.epoch;
        let status_peers = self.controls.len();
        let status_games = self.games.len();
        let mut leave = Box::pin(self.graceful_leave(epoch, abandon));
        loop {
            tokio::select! {
                result = &mut leave => {
                    result?;
                    return Ok(true);
                }
                changed = failed_ipc.changed() => {
                    drop(leave);
                    return match changed {
                        Ok(()) => Err(failed("IPC disconnected")),
                        Err(_) => Err(failed("IPC failure watcher closed")),
                    };
                }
                request = commands.recv() => {
                    let Some(request) = request else {
                        drop(leave);
                        return Err(failed("IPC disconnected"));
                    };
                    match request.command {
                        Command::Status => {
                            let _ = events.try_send(Event::Status {
                                request_id: request.id,
                                endpoint: endpoint.id(),
                                ip_transports: endpoint.bound_sockets().len(),
                                epoch: status_epoch,
                                peers: status_peers,
                                games: status_games,
                            });
                        }
                        Command::Shutdown => {
                            drop(leave);
                            return Ok(false);
                        }
                        Command::Leave { epoch: requested_epoch, abandon: true } => {
                            if requested_epoch != epoch || requested_epoch == 0 {
                                let _ = events.try_send(Event::Error {
                                    probe_failure: None,
                                    request_id: request.id,
                                    epoch: status_epoch,
                                    peer: None,
                                    code: "stale_epoch".into(),
                                });
                                continue;
                            }
                            drop(leave);
                            self.clear_room();
                            self.emit(Event::RoomClosed { epoch })?;
                            return Ok(true);
                        }
                        Command::Leave { epoch: requested_epoch, abandon: false } => {
                            let event = if requested_epoch == epoch && requested_epoch != 0 {
                                // The original leave future still owns the
                                // membership handoff.  A duplicate must not
                                // manufacture RoomClosed before the old
                                // quorum has committed our exclusion and a
                                // successor has proved its authority.
                                Event::Error {
                                    probe_failure: None,
                                    request_id: request.id,
                                    epoch: status_epoch,
                                    peer: None,
                                    code: "leave_in_progress".into(),
                                }
                            } else {
                                Event::Error {
                                    probe_failure: None,
                                    request_id: request.id,
                                    epoch: status_epoch,
                                    peer: None,
                                    code: "stale_epoch".into(),
                                }
                            };
                            let _ = events.try_send(event);
                        }
                        _ => {
                            let _ = events.try_send(Event::Error {
                                probe_failure: None,
                                request_id: request.id,
                                epoch: status_epoch,
                                peer: None,
                                code: "leave_in_progress".into(),
                            });
                        }
                    }
                }
            }
        }
    }

    /// Close native/control state while retaining only the coordination
    /// authority read route for a bounded handoff window. This is needed when
    /// two helpers issue healthy Leave commands together: the new follower
    /// can still query the departing leader after the leader has reported its
    /// own room_closed event, then both processes may shut down normally.
    pub(super) fn enter_departure_grace(&mut self) {
        self.hosted = None;
        self.room_invite = None;
        self.host_address = None;
        self.opening = false;
        self.controls.clear();
        self.games.clear();
        self.tasks.abort_all();
        self.admissions.clear();
        self.admission_order.clear();
        self.applied_admission_members.clear();
        self.incoming_transfer = None;
        self.pending_checkpoint_proposal = None;
        self.pending_checkpoint_retry = None;
        self.outgoing_transfer = None;
        self.committed_native_members = None;
        self.pending_retired_incarnations.clear();
        self.retired_incarnations.clear();
        self.pending_checkpoint_ack = None;
        self.pending_checkpoint_committed = None;
        self.pending_coordination_refresh = None;
        self.pending_membership_operation = None;
        self.pending_admission_operation = None;
        self.deferred_admissions.clear();
        self.pending_membership_publications.clear();
        self.last_coordination_state = None;
        self.last_control_rebound = None;
        self.unwritable_leader_since = None;
        self.reconnect_target = None;
        self.probe_reservations.clear();
        self.pending_game_admissions.clear();
        self.pending_probe_invalidations.clear();
        self.probe_peers.clear();
        self.probe_permissions.clear();
        self.pending_probe_authorizations.clear();
        self.retirement_started = Some(Instant::now());
    }

    pub(super) fn expire_departure_grace(&mut self) {
        if self
            .retirement_started
            .is_some_and(|started| started.elapsed() >= Duration::from_secs(30))
        {
            self.clear_room();
        }
    }

    pub(super) async fn completed_membership_reconciliation(
        &mut self,
        key: MembershipOperationKey,
        result: io::Result<MembershipOperationResult>,
    ) -> io::Result<()> {
        if self.pending_membership_operation.as_ref() != Some(&key) {
            return Ok(());
        }
        self.pending_membership_operation = None;
        let Some(recovery) = self.recovery.clone() else {
            return Ok(());
        };
        let committed = recovery.committed().await;
        let current = key.epoch == self.epoch
            && self.room == Some(key.room)
            && recovery.room == key.room
            && recovery.incarnation == key.incarnation
            && recovery.coordinator.current_term() == key.term
            && committed.revision == key.revision;
        if current && let Ok(result) = result {
            self.apply_confirmed_retirements(result.confirmed_retirements);
        }
        // Stale completions leave the pre-write retirement fences in
        // place. The next exact committed-roster refresh retries the
        // operation through the current authority.
        self.last_coordination_state = None;
        self.last_control_rebound = None;
        self.emit_coordination_state().await?;
        Ok(())
    }

    pub(super) async fn completed_admission(
        &mut self,
        key: AdmissionOperationKey,
        result: io::Result<AdmissionOperationResult>,
    ) -> io::Result<()> {
        if self.pending_admission_operation.as_ref() != Some(&key) {
            return Ok(());
        }
        self.pending_admission_operation = None;
        let Some(recovery) = self.recovery.clone() else {
            self.deferred_admissions.clear();
            return Ok(());
        };
        let current_room = key.epoch == self.epoch
            && self.room == Some(key.room)
            && recovery.room == key.room
            && recovery.incarnation == key.incarnation;
        let mut published_roster_changed = false;
        if current_room && let Ok(result) = result {
            let (members, history) = recovery.applied_membership_provenance().await;
            for admission in result.admissions {
                if self.retired_incarnations.contains(&admission.incarnation)
                    || self
                        .pending_retired_incarnations
                        .contains(&admission.incarnation)
                    || (history.contains(&admission.incarnation)
                        && !members.contains(&admission.incarnation))
                {
                    self.pending_retired_incarnations
                        .insert(admission.incarnation);
                    continue;
                }
                self.remember_admission(admission);
                published_roster_changed = true;
            }
        }
        if published_roster_changed
            && recovery.coordinator.current_leader() == Some(recovery.incarnation)
        {
            // Admission is asynchronous, so the old receive-site
            // broadcast observes the pre-admission roster. Publish
            // only after the authenticated binding is installed, and
            // retain every destination until its bounded worker queue
            // accepts the current full snapshot.
            self.queue_membership_publication();
        }
        self.last_coordination_state = None;
        self.last_control_rebound = None;
        self.emit_coordination_state().await?;
        self.start_next_admission_operation();
        Ok(())
    }
}
