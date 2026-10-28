use serde_json::{Value, json};

use super::{envelope_view, value_view};
use crate::coordination::{latency_roster::latency_roster_in, probe_pair_bound_in};

fn key(seed: u8) -> String {
    iroh::SecretKey::from_bytes(&[seed; 32])
        .public()
        .to_string()
}

/// A committed envelope with two seated fighters and a watcher, as the native
/// side writes it, with the room state the readers skip.
fn envelope() -> Value {
    json!({
        "version": 1,
        "request": 9,
        "effects": [{"type": "room_snapshot", "payload": {"chat": ["a", "b\u{e9}"]}}],
        "checkpoint": {
            "journal": {"entries": [{"revision": 3, "payload": "x".repeat(4096)}]},
            "members": [
                {"member": 1, "incarnation": 11, "data": {
                    "authenticatedEndpoint": key(1), "incarnation": 11,
                    "latency": {"endpoint": key(101), "relay": "use"}}},
                {"member": 2, "incarnation": 22, "data": {
                    "authenticated_endpoint": key(2),
                    "latency": {"endpoint": key(102)}}},
                {"member": 3, "data": {
                    "authenticatedEndpoint": key(3), "incarnation": 33}},
                {"member": 4, "incarnation": 44, "frozen": true, "data": {
                    "authenticatedEndpoint": key(4)}}
            ],
            "room": {
                "version": 2,
                "chat": ["hello"],
                "snapshot": {
                    "members": [
                        {"id": 1, "status": 4, "table": 0},
                        {"id": 2, "status": 4, "table": 0},
                        {"id": 3, "status": 5, "table": 0}
                    ],
                    "tables": [{"id": 0, "phase": 3, "revision": 42, "p1": 1, "p2": 2}],
                    "tournament": {"fighters": [{"endpoint": key(1)}, {"endpoint": key(2)}]}
                }
            }
        }
    })
}

/// What a text's view answers equals what its whole parse answers, for
/// every reader of the committed checkpoint the view serves.
fn same_answers(text: &str) {
    let whole = serde_json::from_str::<Value>(text).ok();
    let view = envelope_view(text);
    assert_eq!(
        whole.is_some(),
        view.is_some(),
        "accepted differently: {text}"
    );
    assert_eq!(
        latency_roster_in(whole.as_ref()),
        latency_roster_in(view.as_ref()),
        "latency roster: {text}"
    );
    let (Some(whole), Some(view)) = (whole, view) else {
        return;
    };
    // The text's single pass keeps what pruning the parsed value keeps.
    assert_eq!(value_view(&whole), view, "view: {text}");
    for (source, target, revision) in [(11, 22, 42), (22, 11, 42), (11, 33, 42), (11, 22, 43)] {
        let endpoint = |incarnation: u64| key((incarnation / 11) as u8);
        assert_eq!(
            probe_pair_bound_in(
                &whole,
                source,
                target,
                &endpoint(source),
                &endpoint(target),
                revision
            ),
            probe_pair_bound_in(
                &view,
                source,
                target,
                &endpoint(source),
                &endpoint(target),
                revision
            ),
            "probe pair: {text}"
        );
    }
    assert_eq!(
        whole.pointer("/checkpoint/room/snapshot/tournament"),
        view.pointer("/checkpoint/room/snapshot/tournament"),
        "tournament: {text}"
    );
    assert_eq!(
        whole.pointer("/checkpoint/members"),
        view.pointer("/checkpoint/members"),
        "members: {text}"
    );
}

/// Sets `path` (members separated by `/`) in a copy of the envelope, adding
/// its last member when the envelope has none.
fn with(path: &str, value: Value) -> String {
    let mut envelope = envelope();
    let (parent, last) = path.rsplit_once('/').unwrap();
    match envelope
        .pointer_mut(parent)
        .unwrap_or_else(|| panic!("{path}"))
    {
        Value::Object(map) => {
            map.insert(last.to_owned(), value);
        }
        Value::Array(rows) => rows[last.parse::<usize>().unwrap()] = value,
        _ => panic!("{path}"),
    }
    envelope.to_string()
}

