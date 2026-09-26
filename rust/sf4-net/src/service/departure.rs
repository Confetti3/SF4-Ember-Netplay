//! Leaving a room: graceful Leave, the departure grace that keeps the
//! coordination route alive for the handoff, and abandon.
use super::*;

/// Each proof a departing helper waits for (removal, promotion, successor
/// confirmation) gets this long before Leave reports the step as failed.
const LEAVE_STEP_TIMEOUT: Duration = Duration::from_secs(5);
const LEAVE_POLL_INTERVAL: Duration = Duration::from_millis(25);
/// How long a departing follower keeps offering its departure notice to a
/// control whose queue is full before it falls back to the bounded wait.
const DEPARTURE_NOTICE_BOUND: Duration = Duration::from_secs(1);
/// How long a departed room's coordination route stays alive: long enough
/// for the leader to commit this member's removal with its vote, or for a
/// follower to obtain a departing leader's successor proof.
pub(super) const DEPARTURE_GRACE: Duration = Duration::from_secs(30);

/// What one graceful Leave achieved.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Departure {
    /// The command did not name the current room; nothing changed.
    Rejected,
    /// The room was released, or entered its bounded departure grace.
    Completed,
    /// No successor or membership proof arrived within the bounded steps.
    /// The room is still held so a retry can continue the handoff; a newer
    /// epoch or an abandon releases it.
    Unconfirmed,
}

