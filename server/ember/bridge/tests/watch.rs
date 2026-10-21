//! Watching a bridge match (docs/design/MATCH_WATCH.md): who may watch, how
//! the bridge names watchers in the fighters' binding, and that watchers
//! coming and going never disturb the fighters' permits.
mod common;

use common::{
    blumint, code,
    play::{
        BUILD, ROOM, Table, act, bound, bound_as, claim_as, descriptor, endpoint, permit, prepare,
        publish, report,
    },
    watch::*,
};
use ember_protocol::{
    challenge::{Action, command_digest},
    play::SignedPermit,
    report::Outcome,
};
use reqwest::{Method, StatusCode};
use serde_json::json;

#[tokio::test]
async fn a_match_is_closed_to_watchers_until_its_organizer_opens_it() {
    let f = fixture().await;
    let player3 = viewer(&f.bridge, 40).await;
    let (status, refused) = ask(&f, &f.id, &player3).await;
    assert_eq!(status, StatusCode::FORBIDDEN, "{refused}");
    assert_eq!(
        (code(&refused), reason(&refused)),
        ("forbidden", "not_allowed")
    );
    let (status, fighter) = ask(&f, &f.id, &f.a).await;
    assert_eq!(status, StatusCode::FORBIDDEN, "{fighter}");
    assert_eq!(reason(&fighter), "fighter");
    let binding = bound_as(&f, &f.id, true).await;
    assert!(binding.get("watchers").is_none(), "{binding}");
    assert_eq!(
        count(&f.bridge, "SELECT COUNT(*) FROM match_watchers").await,
        0
    );

    let (status, unknown) = ask(&f, "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a99", &player3).await;
    assert_eq!(
        (status, code(&unknown)),
        (StatusCode::NOT_FOUND, "not_found")
    );

    // A match whose games an organizer enters is not played through Ember.
    // A stranger is told the watch policy before that.
    let provider = f.bridge.provider(CONNECTION).await;
    let (mut c, mut d) = (f.bridge.player(41), f.bridge.player(42));
    let entered = make_match(
        &f.bridge,
        &provider,
        [&mut c, &mut d],
        "entered-1",
        "organizer-reported-v1",
    )
    .await;
    let (status, closed) = ask(&f, &entered, &player3).await;
    assert_eq!(
        (status, reason(&closed)),
        (StatusCode::FORBIDDEN, "not_allowed")
    );
    assert_eq!(add_staff(&f, &player3).await, StatusCode::OK);
    let (status, unsupported) = ask(&f, &entered, &player3).await;
    assert_eq!(
        (status, code(&unsupported)),
        (StatusCode::UNPROCESSABLE_ENTITY, "unsupported_rules")
    );

    // A finished match cannot be watched.
    let id = f.id.clone();
    f.bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "UPDATE matches SET state = 'cancelled' WHERE id = ?1",
                [&id],
            )?;
            Ok(())
        })
        .await
        .unwrap();
    let (status, over) = ask(&f, &f.id, &player3).await;
    assert_eq!(
        (status, code(&over), reason(&over)),
        (StatusCode::CONFLICT, "stale_revision", "finished")
    );
}

#[tokio::test]
async fn a_stranger_learns_nothing_of_a_finished_match() {
    let f = fixture().await;
    let id = f.id.clone();
    f.bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "UPDATE matches SET state = 'cancelled' WHERE id = ?1",
                [&id],
            )?;
            Ok(())
        })
        .await
        .unwrap();
    let player3 = viewer(&f.bridge, 40).await;
    let (status, refused) = ask(&f, &f.id, &player3).await;
    assert_eq!(status, StatusCode::FORBIDDEN, "{refused}");
    assert_eq!(
        (code(&refused), reason(&refused)),
        ("forbidden", "not_allowed")
    );
    assert!(
        refused["error"]["details"].get("state").is_none(),
        "{refused}"
    );
}