#[test]
fn the_view_keeps_only_the_read_paths() {
    let view = envelope_view(&envelope().to_string()).unwrap();
    let mut keys = Vec::new();
    fn walk(value: &Value, path: String, keys: &mut Vec<String>) {
        if let Value::Object(map) = value {
            for (key, value) in map {
                let path = format!("{path}/{key}");
                keys.push(path.clone());
                if !matches!(key.as_str(), "members" | "tables" | "tournament") {
                    walk(value, path, keys);
                }
            }
        }
    }
    walk(&view, String::new(), &mut keys);
    keys.sort();
    assert_eq!(
        keys,
        [
            "/checkpoint",
            "/checkpoint/members",
            "/checkpoint/room",
            "/checkpoint/room/snapshot",
            "/checkpoint/room/snapshot/members",
            "/checkpoint/room/snapshot/tables",
            "/checkpoint/room/snapshot/tournament",
        ]
    );
    same_answers(&envelope().to_string());
    assert!(latency_roster_in(Some(&view)).is_ok());
}

#[test]
fn both_endpoint_spellings_and_both_incarnation_places_read_as_before() {
    // The camel case name first; a camel case name that is not text does
    // not fall back to the other spelling.
    for data in [
        json!({"authenticatedEndpoint": key(1), "incarnation": 11}),
        json!({"authenticated_endpoint": key(1), "incarnation": 11}),
        json!({"authenticatedEndpoint": key(1), "authenticated_endpoint": key(9)}),
        json!({"authenticated_endpoint": key(9), "authenticatedEndpoint": key(1)}),
        json!({"authenticatedEndpoint": 7, "authenticated_endpoint": key(1)}),
        json!({"authenticatedEndpoint": null, "authenticated_endpoint": key(1)}),
        json!({"authenticatedEndpoint": key(1).to_uppercase()}),
    ] {
        same_answers(&with("/checkpoint/members/0/data", data));
    }
    // The incarnation on the row, in its data, or in both.
    for row in [
        json!({"member": 1, "data": {"authenticatedEndpoint": key(1), "incarnation": 11}}),
        json!({"member": 1, "incarnation": "11", "data": {"authenticatedEndpoint": key(1), "incarnation": 11}}),
        json!({"member": 1, "incarnation": 11, "data": {"authenticatedEndpoint": key(1), "incarnation": 12}}),
        json!({"member": 1, "incarnation": 11.0, "data": {"authenticatedEndpoint": key(1)}}),
    ] {
        same_answers(&with("/checkpoint/members/0", row));
    }
}

#[test]
fn a_leaf_of_the_wrong_type_reads_as_before() {
    let wrong = [
        json!(null),
        json!("1"),
        json!(1.0),
        json!(-1),
        json!(u64::MAX),
        json!(i64::MAX as u64 + 1),
        json!(true),
        json!([1]),
        json!({"id": 1}),
    ];
    for path in [
        "/checkpoint/members/0/member",
        "/checkpoint/members/0/incarnation",
        "/checkpoint/members/0/frozen",
        "/checkpoint/members/0/data",
        "/checkpoint/members/0/data/latency",
        "/checkpoint/members/0/data/latency/endpoint",
        "/checkpoint/members/0/data/latency/relay",
        "/checkpoint/members/2/data/incarnation",
        "/checkpoint/room/snapshot/members/2/status",
        "/checkpoint/room/snapshot/members/2/table",
        "/checkpoint/room/snapshot/members/0/id",
        "/checkpoint/room/snapshot/tables/0/id",
        "/checkpoint/room/snapshot/tables/0/phase",
        "/checkpoint/room/snapshot/tables/0/revision",
        "/checkpoint/room/snapshot/tables/0/p1",
        "/checkpoint/room/snapshot/tables/0/p2",
        "/checkpoint/room/snapshot/tournament",
        "/checkpoint/room/snapshot/tables",
        "/checkpoint/room/snapshot/members",
        "/checkpoint/room/snapshot",
        "/checkpoint/room",
        "/checkpoint/members",
        "/checkpoint/members/0",
        "/checkpoint",
    ] {
        for value in &wrong {
            same_answers(&with(path, value.clone()));
        }
    }
    // An envelope that is not an object at all.
    for text in ["[1,2]", "5", "null", "\"text\"", "{}", "[]"] {
        same_answers(text);
    }
}

