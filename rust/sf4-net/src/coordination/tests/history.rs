use super::*;

#[test]
fn departed_history_keeps_the_newest_departures_and_all_current_members() {
    let mut machine = Machine::default();
    for departed in 1..=300u64 {
        machine.remember_members(&BTreeSet::from([1000, departed]));
    }
    assert_eq!(
        machine.member_history.len(),
        RETAINED_RETIRED_MEMBER_HISTORY + 2
    );
    assert_eq!(
        machine.departed_order.len() + 2,
        machine.member_history.len()
    );
    // 300 is current; 299 down to 173 are the newest departures.
    assert!(machine.member_history.contains(&1000));
    assert!(machine.member_history.contains(&300));
    assert!(machine.member_history.contains(&299));
    assert!(machine.member_history.contains(&173));
    assert!(!machine.member_history.contains(&172));
    assert!(!machine.member_history.contains(&1));
    // A member that stays is never dropped, however many departures follow.
    for departed in 301..=600u64 {
        machine.remember_members(&BTreeSet::from([1000, departed]));
    }
    assert!(machine.member_history.contains(&1000));
    assert!(machine.member_history.len() <= MAX_MEMBER_HISTORY);
}

#[test]
fn history_from_a_snapshot_without_order_drops_the_lowest_ids_first() {
    let mut machine = Machine {
        member_history: (1..=200u64).collect(),
        ..Machine::default()
    };
    machine.remember_members(&BTreeSet::from([500]));
    assert_eq!(
        machine.member_history.len(),
        RETAINED_RETIRED_MEMBER_HISTORY + 1
    );
    assert!(!machine.member_history.contains(&73));
    assert!(machine.member_history.contains(&74));
    assert!(machine.member_history.contains(&200));
    assert!(machine.member_history.contains(&500));
    assert_eq!(
        machine.departed_order.len() + 1,
        machine.member_history.len()
    );
}

/// A member that stayed through many rotations, then leaves. Its incarnation
/// entered the history first, but it departed last, so it is the newest
/// departure and must outlive every older one.
fn machine_after_long_lived_member_left() -> Machine {
    let mut machine = Machine::default();
    for departed in 1..=300u64 {
        machine.remember_members(&BTreeSet::from([1000, departed]));
    }
    machine.remember_members(&BTreeSet::from([300]));
    machine
}

#[test]
fn a_long_lived_member_that_just_left_outlives_older_departures() {
    let mut machine = machine_after_long_lived_member_left();
    // 1000 departed last: kept. 173 was the oldest departure: dropped.
    assert!(machine.member_history.contains(&1000));
    assert!(machine.member_history.contains(&300));
    assert!(machine.member_history.contains(&174));
    assert!(!machine.member_history.contains(&173));
    assert_eq!(
        machine.member_history.len(),
        RETAINED_RETIRED_MEMBER_HISTORY + 1
    );
    assert_eq!(machine.departed_order.last(), Some(&1000));
    assert_eq!(machine.departed_order.first(), Some(&174));
    // It is dropped only once 127 newer departures follow it: 126 kept.
    for live in 301..=426u64 {
        machine.remember_members(&BTreeSet::from([live]));
        assert!(machine.member_history.contains(&1000));
    }
    machine.remember_members(&BTreeSet::from([427]));
    assert!(!machine.member_history.contains(&1000));
    assert!(machine.member_history.contains(&300));
}

#[test]
fn snapshot_carries_the_departure_order_of_a_long_lived_member() {
    let machine = machine_after_long_lived_member_left();
    let restored = decode_snapshot(&encode_snapshot(&machine).unwrap()).unwrap();
    assert_eq!(restored.member_history, machine.member_history);
    assert_eq!(restored.departed_order, machine.departed_order);
    // Both replicas evict the same incarnations from here on.
    let (mut original, mut restored) = (machine, restored);
    for next in 301..=340u64 {
        original.remember_members(&BTreeSet::from([next]));
        restored.remember_members(&BTreeSet::from([next]));
        assert_eq!(restored.member_history, original.member_history);
        assert_eq!(restored.departed_order, original.departed_order);
    }
    assert!(restored.member_history.contains(&1000));
}

#[test]
fn snapshot_without_a_departure_order_falls_back_to_ascending_ids() {
    let machine = machine_after_long_lived_member_left();
    let mut value = serde_json::to_value(&machine).unwrap();
    value.as_object_mut().unwrap().remove("departed_order");
    let mut legacy: Machine = serde_json::from_value(value).unwrap();
    assert!(legacy.departed_order.is_empty());
    assert_eq!(legacy.member_history, machine.member_history);
    legacy.remember_members(&BTreeSet::from([301]));
    // The unordered entries depart together in ascending order, so the
    // lowest ids go first and 1000 is not the one dropped.
    assert_eq!(
        legacy.member_history.len(),
        RETAINED_RETIRED_MEMBER_HISTORY + 1
    );
    assert!(legacy.member_history.contains(&1000));
    assert!(legacy.member_history.contains(&301));
    assert!(!legacy.member_history.contains(&174));
    assert!(legacy.member_history.contains(&175));
    // The same input gives the same result on every replica.
    let mut again: Machine =
        serde_json::from_value(serde_json::to_value(&legacy).unwrap()).unwrap();
    again.remember_members(&BTreeSet::from([302]));
    legacy.remember_members(&BTreeSet::from([302]));
    assert_eq!(again.member_history, legacy.member_history);
    assert_eq!(again.departed_order, legacy.departed_order);
}

#[tokio::test]
async fn a_room_keeps_admitting_members_past_the_departed_history_bound() {
    let (bus, nodes) = cluster(1).await;
    let last = MAX_RETIRED_MEMBER_HISTORY as u64 + 20;
    for id in 2..=last {
        let learner = Arc::new(
            Coordinator::new(
                id,
                Arc::new(TestNetwork {
                    source: id,
                    bus: bus.clone(),
                }),
            )
            .await
            .unwrap(),
        );
        bus.peers.lock().await.insert(id, Arc::downgrade(&learner));
        nodes[0]
            .raft
            .add_learner(id, BasicNode::new(id.to_string()), true)
            .await
            .unwrap();
        nodes[0]
            .raft
            .change_membership(ChangeMembers::RemoveNodes(BTreeSet::from([id])), false)
            .await
            .unwrap();
        let _ = learner.raft.shutdown().await;
    }
    let (members, history) = nodes[0].applied_membership_provenance().await;
    assert_eq!(members, BTreeSet::from([1]));
    assert!(history.len() <= RETAINED_RETIRED_MEMBER_HISTORY + 1);
    // The newest departure is still remembered, the first is not.
    assert!(history.contains(&last));
    assert!(!history.contains(&2));
    stop(nodes).await;
}
