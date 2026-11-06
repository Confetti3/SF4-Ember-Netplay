//! Recovery session setup and applying refreshed coordination state.
use super::*;

#[derive(Clone, PartialEq, Eq)]
pub(super) struct CoordinationRefreshKey {
    pub(super) sequence: u64,
    pub(super) epoch: u64,
    pub(super) room: [u8; 16],
    pub(super) incarnation: u64,
    pub(super) term: u64,
    pub(super) leader: Option<u64>,
}

pub(super) struct CoordinationRefresh {
    pub(super) state: AuthorityState,
    pub(super) leader: Option<u64>,
    pub(super) applied_members: BTreeSet<u64>,
    pub(super) applied_history: BTreeSet<u64>,
    pub(super) retired: BTreeSet<u64>,
    pub(super) committed: Arc<crate::coordination::Committed>,
    /// The applied membership is a joint configuration nobody finished.
    pub(super) joint: bool,
}

impl Actor {
    /// An invitation lasts INVITE_LIFETIME from when it was made, and a room
    /// can outlive that: every copy, and every rejoin of a member who left,
    /// would then be refused as expired. While the room is open each member
    /// re-stamps its own copy once a tenth of the lifetime is gone and
    /// republishes it, so a link copied at any moment stays good for nearly
    /// the whole lifetime. Admission checks the leader's copy, so the room
    /// stays joinable.
    pub(super) fn renew_invitation(&mut self, now: u64) {
        let Some(current) = self.room_invite.clone().or_else(|| self.hosted.clone()) else {
            return;
        };
        if current.expires().saturating_sub(now) > INVITE_LIFETIME - INVITE_LIFETIME / 10 {
            return;
        }
        let Ok(updated) = current.renewed(now, INVITE_LIFETIME) else {
            return;
        };
        let (Ok(invitation), Ok(secret)) = (updated.encode(), updated.encode_discord()) else {
            return;
        };
        if !self.emit_bulk(Event::DiscordInvite {
            epoch: self.epoch,
            invitation,
            secret,
        }) {
            return;
        }
        if self.room_invite.is_some() {
            self.room_invite = Some(updated.clone());
        }
        if self.hosted.is_some() {
            self.hosted = Some(updated);
        }
    }