#[test]
fn a_repeated_key_keeps_its_last_value_at_every_level() {
    let good = envelope();
    let checkpoint = good["checkpoint"].to_string();
    let room = good["checkpoint"]["room"].to_string();
    let snapshot = good["checkpoint"]["room"]["snapshot"].to_string();
    let members = good["checkpoint"]["members"].to_string();
    let tournament = good["checkpoint"]["room"]["snapshot"]["tournament"].to_string();
    let texts = [
        format!(r#"{{"checkpoint":{{"members":"x"}},"checkpoint":{checkpoint}}}"#),
        format!(r#"{{"checkpoint":{checkpoint},"checkpoint":{{"members":"x"}}}}"#),
        format!(r#"{{"checkpoint":{checkpoint},"checkpoint":7}}"#),
        format!(r#"{{"checkpoint":{{"members":{members},"members":[],"room":{room}}}}}"#),
        format!(r#"{{"checkpoint":{{"members":[],"room":{room},"members":{members}}}}}"#),
        format!(r#"{{"checkpoint":{{"members":{members},"room":{room},"room":null}}}}"#),
        format!(r#"{{"checkpoint":{{"members":{members},"room":null,"room":{room}}}}}"#),
        format!(
            r#"{{"checkpoint":{{"members":{members},"room":{{"snapshot":{snapshot},"snapshot":{{}}}}}}}}"#
        ),
        format!(
            r#"{{"checkpoint":{{"members":{members},"room":{{"snapshot":{{"members":[],"tournament":{tournament},"tournament":null}}}}}}}}"#
        ),
        format!(
            r#"{{"checkpoint":{{"members":{members},"room":{{"snapshot":{{"tournament":null,"tournament":{tournament}}}}}}}}}"#
        ),
        // A name written with escapes is the same name.
        format!(r#"{{"\u0063heckpoint":{checkpoint}}}"#),
        format!(r#"{{"checkpoint":{checkpoint},"\u0063heckpoint":[]}}"#),
    ];
    for text in &texts {
        same_answers(text);
    }
    // And the last value is the one read.
    let last = envelope_view(&texts[0]).unwrap();
    assert!(latency_roster_in(Some(&last)).is_ok());
    let last = envelope_view(&texts[1]).unwrap();
    assert!(latency_roster_in(Some(&last)).is_err());
}

#[test]
fn a_text_a_value_parse_refuses_is_refused_wherever_it_is() {
    let checkpoint = envelope()["checkpoint"].to_string();
    let deep = format!("{}{}", "[".repeat(200), "]".repeat(200));
    let shallow = format!("{}{}", "[".repeat(100), "]".repeat(100));
    let refused_inside = [
        r#""\ud800""#.to_owned(),
        r#""\udc00x""#.to_owned(),
        "1e400".to_owned(),
        "-1e400".to_owned(),
        r#""\x""#.to_owned(),
        "\"\u{1}\"".to_owned(),
        deep,
        "01".to_owned(),
        "[1,]".to_owned(),
        "{\"a\":1,}".to_owned(),
        "{\"\\ud800\":1}".to_owned(),
    ];
    let accepted_inside = [
        shallow,
        r#""\ud83d\ude00""#.to_owned(),
        "1e300".to_owned(),
        "-0".to_owned(),
        "18446744073709551616".to_owned(),
    ];
    for inner in refused_inside.iter().chain(accepted_inside.iter()) {
        // In a skipped member, at the top and inside the checkpoint, and in
        // a kept one.
        same_answers(&format!(
            r#"{{"journal":{inner},"checkpoint":{checkpoint}}}"#
        ));
        same_answers(&format!(
            r#"{{"checkpoint":{{"journal":{inner},"members":[],"room":{{"snapshot":{{"x":{inner}}}}}}}}}"#
        ));
        same_answers(&format!(r#"{{"checkpoint":{{"members":[{inner}]}}}}"#));
    }
    for inner in &refused_inside {
        assert!(
            envelope_view(&format!(r#"{{"journal":{inner}}}"#)).is_none(),
            "{inner}"
        );
    }
    for text in ["", " ", "{", "{} x", "{}{}", "nul"] {
        same_answers(text);
        assert!(envelope_view(text).is_none(), "{text}");
    }
}

#[test]
fn nesting_counts_toward_the_same_depth_limit() {
    // The depth limit counts the kept levels too: find the deepest accepted
    // nesting under each path and check the view agrees at and past it.
    for prefix in [
        r#"{"x":"#,
        r#"{"checkpoint":{"x":"#,
        r#"{"checkpoint":{"room":{"snapshot":{"x":"#,
        r#"{"checkpoint":{"room":{"snapshot":{"tournament":"#,
    ] {
        let closing: String = prefix.chars().filter(|c| *c == '{').map(|_| '}').collect();
        for depth in 120..=130 {
            let text = format!(
                "{prefix}{}{}{closing}",
                "[".repeat(depth),
                "]".repeat(depth)
            );
            same_answers(&text);
        }
    }
}
