//! Voters and retirement in a bound room, driven by hand on the leader (and
//! once on a follower): who votes as watchers join, and how an admission the
//! binding does not name leaves, its control closing only once its
//! retirement is confirmed.
use super::*;

/// A room led by this actor, driven by hand.
struct Room {
    actor: Actor,
    events: mpsc::Receiver<Event>,
    /// Every event drained so far, oldest first.
    seen: Vec<Event>,
    own: Endpoint,
    invite: Invite,
    recovery: RecoverySession,
    /// Native roster identities survive retirement from the helper admissions.
    incarnations: BTreeMap<EndpointId, u64>,
}

/// A member in the room's coordination as a learner, its admission recorded.
struct Member {
    primary: Endpoint,
    session: RecoverySession,
    incarnation: u64,
    /// Its end of a control to the leader, bound to the incarnation.
    control: Option<Heard>,
}

/// A member's end of its control: what the leader sent on it, read on a task
/// of its own, since a read cut off part way would lose its place.
struct Heard {
    frames: mpsc::UnboundedReceiver<ControlFrame>,
    _sender: transport::ControlSender,
    _reader: TaskScope,
}

impl Heard {
    fn new(channel: transport::ControlChannel) -> Self {
        let (frames, heard) = mpsc::unbounded_channel();
        let mut receiver = channel.receiver;
        let reader = tokio::spawn(async move {
            while let Ok(frame) = receiver.receive().await {
                if frames.send(frame).is_err() {
                    break;
                }
            }
        });
        Self {
            frames: heard,
            _sender: channel.sender,
            _reader: TaskScope(vec![reader.abort_handle()]),
        }
    }
}

impl Member {
    fn id(&self) -> EndpointId {
        self.primary.id()
    }

    /// The next frame the leader sent on this member's control, if one comes
    /// within `wait`.
    async fn frame(&mut self, wait: Duration) -> Option<CoordinationControl> {
        let control = self.control.as_mut()?;
        let frame = timeout(wait, control.frames.recv()).await.ok()??;
        Some(serde_json::from_slice(&frame.payload).unwrap())
    }
}

