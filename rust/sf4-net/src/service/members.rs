//! Admission and retirement of room members.
use super::{seats::Voters, *};

impl Actor {
    pub(super) fn remember_admission(&mut self, admission: Admission) {
        let incarnation = admission.incarnation;
        if self.retired_incarnations.contains(&incarnation)
            || self.pending_retired_incarnations.contains(&incarnation)
            || self.admission_conflicts(&admission)
        {
            return;
        }
        self.admissions.insert(incarnation, admission);
        if !self.admission_order.contains(&incarnation) {
            self.admission_order.push(incarnation);
        }
    }

    /// Whether an Admission would change the identity already accepted for
    /// its incarnation or claim a coordination endpoint that another
    /// incarnation holds. The leader route and the refreshed invitation are
    /// resolved through these records, so the first accepted binding of an
    /// incarnation stays fixed until the incarnation retires. A changed
    /// address for the same identities, such as a new relay, still updates.
    pub(super) fn admission_conflicts(&self, admission: &Admission) -> bool {
        if !admission.identity_consistent() {
            return true;
        }
        if let Some(known) = self.admissions.get(&admission.incarnation)
            && (known.primary_endpoint != admission.primary_endpoint
                || known.coordination_endpoint != admission.coordination_endpoint)
        {
            return true;
        }
        self.admissions.values().any(|known| {
            known.incarnation != admission.incarnation
                && known.coordination_endpoint == admission.coordination_endpoint
        })
    }

    /// Hold an Admission whose asynchronous validation has not succeeded yet,
    /// so the leader can retry it from a later refresh. Nothing resolves a
    /// route through this map; one entry per authenticated control peer keeps
    /// it bounded.
    pub(super) fn hold_pending_admission(&mut self, admission: Admission) {
        self.pending_admissions
            .insert(admission.primary_endpoint, admission);
    }

    pub(super) fn apply_confirmed_retirements(
        &mut self,
        confirmed: BTreeSet<u64>,
        history: &BTreeSet<u64>,
    ) {
        let mut departed = false;
        for incarnation in confirmed {
            self.admissions.remove(&incarnation);
            self.admission_order.retain(|id| *id != incarnation);
            departed |= !self.retired_incarnations.contains(&incarnation);
            // A departure that cannot be recorded stays pending: the RPC
            // binding is already read-only, and forgetting the ID would let a
            // lagging peer replay the old Admission after a control reconnect.
            if self.record_retirement(incarnation, history) {
                self.pending_retired_incarnations.remove(&incarnation);
            }
        }
        // The member that left may have shared the room's short link, which
        // only the leader keeps current now; the pump looks only while this
        // game leads. Once per departure: a retirement already confirmed is
        // never confirmed again.
        if departed {
            self.short.schedule_adopt(now().unwrap_or(0));
        }
    }

    /// Add a tombstone to the bounded retirement set. When the set is full, the
    /// tombstone of an incarnation that is no longer in the replicated
    /// membership history has aged out of the window every replica keeps, so it
    /// makes room. A set that the history still holds in full fails closed.
    pub(super) fn record_retirement(&mut self, incarnation: u64, history: &BTreeSet<u64>) -> bool {
        if self.retired_incarnations.contains(&incarnation) {
            return true;
        }
        if self.retired_incarnations.len() >= MAX_RETIRED_INCARNATIONS {
            let Some(aged) = self
                .retired_incarnations
                .iter()
                .copied()
                .find(|id| !history.contains(id))
            else {
                return false;
            };
            self.retired_incarnations.remove(&aged);
        }
        self.retired_incarnations.insert(incarnation);
        true
    }

    /// A helper that presents a new incarnation for a primary endpoint which
    /// still holds an older one has replaced that process, for example by
    /// rejoining right after an abandoned exit. The older incarnation's route
    /// binding would refuse the new one for as long as it stayed live, so the
    /// leader retires it: the tombstone frees the binding and the pending
    /// retirement removes the process from the coordination membership at the
    /// next reconciliation.
    pub(super) async fn supersede_endpoint_incarnations(
        &mut self,
        recovery: &crate::recovery::RecoverySession,
        admission: &Admission,
    ) {
        let stale = self
            .admissions
            .values()
            .filter(|known| {
                known.primary_endpoint == admission.primary_endpoint
                    && known.incarnation != admission.incarnation
            })
            .map(|known| known.incarnation)
            .collect::<Vec<_>>();
        for incarnation in stale {
            self.admissions.remove(&incarnation);
            self.admission_order.retain(|id| *id != incarnation);
            self.pending_retired_incarnations.insert(incarnation);
            recovery.rpc.retire(incarnation).await;
        }
    }