#[tokio::test]
async fn a_watcher_waits_for_the_room_and_for_both_fighters() {
    let f = fixture().await;
    let player3 = viewer(&f.bridge, 40).await;
    add_staff(&f, &player3).await;
    assert_eq!(waiting(&f, &f.id, &player3).await, "no_room");
    let (_, host) = claim_as(&f, &f.a, &f.id, &endpoint(&f.a), BUILD, true).await;
    publish(&f, &f.a, &f.id, ROOM, Some(&host), None).await;
    assert_eq!(waiting(&f, &f.id, &player3).await, "not_bound");
    claim_as(&f, &f.b, &f.id, &endpoint(&f.b), BUILD, true).await;
    let binding = binding_for(&f, &f.id, &f.b).await;
    assert_eq!(watchers(&binding), [player3.id().as_str()]);
    let answer = admitted(&f, &f.id, &player3).await;
    assert_eq!(
        (answer["room_id"].as_str(), answer["invitation"].as_str()),
        (Some(ROOM), Some(format!("sf4e3:{ROOM}").as_str()))
    );
    assert!(
        answer.get("binding").is_none(),
        "a watcher is given no binding: {answer}"
    );
}

#[tokio::test]
async fn a_staff_watcher_is_named_in_the_next_binding_and_moves_only_the_binding_revision() {
    let f = fixture().await;
    let before = bound_as(&f, &f.id, true).await;
    let player3 = viewer(&f.bridge, 40).await;
    assert_eq!(add_staff(&f, &player3).await, StatusCode::OK);
    admitted(&f, &f.id, &player3).await;
    for fighter in [&f.b, &f.a] {
        let binding = binding_for(&f, &f.id, fighter).await;
        assert_eq!(revision(&binding), revision(&before) + 1);
        assert_eq!(
            binding["watchers"],
            json!([{ "ember_id": player3.id(), "endpoint_id": endpoint(&player3) }])
        );
    }
    let (revision_now, fighters_revision, _) = room_row(&f.bridge, &f.id).await;
    assert_eq!(
        (revision_now, fighters_revision),
        (revision(&before) + 1, revision(&before))
    );
}

#[tokio::test]
async fn watchers_coming_and_going_never_fail_a_prepare() {
    let f = fixture().await;
    let first = bound_as(&f, &f.id, true).await;
    let mut crowd = vec![];
    for byte in 40..44 {
        let player = viewer(&f.bridge, byte).await;
        add_staff(&f, &player).await;
        crowd.push(player);
    }
    let (start, fighters_start, _) = room_row(&f.bridge, &f.id).await;
    const CYCLES: u8 = 20;
    for cycle in 0..CYCLES {
        // Each cycle's challenges would pass a fighter's limit of 30 a minute.
        if cycle == CYCLES / 2 {
            f.bridge.clock.advance(61);
        }
        let player = &crowd[cycle as usize % crowd.len()];
        let generation = 10 + u64::from(cycle);
        // The first fighter holds the binding of its last claim throughout.
        let (status, pending) = prepare(&f, &f.a, &f.id, &first, generation).await;
        assert_eq!(
            (status, pending["state"].as_str()),
            (StatusCode::OK, Some("pending")),
            "{pending}"
        );
        admitted(&f, &f.id, player).await;
        let second = binding_for(&f, &f.id, &f.b).await;
        assert_eq!(watchers(&second), [player.id().as_str()]);
        leave(&f, &f.id, player).await;
        let (status, agreed) = prepare(&f, &f.b, &f.id, &second, generation).await;
        assert_eq!(
            (status, agreed["state"].as_str()),
            (StatusCode::OK, Some("permitted")),
            "{agreed}"
        );
        let permit: SignedPermit = serde_json::from_value(agreed["permit"].clone()).unwrap();
        let n = 2 * cycle + 1;
        report(&f, &f.a, &f.a, &f.id, &permit, n, Outcome::Draw).await;
        report(&f, &f.b, &f.b, &f.id, &permit, n + 1, Outcome::Draw).await;
    }
    // Two moves of the binding revision per cycle, none of the fighters'.
    let (end, fighters_end, named) = room_row(&f.bridge, &f.id).await;
    assert_eq!(
        (end, fighters_end, named),
        (start + 2 * u64::from(CYCLES), fighters_start, None)
    );
}

