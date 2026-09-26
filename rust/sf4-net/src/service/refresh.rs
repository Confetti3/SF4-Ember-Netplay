//! Recovery session setup and applying refreshed coordination state.
use super::*;

impl Actor {
    pub(super) async fn setup_host_recovery(&mut self, invite: Invite) -> io::Result<Invite> {
        let session = crate::recovery::RecoverySession::host(
            invite.room(),
            self.endpoint.id(),
            self.relay_only,
        )
        .await?;
        let authority = session.state().await;
        let invite = invite.with_authority(
            session.coordination_endpoint,
            authority.term.max(1),
            session.incarnation,
        )?;
        let own = session.advertise().await;
        self.remember_admission(own);
        self.recovery = Some(session);
        Ok(invite)
    }

    pub(super) async fn setup_join_recovery(&mut self, invite: &Invite) -> io::Result<()> {
        let coordination_address = invite
            .coordination_address()
            .ok_or_else(|| failed("invitation has no coordination route"))?;
        let session = crate::recovery::RecoverySession::join(
            invite.room(),
            self.endpoint.id(),
            invite.authority_incarnation(),
            coordination_address,
            self.relay_only,
        )
        .await?;
        let own = session.advertise().await;
        self.remember_admission(own);
        self.remember_admission(Admission {
            room: invite.room(),
            incarnation: invite.authority_incarnation(),
            authority_term: invite.authority_term(),
            coordination_endpoint: invite
                .coordination_endpoint()
                .ok_or_else(|| failed("invitation coordination identity missing"))?,
            coordination_address: invite
                .coordination_address()
                .ok_or_else(|| failed("invitation coordination route missing"))?,
            primary_endpoint: invite.endpoint(),
        });
        self.recovery = Some(session);
        Ok(())
    }

    pub(super) async fn emit_coordination_state(&mut self) -> io::Result<()> {
        if self.retirement_started.is_some() {
            return Ok(());
        }
        let Some(recovery) = self.recovery.clone() else {
            self.coordination_writable = false;
            return Ok(());
        };
        if self.pending_coordination_refresh.is_some() || self.tasks.len() >= MAX_TASKS {
            return Ok(());
        }
        let Some(room) = self.room else {
            return Ok(());
        };
        let sequence = self.next_coordination_operation;
        self.next_coordination_operation = self.next_coordination_operation.saturating_add(1);
        let key = CoordinationRefreshKey {
            sequence,
            epoch: self.epoch,
            room,
            incarnation: recovery.incarnation,
            term: recovery.coordinator.current_term(),
            leader: recovery.coordinator.current_leader(),
        };
        let retired_candidates: Vec<u64> = self
            .admissions
            .keys()
            .copied()
            .filter(|incarnation| *incarnation != recovery.incarnation)
            .collect();
        self.pending_coordination_refresh = Some(key.clone());
        self.tasks.spawn(async move {
            let (applied_members, applied_history) = recovery.applied_membership_provenance().await;
            let mut retired = BTreeSet::new();
            for incarnation in retired_candidates {
                if recovery.rpc.is_retired(incarnation).await {
                    retired.insert(incarnation);
                }
            }
            let state = recovery.state().await;
            let committed = recovery.committed().await;
            let leader = recovery.coordinator.current_leader();
            Completion::CoordinationRefresh(
                key,
                CoordinationRefresh {
                    state,
                    leader,
                    applied_members,
                    applied_history,
                    retired,
                    committed,
                },
            )
        });
        Ok(())
    }

