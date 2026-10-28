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

/// The committed roster and seats read from a commit's view are the ones the
/// whole checkpoint names, for each endpoint spelling, wrong-type leaf and
/// repeated key.
#[test]
fn the_commit_view_names_the_same_roster_and_seats() {
    let (a, b) = (
        iroh::SecretKey::from_bytes(&[1; 32]).public().to_string(),
        iroh::SecretKey::from_bytes(&[2; 32]).public().to_string(),
    );
    let tournament = serde_json::json!({"fighters": [{"endpoint": a}, {"endpoint": b}]});
    let rows = [
        serde_json::json!({"incarnation": 7, "data": {"authenticatedEndpoint": a}}),
        serde_json::json!({"incarnation": 7, "data": {"authenticated_endpoint": a}}),
        serde_json::json!({"incarnation": 7, "data": {"authenticatedEndpoint": 1, "authenticated_endpoint": a}}),
        serde_json::json!({"incarnation": 7, "data": {"authenticated_endpoint": b, "authenticatedEndpoint": a}}),
        serde_json::json!({"incarnation": "7", "data": {"authenticatedEndpoint": a}}),
        serde_json::json!({"incarnation": 7.0, "data": {"authenticatedEndpoint": a}}),
        serde_json::json!({"incarnation": 7, "frozen": "true", "data": {"authenticatedEndpoint": a}}),
        serde_json::json!({"incarnation": 7, "frozen": true, "data": {"authenticatedEndpoint": a}}),
        serde_json::json!({"incarnation": 7, "data": null}),
        serde_json::json!({"incarnation": 7}),
        serde_json::json!([7]),
    ];
    let mut texts = Vec::new();
    for row in &rows {
        for tournament in [
            serde_json::json!(null),
            serde_json::json!({}),
            tournament.clone(),
            serde_json::json!([]),
        ] {
            let envelope = serde_json::json!({
                "journal": {"payload": "x".repeat(512)},
                "checkpoint": {
                    "members": [row, {"incarnation": 8, "data": {"authenticatedEndpoint": b}}],
                    "room": {"snapshot": {"members": [], "tournament": tournament}}
                }
            });
            texts.push(envelope.to_string());
        }
    }
    let members = serde_json::json!([{"incarnation": 7, "data": {"authenticatedEndpoint": a}}]);
    texts.extend([
        format!(r#"{{"checkpoint":{{"members":"x"}},"checkpoint":{{"members":{members}}}}}"#),
        format!(r#"{{"checkpoint":{{"members":{members}}},"checkpoint":{{"members":"x"}}}}"#),
        format!(r#"{{"checkpoint":{{"members":{members},"members":[]}}}}"#),
        format!(r#"{{"checkpoint":{{"members":[],"members":{members}}}}}"#),
        format!(
            r#"{{"checkpoint":{{"members":{members},"room":{{"snapshot":{{"tournament":{tournament},"tournament":{{}}}}}}}}}}"#
        ),
        format!(
            r#"{{"checkpoint":{{"members":{members},"room":{{"snapshot":{{"tournament":{{}},"tournament":{tournament}}}}}}}}}"#
        ),
        r#"{"checkpoint":[]}"#.to_owned(),
        "[]".to_owned(),
    ]);
    for text in &texts {
        let whole: serde_json::Value = serde_json::from_str(text).unwrap();
        let view = crate::coordination::envelope_view(text).unwrap();
        for allow_empty in [false, true] {
            assert_eq!(
                primary_members(&whole, allow_empty),
                primary_members(&view, allow_empty),
                "{text}"
            );
        }
        assert_eq!(
            seats::Seats::read(&whole),
            seats::Seats::read(&view),
            "{text}"
        );
    }
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
