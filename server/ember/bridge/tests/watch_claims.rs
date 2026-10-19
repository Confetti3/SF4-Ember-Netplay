//! Which fighters let the bridge name watchers (docs/design/MATCH_WATCH.md
//! section 2.2), and the edge cases of the set it names: endpoints, reassigned
//! fighters, returning watchers and the organizer's audit trail.
mod common;

use common::{
    play::{BUILD, Table, bound, claim, endpoint},
    watch::*,
};
use reqwest::StatusCode;
use serde_json::Value as Json;

/// The binding a claim from a released helper (one that does not say it reads
/// watchers) is answered with.
async fn plain_binding_for(f: &impl Table, id: &str, fighter: &common::Player) -> Json {
    let (status, answer) = claim(f, fighter, id, &endpoint(fighter), BUILD).await;
    verified_binding(f, &status, &answer).await
}

#[tokio::test]
async fn a_watcher_is_never_named_to_fighters_that_do_not_say_they_read_watchers() {
    let f = fixture().await;
    // Fighters whose claims are the bytes a released helper sends.
    let before = bound(&f, &f.id).await;
    let (staff, holder) = (viewer(&f.bridge, 40).await, viewer(&f.bridge, 41).await);
    add_staff(&f, &staff).await;
    open_to_link_holders(&f, true).await;
    // The watchers declare the fighters' own build, which is public: every
    // install of it sends the same one in its claims.
    for who in [&staff, &holder] {
        assert_eq!(waiting(&f, &f.id, who).await, "not_bound");
    }
    for fighter in [&f.a, &f.b, &f.a] {
        let binding = plain_binding_for(&f, &f.id, fighter).await;
        assert!(binding.get("watchers").is_none(), "{binding}");
        assert_eq!(strip_issued(&binding, &before), before);
    }
    let (revision_now, fighters_revision, named) = room_row(&f.bridge, &f.id).await;
    assert_eq!(
        (revision_now, fighters_revision, named),
        (revision(&before), revision(&before), None)
    );
    // They kept asking, and the organizer still sees them.
    assert_eq!(
        count(&f.bridge, "SELECT COUNT(*) FROM match_watchers").await,
        2
    );
}

/// `binding` with `issued_at` of `like`, so two bindings compare by the fields
/// that do not depend on when they were signed.
fn strip_issued(binding: &Json, like: &Json) -> Json {
    let mut copy = binding.clone();
    copy["issued_at"] = like["issued_at"].clone();
    copy
}

#[tokio::test]
async fn watchers_are_named_when_both_fighters_say_they_read_them() {
    let f = fixture().await;
    bound_watching(&f, &f.id).await;
    let player3 = viewer(&f.bridge, 40).await;
    add_staff(&f, &player3).await;
    admitted(&f, &f.id, &player3).await;
    for fighter in [&f.a, &f.b] {
        let binding = binding_for(&f, &f.id, fighter).await;
        assert_eq!(watchers(&binding), [player3.id().as_str()]);
    }
}

#[tokio::test]
async fn a_fighter_that_stops_saying_it_reads_watchers_drops_them_from_the_binding() {
    let f = fixture().await;
    bound_watching(&f, &f.id).await;
    let player3 = viewer(&f.bridge, 40).await;
    add_staff(&f, &player3).await;
    admitted(&f, &f.id, &player3).await;
    assert_eq!(watchers(&binding_for(&f, &f.id, &f.a).await).len(), 1);
    let (_, fighters_revision, _) = room_row(&f.bridge, &f.id).await;

    // The second fighter's helper claims again without `watch`.
    let dropped = plain_binding_for(&f, &f.id, &f.b).await;
    assert!(dropped.get("watchers").is_none(), "{dropped}");
    assert_eq!(room_row(&f.bridge, &f.id).await.2, None);
    assert_eq!(waiting(&f, &f.id, &player3).await, "not_bound");
    // The other fighter's next claim carries no watchers either, though its
    // own helper reads them.
    let other = binding_for(&f, &f.id, &f.a).await;
    assert!(other.get("watchers").is_none(), "{other}");
    assert_eq!(room_row(&f.bridge, &f.id).await.1, fighters_revision);

    // Saying so again names the watcher again.
    assert_eq!(
        watchers(&binding_for(&f, &f.id, &f.b).await),
        [player3.id().as_str()]
    );
    admitted(&f, &f.id, &player3).await;
}

#[tokio::test]
async fn a_watcher_on_a_fighters_endpoint_is_not_named() {
    let f = fixture().await;
    let before = bound_watching(&f, &f.id).await;
    let player3 = viewer(&f.bridge, 40).await;
    add_staff(&f, &player3).await;
    for fighter in [&f.a, &f.b] {
        let (status, answer) = ask_from(&f, &f.id, &player3, &endpoint(fighter), BUILD).await;
        assert_eq!(
            (status, answer["state"].as_str(), answer["reason"].as_str()),
            (StatusCode::OK, Some("waiting"), Some("full")),
            "{answer}"
        );
        // Both fighters' claims still answer with a binding the bridge signed.
        for claimant in [&f.a, &f.b] {
            let binding = binding_for(&f, &f.id, claimant).await;
            assert!(binding.get("watchers").is_none(), "{binding}");
            assert_eq!(revision(&binding), revision(&before));
        }
    }
}