    pub(super) fn apply_coordination_refresh(
        &mut self,
        refresh: CoordinationRefresh,
    ) -> io::Result<()> {
        let Some(recovery) = self.recovery.clone() else {
            return Ok(());
        };
        let CoordinationRefresh {
            mut state,
            applied_members,
            applied_history,
            retired,
            committed,
            ..
        } = refresh;
        // Derive retirement provenance from the locally applied membership as
        // well as the explicit native/membership tombstone path. This closes
        // the leader-loss window without waiting in the IPC actor.
        // A departed incarnation stays in the applied history for good, so a
        // retirement already confirmed is not fenced again: every fence starts
        // a reconciliation whose completion refreshes, and the leader
        // publishes Membership to every member on each such refresh.
        // `retired` can name one too when this read began before the
        // confirmation landed.
        let confirmed = &self.retired_incarnations;
        let unconfirmed = self
            .applied_admission_members
            .iter()
            .chain(applied_history.iter())
            .copied()
            .filter(|incarnation| {
                *incarnation != recovery.incarnation && !applied_members.contains(incarnation)
            })
            .chain(retired)
            .filter(|incarnation| !confirmed.contains(incarnation))
            .collect::<Vec<_>>();
        self.pending_retired_incarnations.extend(unconfirmed);
        self.applied_admission_members
            .extend(applied_history.iter().copied());
        let retired_for_broadcast = self.pending_retired_incarnations.clone();
        if let Some(retained) = committed_primary_endpoints(committed.checkpoint.as_bytes()) {
            // A leave hands authority to a single voter; the leader restores
            // the stable voter count from here until it has.
            let restore_voters = state.leader_local
                && state.voter_count < crate::recovery::stable_voter_count(retained.len());
            self.schedule_membership_reconciliation(
                retained,
                state.term,
                committed.revision,
                restore_voters,
            );
        } else {
            self.schedule_pending_membership_reconciliation(state.term, committed.revision);
        }
        let became_local_leader = state.leader_local
            && self
                .last_coordination_state
                .is_none_or(|marker| !marker.3 || marker.0 != state.term);
        if state.leader_local && (!retired_for_broadcast.is_empty() || became_local_leader) {
            self.queue_membership_publication();
        }
        // A relay dial or learner catch-up can fail after the authenticated
        // Admission control frame has been consumed. Keep that binding private
        // until it appears in applied membership and retry it from each fresh
        // leader read; otherwise one transient add_learner failure strands the
        // native join forever on an otherwise healthy control connection.
        if state.leader_local {
            let unapplied = self
                .admissions
                .values()
                .filter(|admission| {
                    admission.incarnation != recovery.incarnation
                        && !applied_members.contains(&admission.incarnation)
                        && !applied_history.contains(&admission.incarnation)
                        && !retired_for_broadcast.contains(&admission.incarnation)
                        && !self.retired_incarnations.contains(&admission.incarnation)
                        && self.controls.contains_key(&admission.primary_endpoint)
                })
                .cloned()
                .collect::<Vec<_>>();
            for admission in unapplied {
                if self
                    .queue_admission_operation(
                        admission.primary_endpoint,
                        vec![admission],
                        true,
                        None,
                    )
                    .is_err()
                {
                    break;
                }
            }
        }
        let leader_coord = state.leader.clone();
        let leader_admission = self
            .admissions
            .values()
            .find(|admission| admission.coordination_endpoint.to_string() == leader_coord)
            .cloned();
        let leader = leader_admission
            .as_ref()
            .map(|admission| admission.primary_endpoint.to_string())
            .unwrap_or_default();
        state.leader = leader.clone();
        // Raft may report a leader before this helper has received its
        // authenticated primary endpoint binding. Never leak the coordination
        // endpoint as a gameplay route or advertise native writability until
        // the committed leader is routable.
        if state.leader.is_empty() {
            state.writable = false;
        }
        self.coordination_writable = state.writable;
        // Refresh the compact/full invitation only after the new authority is
        // visible through the committed Raft leader binding. This keeps old
        // leader references from being used for a future join while retaining
        // the room capability and expiry.
        if let Some(admission) = leader_admission.as_ref()
            && let Some(current) = self.room_invite.clone()
            && (current.endpoint() != admission.primary_endpoint
                || current.authority_term() != state.term
                || current.authority_incarnation() != admission.incarnation)
            && let Ok(updated) = current.with_authority_route(
                admission.primary_endpoint,
                admission.coordination_endpoint,
                state.term.max(1),
                admission.incarnation,
            )
            && let (Ok(invitation), Ok(secret)) = (updated.encode(), updated.encode_discord())
            && self.emit_bulk(Event::DiscordInvite {
                epoch: self.epoch,
                invitation,
                secret,
            })
        {
            self.room_invite = Some(updated.clone());
            if self.hosted.is_some() {
                self.hosted = Some(updated);
            }
        }
        let marker = (
            state.term,
            state.revision,
            state.writable,
            state.leader_local,
        );
        if self.last_coordination_state != Some(marker) {
            if !self.emit_bulk(Event::CoordinationState {
                epoch: self.epoch,
                room: recovery.room,
                term: state.term,
                revision: state.revision,
                incarnation: state.incarnation,
                leader: state.leader.clone(),
                writable: state.writable,
                leader_local: state.leader_local,
                voter_count: state.voter_count,
                learner_count: state.learner_count,
            }) {
                return Ok(());
            }
            self.last_coordination_state = Some(marker);
        }
        let connected: BTreeSet<String> = self
            .controls
            .keys()
            .map(ToString::to_string)
            .chain(std::iter::once(self.endpoint.id().to_string()))
            .collect();
        let member_incarnations =
            applied_member_incarnations(&self.admissions, &applied_members, &retired_for_broadcast);
        let members = member_incarnations
            .keys()
            .filter(|endpoint| connected.contains(*endpoint))
            .cloned()
            .collect::<Vec<_>>();
        // ControlRebound is a routing event. Defer it while a new leader is
        // only present in admissions; the native router must retain its live
        // endpoint until the replacement control socket is ready.
        let leader_ready = !state.leader.is_empty()
            && (state.leader == self.endpoint.id().to_string()
                || self
                    .controls
                    .keys()
                    .any(|peer| peer.to_string() == state.leader));
        if state.leader.is_empty() {
            return Ok(());
        }
        if !leader_ready {
            if let Some(admission) = leader_admission {
                self.reconnect_control(admission.primary_endpoint);
            }
            return Ok(());
        }
        let rebound_marker = (
            state.leader.clone(),
            members.clone(),
            member_incarnations.clone(),
        );
        // ControlRebound is an idempotent native routing snapshot.  Repeat
        // the current snapshot on the one-second coordination cadence even
        // when the marker is unchanged: a joining native owner may not have
        // installed its IPC reader when the first snapshot was emitted, and
        // caching that send would strand it in Joining forever.  Bulk queue
        // reserve still bounds retries during checkpoint pressure.
        if !self.emit_bulk(Event::ControlRebound {
            epoch: self.epoch,
            room: recovery.room,
            leader: state.leader,
            members,
            member_incarnations,
        }) {
            return Ok(());
        }
        self.last_control_rebound = Some(rebound_marker);
        Ok(())
    }

