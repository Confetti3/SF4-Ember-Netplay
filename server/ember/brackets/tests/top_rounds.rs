//! First to N from the top on, and the grand final's length: worked out from
//! the whole bracket, read before any match is made, so an early round is
//! never taken for the top or the final.
mod support;

use ember_brackets::{Rounds, Top};
use support::{Setup, World, challonge::FakeMatch};

/// A 16-player double elimination bracket, as Challonge numbers it (winners
/// rounds 1 to 4 and the grand final 5, losers rounds -1 to -6), with set
/// 1001 open at `round` between players 1 and 2 and every other set pending.
fn bracket(round: i32) -> Vec<FakeMatch> {
    let mut matches = vec![FakeMatch::open("1001", round, ["355", "354"])];
    let rounds = (1..=5).chain((1..=6).map(|r| -r)).filter(|&r| r != round);
    for (index, r) in rounds.enumerate() {
        matches.push(FakeMatch::fed_by(
            &format!("{}", 2000 + index),
            r,
            ["1001", "1001"],
        ));
    }
    matches
}

/// How many games set 1001's Ember match is first to, with `round` open.
async fn first_to(round: i32) -> u64 {
    let world = World::start(Setup {
        rounds: Rounds {
            games_to_win: 2,
            rules: vec![],
            top: Some(Top {
                top: 8,
                games_to_win: 3,
            }),
            grand_final: Some(5),
            tiers: vec![],
        },
        matches: bracket(round),
        ..Setup::default()
    })
    .await;
    world.bridge.connect(1, "player1").await;
    world.bridge.connect(2, "Player_2").await;
    world.connector.pass(true).await;
    let matches = world.bridge.matches().await;
    assert_eq!(
        matches.len(),
        1,
        "set 1001 gets its match on the first pass"
    );
    world.snapshot(&matches[0].1).await["games_to_win"]
        .as_u64()
        .unwrap()
}

#[tokio::test]
async fn round_one_of_a_bracket_still_open_is_the_base_length() {
    // Only round 1 is open when the tournament is first seen; the whole read
    // still shows it is a bracket of 16, not the top or the final.
    assert_eq!(first_to(1).await, 2);
    assert_eq!(first_to(-2).await, 2);
}

#[tokio::test]
async fn the_top_and_the_grand_final_get_their_lengths() {
    assert_eq!(first_to(3).await, 3);
    assert_eq!(first_to(-3).await, 3);
    assert_eq!(first_to(5).await, 5);
}