#[tokio::test]
async fn a_fighter_reconnecting_on_a_watchers_endpoint_takes_it_from_the_watcher() {
    let f = fixture().await;
    bound_watching(&f, &f.id).await;
    let player3 = viewer(&f.bridge, 40).await;
    add_staff(&f, &player3).await;
    admitted(&f, &f.id, &player3).await;
    let taken = endpoint(&player3);
    let (status, back) = claim_watching(&f, &f.b, &f.id, &taken, BUILD).await;
    let binding = verified_binding(&f, &status, &back).await;
    assert_eq!(binding["fighters"][1]["endpoint_id"], taken);
    assert!(binding.get("watchers").is_none(), "{binding}");
    assert_eq!(waiting(&f, &f.id, &player3).await, "full");
    // The other fighter's claim is answered with a binding that verifies too.
    let other = binding_for(&f, &f.id, &f.a).await;
    assert_eq!(other["fighters"][1]["endpoint_id"], taken);
}

#[tokio::test]
async fn a_watcher_who_becomes_a_fighter_is_no_longer_named() {
    let f = fixture().await;
    bound_watching(&f, &f.id).await;
    let player3 = viewer(&f.bridge, 40).await;
    add_staff(&f, &player3).await;
    admitted(&f, &f.id, &player3).await;
    // The organizer's platform gives the second slot to the watcher.
    let (id, who) = (f.id.clone(), player3.id().as_str().to_owned());
    f.bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "UPDATE match_participants SET ember_id = ?1 WHERE match_id = ?2 AND slot = 1",
                rusqlite::params![who, id],
            )?;
            Ok(())
        })
        .await
        .unwrap();
    let (status, claimed) = claim_watching(&f, &player3, &f.id, &endpoint(&player3), BUILD).await;
    let binding = verified_binding(&f, &status, &claimed).await;
    assert_eq!(binding["fighters"][1]["ember_id"], player3.id().as_str());
    assert!(binding.get("watchers").is_none(), "{binding}");
    let (status, refused) = ask(&f, &f.id, &player3).await;
    assert_eq!(
        (status, reason(&refused)),
        (StatusCode::FORBIDDEN, "fighter")
    );
}

#[tokio::test]
async fn a_returning_watcher_does_not_push_out_the_one_who_took_its_place() {
    let f = fixture().await;
    bound_watching(&f, &f.id).await;
    open_to_link_holders(&f, true).await;
    let mut holders = vec![];
    for byte in 40..45 {
        holders.push(viewer(&f.bridge, byte).await);
    }
    for holder in &holders[..4] {
        admitted(&f, &f.id, holder).await;
        f.bridge.clock.advance(1);
    }
    // The first goes quiet while the others keep asking.
    for _ in 0..3 {
        f.bridge.clock.advance(25);
        for holder in &holders[1..4] {
            admitted(&f, &f.id, holder).await;
        }
    }
    assert_eq!(watchers(&binding_for(&f, &f.id, &f.a).await).len(), 3);
    admitted(&f, &f.id, &holders[4]).await;
    f.bridge.clock.advance(1);
    // It comes back, and is the newest arrival, not the oldest.
    assert_eq!(waiting(&f, &f.id, &holders[0]).await, "full");
    let named = watchers(&binding_for(&f, &f.id, &f.a).await);
    assert!(
        named.contains(&holders[4].id().as_str().to_owned()),
        "{named:?}"
    );
    assert!(
        !named.contains(&holders[0].id().as_str().to_owned()),
        "{named:?}"
    );
}

#[tokio::test]
async fn organizer_changes_are_audited_with_who_they_name() {
    let f = fixture().await;
    let player3 = viewer(&f.bridge, 40).await;
    let who = player3.id().as_str().to_owned();
    add_staff(&f, &player3).await;
    deny(&f, &player3).await;
    clear_denial(&f, &player3).await;
    remove_staff(&f, &player3).await;
    open_to_link_holders(&f, true).await;
    open_to_link_holders(&f, false).await;
    let entry = |action: &str, reason: &str| (action.to_owned(), reason.to_owned());
    assert_eq!(
        audited(&f.bridge).await,
        [
            entry("watch.staff.add", &who),
            entry("watch.deny.set", &who),
            entry("watch.deny.clear", &who),
            entry("watch.staff.remove", &who),
            entry("watch.link_holders.set", "on"),
            entry("watch.link_holders.set", "off"),
        ]
    );
}