    pub(super) async fn completed_coordination_refresh(
        &mut self,
        key: CoordinationRefreshKey,
        refresh: CoordinationRefresh,
    ) -> io::Result<()> {
        if self.pending_coordination_refresh.as_ref() != Some(&key) {
            return Ok(());
        }
        self.pending_coordination_refresh = None;
        let Some(recovery) = self.recovery.clone() else {
            return Ok(());
        };
        let committed_now = recovery.committed().await;
        let current_term = recovery.coordinator.current_term();
        let current_leader = recovery.coordinator.current_leader();
        let current = key.epoch == self.epoch
            && self.room == Some(key.room)
            && recovery.room == key.room
            && recovery.incarnation == key.incarnation
            && key.term == refresh.state.term
            && key.leader == refresh.leader
            && current_term == key.term
            && current_leader == key.leader
            && refresh.state.incarnation == key.incarnation
            && refresh.state.revision == refresh.committed.revision
            && committed_now.revision == refresh.committed.revision
            && committed_now.term == refresh.committed.term
            && committed_now.request == refresh.committed.request
            && committed_now.checkpoint == refresh.committed.checkpoint;
        let failed_leader = if current && !refresh.state.writable {
            current_leader
                .filter(|leader| *leader != recovery.incarnation)
                .and_then(|leader| {
                    let now = Instant::now();
                    match self.unwritable_leader_since {
                        Some((term, failed, since)) if term == current_term && failed == leader => {
                            if now.duration_since(since) >= RECOVERY_ELECTION_GRACE {
                                // Rate-limit repeated triggers while an election is in
                                // progress. A later committed term or healthy proof
                                // clears this marker.
                                self.unwritable_leader_since = Some((current_term, leader, now));
                                Some(leader)
                            } else {
                                None
                            }
                        }
                        _ => {
                            self.unwritable_leader_since = Some((current_term, leader, now));
                            None
                        }
                    }
                })
        } else {
            if current {
                self.unwritable_leader_since = None;
            }
            None
        };
        if current {
            self.apply_coordination_refresh(refresh)?;
            if let Some(leader) = failed_leader {
                self.trigger_recovery_election_for_leader(leader).await;
            }
        } else {
            // A read that crossed a leadership or applied-revision
            // change can only withdraw writability. A fresh worker
            // will establish the next positive claim.
            self.coordination_writable = false;
            self.last_coordination_state = None;
            self.last_control_rebound = None;
            self.unwritable_leader_since = None;
            self.emit_coordination_state().await?;
        }
        Ok(())
    }
}