#[tokio::test]
async fn a_prepare_below_the_fighters_revision_is_stale() {
    let f = fixture().await;
    let old = bound_as(&f, &f.id, true).await;
    // The second fighter comes back on another endpoint.
    let (status, back) = claim_as(&f, &f.b, &f.id, &"b".repeat(64), BUILD, true).await;
    assert_eq!(status, StatusCode::OK, "{back}");
    assert_eq!(revision(&back["binding"]["binding"]), revision(&old) + 1);
    let (status, stale) = prepare(&f, &f.a, &f.id, &old, 11).await;
    assert_eq!(status, StatusCode::CONFLICT, "{stale}");
    assert_eq!(
        (code(&stale), reason(&stale)),
        ("stale_revision", "stale_binding")
    );
}

#[tokio::test]
async fn agreement_uses_the_current_fighters_revision() {
    let f = fixture().await;
    let old = bound_as(&f, &f.id, true).await;
    let (status, first) = prepare(&f, &f.a, &f.id, &old, 11).await;
    assert_eq!(
        (status, first["state"].as_str()),
        (StatusCode::OK, Some("pending")),
        "{first}"
    );
    // The other fighter reconnects on a new endpoint and prepares at the new
    // revision. The first fighter's description is below it, so no permit.
    let new_endpoint = "b".repeat(64);
    let (_, back) = claim_as(&f, &f.b, &f.id, &new_endpoint, BUILD, true).await;
    let current = back["binding"]["binding"].clone();
    let mut second = descriptor(&current, &f.b, 11);
    second["endpoint_id"] = json!(new_endpoint);
    let (status, answer) = act(
        &f,
        &f.b,
        Action::AttemptPrepare,
        &format!("/v1/matches/{}/attempts/prepare", f.id),
        second,
    )
    .await;
    assert_eq!(
        (status, answer["state"].as_str()),
        (StatusCode::OK, Some("pending")),
        "{answer}"
    );
    // The first fighter describes the game again at the new revision.
    let (status, agreed) = prepare(&f, &f.a, &f.id, &current, 11).await;
    assert_eq!(
        (status, agreed["state"].as_str()),
        (StatusCode::OK, Some("permitted")),
        "{agreed}"
    );
}

#[tokio::test]
async fn restarting_the_bridge_changes_no_live_match() {
    let mut f = blumint::fixture().await;
    let live_blumint = blumint_match(&f).await;
    let before_blumint = bound(&f, &live_blumint).await;
    let provider = f.bridge.provider(CONNECTION).await;
    let (mut c, mut d) = (f.bridge.player(61), f.bridge.player(62));
    let live_generic = make_match(
        &f.bridge,
        &provider,
        [&mut c, &mut d],
        "deploy-1",
        "ember-room-v1",
    )
    .await;
    let before_generic = bound(
        &Pair {
            bridge: &f.bridge,
            a: &c,
            b: &d,
        },
        &live_generic,
    )
    .await;

    // The bridge starts again over the same database, as a deploy does. The
    // migration's backfill is covered by the unit test in `db.rs`, which
    // migrates rows made before 028.
    f.bridge.restart(blumint::keyed()).await;

    let pair = Pair {
        bridge: &f.bridge,
        a: &c,
        b: &d,
    };
    let after_generic = binding_for(&pair, &live_generic, &d).await;
    let after_blumint = binding_for(&f, &live_blumint, f.b()).await;
    for (before, after) in [
        (&before_blumint, &after_blumint),
        (&before_generic, &after_generic),
    ] {
        // Exactly the fields a binding had before watching: no watchers, no
        // starting wins, the same revision.
        let mut keys: Vec<_> = after.as_object().unwrap().keys().cloned().collect();
        keys.sort();
        let mut expected = [
            "assignment_generation",
            "binding_revision",
            "bridge_id",
            "build_id",
            "fighters",
            "games_to_win",
            "issued_at",
            "match_id",
            "room_id",
            "roster_digest",
            "rules_digest",
            "version",
        ];
        expected.sort();
        assert_eq!(keys, expected);
        let (mut before, mut after) = ((*before).clone(), (*after).clone());
        for binding in [&mut before, &mut after] {
            binding.as_object_mut().unwrap().remove("issued_at");
        }
        assert_eq!(before, after);
    }
    assert_eq!(count(&f.bridge, "SELECT COUNT(*) FROM match_rooms WHERE fighters_revision != binding_revision OR bound_watchers IS NOT NULL").await, 0);
    // Both fighters' prepares, made after the restart on the bindings they
    // held before it, still agree.
    permit(&pair, &live_generic, &before_generic, 11).await;
    permit(&f, &live_blumint, &before_blumint, 11).await;
}