impl Actor {
    pub(super) async fn graceful_leave(
        &mut self,
        epoch: u64,
        abandon: bool,
    ) -> io::Result<Departure> {
        if !self.matches(epoch) {
            self.error(0, "stale_epoch")?;
            return Ok(Departure::Rejected);
        }
        // Leave can be retried while the native adapter drains its closing
        // event. Preserve the original authority proof window instead of
        // clearing the coordination route on the duplicate command.
        if self.retirement_started.is_some() {
            self.emit(Event::RoomClosed { epoch })?;
            return Ok(Departure::Completed);
        }
        if abandon {
            self.abandon_room();
            self.emit(Event::RoomClosed { epoch })?;
            return Ok(Departure::Completed);
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
                    return Ok(Departure::Unconfirmed);
                }
                if committed_self_removal {
                    self.clear_room();
                    self.emit(Event::RoomClosed { epoch })?;
                    return Ok(Departure::Completed);
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
                        return Ok(Departure::Unconfirmed);
                    }
                }
            }
            // A follower's departure is committed by the native room authority.
            // Announce it on the open controls so the leader retires this seat
            // now even when no native Leave was committed first; the vote below
            // stays live until the leader has excluded it. A full control queue
            // drains on its own task, so keep offering the notice briefly.
            if !state.leader_local && !coordination_only_departure {
                let mut pending = self.controls.keys().copied().collect::<BTreeSet<_>>();
                let offered = Instant::now();
                while !self.send_departure_control(&mut pending)
                    && offered.elapsed() < DEPARTURE_NOTICE_BOUND
                {
                    tokio::time::sleep(LEAVE_POLL_INTERVAL).await;
                }
            }
            if voters.contains(&recovery.incarnation) && voters.len() > 1 {
                if state.leader_local {
                    // Hand authority to one successor. OpenRaft 0.9 voters
                    // refuse votes for the leader lease (election_timeout_max,
                    // 12 s) after the last append, so a multi-voter successor
                    // set leaves the room without control that long. A lone
                    // voter elects itself at once; the new leader restores
                    // the stable voter count (spawn_membership_operation).
                    // OpenRaft commits joint old/new membership through the
                    // old quorum before the helper retires its route.
                    let successor = handoff_successor(&voters, recovery.incarnation, |id| {
                        self.admissions.get(&id).is_some_and(|admission| {
                            self.controls.contains_key(&admission.primary_endpoint)
                        })
                    });
                    let successor_voters = successor.into_iter().collect::<BTreeSet<_>>();
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
                        return Ok(Departure::Unconfirmed);
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
                    self.error(0, "leave_successor_unconfirmed")?;
                    return Ok(Departure::Unconfirmed);
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
                    self.error(0, "leave_successor_unconfirmed")?;
                    return Ok(Departure::Unconfirmed);
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
        } else {
            self.clear_room();
        }
        self.emit(Event::RoomClosed { epoch })?;
        Ok(Departure::Completed)
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
        let events = self.events.sender();
        let endpoint = self.endpoint.clone();
        let status_epoch = self.epoch;
        let status_peers = self.controls.len();
        let status_games = self.games.len();
        let mut leave = Box::pin(self.graceful_leave(epoch, abandon));
        loop {
            tokio::select! {
                result = &mut leave => {
                    drop(leave);
                    // The native client retries an unconfirmed departure, then
                    // abandons it; a newer epoch may also release the room.
                    if result? == Departure::Unconfirmed {
                        self.departure_failed = true;
                    }
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
                            let _ = events.try_send(status(
                                request.id,
                                &endpoint,
                                status_epoch,
                                status_peers,
                                status_games,
                            ));
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
                                    reason: None,
                                });
                                continue;
                            }
                            drop(leave);
                            self.abandon_room();
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
                                    reason: None,
                                }
                            } else {
                                Event::Error {
                                    probe_failure: None,
                                    request_id: request.id,
                                    epoch: status_epoch,
                                    peer: None,
                                    code: "stale_epoch".into(),
                                    reason: None,
                                }
                            };
                            let _ = events.try_send(event);
                        }
                        // A refused room opening names the epoch it asked for:
                        // the native client filters events by its own epoch.
                        Command::Host { epoch: requested, .. }
                        | Command::Join { epoch: requested, .. } => {
                            let _ = events.try_send(Event::Error {
                                probe_failure: None,
                                request_id: request.id,
                                epoch: requested,
                                peer: None,
                                code: "leave_in_progress".into(),
                                reason: None,
                            });
                        }
                        _ => {
                            let _ = events.try_send(Event::Error {
                                probe_failure: None,
                                request_id: request.id,
                                epoch: status_epoch,
                                peer: None,
                                code: "leave_in_progress".into(),
                                reason: None,
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
        self.pending_admission_bindings.clear();
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
            .is_some_and(|started| started.elapsed() >= DEPARTURE_GRACE)
        {
            self.clear_room();
        }
    }

    /// The native client gave up on a departure the room never confirmed.
    /// Its control and native state go now, but a coordinated room keeps
    /// answering the old room's authority RPCs for the departure grace: the
    /// leader still needs this vote to commit the removal, and in a two-voter
    /// room losing it unannounced would leave the leader unwritable.
    pub(super) fn abandon_room(&mut self) {
        if self.recovery.is_some() {
            self.enter_departure_grace();
        } else {
            self.clear_room();
        }
    }

    /// A new room is beginning while the old one is in departure grace. Keep
    /// the old coordination route alive on its own for the rest of the grace
    /// instead of cutting the leader's quorum short; it binds its own socket,
    /// so the new session does not contend with it.
    pub(super) fn detach_departure_grace(&mut self) {
        if let (Some(started), Some(recovery)) = (self.retirement_started, self.recovery.take()) {
            let remaining = DEPARTURE_GRACE.saturating_sub(started.elapsed());
            tokio::spawn(async move {
                tokio::time::sleep(remaining).await;
                recovery.stop().await;
            });
        }
    }
}

/// The voter a leaving leader hands authority to. A voter whose game is gone
/// can never acknowledge the singleton membership, so the handoff would never
/// commit and the survivors would be left without a quorum. Prefer a voter
/// with a live control link, then fall back to voter order.
pub(super) fn handoff_successor(
    voters: &BTreeSet<u64>,
    own: u64,
    reachable: impl Fn(u64) -> bool,
) -> Option<u64> {
    let others = || voters.iter().copied().filter(|id| *id != own);
    others()
        .find(|id| reachable(*id))
        .or_else(|| others().next())
}
