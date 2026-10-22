//! Slots: both directions at once, replacement, the budget across
//! replacements, and the live connections and tasks behind the maps
//! (MEMBER_LATENCY.md 5, Rust unit 6 and 8).
use super::{
    openings::{dial_into, open},
    *,
};
use crate::service::latency::{
    endpoint::latency_transport,
    opening::{REPLACE_BUDGET, lock},
};

/// Open connections behind `actor`'s slots.
fn open_slots(actor: &Actor) -> (usize, usize) {
    let live = |connection: &iroh::endpoint::Connection| connection.close_reason().is_none();
    (
        actor
            .latency
            .inbound
            .values()
            .filter(|slot| live(&slot.guard.connection))
            .count(),
        actor
            .latency
            .outbound
            .values()
            .filter(|slot| live(&slot.guard.connection))
            .count(),
    )
}

fn idle(actor: &Actor) -> bool {
    actor.latency.openings_in.is_empty()
        && actor.latency.openings_out.is_empty()
        && actor.latency.handles.is_empty()
}

async fn both_subscribed(members: &mut [Member]) {
    for member in members.iter_mut() {
        member.actor.latency_watch(1, ROOM, 1, true);
    }
    settle(members, Duration::from_secs(10), |members| {
        members.iter().all(|member| {
            member.actor.latency.outbound.len() == members.len() - 1
                && member.actor.latency.inbound.len() == members.len() - 1
        })
    })
    .await;
}

#[tokio::test]
async fn both_boards_subscribed_hold_one_slot_each_way_and_values_come_from_outbound() {
    let (_lookup, mut members) = room(2).await;
    both_subscribed(&mut members).await;
    for member in &members {
        assert_eq!(open_slots(&member.actor), (1, 1));
        assert!(idle(&member.actor));
        let slot = member.actor.latency.outbound.values().next().unwrap();
        assert!(slot.value.is_some(), "the opening time is the first value");
    }
    // Sampling reads the selected path, and only outbound slots carry one.
    for _ in 0..2 {
        for member in &mut members {
            member.actor.latency_second().await;
        }
        sleep(Duration::from_millis(1100)).await;
    }
    for member in &mut members {
        member.actor.latency_second().await;
        // Nothing else runs on this test's thread between the sample and
        // this read, so both see the same estimate.
        let slot = member.actor.latency.outbound.values().next().unwrap();
        let path = {
            let paths = slot.guard.connection.paths();
            paths
                .iter()
                .find(|path| path.is_selected())
                .map(|path| milliseconds(path.rtt()))
        };
        let value = slot.value.map(|(rtt, _)| rtt);
        assert!(path.is_some());
        assert_eq!(value, path);
        let events = member.latency_events();
        let rows = &events.last().expect("a table").1;
        assert_eq!(rows.len(), 1);
        assert!(matches!(rows[0].state, LatencyState::Value { .. }), "{rows:?}");
        assert_eq!(open_slots(&member.actor), (1, 1));
    }
    for member in members {
        member.stop().await;
    }
}

#[tokio::test]
async fn an_inbound_replacement_leaves_the_outbound_slot_and_its_values_alone() {
    let (_lookup, mut members) = room(2).await;
    both_subscribed(&mut members).await;
    members[0].actor.latency_second().await;
    let (generation, value) = {
        let slot = &members[0].actor.latency.outbound[&2];
        (slot.generation, slot.value)
    };
    let old = members[0]
        .actor
        .latency
        .inbound
        .values()
        .next()
        .unwrap()
        .guard
        .connection
        .clone();
    // Member 2 opens to member 1 again, as a redial would.
    let (a, b) = members.split_at_mut(1);
    let (a, b) = (&mut a[0], &b[0]);
    let connection = dial_into(&mut a.actor, b.latency_endpoint(), latency_transport(0)).await;
    let _held = open(&connection, b.incarnation(), a.incarnation())
        .await
        .unwrap();
    let result = a
        .actor
        .latency
        .openings_in
        .join_next_with_id()
        .await
        .unwrap();
    a.actor.latency_inbound_done(result).await;
    assert!(
        old.close_reason().is_some(),
        "the old inbound connection is closed"
    );
    assert_eq!(a.actor.latency.inbound.len(), 1);
    assert!(connection.close_reason().is_none());
    a.actor.latency_second().await;
    let slot = &a.actor.latency.outbound[&2];
    assert_eq!(slot.generation, generation);
    assert!(slot.value.is_some() && value.is_some());
    assert!(slot.guard.connection.close_reason().is_none());
    assert_eq!(open_slots(&a.actor), (1, 1));
    for member in members {
        member.stop().await;
    }
}

