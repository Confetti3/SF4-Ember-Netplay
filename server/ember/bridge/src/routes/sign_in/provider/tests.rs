use super::*;
use crate::config::sites::Characters;

/// Who signed in, as (ID, username), or the unusable username.
fn read(found: Option<SignedIn>) -> Option<Result<(String, String), String>> {
    found.map(|signed_in| match signed_in {
        SignedIn::User(user) => Ok((user.id, user.username)),
        SignedIn::Unusable(username) => Err(username),
    })
}

#[test]
fn a_listing_reads_events_inside_tournaments_and_skips_what_it_cannot_read() {
    let read: TournamentsRead = serde_json::from_value(serde_json::json!({
        "method": "POST",
        "url": "https://api.start.gg/gql/alpha",
        "list_pointer": "/data/currentUser/tournaments/nodes",
        "events_pointer": "/events",
        "id_pointer": "/slug",
        "name_pointer": "/name",
        "tournament_name_pointer": "/name",
    }))
    .unwrap();
    let answer = serde_json::json!({ "data": { "currentUser": { "tournaments": { "nodes": [
        { "name": "Ember Weekly 12", "events": [
            { "name": "USF4 Singles", "slug": "tournament/ember-weekly-12/event/usf4-singles" },
            { "name": "No slug" },
            { "name": "", "slug": "tournament/ember-weekly-12/event/blank" },
        ] },
        { "name": "No events" },
    ] } } } });
    assert_eq!(
        listed(&answer, &read),
        [Tournament {
            id: "tournament/ember-weekly-12/event/usf4-singles".into(),
            name: "Ember Weekly 12: USF4 Singles".into(),
            state: None,
            actions: vec![],
        }]
    );
    assert!(listed(&serde_json::json!({ "errors": [] }), &read).is_empty());
}

#[test]
fn a_listing_reads_each_events_state_and_allowed_moves() {
    let read: TournamentsRead = serde_json::from_value(serde_json::json!({
        "method": "GET",
        "url": "https://brackets.example/tournaments",
        "list_pointer": "/tournaments",
        "id_pointer": "/id",
        "name_pointer": "/title",
        "state_pointer": "/state",
        "actions_pointer": "/actions",
    }))
    .unwrap();
    let answer = serde_json::json!({ "tournaments": [
        { "id": "weekly", "title": "Weekly", "state": "sign_ups", "actions": ["close_sign_ups", 7, "start"] },
        { "id": "cup", "title": "Cup" },
    ] });
    let found = listed(&answer, &read);
    assert_eq!(found[0].state.as_deref(), Some("sign_ups"));
    assert_eq!(found[0].actions, ["close_sign_ups", "start"]);
    assert_eq!((found[1].state.clone(), found[1].actions.len()), (None, 0));
}

#[test]
fn discord_says_who_signed_in_as_it_always_did() {
    assert_eq!(
        read(discord_user(
            br#"{"id":"274220342558756145","username":"player1\u0007"}"#
        )),
        Some(Ok(("274220342558756145".into(), "player1".into())))
    );
    assert_eq!(
        read(discord_user(br#"{"id":"0123","username":"player1"}"#)),
        None
    );
}

#[test]
fn a_site_says_who_signed_in_where_its_pointers_point() {
    let rule = UsernameRule {
        characters: Characters::LettersDigits,
        also: "_".into(),
        max_length: 64,
        case_insensitive: true,
    };
    let user = |body: &[u8], by_username: bool| {
        read(pointed_user(
            body,
            "/data/id",
            Some("/data/name"),
            by_username.then_some(&rule),
        ))
    };
    // A string ID, or a number read as its digits.
    assert_eq!(
        user(br#"{"data":{"id":"1001","name":"Player_1"}}"#, true),
        Some(Ok(("1001".into(), "Player_1".into())))
    );
    assert_eq!(
        user(br#"{"data":{"id":1002,"name":"Player_1"}}"#, true),
        Some(Ok(("1002".into(), "Player_1".into())))
    );
    // A username lookups cannot match is told apart from no answer,
    // where lookups match usernames; elsewhere it is only shown.
    assert_eq!(
        user(br#"{"data":{"id":"1","name":"has space"}}"#, true),
        Some(Err("has space".into()))
    );
    assert_eq!(
        user(br#"{"data":{"id":"1","name":"has space"}}"#, false),
        Some(Ok(("1".into(), "has space".into())))
    );
    // No username, or one with nothing to show: the ID is shown where
    // lookups match IDs, and it is no answer where they match usernames.
    // No ID: no answer.
    for body in [
        &br#"{"data":{"id":7}}"#[..],
        br#"{"data":{"id":7,"name":null}}"#,
        br#"{"data":{"id":7,"name":""}}"#,
        br#"{"data":{"id":7,"name":"\u0007"}}"#,
    ] {
        assert_eq!(user(body, false), Some(Ok(("7".into(), "7".into()))));
        assert!(user(body, true).is_none());
    }
    for body in [
        &br#"{"data":{"name":"player1"}}"#[..],
        br#"{"data":{"id":1.5,"name":"player1"}}"#,
        br#"{"data":{"id":-5,"name":"player1"}}"#,
        br#"{"data":{"id":"has space","name":"player1"}}"#,
        br#"not json"#,
    ] {
        assert!(user(body, true).is_none());
    }
}

#[test]
fn a_refusal_is_the_sites_own_words_and_short() {
    let challonge = br#"{"errors":[{"detail":{"grand_finals_modifier":["must be one of: single match, skip, "]},"source":{"pointer":"/data/attributes/double_elimination_options"},"status":422}]}"#;
    assert_eq!(
        refusal(422, challonge),
        "422 double_elimination_options: grand_finals_modifier must be one of: single match, skip,"
    );
    let long = format!("line\n{}", "x".repeat(400));
    let said = refusal(500, long.as_bytes());
    assert!(said.starts_with("500 line x") && said.chars().count() <= MAX_REFUSAL);
}
