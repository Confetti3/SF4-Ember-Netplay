use super::*;

#[test]
fn an_organizer_not_invited_is_told_so_from_any_route() {
    let body = br#"{"error":{"code":"forbidden","message":"x","retryable":false,"request_id":"r","details":{"reason":"invite_required"}}}"#;
    assert_eq!(refusal(403, body, &[]).as_str(), "invite_required");
    let other = br#"{"error":{"code":"forbidden","message":"x","retryable":false,"request_id":"r","details":{"reason":"needs_game_proof"}}}"#;
    assert_eq!(refusal(403, other, &[]).as_str(), "forbidden");
}

/// The capabilities hold to the strict JSON rules everywhere but inside
/// an older bridge's `sign_in_sites`, which is dropped whatever it holds.
#[test]
fn only_the_site_list_is_dropped_unread() {
    const FIELDS: &str = concat!(
        r#""api_version":"v1","event_version":"v1","games":["usf4"],"games_to_win":[1],"#,
        r#""native_rules_profiles":[],"native_play":true,"result_sources":[],"#,
        r#""features":["discord","matches.play"],"lobby_rotations":[],"#,
        r#""tournament_formats":[],"connections":[],"limits":{"max_body_bytes":1,"#,
        r#""max_proof_body_bytes":1,"max_json_depth":1,"challenge_lifetime_secs":1,"#,
        r#""session_lifetime_secs":1,"link_code_lifetime_secs":1}"#,
    );
    let read = |extra: &str| read_capabilities(format!("{{{FIELDS}{extra}}}").as_bytes());
    let served: Value = serde_json::from_str(&format!("{{{FIELDS}}}")).unwrap();
    assert_eq!(json!(read("").unwrap()), served);

    // Outside the site list: a duplicate key, a fraction, an exponent, an
    // unsafe integer, nesting past the limit, trailing data, a missing
    // field, or no object at all.
    let deep = format!("{}1{}", "[".repeat(17), "]".repeat(17));
    for extra in [
        r#","features":["discord"]"#.to_owned(),
        r#","weight":0.5"#.to_owned(),
        r#","weight":1e3"#.to_owned(),
        r#","weight":9007199254740993"#.to_owned(),
        format!(r#","nested":{deep}"#),
    ] {
        assert!(read(&extra).is_none(), "{extra}");
    }
    assert!(read_capabilities(format!("{{{FIELDS}}} {{}}").as_bytes()).is_none());
    assert!(read_capabilities(br#"{"api_version":"v1"}"#).is_none());
    assert!(read_capabilities(b"[]").is_none());

    // Inside it, any shape at all is dropped.
    for sites in [
        r#"[{"id":"challonge","lookup_by":"username","weight":0.5}]"#.to_owned(),
        r#""challonge""#.to_owned(),
        "2.5e10".to_owned(),
        format!("[{deep}]"),
    ] {
        let offered =
            read(&format!(r#","sign_in_sites":{sites}"#)).unwrap_or_else(|| panic!("{sites}"));
        assert_eq!(json!(offered), served, "{sites}");
    }
}

#[test]
fn a_session_is_offered_only_to_its_origin() {
    let client = Client::new();
    let ember_id = ember_protocol::SigningIdentity::from_seed(&Zeroizing::new([7; 32]))
        .unwrap()
        .ember_id()
        .clone();
    let bridge_id = ember_protocol::encoding::prefixed_id("brg", [1; 16]);
    client.sessions.lock().unwrap().insert(
        bridge_id.clone(),
        Session {
            token: Zeroizing::new("token".into()),
            expires_at: now() + 600,
            origin: "https://a.example".into(),
            ember_id: ember_id.clone(),
        },
    );
    let at = |origin: &str| Approved {
        bridge_id: bridge_id.clone(),
        origin: origin.into(),
        display_name: "Bridge".into(),
        approved_at: 0,
        keys: Vec::new(),
    };
    assert!(client.cached(&at("https://a.example"), &ember_id).is_some());
    assert!(client.cached(&at("https://b.example"), &ember_id).is_none());
}

/// A proof is made for the method the request is sent with, so the two
/// cannot differ, and a route that takes no proof has none.
#[test]
fn a_proof_is_made_for_the_verb_the_request_is_sent_with() {
    assert_eq!(Verb::Post.proven(), Some(Method::Post));
    assert_eq!(Verb::Delete.proven(), Some(Method::Delete));
    assert_eq!(Verb::Get.proven(), None);
    assert_eq!(Verb::Put.proven(), None);
    assert_eq!(Verb::Post.http(), reqwest::Method::POST);
    assert_eq!(Verb::Delete.http(), reqwest::Method::DELETE);
    assert_eq!(Verb::Get.http(), reqwest::Method::GET);
    assert_eq!(Verb::Put.http(), reqwest::Method::PUT);
}

fn refusal_body(code: &str, reason: Option<&str>) -> Vec<u8> {
    let details = reason.map_or(json!({}), |reason| json!({ "reason": reason }));
    serde_json::to_vec(&json!({ "error": {
        "code": code, "message": "m", "retryable": false, "request_id": "req_1",
        "details": details,
    }}))
    .unwrap()
}

/// An answer read as a type is held to the same strict rules as one read
/// as JSON first and then as the type, and refused with the same code.
#[test]
fn a_typed_answer_is_read_as_strictly_as_an_untyped_one() {
    #[derive(Debug, Deserialize, PartialEq)]
    #[serde(deny_unknown_fields)]
    struct Shape {
        a: u64,
    }
    let none = Refusals::NONE;
    assert_eq!(
        read::<Shape>(201, br#"{"a":1}"#, &[200, 201], &none).unwrap(),
        Shape { a: 1 }
    );
    assert_eq!(
        answer(201, br#"{"a":1}"#, &[200, 201], &none).unwrap(),
        json!({ "a": 1 })
    );
    for body in [
        &br#"{"a":1,"b":2}"#[..],
        br#"{"a":1,"a":1}"#,
        br#"{"a":1.5}"#,
        br#"{"a":9007199254740993}"#,
        br#"{"a":"1"}"#,
        br#"{"a":1} {}"#,
        b"",
    ] {
        let typed = read::<Shape>(200, body, &[200], &none).unwrap_err();
        assert_eq!(typed.as_str(), "bridge_invalid_response", "{body:?}");
    }
    // The untyped read keeps what the type would refuse, as before.
    assert!(answer(200, br#"{"a":1,"b":2}"#, &[200], &none).is_ok());
    assert!(answer(200, br#"{"a":1,"a":1}"#, &[200], &none).is_err());
    // A status that is not a success is refused alike, by code.
    let body = refusal_body("forbidden", None);
    assert_eq!(
        read::<Shape>(403, &body, &[200], &none)
            .unwrap_err()
            .as_str(),
        "forbidden"
    );
    assert_eq!(
        answer(403, &body, &[200], &none).unwrap_err().as_str(),
        "forbidden"
    );
}

/// A refusal is the bridge's code, kept apart from the helper's own: a
/// code this build does not know is `http_<status>`, and a 404 naming no
/// reason is a bridge without the route only for a route that says so.
#[test]
fn a_refusal_is_the_bridges_code_or_the_status_and_a_404_may_be_a_missing_route() {
    let body = refusal_body("brand_new_code", None);
    let unknown = refusal(409, &body, &[]);
    assert!(matches!(&unknown, Failure::Bridge(Refusal::Status(409))));
    let known = refusal(403, &refusal_body("forbidden", None), &[]);
    assert!(matches!(&known, Failure::Bridge(Refusal::Error(ErrorCode::Forbidden))));
    let reasons = ["room_full"];
    let worded = refusal(409, &refusal_body("forbidden", Some("room_full")), &reasons);
    assert!(matches!(&worded, Failure::Bridge(refusal) if refusal.is_reason("room_full")));

    let route = Refusals {
        reasons: &reasons,
        missing: Some(Code::RoomsUnavailable),
    };
    let missing = answer(404, &refusal_body("not_found", None), &[200], &route);
    assert!(matches!(
        missing,
        Err(Failure::Local(Code::RoomsUnavailable))
    ));
    let named = answer(
        404,
        &refusal_body("not_found", Some("room_full")),
        &[200],
        &route,
    );
    assert!(matches!(named, Err(Failure::Bridge(refusal)) if refusal.is_reason("room_full")));
    let other = answer(403, &refusal_body("forbidden", None), &[200], &route);
    assert!(matches!(other, Err(Failure::Bridge(Refusal::Error(ErrorCode::Forbidden)))));
    let plain = Refusals {
        reasons: &reasons,
        missing: None,
    };
    let not_found = answer(404, &refusal_body("not_found", None), &[200], &plain);
    assert!(matches!(not_found, Err(Failure::Bridge(Refusal::Error(ErrorCode::NotFound)))));
}