async fn open_room() -> Room {
    let own = endpoint().await;
    let seed = Invite::create(
        own.id(),
        test_relay(),
        "test-build".into(),
        now().unwrap(),
        3600,
    )
    .unwrap();
    let (events_tx, events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(own.clone(), events_tx);
    let (invite, recovery) = led_host(&mut actor, seed).await;
    let incarnations = BTreeMap::from([(own.id(), recovery.incarnation)]);
    Room {
        incarnations,
        actor,
        events,
        seen: Vec::new(),
        own,
        invite,
        recovery,
    }
}

impl Room {
    async fn member(&mut self, control: bool) -> Member {
        let primary = endpoint().await;
        let session = RecoverySession::join(
            self.invite.room(),
            primary.id(),
            self.recovery.incarnation,
            self.recovery.coordination_address.clone(),
            crate::transport::Transport::Online,
        )
        .await
        .unwrap();
        let admission = session.advertise().await;
        self.recovery.add_learner(&admission).await.unwrap();
        self.actor.remember_admission(admission.clone());
        self.incarnations.insert(primary.id(), admission.incarnation);
        let control = if control {
            Some(Heard::new(
                self.connect(&primary, Session::Bound(admission.incarnation))
                    .await,
            ))
        } else {
            None
        };
        Member {
            primary,
            session,
            incarnation: admission.incarnation,
            control,
        }
    }

    /// Opens a control from `from` to the leader, installed in `session`.
    async fn connect(&mut self, from: &Endpoint, session: Session) -> transport::ControlChannel {
        let dialing = async {
            let connection = from
                .connect(address(&self.own), CONTROL_ALPN)
                .await
                .unwrap();
            transport::connect_control_on(connection, &self.invite)
                .await
                .unwrap()
        };
        let accepting = async {
            let connection = self.own.accept().await.unwrap().await.unwrap();
            transport::accept_control(connection, &self.invite)
                .await
                .unwrap()
        };
        let (theirs, ours) = tokio::join!(dialing, accepting);
        let mut worker = ControlWorker::start(ours);
        worker.set_session(session);
        self.actor.controls.insert(from.id(), worker);
        theirs
    }

    async fn commit(&self, members: &[EndpointId], tournament: Value) {
        // The native checkpoint carries the authenticated process identity,
        // not just its endpoint. Keep those identities when testing departure.
        let mut envelope: Value = serde_json::from_str(&checkpoint(members, tournament)).unwrap();
        for (row, endpoint) in envelope["checkpoint"]["members"].as_array_mut().unwrap().iter_mut().zip(members) {
            let incarnation = self.incarnations[endpoint];
            row["incarnation"] = json!(incarnation);
        }
        commit(&self.recovery, envelope.to_string()).await;
    }

    fn drain(&mut self) {
        while let Ok(event) = self.events.try_recv() {
            self.seen.push(event);
        }
    }

    /// Hands the next worker completion to the actor.
    async fn step(&mut self) {
        complete_next(&mut self.actor).await;
        self.drain();
    }

    /// One coordination refresh, read and applied after any already running.
    async fn refresh(&mut self) {
        while self.actor.pending_coordination_refresh.is_some() {
            self.step().await;
        }
        self.actor.emit_coordination_state().await.unwrap();
        assert!(self.actor.pending_coordination_refresh.is_some());
        while self.actor.pending_coordination_refresh.is_some() {
            self.step().await;
        }
    }

    /// Refreshes and completes work until nothing is left running.
    async fn settle(&mut self) {
        self.refresh().await;
        while !self.actor.tasks.is_empty() {
            self.step().await;
        }
    }

    async fn voters(&self) -> BTreeSet<u64> {
        self.recovery.coordinator.store().applied_voters().await
    }

    fn closed(&self, peer: EndpointId) -> bool {
        self.seen.iter().any(
            |event| matches!(event, Event::ControlClosed { peer: closed, .. } if *closed == peer),
        )
    }

    async fn stop(self, members: Vec<Member>) {
        for member in members {
            member.session.stop().await;
            member.primary.close().await;
        }
        self.recovery.stop().await;
        self.own.close().await;
    }
}

#[tokio::test]
async fn a_named_watcher_is_a_learner_however_many_watch() {
    timeout(Duration::from_secs(120), async {
        let mut room = open_room().await;
        let own = room.own.id();
        let fighter = endpoint().await;
        let watchers = [(); 8].map(|_| key());
        room.commit(&[own], tournament(&[own, fighter.id()], &watchers))
            .await;
        room.settle().await;
        let mut sessions = Vec::new();
        for (count, primary) in std::iter::once(fighter.id()).chain(watchers).enumerate() {
            let session = RecoverySession::join(
                room.invite.room(),
                primary,
                room.recovery.incarnation,
                room.recovery.coordination_address.clone(),
                crate::transport::Transport::Online,
            )
            .await
            .unwrap();
            let admission = session.advertise().await;
            room.actor
                .queue_admission_operation(primary, vec![admission.clone()], true, None)
                .unwrap();
            while !room.actor.admissions.contains_key(&admission.incarnation) {
                room.step().await;
            }
            room.incarnations.insert(primary, admission.incarnation);
            sessions.push(session);
            // Two fighters keep the one voter a room of two has.
            if [1, 4, 8].contains(&count) {
                assert_eq!(
                    room.voters().await,
                    BTreeSet::from([room.recovery.incarnation])
                );
            }
        }
        // Nor does the per-second refresh keep trying to restore a count
        // the watchers would have raised.
        let everyone = std::iter::once(own)
            .chain(std::iter::once(fighter.id()))
            .chain(watchers)
            .collect::<Vec<_>>();
        room.commit(&everyone, tournament(&[own, fighter.id()], &watchers))
            .await;
        room.settle().await;
        room.refresh().await;
        assert!(room.actor.pending_membership_operation.is_none());
        assert_eq!(
            room.voters().await,
            BTreeSet::from([room.recovery.incarnation])
        );
        for session in sessions {
            session.stop().await;
        }
        fighter.close().await;
        room.stop(Vec::new()).await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn a_stranger_is_fenced_at_the_bind_and_closed_once_its_retirement_is_confirmed() {
    timeout(Duration::from_secs(60), async {
        let mut room = open_room().await;
        let own = room.own.id();
        let mut fighter = room.member(true).await;
        let stranger = room.member(true).await;
        let members = [own, fighter.id(), stranger.id()];
        room.commit(&members, json!({})).await;
        room.settle().await;
        while fighter.frame(Duration::from_millis(200)).await.is_some() {}

        room.commit(&members, tournament(&[own, fighter.id()], &[]))
            .await;
        room.refresh().await;
        // Fenced, not closed, and published at the same refresh.
        assert!(
            room.actor
                .pending_retired_incarnations
                .contains(&stranger.incarnation)
        );
        assert!(room.actor.controls.contains_key(&stranger.id()));
        assert!(room.actor.pending_membership_operation.is_some());
        let published = loop {
            match fighter.frame(Duration::from_secs(5)).await {
                Some(CoordinationControl::Membership { retired, .. }) => break retired,
                Some(_) => continue,
                None => panic!("the fence was not published at the refresh that set it"),
            }
        };
        assert!(published.contains(&stranger.incarnation));

        // The removal commits with the stranger's control still open; the
        // completion that confirms it tombstones it, then closes it.
        while room.actor.controls.contains_key(&stranger.id()) {
            assert!(
                !room
                    .actor
                    .retired_incarnations
                    .contains(&stranger.incarnation)
            );
            room.step().await;
        }
        assert!(
            room.actor
                .retired_incarnations
                .contains(&stranger.incarnation)
        );
        assert!(room.closed(stranger.id()));
        assert!(
            !room
                .recovery
                .coordinator.store().applied_members()
                .await
                .contains(&stranger.incarnation)
        );
        assert_eq!(
            room.voters().await,
            BTreeSet::from([room.recovery.incarnation])
        );
        assert!(room.actor.controls.contains_key(&fighter.id()));
        room.stop(vec![fighter, stranger]).await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn a_control_that_never_presented_an_admission_closes_at_the_bind() {
    timeout(Duration::from_secs(60), async {
        let mut room = open_room().await;
        let own = room.own.id();
        let (fighter, stranger, applying) = (endpoint().await, endpoint().await, endpoint().await);
        let _fighter = room.connect(&fighter, Session::Unbound).await;
        let _stranger = room.connect(&stranger, Session::Unbound).await;
        let _applying = room.connect(&applying, Session::Pending(0x5151)).await;
        room.commit(&[own], json!({})).await;
        room.settle().await;
        assert!(room.actor.controls.contains_key(&stranger.id()));

        room.commit(&[own], tournament(&[own, fighter.id()], &[]))
            .await;
        room.refresh().await;
        assert!(!room.actor.controls.contains_key(&stranger.id()));
        assert!(room.closed(stranger.id()));
        // A named endpoint is untouched; one whose Admission is being applied
        // is left to that operation.
        assert!(room.actor.controls.contains_key(&fighter.id()));
        assert!(room.actor.controls.contains_key(&applying.id()));
        assert!(room.actor.pending_retired_incarnations.is_empty());
        for endpoint in [fighter, stranger, applying] {
            endpoint.close().await;
        }
        room.stop(Vec::new()).await;
    })
    .await
    .unwrap();
}

// A named watcher whose game left the room is retired as any member that
// leaves is, and its control is left to the rejoin handling.
#[tokio::test]
async fn a_named_member_that_left_keeps_its_control_for_the_rejoin() {
    timeout(Duration::from_secs(60), async {
        let mut room = open_room().await;
        let own = room.own.id();
        let fighter = room.member(true).await;
        let watcher = room.member(true).await;
        let bind = tournament(&[own, fighter.id()], &[watcher.id()]);
        room.commit(&[own, fighter.id(), watcher.id()], bind.clone())
            .await;
        room.settle().await;
        room.commit(&[own, fighter.id()], bind).await;
        room.settle().await;
        assert!(
            room.actor
                .retired_incarnations
                .contains(&watcher.incarnation)
        );
        assert!(room.actor.controls.contains_key(&watcher.id()));
        assert!(!room.closed(watcher.id()));
        room.stop(vec![fighter, watcher]).await;
    })
    .await
    .unwrap();
}

// Case 1 of 5.4: a fighter back on a new endpoint before the first bind, and
// a bind that still names the old one. The order is the claim: the fenced
// member's control stays open through the removal, so the shrink to the leader
// commits through it, and closes only once the tombstone is in. How long the
// fighter then waits for its game's rejoin is not measured here: it is the
// leader's claim interval (at most 10 s), the commit of the bind, and the
// game's 30 s `OpenTimeoutMs` retry.
#[tokio::test]
async fn a_fighter_back_on_a_new_endpoint_before_the_bind_leaves_the_room_committing() {
    timeout(Duration::from_secs(90), async {
        let mut room = open_room().await;
        let own = room.own.id();
        let gone = room.member(false).await;
        let back = room.member(true).await;
        room.recovery
            .promote_voters(BTreeSet::from([
                room.recovery.incarnation,
                gone.incarnation,
                back.incarnation,
            ]))
            .await
            .unwrap();
        gone.session.stop().await;
        let members = [own, gone.id(), back.id()];
        room.commit(&members, tournament(&[own, gone.id()], &[]))
            .await;
        room.refresh().await;
        assert!(
            room.actor
                .pending_retired_incarnations
                .contains(&back.incarnation)
        );
        // Only the leader and the fenced member answer: the shrink to the
        // leader commits through them, with the fenced control open at every
        // step until the tombstone is in.
        assert!(room.actor.controls.contains_key(&back.id()));
        while room.actor.controls.contains_key(&back.id()) {
            assert!(!room.actor.retired_incarnations.contains(&back.incarnation));
            assert!(!room.closed(back.id()));
            room.step().await;
        }
        assert!(room.actor.retired_incarnations.contains(&back.incarnation));
        assert!(room.closed(back.id()));
        assert!(
            !room
                .recovery
                .coordinator.store().applied_members()
                .await
                .contains(&back.incarnation)
        );
        assert_eq!(
            room.voters().await,
            BTreeSet::from([room.recovery.incarnation])
        );

        // Until the fighter's claim brings a bind naming its new endpoint, the
        // gate refuses it.
        assert!(!room.actor.admits_control(&back.id()));
        room.commit(&[own, back.id()], tournament(&[own, back.id()], &[]))
            .await;
        room.settle().await;
        // The bind admits the new endpoint, and the old one, which the bind no
        // longer names, is retired.
        assert!(room.actor.admits_control(&back.id()));
        assert!(!room.actor.admits_control(&gone.id()));
        assert!(room.actor.retired_incarnations.contains(&gone.incarnation));
        // The game's rejoin comes in with a fresh incarnation.
        let rejoined = RecoverySession::join(
            room.invite.room(),
            back.id(),
            room.recovery.incarnation,
            room.recovery.coordination_address.clone(),
            crate::transport::Transport::Online,
        )
        .await
        .unwrap();
        let admission = rejoined.advertise().await;
        assert_ne!(admission.incarnation, back.incarnation);
        room.actor
            .queue_admission_operation(back.id(), vec![admission.clone()], true, None)
            .unwrap();
        while !room.actor.admissions.contains_key(&admission.incarnation) {
            room.step().await;
        }
        rejoined.stop().await;
        room.stop(vec![gone, back]).await;
    })
    .await
    .unwrap();
}

// Case 2 of 5.4: a stranger holds a vote while the other fighter's control
// is down. A completion made stale by a commit during the operation changes
// nothing, and the next refresh finishes the retirement.
#[tokio::test]
async fn a_stranger_voter_keeps_its_control_until_a_current_completion_confirms_it() {
    timeout(Duration::from_secs(90), async {
        let mut room = open_room().await;
        let own = room.own.id();
        let other = room.member(false).await;
        let stranger = room.member(true).await;
        room.recovery
            .promote_voters(BTreeSet::from([
                room.recovery.incarnation,
                other.incarnation,
                stranger.incarnation,
            ]))
            .await
            .unwrap();
        other.session.stop().await;
        let members = [own, other.id(), stranger.id()];
        let bind = tournament(&[own, other.id()], &[]);
        room.commit(&members, bind.clone()).await;
        room.refresh().await;
        assert!(room.actor.pending_membership_operation.is_some());
        // A room change commits while the operation runs.
        room.commit(&members, bind).await;
        while room.actor.pending_membership_operation.is_some() {
            room.step().await;
        }
        assert!(room.actor.controls.contains_key(&stranger.id()));
        assert!(
            room.actor
                .pending_retired_incarnations
                .contains(&stranger.incarnation)
        );
        assert!(!room.closed(stranger.id()));

        while room.actor.controls.contains_key(&stranger.id()) {
            if room.actor.tasks.is_empty() {
                room.refresh().await;
            } else {
                room.step().await;
            }
        }
        assert!(
            room.actor
                .retired_incarnations
                .contains(&stranger.incarnation)
        );
        assert_eq!(
            room.voters().await,
            BTreeSet::from([room.recovery.incarnation])
        );
        room.stop(vec![other, stranger]).await;
    })
    .await
    .unwrap();
}

// A room that had three voters before its bind, a watcher among them (a
// client that joined without saying it watches), falls to the leader alone
// within one refresh of the bind; a second voter added by a racing removal
// is shed again at the next.
#[tokio::test]
async fn a_bound_room_returns_to_one_voter_at_the_next_refresh() {
    timeout(Duration::from_secs(90), async {
        let mut room = open_room().await;
        let own = room.own.id();
        let fighter = room.member(true).await;
        let watcher = room.member(true).await;
        let leader = room.recovery.incarnation;
        room.recovery
            .promote_voters(BTreeSet::from([
                leader,
                fighter.incarnation,
                watcher.incarnation,
            ]))
            .await
            .unwrap();
        let members = [own, fighter.id(), watcher.id()];
        room.commit(&members, tournament(&[own, fighter.id()], &[watcher.id()]))
            .await;
        room.settle().await;
        assert_eq!(room.voters().await, BTreeSet::from([leader]));
        assert!(room.actor.controls.contains_key(&watcher.id()));

        room.recovery
            .promote_voters(BTreeSet::from([leader, watcher.incarnation]))
            .await
            .unwrap();
        room.settle().await;
        assert_eq!(room.voters().await, BTreeSet::from([leader]));
        room.stop(vec![fighter, watcher]).await;
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn a_leaving_leader_hands_its_vote_to_a_fighter_and_never_to_a_watcher() {
    timeout(Duration::from_secs(90), async {
        // Only a watcher left: no handoff.
        let mut room = open_room().await;
        let own = room.own.id();
        let absent = key();
        let watcher = room.member(true).await;
        room.commit(
            &[own, watcher.id()],
            tournament(&[own, absent], &[watcher.id()]),
        )
        .await;
        room.settle().await;
        let departure = room.actor.graceful_leave(1, false).await.unwrap();
        assert_eq!(departure, departure::Departure::Completed);
        assert_eq!(
            room.voters().await,
            BTreeSet::from([room.recovery.incarnation])
        );
        room.actor.clear_room();
        room.stop(vec![watcher]).await;

        // The other fighter present: it takes the vote.
        let mut room = open_room().await;
        let own = room.own.id();
        let watcher = room.member(true).await;
        let fighter = room.member(true).await;
        room.commit(
            &[own, watcher.id(), fighter.id()],
            tournament(&[own, fighter.id()], &[watcher.id()]),
        )
        .await;
        room.settle().await;
        let departure = room.actor.graceful_leave(1, false).await.unwrap();
        assert_eq!(departure, departure::Departure::Completed);
        assert_eq!(room.voters().await, BTreeSet::from([fighter.incarnation]));
        room.actor.clear_room();
        room.stop(vec![watcher, fighter]).await;
    })
    .await
    .unwrap();
}

// A control opened before the bind is not covered by the handshake gate. A
// stranger on one keeps presenting fresh Admissions, as a modified client
// would: each is ignored, the incarnation it was admitted under stays fenced,
// and once that retirement is confirmed its control closes. Were one taken, it
// would supersede the fenced incarnation and bind the control to a new one
// that the confirmation never names, and the control would stay for good.
#[tokio::test]
async fn a_stranger_that_keeps_presenting_admissions_is_still_retired_and_closed() {
    timeout(Duration::from_secs(90), async {
        let mut room = open_room().await;
        let own = room.own.id();
        let mut fighter = room.member(true).await;
        let stranger = room.member(true).await;
        let members = [own, fighter.id(), stranger.id()];
        room.commit(&members, json!({})).await;
        room.settle().await;
        while fighter.frame(Duration::from_millis(200)).await.is_some() {}

        let mut offers = Vec::new();
        room.commit(&members, tournament(&[own, fighter.id()], &[]))
            .await;
        room.refresh().await;
        assert!(
            room.actor
                .pending_retired_incarnations
                .contains(&stranger.incarnation)
        );

        // A fresh Admission in every round, from a process of the stranger's
        // that never stops offering one. The control has to close all the same
        // (with nothing offered it takes one round) within the cap.
        let mut rounds = 0;
        let mut fresh = Vec::new();
        while room.actor.controls.contains_key(&stranger.id()) && rounds < 16 {
            let session = RecoverySession::join(
                room.invite.room(),
                stranger.id(),
                room.recovery.incarnation,
                room.recovery.coordination_address.clone(),
                crate::transport::Transport::Online,
            )
            .await
            .unwrap();
            let admission = session.advertise().await;
            offers.push(session);
            let payload = serde_json::to_string(&CoordinationControl::Admission {
                admission: admission.clone(),
            })
            .unwrap();
            // Taken as handled, so the control is not closed early.
            assert!(
                room.actor
                    .accept_coordination_control(stranger.id(), &payload)
                    .await
                    .unwrap()
            );
            fresh.push(admission.incarnation);
            if room.actor.tasks.is_empty() {
                room.refresh().await;
            } else {
                room.step().await;
            }
            rounds += 1;
        }
        assert!(
            !room.actor.controls.contains_key(&stranger.id()),
            "still open after {rounds} rounds"
        );
        assert!(room.closed(stranger.id()));
        assert!(
            room.actor
                .retired_incarnations
                .contains(&stranger.incarnation)
        );
        for incarnation in fresh {
            assert!(!room.actor.admissions.contains_key(&incarnation));
            assert!(
                !room
                    .recovery
                    .coordinator.store().applied_members()
                    .await
                    .contains(&incarnation)
            );
        }
        assert_eq!(
            room.voters().await,
            BTreeSet::from([room.recovery.incarnation])
        );
        assert!(room.actor.controls.contains_key(&fighter.id()));
        for session in offers {
            session.stop().await;
        }
        room.stop(vec![fighter, stranger]).await;
    })
    .await
    .unwrap();
}

// A binding that stops reading (the writer and this reader have drifted) does
// not unbind the room: the last binding it read keeps ruling, so watchers stay
// out of the vote and strangers out of the room. An empty binding, which is
// what the native side writes to unbind, does unbind it, and a readable one
// replaces the old.
#[tokio::test]
async fn a_binding_that_stops_reading_leaves_the_last_one_in_force() {
    timeout(Duration::from_secs(60), async {
        let mut room = open_room().await;
        let own = room.own.id();
        let (fighter, stranger) = (key(), key());
        let members = [own];
        room.commit(&members, tournament(&[own, fighter], &[]))
            .await;
        room.refresh().await;
        let rule = bound(&[own, fighter], &[]);
        assert_eq!(room.actor.seats, rule);
        assert!(!room.actor.admits_control(&stranger));

        for unreadable in [
            tournament(&[own, fighter, key()], &[]),
            json!({ "fighters": "both" }),
            json!({ "fighters": [{ "ember_id": "emb_1" }, { "ember_id": "emb_2" }] }),
        ] {
            room.commit(&members, unreadable).await;
            room.refresh().await;
            assert_eq!(room.actor.seats, rule);
            assert!(room.actor.admits_control(&fighter));
            assert!(!room.actor.admits_control(&stranger));
            let roster = BTreeSet::from([own, fighter, stranger]);
            assert_eq!(room.actor.voters().desired(&roster), 1);
        }

        let moved = tournament(&[own, stranger], &[]);
        room.commit(&members, moved).await;
        room.refresh().await;
        assert_eq!(room.actor.seats, bound(&[own, stranger], &[]));

        room.commit(&members, json!({})).await;
        room.refresh().await;
        assert_eq!(room.actor.seats, Seats::Open);
        room.stop(Vec::new()).await;
    })
    .await
    .unwrap();
}

// A leader the binding does not name is not fenced: the fence skips the
// local incarnation, so the room keeps its leader and its vote until the
// native side removes that member, which hands the room off like any leaving
// leader.
#[tokio::test]
async fn a_leader_the_binding_does_not_name_is_not_fenced() {
    timeout(Duration::from_secs(60), async {
        let mut room = open_room().await;
        let own = room.own.id();
        let (first, second) = (room.member(true).await, room.member(true).await);
        let members = [own, first.id(), second.id()];
        room.commit(&members, tournament(&[first.id(), second.id()], &[]))
            .await;
        room.settle().await;
        assert_eq!(room.actor.seats, bound(&[first.id(), second.id()], &[]));
        assert!(
            !room
                .actor
                .pending_retired_incarnations
                .contains(&room.recovery.incarnation)
        );
        assert!(room.actor.retired_incarnations.is_empty());
        assert!(room.actor.controls.contains_key(&first.id()));
        assert!(room.actor.controls.contains_key(&second.id()));
        assert_eq!(
            room.voters().await,
            BTreeSet::from([room.recovery.incarnation])
        );
        room.stop(vec![first, second]).await;
    })
    .await
    .unwrap();
}

// The same leader seen from a fighter that follows it, whose control to the
// leader has just been opened again and not yet bound. Only the leader fences,
// so the follower neither fences its leader nor closes that control, and the
// leader's Admission on it binds it.
#[tokio::test]
async fn a_follower_keeps_its_control_to_a_leader_the_binding_does_not_name() {
    timeout(Duration::from_secs(60), async {
        let mut room = open_room().await;
        let leader = room.own.id();
        let fighter = room.member(false).await;
        let (events_tx, events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let mut follower = Room {
            incarnations: room.incarnations.clone(),
            actor: test_actor(fighter.primary.clone(), events_tx),
            events,
            seen: Vec::new(),
            own: fighter.primary.clone(),
            invite: room.invite.clone(),
            recovery: fighter.session.clone(),
        };
        follower.actor.epoch = 1;
        follower.actor.room = Some(room.invite.room());
        follower.actor.room_invite = Some(room.invite.clone());
        let advertised = room.recovery.advertise().await;
        follower
            .actor
            .remember_admission(fighter.session.advertise().await);
        follower.actor.remember_admission(advertised.clone());
        follower
            .actor
            .install_recovery(fighter.session.clone())
            .await;
        let dialing = async {
            let connection = fighter
                .primary
                .connect(address(&room.own), CONTROL_ALPN)
                .await
                .unwrap();
            transport::connect_control_on(connection, &room.invite)
                .await
                .unwrap()
        };
        let accepting = async {
            let connection = room.own.accept().await.unwrap().await.unwrap();
            transport::accept_control(connection, &room.invite)
                .await
                .unwrap()
        };
        let (ours, theirs) = tokio::join!(dialing, accepting);
        let _leader_end = Heard::new(theirs);
        follower
            .actor
            .controls
            .insert(leader, ControlWorker::start(ours));

        let members = [leader, fighter.id(), key()];
        // The other fighter is named by the native checkpoint but offline.
        room.incarnations.insert(members[2], u64::MAX);
        room.commit(&members, tournament(&members[1..], &[])).await;
        let revision = room.recovery.coordinator.store().committed().await.revision();
        while fighter.session.coordinator.store().committed().await.revision() < revision {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
        follower.refresh().await;
        let fenced = |follower: &Room| {
            follower
                .actor
                .pending_retired_incarnations
                .contains(&advertised.incarnation)
        };
        assert!(!fenced(&follower));
        assert!(!follower.closed(leader));

        let payload = serde_json::to_string(&CoordinationControl::Admission {
            admission: advertised.clone(),
        })
        .unwrap();
        assert!(
            follower
                .actor
                .accept_coordination_control(leader, &payload)
                .await
                .unwrap()
        );
        follower.settle().await;
        follower.settle().await;
        assert_eq!(
            follower
                .actor
                .controls
                .get(&leader)
                .map(|control| control.session()),
            Some(Session::Bound(advertised.incarnation))
        );
        assert!(!follower.closed(leader) && !fenced(&follower));
        follower.actor.clear_room();
        follower.stop(Vec::new()).await;
        room.stop(Vec::new()).await;
    })
    .await
    .unwrap();
}