    pub(super) fn schedule_membership_reconciliation(
        &mut self,
        retained: BTreeMap<EndpointId, u64>,
        term: u64,
        revision: u64,
        restore_voters: bool,
    ) {
        if retained.is_empty() || !retained.contains_key(&self.endpoint.id()) {
            return;
        }
        let roster_changed = self.committed_native_members.as_ref() != Some(&retained);
        if roster_changed {
            let previous = self.committed_native_members.clone().unwrap_or_default();
            // A returning process can authenticate before the old departure
            // is observed here. Retire the incarnation in that old checkpoint,
            // never a newer admission which happens to use the same endpoint.
            let removed = previous.iter().filter_map(|(endpoint, incarnation)| {
                (retained.get(endpoint) != Some(incarnation)).then_some(*incarnation)
            });
            // The roster is already committed. Fence every omitted identity
            // before any asynchronous membership write can stall or lose its
            // waiter during a leadership change.
            self.pending_retired_incarnations.extend(removed);
            self.committed_native_members = Some(retained.clone());
        }
        if !roster_changed && self.pending_retired_incarnations.is_empty() && !restore_voters {
            return;
        }
        self.spawn_membership_operation(retained.into_keys().collect(), term, revision);
    }

    pub(super) fn schedule_pending_membership_reconciliation(&mut self, term: u64, revision: u64) {
        if self.pending_retired_incarnations.is_empty() {
            return;
        }
        let retained = self.committed_native_members.clone().unwrap_or_else(|| {
            self.admissions
                .values()
                .map(|admission| (admission.primary_endpoint, admission.incarnation))
                .collect()
        });
        let endpoints = retained
            .into_keys()
            .chain(std::iter::once(self.endpoint.id()))
            .collect();
        self.spawn_membership_operation(endpoints, term, revision);
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
        let snapshot = MembershipSnapshot {
            reachable: self.controls.keys().copied().collect(),
            admissions: self.admissions.clone(),
            pending: self.pending_retired_incarnations.clone(),
            revision,
            voters: self.voters(),
            retained,
        };
        // Go straight to the stable count for the retained roster. Shrinking
        // to two members through a commit of two voters first would leave a
        // window in which the other one dropping freezes the room. Only the
        // members that may vote count, so a match room keeps one voter: its
        // leader, who comes first in every set below.
        let desired = snapshot.desired();
        self.pending_membership_operation = Some(key.clone());
        self.tasks.spawn(async move {
            let operation = async {
                let MembershipSnapshot {
                    retained,
                    reachable,
                    admissions,
                    pending,
                    ..
                } = &snapshot;
                if recovery.coordinator.current_term() != term {
                    return Err(failed("obsolete membership operation"));
                }
                if recovery.coordinator.current_leader() == Some(recovery.incarnation) {
                    // An abandoned joint configuration first: every other
                    // change below commits through its quorum.
                    recovery.finish_joint_membership().await?;
                    let applied_voters = recovery.applied_voter_ids().await;
                    let voter_removals = pending
                        .intersection(&applied_voters)
                        .copied()
                        .collect::<BTreeSet<_>>();
                    if !voter_removals.is_empty() {
                        let remaining = applied_voters
                            .difference(&voter_removals)
                            .copied()
                            .collect::<BTreeSet<_>>();
                        // A voter with no control link cannot acknowledge the
                        // joint configuration, and the change would never
                        // commit.
                        let answering = remaining
                            .iter()
                            .copied()
                            .filter(|id| {
                                *id == recovery.incarnation
                                    || admissions.get(id).is_none_or(|admission| {
                                        reachable.contains(&admission.primary_endpoint)
                                    })
                            })
                            .collect::<BTreeSet<_>>();
                        let replacement =
                            replacement_voters(recovery.incarnation, &answering, desired, |id| {
                                snapshot.may_vote(id)
                            });
                        if replacement.is_empty() {
                            return Err(failed("empty replacement membership"));
                        }
                        if replacement == remaining {
                            recovery.remove_members(replacement).await?;
                        } else {
                            // Dropping the unreachable or surplus voters as
                            // well would remove their incarnations for good.
                            // They stay learners instead, and the leaving ones
                            // are removed below; the stable count is restored
                            // once they answer again.
                            recovery.promote_voters(replacement).await?;
                        }
                    }
                    if recovery.coordinator.current_term() != term {
                        return Err(failed("obsolete membership operation"));
                    }
                    let voters_after = recovery.applied_voter_ids().await;
                    // Only members the coordination group does not hold yet are
                    // added. Adding a learner waits for it to catch up, so
                    // repeating it for one that has gone away would hold this
                    // operation until its timeout. A fenced member is never
                    // added back: a match room fences members the native
                    // roster still lists, and a retry after a stale completion
                    // finds them already removed.
                    let members_before = recovery.applied_member_ids().await;
                    for admission in admissions.values().filter(|admission| {
                        retained.contains(&admission.primary_endpoint)
                            && !pending.contains(&admission.incarnation)
                            && !voters_after.contains(&admission.incarnation)
                            && !members_before.contains(&admission.incarnation)
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
                    if snapshot.voters.changes_voters() {
                        restore_stable_voters(&recovery, &snapshot).await?;
                    }
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
            };
            let result = timeout(MEMBERSHIP_OPERATION_TIMEOUT, operation)
                .await
                .unwrap_or_else(|_| Err(failed("membership operation timeout")));
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
        if self.deferred_admissions.len() >= self.max_control_peers() {
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
        // Every admission on record and the ones now presented. The leader
        // has superseded an endpoint's older incarnation before it queues a
        // new one, so each endpoint holds one.
        let mut known = self.admissions.clone();
        known.extend(
            admissions
                .iter()
                .map(|admission| (admission.incarnation, admission.clone())),
        );
        for admission in self.admissions.values().chain(admissions.iter()) {
            if !candidates.contains(&admission.incarnation) {
                candidates.push(admission.incarnation);
            }
        }
        let eligible = self.voters();
        let desired = eligible.desired(
            &known
                .values()
                .map(|admission| admission.primary_endpoint)
                .collect(),
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
                        let voters = admission_voters(
                            &current,
                            candidates,
                            desired,
                            |id| pending_retired.contains(&id) || retired.contains(&id),
                            |id| eligible.may_vote_member(&known, id),
                        );
                        // A room of two keeps its single voter, so the set is
                        // often unchanged; an identical change would only add
                        // a membership entry. Members of a server-owned room
                        // are learners for good.
                        if eligible.changes_voters()
                            && !voters.is_empty()
                            && voters.len() <= crate::recovery::MAX_VOTERS
                            && voters != current
                        {
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
        let committed_revision = recovery.committed().await.revision();
        let current = key.epoch == self.epoch
            && self.room == Some(key.room)
            && recovery.room == key.room
            && recovery.incarnation == key.incarnation
            && recovery.coordinator.current_term() == key.term
            && committed_revision == key.revision;
        if current && let Ok(result) = result {
            let (_, history) = recovery.applied_membership_provenance().await;
            let confirmed = result.confirmed_retirements;
            self.apply_confirmed_retirements(confirmed.clone(), &history);
            // Only now, with the old quorum no longer needing it, does a
            // bound room close what it retired.
            self.close_retired_strangers(&confirmed)?;
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
                if self
                    .pending_admissions
                    .get(&admission.primary_endpoint)
                    .is_some_and(|pending| pending.incarnation == admission.incarnation)
                {
                    self.pending_admissions.remove(&admission.primary_endpoint);
                }
                // An accepted record keeps its identity whatever a later
                // operation reports.
                if self.admission_conflicts(&admission) {
                    continue;
                }
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

/// The voters an admission leaves the room with: the `current` ones that
/// `excluded` (retired) does not name, then `candidates` in order up to
/// `desired`, only those that `may_vote` takes. A room of two keeps its single
/// voter, so the set is often what it was.
fn admission_voters(
    current: &BTreeSet<u64>,
    candidates: Vec<u64>,
    desired: usize,
    excluded: impl Fn(u64) -> bool,
    may_vote: impl Fn(u64) -> bool,
) -> BTreeSet<u64> {
    let mut voters = current
        .iter()
        .copied()
        .filter(|id| !excluded(*id))
        .collect::<BTreeSet<_>>();
    for id in candidates {
        if voters.len() >= desired {
            break;
        }
        if !excluded(id) && may_vote(id) {
            voters.insert(id);
        }
    }
    voters
}

/// The voters a joint change keeps: the leader `own` first, when it answers,
/// then the other `answering` voters that `may_vote`, up to `desired`.
fn replacement_voters(
    own: u64,
    answering: &BTreeSet<u64>,
    desired: usize,
    may_vote: impl Fn(u64) -> bool,
) -> BTreeSet<u64> {
    std::iter::once(own)
        .filter(|id| answering.contains(id))
        .chain(
            answering
                .iter()
                .copied()
                .filter(|id| *id != own && may_vote(*id)),
        )
        .take(desired)
        .collect()
}

/// What a membership operation reads of the actor when it starts: it runs on
/// its own task, so it takes this by value.
struct MembershipSnapshot {
    /// The committed roster at `revision`.
    retained: BTreeSet<EndpointId>,
    /// The endpoints with a control link.
    reachable: BTreeSet<EndpointId>,
    admissions: BTreeMap<u64, Admission>,
    /// The fenced incarnations.
    pending: BTreeSet<u64>,
    revision: u64,
    voters: Voters,
}

impl MembershipSnapshot {
    /// The stable voter count for the members of `retained` that may vote.
    fn desired(&self) -> usize {
        self.voters.desired(&self.retained)
    }

    fn may_vote(&self, id: u64) -> bool {
        self.voters.may_vote_member(&self.admissions, id)
    }
}

/// A departing leader hands authority to a single voter so it can elect itself
/// at once. The new leader brings the retained members back up to the stable
/// voter count, or down to it once the room has shrunk: a room that falls to
/// two members would otherwise keep two voters and freeze when either one
/// drops. `retained` is the committed roster at `revision`; a newer commit may
/// have removed a candidate, so the change is skipped then and the next
/// refresh retries from the newer roster.
async fn restore_stable_voters(
    recovery: &crate::recovery::RecoverySession,
    snapshot: &MembershipSnapshot,
) -> io::Result<()> {
    let MembershipSnapshot {
        retained,
        reachable,
        admissions,
        pending,
        revision,
        voters: eligible,
    } = snapshot;
    let desired = snapshot.desired();
    let current = recovery.applied_voter_ids().await;
    if current.len() > desired {
        // The leader keeps its vote, then the voters that still answer. The
        // joint change commits through the current voters, so a set that
        // keeps only reachable ones adds no new way to stall.
        let own = recovery.incarnation;
        let answers = |id: u64| {
            admissions.get(&id).is_some_and(|admission| {
                retained.contains(&admission.primary_endpoint)
                    && reachable.contains(&admission.primary_endpoint)
                    && !pending.contains(&id)
            })
        };
        let answering = current
            .iter()
            .copied()
            .filter(|id| *id == own || answers(*id))
            .collect::<BTreeSet<_>>();
        let voters = replacement_voters(own, &answering, desired, |id| snapshot.may_vote(id));
        if current.contains(&own) && voters.len() < current.len() {
            recovery
                .promote_voters_at_revision(*revision, voters)
                .await?;
        }
        return Ok(());
    }
    if current.len() == desired {
        return Ok(());
    }
    let members = recovery.applied_member_ids().await;
    // Only promote members with a live control link. A voter that cannot
    // acknowledge the joint membership freezes every later commit; the next
    // refresh retries once the member reconnects or leaves the roster.
    let candidates = admissions.values().filter(|admission| {
        eligible.may_vote(&admission.primary_endpoint)
            && retained.contains(&admission.primary_endpoint)
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
            .promote_voters_at_revision(*revision, voters)
            .await?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    // However many voters the count asks for, a member that may not vote is
    // never chosen: the filter holds without the count agreeing.
    #[test]
    fn only_voters_that_may_vote_are_chosen_whatever_the_count_says() {
        // Incarnations 1 and 2 are the fighters, 3 to 9 watch; 1 leads.
        let may_vote = |id: u64| id <= 2;
        let current = BTreeSet::from([1]);
        let candidates = (2..=9).rev().collect::<Vec<_>>();
        for desired in [1, 3, 5, 9] {
            let voters =
                admission_voters(&current, candidates.clone(), desired, |_| false, may_vote);
            assert!(
                voters.iter().all(|id| may_vote(*id)),
                "{desired}: {voters:?}"
            );
            assert_eq!(voters.len(), desired.min(2), "{desired}");
            let answering = (1..=9).collect::<BTreeSet<_>>();
            let kept = replacement_voters(1, &answering, desired, may_vote);
            assert!(kept.iter().all(|id| may_vote(*id)), "{desired}: {kept:?}");
            assert_eq!(kept.len(), desired.min(2), "{desired}");
        }
        // The leader is kept even when the binding does not name it, and an
        // unanswering one is not kept.
        assert_eq!(
            replacement_voters(7, &BTreeSet::from([2, 7, 8]), 3, may_vote),
            BTreeSet::from([2, 7])
        );
        assert_eq!(
            replacement_voters(1, &BTreeSet::from([2]), 3, may_vote),
            BTreeSet::from([2])
        );
    }

    // Without a binding everyone may vote, and the sets are what they were.
    #[test]
    fn a_room_without_a_binding_picks_voters_as_it_always_did() {
        let candidates = vec![5, 3, 9, 4];
        let voters = admission_voters(
            &BTreeSet::from([1, 2]),
            candidates,
            4,
            |id| id == 3,
            |_| true,
        );
        assert_eq!(voters, BTreeSet::from([1, 2, 5, 9]));
        let answering = BTreeSet::from([2, 3, 4, 8]);
        assert_eq!(
            replacement_voters(8, &answering, 3, |_| true),
            BTreeSet::from([2, 3, 8])
        );
        // The set is kept whole when it already has the count.
        let voters = admission_voters(&BTreeSet::from([1, 2, 3]), vec![4], 2, |_| false, |_| true);
        assert_eq!(voters, BTreeSet::from([1, 2, 3]));
    }
}
