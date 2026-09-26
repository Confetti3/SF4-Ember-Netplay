//! Admission and retirement of room members.
use super::*;

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
        restore_voters: bool,
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
        if !roster_changed && self.pending_retired_incarnations.is_empty() && !restore_voters {
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
        let reachable = self.controls.keys().copied().collect::<BTreeSet<_>>();
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
                    restore_stable_voters(
                        &recovery,
                        &retained,
                        &reachable,
                        &admissions,
                        &pending,
                        revision,
                    )
                    .await?;
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

    /// `binding` names the control that presented these admissions, if one
    /// did; the operation's outcome then settles that control's session. An
    /// identical operation already queued or running takes the binding over
    /// instead of running twice.
    pub(super) fn queue_admission_operation(
        &mut self,
        peer: EndpointId,
        admissions: Vec<Admission>,
        add_member_if_leader: bool,
        binding: Option<ControlBinding>,
    ) -> io::Result<()> {
        if admissions.is_empty() {
            return Ok(());
        }
        let fingerprint = serde_json::to_vec(&admissions)
            .map_err(|_| failed("coordination admission encoding failed"))?;
        let same = |key_peer: EndpointId, key_fingerprint: &[u8], key_add: bool| {
            key_peer == peer && key_fingerprint == fingerprint && key_add == add_member_if_leader
        };
        if self
            .pending_admission_operation
            .as_ref()
            .is_some_and(|key| same(key.peer, &key.fingerprint, key.add_member_if_leader))
        {
            self.pending_admission_bindings.extend(binding);
            return Ok(());
        }
        if let Some(deferred) = self.deferred_admissions.iter_mut().find(|deferred| {
            same(
                deferred.peer,
                &deferred.fingerprint,
                deferred.add_member_if_leader,
            )
        }) {
            deferred.bindings.extend(binding);
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
            bindings: binding.into_iter().collect(),
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
        self.pending_admission_bindings = deferred.bindings;
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
        let bindings = std::mem::take(&mut self.pending_admission_bindings);
        let Some(recovery) = self.recovery.clone() else {
            self.deferred_admissions.clear();
            return Ok(());
        };
        let current_room = key.epoch == self.epoch
            && self.room == Some(key.room)
            && recovery.room == key.room
            && recovery.incarnation == key.incarnation;
        let mut published_roster_changed = false;
        let mut accepted = BTreeSet::new();
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
                accepted.insert(admission.incarnation);
                self.remember_admission(admission);
                published_roster_changed = true;
            }
        }
        for binding in bindings {
            self.settle_control_session(binding, accepted.contains(&binding.incarnation))?;
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

/// A departing leader hands authority to a single voter so it can elect itself
/// at once. The new leader brings the retained members back up to the stable
/// voter count. `retained` is the committed roster at `revision`; a newer
/// commit may have removed a candidate, so promotion is skipped then and the
/// next refresh retries from the newer roster.
async fn restore_stable_voters(
    recovery: &crate::recovery::RecoverySession,
    retained: &BTreeSet<EndpointId>,
    reachable: &BTreeSet<EndpointId>,
    admissions: &[Admission],
    pending: &BTreeSet<u64>,
    revision: u64,
) -> io::Result<()> {
    let desired = crate::recovery::stable_voter_count(retained.len());
    let current = recovery.applied_voter_ids().await;
    if current.len() >= desired {
        return Ok(());
    }
    let members = recovery.applied_member_ids().await;
    // Only promote members with a live control link. A voter that cannot
    // acknowledge the joint membership freezes every later commit; the next
    // refresh retries once the member reconnects or leaves the roster.
    let candidates = admissions.iter().filter(|admission| {
        retained.contains(&admission.primary_endpoint)
            && reachable.contains(&admission.primary_endpoint)
            && members.contains(&admission.incarnation)
            && !pending.contains(&admission.incarnation)
            && !current.contains(&admission.incarnation)
    });
    let voters = current
        .iter()
        .copied()
        .chain(candidates.map(|admission| admission.incarnation))
        .take(desired)
        .collect::<BTreeSet<_>>();
    if voters.len() > current.len() {
        recovery
            .promote_voters_at_revision(revision, voters)
            .await?;
    }
    Ok(())
}
