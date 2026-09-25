use super::*;

fn stopped() -> Event {
    Event::Stopped
}

fn load(tag: u64) -> Event {
    Event::HelperLoad {
        epoch: tag,
        actor_tick_lag_max_us: 0,
        actor_tick_body_max_us: 0,
        event_queue_free_min: 0,
    }
}

fn epoch_of(event: &Event) -> u64 {
    match event {
        Event::HelperLoad { epoch, .. } => *epoch,
        _ => panic!("unexpected event kind"),
    }
}

#[test]
fn lifecycle_events_wait_in_order_for_a_full_queue() {
    let (sender, mut receiver) = mpsc::channel(2);
    let outbox = EventOutbox::new(sender);
    for tag in 1..=5 {
        outbox.emit(load(tag)).expect("a full queue is not fatal");
    }
    assert!(!outbox.has_headroom(), "held events deny headroom");
    let mut seen = Vec::new();
    while let Ok(event) = receiver.try_recv() {
        seen.push(epoch_of(&event));
    }
    assert_eq!(seen, vec![1, 2], "the queue took what fit");
    outbox.flush().expect("flush moves held events");
    while let Ok(event) = receiver.try_recv() {
        seen.push(epoch_of(&event));
    }
    assert_eq!(seen, vec![1, 2, 3, 4]);
    outbox.flush().expect("flush moves the rest");
    seen.push(epoch_of(&receiver.try_recv().expect("the last held event")));
    assert_eq!(seen, vec![1, 2, 3, 4, 5]);
    assert!(
        !outbox.has_headroom(),
        "two slots are inside the lifecycle reserve"
    );
}

#[test]
fn a_later_event_never_overtakes_a_held_one() {
    let (sender, mut receiver) = mpsc::channel(1);
    let outbox = EventOutbox::new(sender);
    outbox.emit(load(1)).unwrap();
    outbox.emit(load(2)).unwrap(); // held
    receiver.try_recv().unwrap(); // the queue has room again
    outbox.emit(load(3)).unwrap(); // must queue behind 2, not jump ahead
    outbox.flush().unwrap();
    assert_eq!(epoch_of(&receiver.try_recv().unwrap()), 2);
    outbox.flush().unwrap();
    assert_eq!(epoch_of(&receiver.try_recv().unwrap()), 3);
}

#[test]
fn bulk_events_are_dropped_while_lifecycle_events_wait() {
    let (sender, mut receiver) = mpsc::channel(LIFECYCLE_EVENT_RESERVE + 1);
    let outbox = EventOutbox::new(sender);
    assert!(outbox.emit_bulk(load(1)), "one slot beyond the reserve");
    assert!(
        !outbox.emit_bulk(load(2)),
        "the reserve is never taken by bulk"
    );
    for tag in 3..=20 {
        outbox.emit(load(tag)).unwrap();
    }
    receiver.try_recv().unwrap();
    receiver.try_recv().unwrap();
    assert!(
        !outbox.emit_bulk(load(99)),
        "held lifecycle events come first"
    );
    outbox.flush().unwrap();
    assert_eq!(epoch_of(&receiver.try_recv().unwrap()), 4);
}

#[test]
fn a_closed_queue_is_fatal() {
    let (sender, receiver) = mpsc::channel(4);
    let outbox = EventOutbox::new(sender);
    outbox.emit(stopped()).unwrap();
    drop(receiver);
    assert!(outbox.emit(stopped()).is_err());
    assert!(!outbox.emit_bulk(stopped()));
}

#[test]
fn a_backlog_that_keeps_growing_is_fatal() {
    let (sender, _receiver) = mpsc::channel(1);
    let outbox = EventOutbox::new(sender);
    outbox.emit(stopped()).unwrap();
    for _ in 0..MAX_EVENT_BACKLOG {
        outbox.emit(stopped()).expect("within the backlog");
    }
    assert!(
        outbox.emit(stopped()).is_err(),
        "the native side stopped reading"
    );
    assert!(
        outbox.flush().is_ok(),
        "flush against a full queue is not an error"
    );
}
