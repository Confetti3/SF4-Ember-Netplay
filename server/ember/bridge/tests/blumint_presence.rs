//! What the bridge reports to a stand-in BluMint about a match's status: who
//! has turned up and pressed Ready as the players arrive and the games start
//! and end, and the presence leases that lapse when a player's Ember stops
//! claiming, across restarts and clock steps. tests/blumint_results.rs covers
//! the results and the scores.
mod common;

use common::{
    Player,
    blumint::*,
    play::{BUILD, bound, claim, claim_ready, endpoint, permit, report},
    results,
};
use ember_bridge::{AppState, config, integrations::Secrets};
use ember_protocol::report::Outcome;
use reqwest::StatusCode;
use serde_json::{Value as Json, json};

const ABSENT: &str = "absent";
const NOT_READY: &str = "present-not-ready";
const READY: &str = "present-ready";

/// The status reports sent for the match so far, without the times they
/// were observed, which never go back.
fn sent_statuses(f: &Fixture, id: &str) -> Vec<Json> {
    sent_statuses_at(f, id).0
}

/// The status reports sent for the match so far, and apart from them the
/// times they were observed, which never go back.
fn sent_statuses_at(f: &Fixture, id: &str) -> (Vec<Json>, Vec<String>) {
    let (sent, times): (Vec<Json>, Vec<String>) = statuses(f, id).into_iter().map(untimed).unzip();
    for at in &times {
        assert!(ember_protocol::event::is_rfc3339_utc(at), "{at}");
    }
    assert!(times.windows(2).all(|pair| pair[0] <= pair[1]), "{times:?}");
    (sent, times)
}

// BluMint reads who has turned up from the status, so the bridge sends it on
// every change: as each player arrives, as each game starts, and as each game
// ends with another to come, when both are waiting for the next. Each is the
// match's next revision. It is pending until the fighters' first game starts.
// A game an agreed abort ends, and the set's end, change it no further.
#[tokio::test]
async fn a_status_follows_each_change_of_presence() {
    // Without a key only the test's own passes send, so each change is sent
    // before the next.
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    ember_bridge::deliver_results(&keyed).await;
    assert!(statuses(&f, &id).is_empty());

    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;
    // Sam arrives while Kate is setting up the room, which moves nothing
    // else about the match.
    let (status_code, wait) = claim(&f, &f.sam, &id, &endpoint(&f.sam), BUILD).await;
    assert_eq!(
        (status_code, wait["role"].as_str()),
        (StatusCode::OK, Some("wait"))
    );
    ember_bridge::deliver_results(&keyed).await;
    assert_eq!(statuses(&f, &id).len(), 2);
    // Both claim again as the room is set up, which changes nothing.
    let binding = bound(&f, &id).await;
    ember_bridge::deliver_results(&keyed).await;
    let first = permit(&f, &id, &binding, 1).await;
    ember_bridge::deliver_results(&keyed).await;
    report(&f, &f.kate, &f.kate, &id, &first, 1, Outcome::Abort).await;
    report(&f, &f.sam, &f.sam, &id, &first, 2, Outcome::Abort).await;
    ember_bridge::deliver_results(&keyed).await;
    let second = permit(&f, &id, &binding, 2).await;
    ember_bridge::deliver_results(&keyed).await;
    report(&f, &f.kate, &f.kate, &id, &second, 3, Outcome::P1Win).await;
    ember_bridge::deliver_results(&keyed).await;
    report(&f, &f.sam, &f.sam, &id, &second, 4, Outcome::P1Win).await;
    ember_bridge::deliver_results(&keyed).await;
    let third = permit(&f, &id, &binding, 3).await;
    ember_bridge::deliver_results(&keyed).await;
    report(&f, &f.kate, &f.kate, &id, &third, 5, Outcome::P1Win).await;
    let (_, decided) = report(&f, &f.sam, &f.sam, &id, &third, 6, Outcome::P1Win).await;
    assert_eq!(decided["attempt_state"], "accepted", "{decided}");
    ember_bridge::deliver_results(&keyed).await;

    assert_eq!(
        sent_statuses(&f, &id),
        vec![
            status(&f, &id, 1, "pending", [NOT_READY, ABSENT]),
            status(&f, &id, 2, "pending", [NOT_READY, NOT_READY]),
            status(&f, &id, 3, "running", [READY, READY]),
            status(&f, &id, 4, "running", [NOT_READY, NOT_READY]),
            status(&f, &id, 5, "running", [READY, READY]),
            status(&f, &id, 6, "running", [NOT_READY, NOT_READY]),
            status(&f, &id, 7, "running", [READY, READY]),
        ]
    );
    assert_eq!(
        submitted(&f),
        vec![progressed(&f, &id, [1, 0]), result(&f, &id, [2, 0])]
    );
    // The status has a row of its own, apart from the progress reports.
    assert_eq!(progress(&f, &id).await, vec![(1, "delivered".into())]);
    assert_eq!(status_row(&f, &id).await, Some((7, "delivered".into())));
}