#[tokio::test]
async fn blumint_matches_stay_closed_to_watchers() {
    let f = blumint::fixture().await;
    let id = blumint_match(&f).await;
    let binding = bound(&f, &id).await;
    assert!(binding.get("watchers").is_none(), "{binding}");
    let player3 = viewer(&f.bridge, 70).await;
    let (status, refused) = ask(&f, &id, &player3).await;
    assert_eq!(
        (status, reason(&refused)),
        (StatusCode::FORBIDDEN, "not_allowed")
    );

    let connection = blumint::CONNECTION;
    let staff = format!("/v1/connections/{connection}/watch/staff/{}", player3.id());
    let denial = format!("/v1/matches/{id}/watch/denials/{}", player3.id());
    let policy = format!("/v1/connections/{connection}/watch");
    for (method, path, body) in [
        (
            Method::PUT,
            policy.clone(),
            Some(json!({ "link_holders": true })),
        ),
        (Method::PUT, staff.clone(), Some(json!({}))),
        (Method::DELETE, staff, None),
        (Method::PUT, denial.clone(), Some(json!({}))),
        (Method::DELETE, denial, None),
    ] {
        let (status, refused) = f
            .bridge
            .send(&f.organizer, method.clone(), &path, body)
            .await;
        assert_eq!(status, StatusCode::FORBIDDEN, "{method} {path}: {refused}");
        assert_eq!(reason(&refused), "connection_fixed");
    }
    let (status, read) = f.bridge.send(&f.provider, Method::GET, &policy, None).await;
    assert_eq!(
        (status, &read["link_holders"]),
        (StatusCode::OK, &json!(false))
    );
    for table in [
        "match_watchers",
        "connection_watch_staff",
        "match_watch_denials",
    ] {
        let sql: &'static str = Box::leak(format!("SELECT COUNT(*) FROM {table}").into_boxed_str());
        assert_eq!(count(&f.bridge, sql).await, 0, "{table}");
    }
    assert_eq!(
        count(
            &f.bridge,
            "SELECT COUNT(*) FROM provider_connections WHERE watch_link_holders != 0"
        )
        .await,
        0
    );
}

#[tokio::test]
async fn a_watcher_must_run_the_fighters_build() {
    let f = fixture().await;
    bound_as(&f, &f.id, true).await;
    let player3 = viewer(&f.bridge, 40).await;
    add_staff(&f, &player3).await;
    let (status, refused) = ask_from(&f, &f.id, &player3, &endpoint(&player3), "build-2").await;
    assert_eq!(
        (status, code(&refused)),
        (StatusCode::UNPROCESSABLE_ENTITY, "incompatible_build")
    );
    admitted(&f, &f.id, &player3).await;

    // Both fighters move to another build, which a claim alone cannot do
    // while the other holds the old one.
    let id = f.id.clone();
    f.bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "UPDATE match_claims SET build_id = 'build-2' WHERE match_id = ?1",
                [&id],
            )?;
            Ok(())
        })
        .await
        .unwrap();
    let (_, moved) = claim_as(&f, &f.a, &f.id, &endpoint(&f.a), "build-2", true).await;
    assert_eq!(moved["binding"]["binding"]["build_id"], "build-2");
    assert!(watchers(&moved["binding"]["binding"]).is_empty(), "{moved}");
    let (status, refused) = ask(&f, &f.id, &player3).await;
    assert_eq!(
        (status, code(&refused)),
        (StatusCode::UNPROCESSABLE_ENTITY, "incompatible_build")
    );
    let (status, back) = ask_from(&f, &f.id, &player3, &endpoint(&player3), "build-2").await;
    assert_eq!(
        (status, back["state"].as_str()),
        (StatusCode::OK, Some("room")),
        "{back}"
    );
}