    pub(super) async fn setup_host_recovery(&mut self, invite: Invite) -> io::Result<Invite> {
        let session = crate::recovery::RecoverySession::host_in(
            invite.room(),
            self.endpoint.id(),
            self.relay_only,
            self.coordination_port,
            self.is_public_host(),
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
        self.install_recovery(session).await;
        Ok(invite)
    }

    pub(super) async fn setup_join_recovery(&mut self, invite: &Invite) -> io::Result<()> {
        let coordination_address = invite
            .coordination_address()
            .ok_or_else(|| failed("invitation has no coordination route"))?;
        let session = crate::recovery::RecoverySession::join_in(
            invite.room(),
            self.endpoint.id(),
            invite.authority_incarnation(),
            coordination_address,
            self.relay_only,
            self.server_owned(),
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
        self.install_recovery(session).await;
        Ok(())
    }

    /// Makes `session` the room's coordination. A follower's Raft store
    /// commits on its own task, so each commit wakes the actor to hand the
    /// checkpoint to the native side; the forwarding ends with the store.
    /// A change of the room's Raft term, leader or membership signals
    /// `raft_changes` (`raft_changed`); that watch ends with the Raft.
    pub(super) async fn install_recovery(&mut self, session: crate::recovery::RecoverySession) {
        let mut commits = session.coordinator.store().committed_changes().await;
        let wake = self.wake.clone();
        tokio::spawn(async move {
            while commits.changed().await.is_ok() {
                wake.notify_one();
            }
        });
        let mut metrics = session.coordinator.raft().metrics();
        let changes = self.raft_changes.clone();
        tokio::spawn(async move {
            let mut seen = None;
            loop {
                let now = {
                    let metrics = metrics.borrow_and_update();
                    (
                        metrics.current_term,
                        metrics.current_leader,
                        *metrics.membership_config.log_id(),
                    )
                };
                if seen.is_some_and(|seen| seen != now) {
                    changes.notify_one();
                }
                seen = Some(now);
                if metrics.changed().await.is_err() {
                    break;
                }
            }
        });
        self.recovery = Some(session);
    }

    /// The room's Raft term, leader or membership changed. Once a leader is
    /// known the state is read now, so a room handed to a successor is
    /// writable again as soon as the successor is elected, not at the next
    /// once-a-second refresh. A read begun before the change can only come
    /// back stale, and a follower's waits on the leader it began with, for
    /// up to `AUTHORITY_READ_TIMEOUT` when that leader has gone, holding the
    /// single refresh slot: it is dropped for the new one. While no leader
    /// is known (an election under way) nothing changes here.
    pub(super) async fn raft_changed(&mut self) -> io::Result<()> {
        let Some(recovery) = self.recovery.as_ref() else {
            return Ok(());
        };
        if self.retirement_started.is_some() || recovery.coordinator.current_leader().is_none() {
            return Ok(());
        }
        if self.pending_coordination_refresh.take().is_some()
            && let Some(task) = self.coordination_refresh_task.take()
        {
            task.abort();
        }
        self.emit_coordination_state().await
    }

    /// A CoordinationState was not sent for want of queue room
    /// (`coordination_deferred`): the state is read again once the queue has
    /// headroom, rather than at the next once-a-second refresh.
    pub(super) async fn pump_deferred_coordination_state(&mut self) -> io::Result<()> {
        if !self.coordination_deferred
            || self.pending_coordination_refresh.is_some()
            || !self.events.has_headroom()
        {
            return Ok(());
        }
        self.coordination_deferred = false;
        self.emit_coordination_state().await
    }

    /// Sends the CoordinationState now even when it is unchanged.
    pub(super) async fn republish_coordination_state(&mut self) -> io::Result<()> {
        self.last_coordination_state = None;
        self.emit_coordination_state().await
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
        self.coordination_refresh_task = Some(self.tasks.spawn(async move {
            let (applied_members, applied_history) = recovery
                .coordinator
                .store()
                .applied_membership_provenance()
                .await;
            let mut retired = BTreeSet::new();
            for incarnation in retired_candidates {
                if recovery.rpc.is_retired(incarnation).await {
                    retired.insert(incarnation);
                }
            }
            let state = recovery.state().await;
            let committed = recovery.coordinator.store().committed().await;
            let joint = recovery
                .coordinator
                .store()
                .applied_joint_goal()
                .await
                .is_some();
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
                    joint,
                },
            )
        }));
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
            joint,
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
        // The history keeps a bounded window of departures; what fell out of
        // it is no longer tracked here, or it would be fenced again.
        self.applied_admission_members
            .retain(|incarnation| applied_history.contains(incarnation));
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
        let roster = self.observe_commit(recovery.room, &committed);
        // Fenced before the snapshot below, so the leader publishes this
        // read's retirements now rather than at the next refresh.
        if state.leader_local {
            self.retire_unnamed(recovery.incarnation)?;
        }
        let retired_for_broadcast = self.pending_retired_incarnations.clone();
        if let Some(retained) = roster {
            // A leave hands authority to a single voter, and a room that
            // shrinks keeps the voters it had; the leader moves the voter
            // count to the one the room should have from here until it
            // matches. A joint membership counts the voters of both halves, so
            // it is finished whatever the count says.
            let restore_voters = state.leader_local
                && (joint
                    || state.voter_count
                        != self.voters().desired(&retained.keys().copied().collect()));
            self.schedule_membership_reconciliation(
                retained,
                state.term,
                committed.revision(),
                restore_voters,
            );
        } else {
            self.schedule_pending_membership_reconciliation(state.term, committed.revision());
        }
        let became_local_leader = state.leader_local
            && self
                .last_coordination_state
                .is_none_or(|marker| !marker.leader_local || marker.term != state.term);
        if state.leader_local && (!retired_for_broadcast.is_empty() || became_local_leader) {
            self.queue_membership_publication();
        }
        // Whoever leads the room keeps a short link any member shared current.
        // `state.leader` still names the coordination leader here.
        self.short.follow_leader(
            state.leader_local,
            state.term,
            !state.leader.is_empty(),
            self.hosted.is_some(),
            now().unwrap_or(0),
        );
        // A relay dial or learner catch-up can fail after the authenticated
        // Admission control frame has been consumed. Keep that binding private
        // until it appears in applied membership and retry it from each fresh
        // leader read; otherwise one transient add_learner failure strands the
        // native join forever on an otherwise healthy control connection.
        // A held admission is retried only while its control is open; a
        // reconnect presents it again.
        let controls = &self.controls;
        self.pending_admissions
            .retain(|peer, _| controls.contains_key(peer));
        if state.leader_local {
            let unapplied = self
                .admissions
                .values()
                .chain(
                    self.pending_admissions
                        .values()
                        .filter(|pending| !self.admissions.contains_key(&pending.incarnation)),
                )
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
        // `state.leader` names the coordination endpoint here, as text. It
        // is read once; an admission matches when its endpoint is the one
        // that text writes exactly.
        let leader_coord = state
            .leader
            .parse::<EndpointId>()
            .ok()
            .filter(|endpoint| endpoint.to_string() == state.leader);
        let leader_admission = self
            .admissions
            .values()
            .find(|admission| Some(admission.coordination_endpoint) == leader_coord)
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
        let marker = CoordinationMarker {
            term: state.term,
            revision: state.revision,
            writable: state.writable,
            leader_local: state.leader_local,
            voter_count: state.voter_count,
            learner_count: state.learner_count,
        };
        if self.last_coordination_state != Some(marker) {
            let event = Event::CoordinationState {
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
            };
            // A new term, or a change of writability or locality, is a
            // lifecycle event, as the old leader's control_closed is: under
            // checkpoint pressure a bulk one was dropped while that close
            // went through, and the native room took a handed-off leader for
            // a lost one. Every other state, a repeat of the last one sent
            // included, is bulk: a republish clears `last_coordination_state`
            // to send it again, and once sent each republish as a lifecycle
            // event, never dropped, so a busy refresh queued hundreds ahead
            // of the next term. One dropped is read again once the queue has
            // room (`pump_deferred_coordination_state`).
            let transition = self.coordination_sent.is_none_or(|sent| {
                sent.term != marker.term
                    || sent.writable != marker.writable
                    || sent.leader_local != marker.leader_local
            });
            if transition {
                self.emit(event)?;
            } else if !self.emit_bulk(event) {
                self.coordination_deferred = true;
                return Ok(());
            }
            // Whatever was deferred is superseded by this one.
            self.coordination_deferred = false;
            self.coordination_sent = Some(marker);
            self.last_coordination_state = Some(marker);
        }
        // Refresh the compact/full invitation only after the new authority is
        // visible through the committed Raft leader binding. This keeps old
        // leader references from being used for a future join while retaining
        // the room capability and expiry. It follows CoordinationState: the native
        // room drops its invitations when the term changes, so a refreshed one
        // sent first would be dropped with them and never sent again.
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
        // `state.leader` now names the leader's primary endpoint, the one
        // `leader_admission` binds, and is empty without one.
        let leader_ready = leader_admission.as_ref().is_some_and(|admission| {
            admission.primary_endpoint == self.endpoint.id()
                || self.controls.contains_key(&admission.primary_endpoint)
        });
        if state.leader.is_empty() {
            return Ok(());
        }
        if !leader_ready {
            if let Some(admission) = leader_admission {
                self.reconnect_control(admission.primary_endpoint);
            }
            return Ok(());
        }
        // ControlRebound is an idempotent native routing snapshot.  Repeat
        // the current snapshot on the one-second coordination cadence even
        // when it is unchanged: a joining native owner may not have
        // installed its IPC reader when the first snapshot was emitted, and
        // caching that send would strand it in Joining forever.  Bulk queue
        // reserve still bounds retries during checkpoint pressure. The native
        // side also clears its rebound state on a term change, a leader-scoped
        // error, the leader's control_closed and Fail(), and only the next
        // snapshot restores it. Until then the room reads as lost, so the
        // first snapshot of a term is a lifecycle event like the term's
        // CoordinationState: a bulk one dropped under checkpoint pressure (a
        // leaving host's last commit) left a healthy handoff showing
        // "recovering" until the next cadence.
        let rebound = Event::ControlRebound {
            epoch: self.epoch,
            room: recovery.room,
            leader: state.leader,
            members,
            member_incarnations,
        };
        if self.rebound_term == state.term {
            self.emit_bulk(rebound);
        } else {
            self.emit(rebound)?;
            self.rebound_term = state.term;
        }
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
        self.coordination_refresh_task = None;
        let Some(recovery) = self.recovery.clone() else {
            return Ok(());
        };
        let committed_now = recovery.coordinator.store().committed().await;
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
            && refresh.state.revision == refresh.committed.revision()
            && committed_now.same_commit(&refresh.committed);
        let grace = self.leader_loss_grace();
        let failed_leader = if current && !refresh.state.writable {
            current_leader
                .filter(|leader| *leader != recovery.incarnation)
                .and_then(|leader| {
                    let now = Instant::now();
                    match self.unwritable_leader_since {
                        Some((term, failed, since)) if term == current_term && failed == leader => {
                            if now.duration_since(since) >= grace {
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
                self.handle_failed_leader(leader).await?;
            }
        } else {
            // A read that crossed a leadership or applied-revision
            // change can only withdraw writability. A fresh worker
            // will establish the next positive claim.
            self.coordination_writable = false;
            self.last_coordination_state = None;
            self.unwritable_leader_since = None;
            self.emit_coordination_state().await?;
        }
        Ok(())
    }
}
