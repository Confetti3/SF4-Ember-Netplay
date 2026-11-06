//! How often the room's coordination state is read and sent: no read follows
//! a reconciliation that changed nothing, a repeat waits for queue room, and a
//! Raft change is read at once.

use super::departures::learner_follower;
use super::*;

/// The CoordinationStates in `events` so far, as (term, writable).
fn sent_states(events: &mut mpsc::Receiver<Event>) -> Vec<(u64, bool)> {
    let mut states = Vec::new();
    while let Ok(event) = events.try_recv() {
        if let Event::CoordinationState { term, writable, .. } = event {
            states.push((term, writable));
        }
    }
    states
}

#[tokio::test]
async fn a_follower_waiting_on_the_leaders_removal_does_not_read_again_and_again() {
    phase(
        "a_follower_waiting_on_the_leaders_removal_does_not_read_again_and_again",
        Duration::from_secs(60),
        async {
            let mut fixture = learner_follower().await;
            // The leader's last checkpoint left it out of the roster, as a
            // leaving host's does before it hands the room off: the follower
            // fences it, but only the leader can commit its removal.
            let leader = fixture.fixture.recovery.incarnation;
            let follower = &mut fixture.follower;
            follower.pending_retired_incarnations.insert(leader);
            follower.emit_coordination_state().await.unwrap();
            complete_next(follower).await;
            assert!(follower.pending_membership_operation.is_some());
            // The reconciliation confirms nothing. Its completion used to read
            // the state again, which started the same reconciliation again:
            // a cycle of about half a millisecond until the handoff, each turn
            // an authority read at the leader and a CoordinationState.
            complete_next(follower).await;
            assert!(follower.pending_membership_operation.is_none());
            assert!(
                follower.pending_coordination_refresh.is_none(),
                "a reconciliation that changed nothing read the state again"
            );
            assert!(follower.tasks.is_empty());
            assert!(follower.pending_retired_incarnations.contains(&leader));
            assert!(sent_states(&mut fixture.events).len() <= 1);
            fixture.learner.stop().await;
            fixture.fixture.recovery.stop().await;
            fixture.fixture.host.close().await;
            fixture.fixture.remote.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn a_repeated_coordination_state_is_bulk_and_waits_for_queue_room() {
    phase(
        "a_repeated_coordination_state_is_bulk_and_waits_for_queue_room",
        Duration::from_secs(30),
        async {
            let host = endpoint().await;
            let seed = Invite::create(host.id(), test_relay(), "test-build".into(), now().unwrap(), 3600)
                .unwrap();
            let (events_tx, mut events) = mpsc::channel(LIFECYCLE_EVENT_RESERVE + 4);
            let mut actor = test_actor(host.clone(), events_tx);
            let (_invite, recovery) = led_host(&mut actor, seed).await;
            // The first state of the room is a transition: sent whatever the
            // queue holds.
            actor.emit_coordination_state().await.unwrap();
            complete_next(&mut actor).await;
            assert_eq!(sent_states(&mut events), vec![(1, true)]);

            // The queue fills; a republish of the same state is a repeat. It
            // used to go as a lifecycle event, never dropped, and a busy
            // refresh queued hundreds of them ahead of the next term.
            while actor.events.capacity() > LIFECYCLE_EVENT_RESERVE {
                actor.emit(Event::Stopped).unwrap();
            }
            actor.republish_coordination_state().await.unwrap();
            complete_next(&mut actor).await;
            assert!(!actor.events.has_backlog(), "a repeat waited as a lifecycle event");
            assert!(actor.coordination_deferred);
            // It is read again once the queue has room, not before.
            actor.pump_deferred_coordination_state().await.unwrap();
            assert!(actor.pending_coordination_refresh.is_none());
            assert!(sent_states(&mut events).is_empty());
            actor.pump_deferred_coordination_state().await.unwrap();
            complete_next(&mut actor).await;
            assert!(!actor.coordination_deferred);
            assert_eq!(sent_states(&mut events), vec![(1, true)]);
            assert!(!actor.events.has_backlog());

            recovery.stop().await;
            host.close().await;
        },
    )
    .await;
}

#[tokio::test]
async fn a_raft_change_reads_the_coordination_state_at_once() {
    phase(
        "a_raft_change_reads_the_coordination_state_at_once",
        Duration::from_secs(60),
        async {
            let mut fixture = learner_follower().await;
            let learner = fixture.learner.incarnation;
            // As a join installs it, with its watch on the Raft.
            fixture.follower.install_recovery(fixture.learner.clone()).await;
            let changes = fixture.follower.raft_changes.clone();
            // The leader hands its vote to the follower, as a leaving host
            // does, and the follower sees the membership change.
            fixture
                .fixture
                .recovery
                .promote_voters(BTreeSet::from([learner]))
                .await
                .unwrap();
            while fixture.learner.coordinator.store().applied_voters().await != BTreeSet::from([learner]) {
                tokio::time::sleep(Duration::from_millis(5)).await;
            }
            timeout(Duration::from_secs(5), changes.notified())
                .await
                .expect("the membership change was not signalled");
            let follower = &mut fixture.follower;
            // A read of the old term is in flight when the follower is elected.
            follower.emit_coordination_state().await.unwrap();
            let stale = follower.pending_coordination_refresh.clone().unwrap();
            fixture.fixture.recovery.rpc.request_election(learner).await.unwrap();
            while fixture.learner.coordinator.current_leader() != Some(learner) {
                tokio::time::sleep(Duration::from_millis(5)).await;
            }
            // The new term is signalled, and nothing else would read it
            // before the next once-a-second refresh. The read in flight is
            // replaced by one of the new term.
            timeout(Duration::from_secs(5), changes.notified())
                .await
                .expect("the new term was not signalled");
            follower.raft_changed().await.unwrap();
            let fresh = follower.pending_coordination_refresh.clone().unwrap();
            assert!(fresh.sequence > stale.sequence && fresh.term > stale.term);
            assert_eq!(fresh.leader, Some(learner));
            for _ in 0..20 {
                let Some(result) = follower.tasks.join_next().await else {
                    break;
                };
                match result {
                    Ok(completion) => follower.completed(completion).await.unwrap(),
                    Err(error) => assert!(error.is_cancelled()),
                }
            }
            assert!(follower.tasks.is_empty());
            let states = sent_states(&mut fixture.events);
            assert_eq!(states.last(), Some(&(fresh.term, true)), "{states:?}");
            assert!(!states.contains(&(fresh.term, false)), "{states:?}");
            fixture.learner.stop().await;
            fixture.fixture.recovery.stop().await;
            fixture.fixture.host.close().await;
            fixture.fixture.remote.close().await;
        },
    )
    .await;
}