#[tokio::test]
async fn link_holders_are_capped_and_staff_hold_the_ceiling() {
    let f = fixture().await;
    bound_as(&f, &f.id, true).await;
    assert_eq!(open_to_link_holders(&f, true).await, StatusCode::OK);
    // Arrivals a second apart, so their order is the order they came in.
    let mut holders = vec![];
    for byte in 40..43 {
        f.bridge.clock.advance(1);
        holders.push(viewer(&f.bridge, byte).await);
    }
    for holder in &holders[..2] {
        admitted(&f, &f.id, holder).await;
        f.bridge.clock.advance(1);
    }
    assert_eq!(waiting(&f, &f.id, &holders[2]).await, "full");
    let mut staff = vec![];
    for byte in 50..55 {
        let player = viewer(&f.bridge, byte).await;
        add_staff(&f, &player).await;
        staff.push(player);
    }
    for player in &staff[..4] {
        f.bridge.clock.advance(1);
        admitted(&f, &f.id, player).await;
    }
    assert_eq!(watchers(&binding_for(&f, &f.id, &f.b).await).len(), 6);
    // A fifth staff watcher pushes the newest link holder out at the ceiling.
    f.bridge.clock.advance(1);
    admitted(&f, &f.id, &staff[4]).await;
    let named = watchers(&binding_for(&f, &f.id, &f.b).await);
    assert_eq!(named.len(), 6);
    assert!(named.contains(&staff[4].id().as_str().to_owned()));
    assert!(!named.contains(&holders[1].id().as_str().to_owned()));
    assert!(named.contains(&holders[0].id().as_str().to_owned()));
    assert_eq!(waiting(&f, &f.id, &holders[1]).await, "full");
    admitted(&f, &f.id, &holders[0]).await;
}

#[tokio::test]
async fn a_denial_removes_a_watcher_and_clearing_it_lets_them_back() {
    let f = fixture().await;
    bound_as(&f, &f.id, true).await;
    let player3 = viewer(&f.bridge, 40).await;
    add_staff(&f, &player3).await;
    admitted(&f, &f.id, &player3).await;
    assert_eq!(watchers(&binding_for(&f, &f.id, &f.a).await).len(), 1);

    assert_eq!(deny(&f, &player3).await, StatusCode::OK);
    assert!(watchers(&binding_for(&f, &f.id, &f.a).await).is_empty());
    let (status, revoked) = ask(&f, &f.id, &player3).await;
    assert_eq!(
        (status, code(&revoked), reason(&revoked)),
        (StatusCode::FORBIDDEN, "forbidden", "revoked")
    );

    assert_eq!(clear_denial(&f, &player3).await, StatusCode::NO_CONTENT);
    admitted(&f, &f.id, &player3).await;
    assert_eq!(
        watchers(&binding_for(&f, &f.id, &f.a).await),
        [player3.id().as_str()]
    );
}

#[tokio::test]
async fn closing_the_setting_or_the_staff_list_drops_watchers_at_the_next_claim() {
    let f = fixture().await;
    bound_as(&f, &f.id, true).await;
    let (holder, staff) = (viewer(&f.bridge, 40).await, viewer(&f.bridge, 41).await);
    open_to_link_holders(&f, true).await;
    add_staff(&f, &staff).await;
    admitted(&f, &f.id, &holder).await;
    admitted(&f, &f.id, &staff).await;
    assert_eq!(watchers(&binding_for(&f, &f.id, &f.a).await).len(), 2);

    open_to_link_holders(&f, false).await;
    assert_eq!(
        watchers(&binding_for(&f, &f.id, &f.a).await),
        [staff.id().as_str()]
    );
    let (status, closed) = ask(&f, &f.id, &holder).await;
    assert_eq!(
        (status, reason(&closed)),
        (StatusCode::FORBIDDEN, "not_allowed")
    );

    assert_eq!(remove_staff(&f, &staff).await, StatusCode::NO_CONTENT);
    assert!(watchers(&binding_for(&f, &f.id, &f.a).await).is_empty());
}

#[tokio::test]
async fn a_watcher_who_goes_quiet_is_dropped_at_the_next_claim() {
    let f = fixture().await;
    // One second short of the window: the wall clock must not move under it.
    f.bridge.clock.freeze();
    bound_as(&f, &f.id, true).await;
    let player3 = viewer(&f.bridge, 40).await;
    add_staff(&f, &player3).await;
    admitted(&f, &f.id, &player3).await;
    f.bridge.clock.advance(59);
    assert_eq!(watchers(&binding_for(&f, &f.id, &f.a).await).len(), 1);
    f.bridge.clock.advance(2);
    assert!(watchers(&binding_for(&f, &f.id, &f.a).await).is_empty());
    assert_eq!(room_row(&f.bridge, &f.id).await.2, None);
    admitted(&f, &f.id, &player3).await;
}