#[tokio::test]
async fn the_budget_survives_replacement_and_goes_with_the_incarnation() {
    let (_lookup, mut members) = room(2).await;
    let seats: Vec<Seat> = members.iter().map(Member::seat).collect();
    let (a, b) = members.split_at_mut(1);
    let (a, b) = (&mut a[0], &b[0]);
    let identity = (b.latency(), b.incarnation());
    let mut held = Vec::new();
    for round in 1..=REPLACE_BUDGET {
        let connection = dial_into(&mut a.actor, b.latency_endpoint(), latency_transport(0)).await;
        let opened = open(&connection, b.incarnation(), a.incarnation())
            .await
            .unwrap();
        let result = a
            .actor
            .latency
            .openings_in
            .join_next_with_id()
            .await
            .unwrap();
        a.actor.latency_inbound_done(result).await;
        assert_eq!(
            lock(&a.actor.latency.admissions).charged[&identity].len(),
            round
        );
        assert_eq!(open_slots(&a.actor), (1, 0), "round {round}");
        if let Some((previous, _)) = held.last() {
            assert!(closed(previous).await, "the replaced connection is closed");
        }
        held.push((connection, opened));
    }
    let refused = dial_into(&mut a.actor, b.latency_endpoint(), latency_transport(0)).await;
    assert!(super::openings::refused(&refused).await);
    let result = a
        .actor
        .latency
        .openings_in
        .join_next_with_id()
        .await
        .unwrap();
    a.actor.latency_inbound_done(result).await;
    // Member 2 rejoins with a new incarnation: its old history goes, and the
    // new identity has its own budget.
    let mut rejoined = seats.clone();
    rejoined[1].incarnation += 1000;
    a.commit(&checkpoint(&rejoined, &[])).await;
    a.actor.latency_refresh().await;
    assert!(
        a.actor.latency.inbound.is_empty(),
        "the old incarnation's slot went too"
    );
    assert!(
        lock(&a.actor.latency.admissions)
            .charged
            .get(&identity)
            .is_none()
    );
    drop(held);
    for member in members {
        member.stop().await;
    }
}

#[tokio::test]
async fn live_connections_and_tasks_follow_each_step() {
    let (_lookup, mut members) = room(2).await;
    both_subscribed(&mut members).await;
    for member in &members {
        assert_eq!(open_slots(&member.actor), (1, 1));
        assert!(idle(&member.actor));
    }
    // Member 1 stops its board: its outbound connection closes, and member
    // 2's inbound slot for it ends with it.
    let outbound = members[0].actor.latency.outbound[&2]
        .guard
        .connection
        .clone();
    members[0].actor.latency.stop_view();
    assert!(outbound.close_reason().is_some());
    assert_eq!(open_slots(&members[0].actor), (1, 0));
    let inbound = members[1]
        .actor
        .latency
        .inbound
        .values()
        .next()
        .unwrap()
        .guard
        .connection
        .clone();
    assert!(closed(&inbound).await);
    members[1].actor.latency_second().await;
    assert_eq!(open_slots(&members[1].actor), (0, 1));
    assert_eq!(
        members[1].actor.latency.inbound.len(),
        0,
        "the dead slot is dropped"
    );

    // A dial in flight, then every opening aborted: nothing outlives the
    // drain.
    members[0].actor.latency_watch(1, ROOM, 2, true);
    assert_eq!(members[0].actor.latency.openings_out.len(), 1);
    members[0].actor.latency.openings_out.abort_all();
    assert_eq!(
        members[0].actor.latency.openings_out.len(),
        1,
        "aborted, not yet reaped"
    );
    while let Some(result) = members[0]
        .actor
        .latency
        .openings_out
        .join_next_with_id()
        .await
    {
        assert!(result.is_err());
        members[0].actor.latency_outbound_done(result).await;
    }
    assert!(idle(&members[0].actor));
    assert!(members[0].actor.latency.outbound.is_empty());
    // The next second dials again and settles back to one slot each way.
    members[0].actor.latency_second().await;
    settle(&mut members, Duration::from_secs(10), |members| {
        open_slots(&members[0].actor) == (1, 1) && open_slots(&members[1].actor) == (1, 1)
    })
    .await;
    for member in &members {
        assert!(idle(&member.actor));
    }
    for member in members {
        member.stop().await;
    }
}
