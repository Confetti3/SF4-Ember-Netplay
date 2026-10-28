//! The actor tick period.

use super::*;

#[tokio::test]
async fn an_arrival_brings_the_tick_forward_but_keeps_an_overdue_one() {
    let now = Instant::now();
    // Due in a second: an arrival makes it due now.
    let later = tokio::time::sleep_until(now + Duration::from_secs(1));
    tokio::pin!(later);
    tick_now(later.as_mut());
    assert!(later.deadline() <= Instant::now());
    // Already 100 ms late: it stays due when it was, so its lag is reported.
    let overdue_at = now - Duration::from_millis(100);
    let overdue = tokio::time::sleep_until(overdue_at);
    tokio::pin!(overdue);
    tick_now(overdue.as_mut());
    assert_eq!(overdue.deadline(), overdue_at);
}

#[tokio::test]
async fn leaving_a_room_lets_go_of_its_cached_checkpoint() {
    let (events, _native) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(endpoint().await, events);
    let committed = Arc::new(crate::coordination::Committed::default());
    actor.room = Some([45; 16]);
    actor.refresh_roster = Some(([45; 16], committed.clone(), None));
    assert_eq!(Arc::strong_count(&committed), 2);
    actor.clear_room();
    assert!(actor.refresh_roster.is_none());
    assert_eq!(Arc::strong_count(&committed), 1);
}

#[tokio::test]
async fn the_tick_slows_only_when_nothing_is_left_for_it() {
    let (events, mut native) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(endpoint().await, events);
    assert_eq!(actor.tick_period(), IDLE_TICK);
    actor.room = Some([44; 16]);
    assert_eq!(actor.tick_period(), ROOM_TICK);
    // The native side drains the queue without telling the actor, so held
    // lifecycle events keep the tick short until they have moved.
    for _ in 0..=IPC_QUEUE_CAPACITY {
        actor
            .emit(Event::HelperLoad {
                epoch: 0,
                actor_tick_lag_max_us: 0,
                actor_tick_body_max_us: 0,
                event_queue_free_min: 0,
                control_queue_peak: 0,
            })
            .unwrap();
    }
    assert_eq!(actor.tick_period(), FAST_TICK);
    while native.try_recv().is_ok() {}
    actor.events.flush().unwrap();
    assert_eq!(actor.tick_period(), ROOM_TICK);
}