#[tokio::test]
async fn leaving_removes_a_watcher_at_once() {
    let f = fixture().await;
    bound_as(&f, &f.id, true).await;
    let player3 = viewer(&f.bridge, 40).await;
    add_staff(&f, &player3).await;
    admitted(&f, &f.id, &player3).await;
    let (status, left) = leave(&f, &f.id, &player3).await;
    assert_eq!((status, &left["left"]), (StatusCode::OK, &json!(true)));
    assert_eq!(room_row(&f.bridge, &f.id).await.2, None);
    assert!(watchers(&binding_for(&f, &f.id, &f.b).await).is_empty());
    assert_eq!(
        count(&f.bridge, "SELECT COUNT(*) FROM match_watchers").await,
        0
    );
}

#[tokio::test]
async fn leaving_without_watching_changes_nothing() {
    let f = fixture().await;
    let player3 = viewer(&f.bridge, 40).await;
    let (status, left) = leave(&f, &f.id, &player3).await;
    assert_eq!((status, &left["left"]), (StatusCode::OK, &json!(true)));
    assert_eq!(
        count(&f.bridge, "SELECT COUNT(*) FROM match_rooms").await,
        0
    );
}

#[tokio::test]
async fn two_watchers_asking_for_one_endpoint_get_one_place() {
    let f = fixture().await;
    bound_as(&f, &f.id, true).await;
    open_to_link_holders(&f, true).await;
    let shared = "e".repeat(64);
    let (holder, first, second) = (
        viewer(&f.bridge, 40).await,
        viewer(&f.bridge, 41).await,
        viewer(&f.bridge, 42).await,
    );
    add_staff(&f, &first).await;
    add_staff(&f, &second).await;

    // Staff beats a link holder.
    let (_, answer) = ask_from(&f, &f.id, &holder, &shared, BUILD).await;
    assert_eq!(answer["state"], "room");
    f.bridge.clock.advance(1);
    ask_from(&f, &f.id, &first, &shared, BUILD).await;
    assert_eq!(
        watchers(&binding_for(&f, &f.id, &f.a).await),
        [first.id().as_str()]
    );
    let (_, answer) = ask_from(&f, &f.id, &holder, &shared, BUILD).await;
    assert_eq!(
        (answer["state"].as_str(), answer["reason"].as_str()),
        (Some("waiting"), Some("full"))
    );

    // Between equals the later arrival wins, and polling does not swap them.
    f.bridge.clock.advance(1);
    let (_, answer) = ask_from(&f, &f.id, &second, &shared, BUILD).await;
    assert_eq!(answer["state"], "room");
    let settled = revision(&binding_for(&f, &f.id, &f.a).await);
    for who in [&first, &second, &first, &second] {
        ask_from(&f, &f.id, who, &shared, BUILD).await;
    }
    let binding = binding_for(&f, &f.id, &f.a).await;
    assert_eq!(watchers(&binding), [second.id().as_str()]);
    assert_eq!(revision(&binding), settled);
    // Each loss is audited once, however often they ask: the link holder to
    // each of the two, and the earlier staff watcher to the later.
    let conflicts = count(
        &f.bridge,
        "SELECT COUNT(*) FROM audit_entries WHERE action = 'watch.endpoint_conflict'",
    )
    .await;
    assert_eq!(conflicts, 3);
}

#[tokio::test]
async fn a_replaced_room_keeps_its_watchers() {
    let f = fixture().await;
    bound_as(&f, &f.id, true).await;
    let player3 = viewer(&f.bridge, 40).await;
    add_staff(&f, &player3).await;
    admitted(&f, &f.id, &player3).await;
    let replacement = "1".repeat(32);
    let (status, replaced) = publish(&f, &f.b, &f.id, &replacement, None, Some(ROOM)).await;
    assert_eq!(status, StatusCode::OK, "{replaced}");
    assert_eq!(replaced["binding"]["binding"]["room_id"], replacement);
    assert_eq!(
        watchers(&replaced["binding"]["binding"]),
        [player3.id().as_str()]
    );
    let answer = admitted(&f, &f.id, &player3).await;
    assert_eq!(answer["room_id"], replacement);
}