// Each fighter's Ember says with every claim whether they pressed Ready at
// the match's table, and claims again at once when that changes, so BluMint
// can tell the player who stalls from the one waiting on them. Each change
// is the status's next revision; a claim that changes nothing sends nothing.
// A decided game takes back both fighters' Ready as it settles, since the room
// clears both seats when a game is played, before each Ember's claim says so.
// A game called off because one fighter took Ready back leaves the other
// Ready, as their Ember last said, and they read so. An Ember too old to say
// reads not ready until a game starts.
#[tokio::test]
async fn a_player_reads_ready_once_their_ember_says_they_pressed_ready() {
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    // Both open the match on an Ember too old to say: the first status
    // BluMint is sent has both present, not ready.
    let binding = bound(&f, &id).await;
    ember_bridge::deliver_results(&keyed).await;
    for (ready, who) in [
        (true, &f.kate),
        (true, &f.kate),
        // Kate takes Ready back, or leaves her seat.
        (false, &f.kate),
        (true, &f.sam),
        (true, &f.kate),
    ] {
        let (status_code, answer) = claim_ready(&f, who, &id, ready).await;
        assert_eq!(status_code, StatusCode::OK, "{answer}");
        ember_bridge::deliver_results(&keyed).await;
    }
    let first = permit(&f, &id, &binding, 1).await;
    ember_bridge::deliver_results(&keyed).await;
    report(&f, &f.kate, &f.kate, &id, &first, 1, Outcome::P1Win).await;
    report(&f, &f.sam, &f.sam, &id, &first, 2, Outcome::P1Win).await;
    ember_bridge::deliver_results(&keyed).await;
    // The game's end clears both seats at the table, and each Ember says so.
    // An older Ember's claim between games still says nothing.
    for who in [&f.kate, &f.sam] {
        claim_ready(&f, who, &id, false).await;
        ember_bridge::deliver_results(&keyed).await;
    }
    claim(&f, &f.sam, &id, &endpoint(&f.sam), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;
    // Both press Ready, and Sam takes it back once the game is permitted,
    // which calls it off: both report it cancelled. Kate is still Ready.
    for who in [&f.kate, &f.sam] {
        claim_ready(&f, who, &id, true).await;
        ember_bridge::deliver_results(&keyed).await;
    }
    let second = permit(&f, &id, &binding, 2).await;
    claim_ready(&f, &f.sam, &id, false).await;
    ember_bridge::deliver_results(&keyed).await;
    report(&f, &f.kate, &f.kate, &id, &second, 3, Outcome::Cancel).await;
    report(&f, &f.sam, &f.sam, &id, &second, 4, Outcome::Cancel).await;
    ember_bridge::deliver_results(&keyed).await;
    let (_, decided) = f
        .bridge
        .get(&f.provider, &format!("/v1/matches/{id}"))
        .await;
    assert_eq!(decided["state"], "between_games", "{decided}");

    assert_eq!(
        sent_statuses(&f, &id),
        vec![
            status(&f, &id, 2, "pending", [NOT_READY, NOT_READY]),
            status(&f, &id, 3, "pending", [READY, NOT_READY]),
            status(&f, &id, 4, "pending", [NOT_READY, NOT_READY]),
            status(&f, &id, 5, "pending", [NOT_READY, READY]),
            status(&f, &id, 6, "pending", [READY, READY]),
            status(&f, &id, 7, "running", [READY, READY]),
            status(&f, &id, 8, "running", [NOT_READY, NOT_READY]),
            status(&f, &id, 9, "running", [READY, NOT_READY]),
            status(&f, &id, 10, "running", [READY, READY]),
            status(&f, &id, 11, "running", [READY, NOT_READY]),
        ]
    );
}

// A fighter who presses Ready for the next game before this one is decided
// stays Ready once it is, while the other's Ready from before the game ended
// is taken back without waiting for their Ember's claim.
#[tokio::test]
async fn a_ready_pressed_after_a_game_ended_outlasts_its_result() {
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    f.bridge.clock.freeze();
    let binding = bound(&f, &id).await;
    for who in [&f.kate, &f.sam] {
        claim_ready(&f, who, &id, true).await;
    }
    let first = permit(&f, &id, &binding, 1).await;
    f.bridge.clock.advance(1);
    report(&f, &f.kate, &f.kate, &id, &first, 1, Outcome::P1Win).await;
    f.bridge.clock.advance(1);
    claim_ready(&f, &f.kate, &id, true).await;
    report(&f, &f.sam, &f.sam, &id, &first, 2, Outcome::P1Win).await;
    ember_bridge::deliver_results(&keyed).await;
    let sent = sent_statuses(&f, &id);
    let last = sent.last().unwrap();
    assert_eq!(
        last["players"]
            .as_array()
            .unwrap()
            .iter()
            .map(|player| player["status"].clone())
            .collect::<Vec<_>>(),
        vec![json!(READY), json!(NOT_READY)],
        "{sent:?}"
    );
}

// A status BluMint has not taken is sent again as it stands, the same
// revision observed at the same time; BluMint asks for that (a 503
// `match_not_ready`) while it has not recorded the create. One that changes
// before then is sent as it now stands, under its next revision, and the
// earlier one is not sent again.
#[tokio::test]
async fn a_status_is_sent_again_until_a_later_one_replaces_it() {
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    f.bridge.clock.freeze();
    let start = f.bridge.clock.now();

    f.blumint.lock().unwrap().answers.push_back((503, None));
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;
    assert_eq!(status_row(&f, &id).await, Some((1, "retrying".into())));
    f.bridge.clock.advance(2);
    ember_bridge::deliver_results(&keyed).await;
    let sent = statuses(&f, &id);
    assert_eq!(sent.len(), 2);
    assert_eq!(sent[0], sent[1]);
    assert_eq!(status_row(&f, &id).await, Some((1, "delivered".into())));

    // Sam's arrival waits on BluMint, and the game starts meanwhile.
    // A wait shorter than the claim lease, so both fighters stay present.
    f.blumint.lock().unwrap().answers.push_back((503, Some(5)));
    let binding = bound(&f, &id).await;
    ember_bridge::deliver_results(&keyed).await;
    assert_eq!(status_row(&f, &id).await, Some((2, "retrying".into())));
    permit(&f, &id, &binding, 1).await;
    ember_bridge::deliver_results(&keyed).await;
    f.bridge.clock.advance(6);
    ember_bridge::deliver_results(&keyed).await;
    let (sent, times) = sent_statuses_at(&f, &id);
    assert_eq!(
        sent,
        vec![
            status(&f, &id, 1, "pending", [NOT_READY, ABSENT]),
            status(&f, &id, 1, "pending", [NOT_READY, ABSENT]),
            status(&f, &id, 2, "pending", [NOT_READY, NOT_READY]),
            status(&f, &id, 3, "running", [READY, READY]),
        ]
    );
    // Each revision carries when it was observed, not when the match's
    // first one was.
    let observed = [start, start, start + 2, start + 2].map(ember_protocol::event::rfc3339);
    assert_eq!(times, observed);
    assert_eq!(status_row(&f, &id).await, Some((3, "delivered".into())));
}

// BluMint answers 404 `unknown_match` to a status for a match it has no
// lobby under, and says to stop without alerting: that revision is not sent
// again. A later change to the match is a new revision, sent as one.
#[tokio::test]
async fn a_status_on_a_match_blumint_does_not_know_is_not_sent_again() {
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    f.bridge.clock.freeze();

    f.blumint.lock().unwrap().answers.push_back((404, None));
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;
    assert_eq!(status_row(&f, &id).await, Some((1, "failed".into())));
    f.bridge.clock.advance(2);
    ember_bridge::deliver_results(&keyed).await;
    assert_eq!(statuses(&f, &id).len(), 1);

    bound(&f, &id).await;
    ember_bridge::deliver_results(&keyed).await;
    assert_eq!(
        sent_statuses(&f, &id),
        vec![
            status(&f, &id, 1, "pending", [NOT_READY, ABSENT]),
            status(&f, &id, 2, "pending", [NOT_READY, NOT_READY]),
        ]
    );
    assert_eq!(status_row(&f, &id).await, Some((2, "delivered".into())));
}

/// The presence lease, as the clock's signed seconds.
const LEASE: i64 = ember_bridge::PRESENCE_LEASE_SECS as i64;

/// Advances the clock `secs` in steps of at most 20 seconds, `who` claiming,
/// the maintenance pass running and BluMint being sent what it is owed after
/// each.
async fn claiming_for(f: &Fixture, keyed: &AppState, who: &Player, id: &str, secs: i64) {
    let mut left = secs;
    while left > 0 {
        let step = left.min(20);
        f.bridge.clock.advance(step);
        claim(f, who, id, &endpoint(who), BUILD).await;
        ember_bridge::maintain(f.bridge.state()).await;
        ember_bridge::deliver_results(keyed).await;
        left -= step;
    }
}

/// Runs the maintenance pass and sends BluMint what it is owed.
async fn sweep(f: &Fixture, keyed: &AppState) {
    ember_bridge::maintain(f.bridge.state()).await;
    ember_bridge::deliver_results(keyed).await;
}

// A player is present only while their Ember is connected to the match. It
// claims the match again ten seconds after each answer while open, so a
// claim older than the lease means it left: the maintenance pass sees the
// claim lapse and BluMint hears the player is absent, whether or not the
// other one is still there. A late claim, as one the helper gave up on and
// retried is, keeps them present, whoever observes the match meanwhile.
// Claiming again brings them back.
#[tokio::test]
async fn a_player_whose_ember_stops_claiming_reads_as_absent() {
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    f.bridge.clock.freeze();
    let start = f.bridge.clock.now();
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;
    claim(&f, &f.sam, &id, &endpoint(&f.sam), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;

    // Kate keeps claiming; Sam stops, and his claim lapses after its lease.
    claiming_for(&f, &keyed, &f.kate, &id, LEASE - 1).await;
    assert_eq!(statuses(&f, &id).len(), 2);
    f.bridge.clock.advance(1);
    sweep(&f, &keyed).await;
    // Sam comes back.
    f.bridge.clock.advance(5);
    claim(&f, &f.sam, &id, &endpoint(&f.sam), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;
    // Then both leave.
    f.bridge.clock.advance(LEASE + 1);
    ember_bridge::maintain(f.bridge.state()).await;
    ember_bridge::deliver_results(&keyed).await;
    ember_bridge::maintain(f.bridge.state()).await;
    ember_bridge::deliver_results(&keyed).await;

    let (sent, times) = sent_statuses_at(&f, &id);
    assert_eq!(
        sent,
        vec![
            status(&f, &id, 1, "pending", [NOT_READY, ABSENT]),
            status(&f, &id, 2, "pending", [NOT_READY, NOT_READY]),
            status(&f, &id, 3, "pending", [NOT_READY, ABSENT]),
            status(&f, &id, 4, "pending", [NOT_READY, NOT_READY]),
            status(&f, &id, 5, "pending", [ABSENT, ABSENT]),
        ]
    );
    let lease = ember_bridge::PRESENCE_LEASE_SECS;
    let observed = [
        start,
        start,
        start + lease,
        start + lease + 5,
        start + 2 * lease + 6,
    ];
    assert_eq!(times, observed.map(ember_protocol::event::rfc3339));
}

// The bridge hears no claims while it is down, so a claim live when it
// stopped is not counted lapsed for that time: once it starts again, each
// fighter has a lease to claim again in. A claim that lapsed before it
// stopped stays lapsed.
#[tokio::test]
async fn a_restart_does_not_read_a_connected_player_as_absent() {
    let mut f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    f.bridge.clock.freeze();
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;
    claim(&f, &f.sam, &id, &endpoint(&f.sam), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;
    // Sam leaves; Kate stays.
    claiming_for(&f, &keyed, &f.kate, &id, LEASE).await;
    f.bridge.clock.advance(1);
    sweep(&f, &keyed).await;
    assert_eq!(statuses(&f, &id).len(), 3);

    // The bridge is down for longer than a lease, and sweeps as it starts.
    f.bridge.clock.advance(LEASE + 30);
    f.bridge.restart(Secrets::default()).await;
    let keyed = keyed_state(&f);
    ember_bridge::maintain(f.bridge.state()).await;
    ember_bridge::deliver_results(&keyed).await;
    f.bridge.clock.advance(5);
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;

    assert_eq!(
        sent_statuses(&f, &id),
        vec![
            status(&f, &id, 1, "pending", [NOT_READY, ABSENT]),
            status(&f, &id, 2, "pending", [NOT_READY, NOT_READY]),
            status(&f, &id, 3, "pending", [NOT_READY, ABSENT]),
        ]
    );
}

// A bridge that stops again within a lease of starting keeps the first
// stop's span: a claim live through both stops gets a lease after the second,
// then lapses and is swept, so BluMint hears both players left.
#[tokio::test]
async fn a_second_restart_within_a_lease_still_sees_both_players_leave() {
    let mut f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    f.bridge.clock.freeze();
    sweep(&f, &keyed).await;
    f.bridge.clock.advance(5);
    for who in [&f.kate, &f.sam] {
        claim(&f, who, &id, &endpoint(who), BUILD).await;
        ember_bridge::deliver_results(&keyed).await;
    }
    // Both quit while the bridge is down. It starts again, sweeps once, and
    // stops again before the first start's lease ends.
    f.bridge.clock.advance(LEASE + 55);
    f.bridge.restart(Secrets::default()).await;
    f.bridge.clock.advance(30);
    sweep(&f, &keyed_state(&f)).await;
    f.bridge.clock.advance(20);
    f.bridge.restart(Secrets::default()).await;
    let keyed = keyed_state(&f);
    for _ in 0..(LEASE / 10 + 2) {
        f.bridge.clock.advance(10);
        sweep(&f, &keyed).await;
    }

    assert_eq!(
        sent_statuses(&f, &id),
        vec![
            status(&f, &id, 1, "pending", [NOT_READY, ABSENT]),
            status(&f, &id, 2, "pending", [NOT_READY, NOT_READY]),
            status(&f, &id, 3, "pending", [ABSENT, ABSENT]),
        ]
    );
}

// A claim can lapse after the bridge's last sweep and before it stops, and
// the other fighter's claim then tells BluMint that player left. The bridge
// was running then, so that claim is not extended across the stop, and
// BluMint does not hear the player come back.
#[tokio::test]
async fn a_claim_that_lapsed_before_a_stop_stays_lapsed_after_it() {
    let mut f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    f.bridge.clock.freeze();
    for who in [&f.kate, &f.sam] {
        claim(&f, who, &id, &endpoint(who), BUILD).await;
        ember_bridge::deliver_results(&keyed).await;
    }
    // Sam stops claiming. The last sweep runs before his claim lapses, and
    // Kate's next claim, after it lapses, observes him absent.
    let mut waited = 0;
    while waited + 20 < LEASE - 5 {
        f.bridge.clock.advance(20);
        waited += 20;
        claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    }
    f.bridge.clock.advance(LEASE - 5 - waited);
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    sweep(&f, &keyed).await;
    f.bridge.clock.advance(6);
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;
    // The bridge stops a few seconds later, and starts again within Kate's
    // lease; she claims again.
    f.bridge.clock.advance(29);
    f.bridge.restart(Secrets::default()).await;
    let keyed = keyed_state(&f);
    f.bridge.clock.advance(5);
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;
    f.bridge.clock.advance(5);
    sweep(&f, &keyed).await;

    assert_eq!(
        sent_statuses(&f, &id),
        vec![
            status(&f, &id, 1, "pending", [NOT_READY, ABSENT]),
            status(&f, &id, 2, "pending", [NOT_READY, NOT_READY]),
            status(&f, &id, 3, "pending", [NOT_READY, ABSENT]),
        ]
    );
}

// A wall clock stepped forward and then back leaves a status observed ahead
// of the time. It is not followed: both fighters, claiming as usual, read
// present again at once rather than once the clock catches up.
#[tokio::test]
async fn a_clock_stepped_back_does_not_read_connected_players_as_absent() {
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    f.bridge.clock.freeze();
    let start = f.bridge.clock.now();
    for who in [&f.kate, &f.sam] {
        claim(&f, who, &id, &endpoint(who), BUILD).await;
        ember_bridge::deliver_results(&keyed).await;
    }
    f.bridge.clock.advance(3600);
    sweep(&f, &keyed).await;
    f.bridge.clock.advance(-3590);
    for who in [&f.kate, &f.sam] {
        claim(&f, who, &id, &endpoint(who), BUILD).await;
        ember_bridge::deliver_results(&keyed).await;
    }

    // The times observed follow the clock back, unlike any other report's.
    let (sent, times): (Vec<Json>, Vec<String>) =
        statuses(&f, &id).into_iter().map(untimed).unzip();
    assert_eq!(
        sent,
        vec![
            status(&f, &id, 1, "pending", [NOT_READY, ABSENT]),
            status(&f, &id, 2, "pending", [NOT_READY, NOT_READY]),
            status(&f, &id, 3, "pending", [ABSENT, ABSENT]),
            status(&f, &id, 4, "pending", [NOT_READY, NOT_READY]),
        ]
    );
    assert_eq!(times[3], ember_protocol::event::rfc3339(start + 10));
}

// Every observer reads the clock before it waits for the database, so a
// claim can commit after a maintenance pass that read a later time. The
// status it observes is the pass's: a fighter the pass saw lapse stays
// absent, and the time observed never goes back.
#[tokio::test]
async fn a_late_commit_does_not_bring_back_a_lapsed_player() {
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    f.bridge.clock.freeze();
    let start = f.bridge.clock.now();
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    claim(&f, &f.sam, &id, &endpoint(&f.sam), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;
    // The pass that sees Sam lapse runs right after Kate's claim.
    claiming_for(&f, &keyed, &f.kate, &id, LEASE).await;
    // Kate's next claim read the clock a second before the pass did.
    f.bridge.clock.advance(-1);
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;

    let (sent, times) = sent_statuses_at(&f, &id);
    assert_eq!(
        sent,
        vec![
            status(&f, &id, 2, "pending", [NOT_READY, NOT_READY]),
            status(&f, &id, 3, "pending", [NOT_READY, ABSENT]),
        ]
    );
    assert_eq!(
        times,
        [start, start + ember_bridge::PRESENCE_LEASE_SECS].map(ember_protocol::event::rfc3339)
    );
}

// A fighter whose build differs from the one the other fighter claimed with
// cannot play the match on it, but they did open it: BluMint hears they are
// present, not that they never came, and the bridge cannot tell whether they
// stay, since their Ember stops claiming. Claiming again on the same build as
// the other fighter changes nothing BluMint hears, and from then on they are
// present while they keep claiming, like the other.
#[tokio::test]
async fn a_player_refused_for_their_build_reads_as_present() {
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    f.bridge.clock.freeze();
    let start = f.bridge.clock.now();
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    let (status_code, refused) = claim(&f, &f.sam, &id, &endpoint(&f.sam), "build-2").await;
    assert_eq!(
        (status_code, refused["error"]["code"].as_str()),
        (StatusCode::UNPROCESSABLE_ENTITY, Some("incompatible_build")),
        "{refused}"
    );
    ember_bridge::deliver_results(&keyed).await;
    let mut expected = vec![status(&f, &id, 2, "pending", [NOT_READY, NOT_READY])];
    assert_eq!(sent_statuses(&f, &id), expected);
    claiming_for(&f, &keyed, &f.kate, &id, LEASE).await;
    f.bridge.clock.advance(1);
    sweep(&f, &keyed).await;
    assert_eq!(sent_statuses(&f, &id), expected);

    let (status_code, _) = claim(&f, &f.sam, &id, &endpoint(&f.sam), BUILD).await;
    assert_eq!(status_code, StatusCode::OK);
    ember_bridge::deliver_results(&keyed).await;
    assert_eq!(sent_statuses(&f, &id), expected);
    // Sam then leaves.
    claiming_for(&f, &keyed, &f.kate, &id, LEASE).await;
    expected.push(status(&f, &id, 3, "pending", [NOT_READY, ABSENT]));
    let (sent, times) = sent_statuses_at(&f, &id);
    assert_eq!(sent, expected);
    assert_eq!(
        times[1],
        ember_protocol::event::rfc3339(start + 2 * ember_bridge::PRESENCE_LEASE_SECS + 1)
    );
}

// A status still waiting when the match ends is never sent, and the match's
// end sends none: the result or abort says how it ended.
#[tokio::test]
async fn no_status_is_sent_once_the_match_has_ended() {
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    cancel(&f, &id, "cancel-1").await;
    ember_bridge::deliver_results(&keyed).await;
    ember_bridge::deliver_results(&keyed).await;
    assert!(statuses(&f, &id).is_empty());
    assert_eq!(
        submitted(&f),
        vec![restart(&f, &id, "serverFailure", [0, 0])]
    );
    assert_eq!(status_row(&f, &id).await, Some((1, "not_required".into())));
}

// A game an organizer recorded before the fighters played is not their first
// game: it says nothing of who is present, and the status stays pending until
// the fighters' first game starts.
#[tokio::test]
async fn an_organizers_game_leaves_the_status_pending() {
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    for (key, game) in [("set-1", "game-1"), ("set-2", "game-2")] {
        let (_, created) = create(&f, key, 2).await;
        let id = created["matchId"].as_str().unwrap().to_owned();
        if key == "set-1" {
            draw(&f, &id, game).await;
        } else {
            win(&f, &id, 0, game).await;
        }
        ember_bridge::deliver_results(&keyed).await;
        assert_eq!(status_row(&f, &id).await, None);
        // Both arrive before the next pass, which sends the second arrival's
        // status alone.
        let binding = bound(&f, &id).await;
        ember_bridge::deliver_results(&keyed).await;
        permit(&f, &id, &binding, 1).await;
        ember_bridge::deliver_results(&keyed).await;
        assert_eq!(
            sent_statuses(&f, &id),
            vec![
                status(&f, &id, 2, "pending", [NOT_READY, NOT_READY]),
                status(&f, &id, 3, "running", [READY, READY]),
            ],
            "{key}"
        );
        close(&f, json!({ "idempotencyKey": key, "reason": "removed" })).await;
    }
}

// How the bridge reads each of BluMint's answers to a report: BluMint
// already having it is as good as delivered, a match it is not ready for
// waits as long as it asks, its own failure is tried again, and a match it
// does not know is not.
#[tokio::test]
async fn a_report_is_settled_as_blumints_answer_to_it_says() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let mut set = 0;
    let mut finished = |answers: Vec<(u16, Option<u64>)>| {
        set += 1;
        let key = format!("set-{set}");
        let f = &f;
        async move {
            let (_, created) = create(f, &key, 1).await;
            let id = created["matchId"].as_str().unwrap().to_owned();
            f.blumint.lock().unwrap().answers.extend(answers);
            win(f, &id, 0, &format!("{key}-game")).await;
            id
        }
    };

    let id = finished(vec![(409, None)]).await;
    results::until(&f.bridge, &id, |(state, _)| state == "delivered").await;

    let id = finished(vec![(503, Some(30)), (500, None)]).await;
    results::until(&f.bridge, &id, |(state, attempts)| {
        state == "retrying" && attempts == 1
    })
    .await;
    assert!(next_attempt(&f, &id).await >= f.bridge.clock.now() + 29);
    f.bridge.clock.advance(31);
    results::until(&f.bridge, &id, |(state, attempts)| {
        state == "retrying" && attempts == 2
    })
    .await;
    f.bridge.clock.advance(7);
    results::until(&f.bridge, &id, |(state, _)| state == "delivered").await;

    for refused in [404, 400] {
        let id = finished(vec![(refused, None)]).await;
        results::until(&f.bridge, &id, |(state, _)| state == "failed").await;
        assert_eq!(results::delivery(&f.bridge, &id).await.1, 1, "{refused}");
    }
    // A key BluMint does not know, or one bound to no game, waits for its
    // operator to fix it.
    for unauthorized in [401, 403] {
        let id = finished(vec![(unauthorized, None)]).await;
        results::until(&f.bridge, &id, |(state, attempts)| {
            state == "retrying" && attempts == 1
        })
        .await;
        f.bridge.clock.advance(2);
        results::until(&f.bridge, &id, |(state, _)| state == "delivered").await;
    }
}

// BluMint asks for a set again after its match ended without a result, with
// the games each team had won. The set carries on from that score, and every
// report counts it, so the set ends when a team reaches its length in all.
#[tokio::test]
async fn starting_wins_continue_the_set() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (status, created) = create_with(&f, "set-1", carried(&f, 2, [1, 0])).await;
    assert_eq!(status, StatusCode::OK, "{created}");
    let id = created["matchId"].as_str().unwrap().to_owned();
    win(&f, &id, 1, "game-1").await;
    assert_eq!(reports(&f, &id).await, vec![progressed(&f, &id, [1, 1])]);
    win(&f, &id, 0, "game-2").await;
    assert_eq!(
        reports(&f, &id).await,
        vec![progressed(&f, &id, [1, 1]), result(&f, &id, [2, 1])]
    );
    let completed: Vec<Json> = f
        .bridge
        .events(&f.provider, "0")
        .await
        .into_iter()
        .filter(|event| {
            event["type"] == "io.ember.tournament.match.completed.v1"
                && event["data"]["match_id"] == id
        })
        .collect();
    assert_eq!(completed[0]["data"]["starting_wins"], json!([1, 0]));

    let (_, created) = create_with(&f, "set-2", carried(&f, 2, [1, 0])).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    win(&f, &id, 0, "game-3").await;
    assert_eq!(reports(&f, &id).await, vec![result(&f, &id, [2, 0])]);

    // Each player's record counts both sets from their starting score, and
    // only the games played in them.
    let (status, kate) = record(&f, &f.kate).await;
    assert_eq!(status, StatusCode::OK, "{kate}");
    assert_eq!(kate["sets"], json!({ "played": 2, "won": 2, "lost": 0 }));
    assert_eq!(kate["games"], json!({ "won": 2, "lost": 1, "drawn": 0 }));
    assert_eq!(
        (
            &kate["recent"][0]["result"],
            &kate["recent"][0]["wins"],
            &kate["recent"][0]["opponent_wins"]
        ),
        (&json!("won"), &json!(2), &json!(0))
    );
    let (_, sam) = record(&f, &f.sam).await;
    assert_eq!(sam["sets"], json!({ "played": 2, "won": 0, "lost": 2 }));
    assert_eq!(
        (
            &sam["recent"][0]["result"],
            &sam["recent"][0]["wins"],
            &sam["recent"][0]["opponent_wins"]
        ),
        (&json!("lost"), &json!(0), &json!(2))
    );

    // Both teams or neither, each a whole number short of the set.
    let mut one_sided = request(&f, 2);
    one_sided["teams"][0]["startingWins"] = json!(1);
    for body in [carried(&f, 2, [2, 0]), carried(&f, 2, [0, 3]), one_sided, {
        let mut negative = carried(&f, 2, [0, 0]);
        negative["teams"][1]["startingWins"] = json!(-1);
        negative
    }] {
        let (status, refused) = create_with(&f, "set-3", body.clone()).await;
        assert_eq!(
            (status, refused),
            (
                StatusCode::BAD_REQUEST,
                json!({ "code": "invalid_request" })
            ),
            "{body}"
        );
    }

    // After a reset BluMint sends 0 for every team, which is a set from the
    // start: the same match as one with no starting wins.
    let (status, created) = create_with(&f, "set-3", carried(&f, 2, [0, 0])).await;
    assert_eq!(status, StatusCode::OK, "{created}");
    assert_eq!(
        create(&f, "set-3", 2).await,
        (StatusCode::OK, created.clone())
    );
    assert_eq!(
        create_with(&f, "set-3", carried(&f, 2, [1, 0])).await,
        (
            StatusCode::CONFLICT,
            json!({ "code": "idempotency_key_reused" })
        )
    );
    let id = created["matchId"].as_str().unwrap().to_owned();
    let binding = bound(&f, &id).await;
    assert_eq!(binding["version"], 1);
    assert!(binding.get("starting_wins").is_none(), "{binding}");
    close(
        &f,
        json!({ "idempotencyKey": "set-3", "reason": "removed" }),
    )
    .await;

    // The room the fighters play in starts from the score too.
    let (_, created) = create_with(&f, "set-4", carried(&f, 2, [1, 0])).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    let binding = bound(&f, &id).await;
    assert_eq!(binding["version"], 1);
    assert_eq!(binding["starting_wins"], json!([1, 0]));
    let game = permit(&f, &id, &binding, 1).await;
    report(&f, &f.kate, &f.kate, &id, &game, 1, Outcome::P1Win).await;
    report(&f, &f.sam, &f.sam, &id, &game, 2, Outcome::P1Win).await;
    assert_eq!(match_state(&f, &id).await, "completed");
}

async fn record(f: &Fixture, player: &Player) -> (StatusCode, Json) {
    f.bridge
        .get(&f.provider, &format!("/v1/players/{}/record", player.id()))
        .await
}
