use super::*;

#[tokio::test]
async fn delayed_departure_does_not_retire_a_new_incarnation_at_the_same_endpoint() {
    let host = endpoint().await;
    let peer = iroh::SecretKey::generate().public();
    let coordination = iroh::SecretKey::generate().public();
    let (events, _receiver) = mpsc::channel(128);
    let mut actor = test_actor(host.clone(), events);
    let admission = |incarnation| Admission {
        room: [7; 16],
        incarnation,
        authority_term: 1,
        coordination_endpoint: coordination,
        coordination_address: EndpointAddr::new(coordination),
        primary_endpoint: peer,
    };
    actor.remember_admission(admission(2));
    let roster = |include_peer| {
        let mut members = vec![
            serde_json::json!({"incarnation":1,"data":{"authenticatedEndpoint":host.id().to_string()}}),
        ];
        if include_peer {
            members.push(serde_json::json!({"incarnation":2,"data":{"authenticatedEndpoint":peer.to_string()}}));
        }
        serde_json::json!({"checkpoint":{"members":members}})
    };
    let retained = actor.committed_roster(&roster(true)).unwrap();
    actor.schedule_membership_reconciliation(retained, 1, 1, false);
    // Authentication of a fresh process can finish before the helper observes
    // the previous process's committed native departure.
    actor.admissions.remove(&2);
    actor.remember_admission(admission(3));
    let retained = actor.committed_roster(&roster(false)).unwrap();
    actor.schedule_membership_reconciliation(retained, 1, 2, false);
    assert!(!actor.pending_retired_incarnations.contains(&3));
    assert!(actor.pending_retired_incarnations.contains(&2));
    host.close().await;
}

#[test]
fn committed_rosters_require_unambiguous_nonzero_incarnations() {
    let endpoint = iroh::SecretKey::generate().public();
    let row =
        serde_json::json!({"incarnation":7,"data":{"authenticatedEndpoint":endpoint.to_string()}});
    let encode = |rows| serde_json::json!({"checkpoint":{"members":rows}});
    assert_eq!(
        primary_members(&encode(vec![row.clone()]), false),
        Some(BTreeMap::from([(endpoint, 7)]))
    );
    assert!(primary_members(&encode(vec![row.clone(), row.clone()]), false).is_none());
    let mut invalid = row;
    invalid["incarnation"] = serde_json::json!(0);
    assert!(primary_members(&encode(vec![invalid.clone()]), false).is_none());
    invalid.as_object_mut().unwrap().remove("incarnation");
    assert!(primary_members(&encode(vec![invalid]), false).is_none());
}