#[tokio::test]
async fn the_organizers_view_says_who_watches_and_who_is_denied() {
    let f = fixture().await;
    bound_as(&f, &f.id, true).await;
    open_to_link_holders(&f, true).await;
    let (staff, holder) = (viewer(&f.bridge, 40).await, viewer(&f.bridge, 41).await);
    add_staff(&f, &staff).await;
    admitted(&f, &f.id, &staff).await;
    admitted(&f, &f.id, &holder).await;
    deny(&f, &holder).await;
    binding_for(&f, &f.id, &f.a).await;
    let path = format!("/v1/matches/{}/watchers", f.id);
    let (status, view) = f.bridge.send(&f.provider, Method::GET, &path, None).await;
    assert_eq!(status, StatusCode::OK, "{view}");
    assert_eq!(view["link_holders"], true);
    assert_eq!(view["denied"], json!([holder.id()]));
    let row = |who: &common::Player| {
        view["watchers"]
            .as_array()
            .unwrap()
            .iter()
            .find(|w| w["ember_id"] == who.id().as_str())
            .cloned()
            .unwrap()
    };
    assert_eq!(
        (row(&staff)["via"].as_str(), row(&staff)["bound"].as_bool()),
        (Some("staff"), Some(true))
    );
    assert_eq!(
        (
            row(&holder)["via"].as_str(),
            row(&holder)["bound"].as_bool()
        ),
        (Some("denied"), Some(false))
    );
    assert!(row(&staff)["first_seen_at"].is_u64() && row(&staff)["last_seen_at"].is_u64());
}

#[tokio::test]
async fn only_the_connections_organizer_writes_its_watch_policy() {
    let f = fixture().await;
    let policy = format!("/v1/connections/{CONNECTION}/watch");
    let player3 = viewer(&f.bridge, 40).await;
    let staff = format!("{policy}/staff/{}", player3.id());
    let denial = format!("/v1/matches/{}/watch/denials/{}", f.id, player3.id());
    let view = format!("/v1/matches/{}/watchers", f.id);
    let other_provider = f.bridge.provider("mock-b").await;
    let other_organizer = f.bridge.organizer("t2").await;

    let (status, read) = f
        .bridge
        .send(&f.organizer, Method::GET, &policy, None)
        .await;
    assert_eq!(status, StatusCode::OK, "{read}");
    assert_eq!(
        read,
        json!({ "connection_id": CONNECTION, "link_holders": false, "link_holder_cap": 2, "ceiling": 6, "staff": [] })
    );
    add_staff(&f, &player3).await;
    let (_, read) = f.bridge.send(&f.provider, Method::GET, &policy, None).await;
    assert_eq!(read["staff"][0]["ember_id"], player3.id().as_str());

    let writes = [
        (
            Method::PUT,
            policy.clone(),
            Some(json!({ "link_holders": true })),
        ),
        (Method::PUT, staff.clone(), Some(json!({}))),
        (Method::DELETE, staff.clone(), None),
        (Method::PUT, denial.clone(), Some(json!({}))),
        (Method::DELETE, denial.clone(), None),
    ];
    for (method, path, body) in &writes {
        // The connection's own provider may read but not write.
        let (status, refused) = f
            .bridge
            .send(&f.provider, method.clone(), path, body.clone())
            .await;
        assert_eq!(
            (status, code(&refused)),
            (StatusCode::FORBIDDEN, "forbidden"),
            "{method} {path}"
        );
        // Anyone else is told there is nothing there.
        for token in [&other_provider, &other_organizer] {
            let (status, hidden) = f
                .bridge
                .send(token, method.clone(), path, body.clone())
                .await;
            assert_eq!(
                (status, code(&hidden)),
                (StatusCode::NOT_FOUND, "not_found"),
                "{method} {path}"
            );
        }
        let (status, _) = f.bridge.send("", method.clone(), path, body.clone()).await;
        assert_eq!(status, StatusCode::UNAUTHORIZED);
        // A player's session is not a service credential.
        let (status, _) = f
            .bridge
            .send(player3.token(), method.clone(), path, body.clone())
            .await;
        assert_eq!(status, StatusCode::FORBIDDEN);
    }
    for token in [&other_provider, &other_organizer] {
        for path in [&policy, &view] {
            let (status, _) = f.bridge.send(token, Method::GET, path, None).await;
            assert_eq!(status, StatusCode::NOT_FOUND, "{path}");
        }
    }
    let (status, _) = f.bridge.send(&f.provider, Method::GET, &view, None).await;
    assert_eq!(status, StatusCode::OK);
    // Malformed requests are refused, and nothing changed.
    let (status, _) = f
        .bridge
        .send(
            &f.organizer,
            Method::PUT,
            &format!("{policy}/staff/not-an-ember-id"),
            Some(json!({})),
        )
        .await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let (status, _) = f
        .bridge
        .send(
            &f.organizer,
            Method::PUT,
            &staff,
            Some(json!({ "label": "x" })),
        )
        .await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let (status, _) = f
        .bridge
        .send(
            &f.organizer,
            Method::PUT,
            &policy,
            Some(json!({ "link_holders": "yes" })),
        )
        .await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let (status, _) = f
        .bridge
        .send(
            &f.organizer,
            Method::GET,
            "/v1/connections/no-such/watch",
            None,
        )
        .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    assert_eq!(
        count(
            &f.bridge,
            "SELECT COUNT(*) FROM provider_connections WHERE watch_link_holders != 0"
        )
        .await,
        0
    );
    // Every write that went through is in the audit log.
    let audited = count(
        &f.bridge,
        "SELECT COUNT(*) FROM audit_entries WHERE action LIKE 'watch.%'",
    )
    .await;
    assert_eq!(audited, 1);
}

#[tokio::test]
async fn watch_calls_are_limited_per_ember_id() {
    let f = fixture().await;
    let player3 = viewer(&f.bridge, 40).await;
    let path = format!("/v1/matches/{}/watch", f.id);
    for _ in 0..30 {
        let (status, _) = f.bridge.post(player3.token(), &path, json!({})).await;
        assert_eq!(status, StatusCode::BAD_REQUEST);
    }
    let (status, limited) = f.bridge.post(player3.token(), &path, json!({})).await;
    assert_eq!(
        (status, code(&limited)),
        (StatusCode::TOO_MANY_REQUESTS, "rate_limited")
    );
    // Another Ember ID has its own allowance.
    let player4 = viewer(&f.bridge, 41).await;
    let (status, _) = f.bridge.post(player4.token(), &path, json!({})).await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
}

#[tokio::test]
async fn the_watch_actions_are_asked_for_only_at_their_own_paths() {
    let f = fixture().await;
    let player3 = viewer(&f.bridge, 40).await;
    let digest = command_digest(&common::value(json!({})));
    let challenge = |action: &'static str, path: String| {
        let (f, player, digest) = (&f, &player3, digest.clone());
        async move {
            f.bridge
                .client
                .post(f.bridge.url("/v1/auth/challenges"))
                .bearer_auth(player.token())
                .header("content-type", "application/json")
                .body(
                    serde_json::to_vec(&json!({
                        "public_key": player.identity.public_key(),
                        "action": action,
                        "method": "POST",
                        "path": path,
                        "request_digest": digest,
                    }))
                    .unwrap(),
                )
                .send()
                .await
                .unwrap()
                .status()
        }
    };
    let base = format!("/v1/matches/{}", f.id);
    assert_eq!(
        challenge("match.watch", format!("{base}/watch")).await,
        StatusCode::CREATED
    );
    assert_eq!(
        challenge("match.unwatch", format!("{base}/watch/leave")).await,
        StatusCode::CREATED
    );
    for (action, path) in [
        ("match.watch", format!("{base}/claims")),
        ("match.watch", format!("{base}/watch/leave")),
        ("match.unwatch", format!("{base}/watch")),
    ] {
        assert_eq!(
            challenge(action, path.clone()).await,
            StatusCode::BAD_REQUEST,
            "{action} {path}"
        );
    }
}

#[tokio::test]
async fn the_bridge_lists_the_watch_feature() {
    let f = fixture().await;
    let (status, capabilities) = f.bridge.get(&f.provider, "/v1/capabilities").await;
    assert_eq!(status, StatusCode::OK);
    assert!(
        capabilities["features"]
            .as_array()
            .unwrap()
            .iter()
            .any(|feature| feature == "matches.watch")
    );
}
