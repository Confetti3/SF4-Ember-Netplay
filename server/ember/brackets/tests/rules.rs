//! A start.gg tournament's set lengths by phase and its sets held for a
//! stream, between a fake start.gg and a real bridge: a rule that names a
//! phase by name or ID, a set with no phase, a set on stream that waits (also
//! when its creation was saved and never answered, and when its match
//! restarted), and the fields asked of start.gg only when used.
mod support;

use ember_brackets::{Hold, RoundRule, Rounds, Side};
use support::{
    Setup,
    startgg::{EVENT_SLUG, FakeSet, four},
};

const BRACKET: &str = "startgg:tournament/ember-weekly/event/usf4";

/// A rule for first to 3 in the phase named `phase`, otherwise first to 2.
fn top_8_is_first_to_3(phase: &str) -> Rounds {
    Rounds {
        games_to_win: 2,
        rules: vec![RoundRule {
            phase: Some(phase.into()),
            from_round: 1,
            to_round: None,
            side: Side::Any,
            games_to_win: 3,
        }],
        top: None,
        grand_final: None,
        tiers: vec![],
    }
}

fn external(set: u64, attempt: u32) -> String {
    format!("startgg:{EVENT_SLUG}:{set}:{attempt}")
}

#[tokio::test]
async fn phases_that_reuse_round_numbers_take_their_own_length() {
    // Both sets are round 1: one of pools, first to 2, one of top 8, first
    // to 3 by the rule naming that phase.
    let mut pools = FakeSet::open(65_089_253, 1, [2_401_411, 2_401_412]);
    pools.phase = Some((1_300_001, "Pools"));
    let mut top = FakeSet::open(65_089_255, 1, [2_401_414, 2_401_415]);
    top.phase = Some((1_300_002, "Top 8"));
    let (fake, world) = four(
        vec![pools, top],
        Setup {
            rounds: top_8_is_first_to_3("top 8"),
            ..Setup::default()
        },
    )
    .await;
    world.connector.pass(true).await;
    assert_eq!(
        world.lengths().await,
        [(external(65_089_253, 1), 2), (external(65_089_255, 1), 3)]
    );
    // The sets' phases are asked for because a rule names one, and their
    // streams are not, since nothing is held.
    let sets = fake.query("Sets");
    assert!(sets.contains("phaseGroup"), "{sets}");
    assert!(!sets.contains("stream"), "{sets}");
}

#[tokio::test]
async fn a_rule_can_name_a_phase_by_its_id_and_a_set_without_a_phase_takes_the_default() {
    let mut top = FakeSet::open(65_089_253, 1, [2_401_411, 2_401_412]);
    top.phase = Some((1_300_002, "Top 8"));
    // No phase: only a rule without one could cover it.
    let none = FakeSet::open(65_089_255, 1, [2_401_414, 2_401_415]);
    let (_fake, world) = four(
        vec![top, none],
        Setup {
            rounds: top_8_is_first_to_3("1300002"),
            ..Setup::default()
        },
    )
    .await;
    world.connector.pass(true).await;
    assert_eq!(
        world.lengths().await,
        [(external(65_089_253, 1), 3), (external(65_089_255, 1), 2)]
    );
}

#[tokio::test]
async fn a_rule_for_a_misspelt_phase_gives_its_sets_the_default_length() {
    // The rule's phase is not a phase of the event, so its set is first to
    // 2. The connector says so once (the watch's own test covers what it
    // says); what must not change is the length.
    let mut top = FakeSet::open(65_089_253, 1, [2_401_411, 2_401_412]);
    top.phase = Some((1_300_002, "Top 8"));
    let (_fake, world) = four(
        vec![top],
        Setup {
            rounds: top_8_is_first_to_3("Top8"),
            ..Setup::default()
        },
    )
    .await;
    world.connector.pass(true).await;
    assert_eq!(world.lengths().await, [(external(65_089_253, 1), 2)]);
}

fn streamed_and_other() -> Vec<FakeSet> {
    let mut streamed = FakeSet::open(65_089_253, 1, [2_401_411, 2_401_412]);
    streamed.stream = Some("Main stage");
    vec![
        streamed,
        FakeSet::open(65_089_255, 1, [2_401_414, 2_401_415]),
    ]
}

fn held() -> Setup {
    Setup {
        hold: Hold { streamed: true },
        ..Setup::default()
    }
}

#[tokio::test]
async fn a_set_on_stream_waits_until_it_is_taken_off_the_stream() {
    let (fake, world) = four(streamed_and_other(), held()).await;
    world.connector.pass(true).await;
    // The set on stream waits; the other is played.
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 1);
    assert_eq!(matches[0].0, external(65_089_255, 1));
    assert_eq!(world.progress(BRACKET, "65089253"), "waiting");
    let note = world.note(BRACKET, "65089253");
    assert_eq!(
        note.as_deref(),
        Some(
            "held: it is assigned to stream Main stage; it is played in Ember once it is taken off the stream"
        )
    );
    // Streams are asked for because they are held.
    assert!(fake.query("Sets").contains("stream"));

    // Still on the stream, later: the same reason stands.
    world.advance(601);
    world.connector.pass(true).await;
    assert_eq!(world.bridge.matches().await.len(), 1);
    assert_eq!(world.note(BRACKET, "65089253"), note);

    // Taken off the stream, it is played in Ember, and nothing waits.
    fake.state.lock().unwrap().sets[0].stream = None;
    world.advance(601);
    world.connector.pass(true).await;
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 2);
    assert!(
        matches
            .iter()
            .any(|(found, _, _)| found == &external(65_089_253, 1))
    );
    assert_eq!(world.progress(BRACKET, "65089253"), "live");
    assert_eq!(world.note(BRACKET, "65089253"), None);
}

#[tokio::test]
async fn a_set_on_stream_is_played_when_holding_is_off() {
    let mut streamed = FakeSet::open(65_089_253, 1, [2_401_411, 2_401_412]);
    streamed.stream = Some("Main stage");
    let (fake, world) = four(vec![streamed], Setup::default()).await;
    world.connector.pass(true).await;
    assert_eq!(world.bridge.matches().await.len(), 1);
    // Nothing is held and no rule names a phase: neither is asked for.
    let sets = fake.query("Sets");
    assert!(
        !sets.contains("stream") && !sets.contains("phaseGroup"),
        "{sets}"
    );
}

#[tokio::test]
async fn a_live_set_put_on_stream_is_left_as_it_is() {
    let (fake, world) = four(
        vec![FakeSet::open(65_089_253, 1, [2_401_411, 2_401_412])],
        held(),
    )
    .await;
    world.connector.pass(true).await;
    let (_, match_id, _) = world.bridge.matches().await.remove(0);
    // The organizer assigns the live set to a stream: its match goes on.
    fake.state.lock().unwrap().sets[0].stream = Some("Main stage");
    world.advance(601);
    world.connector.pass(true).await;
    assert_eq!(world.progress(BRACKET, "65089253"), "live");
    assert_eq!(world.snapshot(&match_id).await["state"], "awaiting_players");
    assert_eq!(world.bridge.matches().await.len(), 1);
}

#[tokio::test]
async fn a_set_whose_match_restarted_waits_if_it_is_on_stream_then() {
    let (fake, world) = four(
        vec![FakeSet::open(65_089_253, 1, [2_401_411, 2_401_412])],
        held(),
    )
    .await;
    world.connector.pass(true).await;
    let (_, first, _) = world.bridge.matches().await.remove(0);
    fake.state.lock().unwrap().sets[0].stream = Some("Main stage");
    // The match restarts: a new attempt, which a stream holds like any set.
    world.cancel(&first).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(world.progress(BRACKET, "65089253"), "waiting");
    assert_eq!(world.bridge.matches().await.len(), 1);
    let note = world.note(BRACKET, "65089253").unwrap();
    assert!(note.contains("its earlier Ember match ended"), "{note}");
    // Off the stream, the second attempt is played.
    fake.state.lock().unwrap().sets[0].stream = None;
    world.advance(601);
    world.connector.pass(true).await;
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 2);
    assert_eq!(matches[1].0, external(65_089_253, 2));
}

#[tokio::test]
async fn a_saved_creation_does_not_give_a_set_on_stream_a_match() {
    let (fake, world) = four(streamed_and_other(), held()).await;
    // Off the stream for now, the set gets its match.
    fake.state.lock().unwrap().sets[0].stream = None;
    world.connector.pass(true).await;
    let first = world
        .bridge
        .matches()
        .await
        .into_iter()
        .find(|(found, _, _)| found == &external(65_089_253, 1))
        .unwrap()
        .1;
    // The bridge's answer to that creation was lost, and the organizer
    // assigned the set to a stream before the connector sent it again.
    world
        .stage_unanswered(BRACKET, "65089253", ["2401411", "2401412"], &first)
        .await;
    fake.state.lock().unwrap().sets[0].stream = Some("Main stage");
    world.advance(601);
    world.connector.pass(true).await;
    // The creation was sent only to learn the match, which is cancelled:
    // no live match waits for players on a set played on stream.
    assert_eq!(world.snapshot(&first).await["state"], "cancelled");
    assert_eq!(world.progress(BRACKET, "65089253"), "waiting");
    assert_eq!(world.bridge.matches().await.len(), 2);
    // The set is played as a new attempt once it is off the stream.
    fake.state.lock().unwrap().sets[0].stream = None;
    world.advance(601);
    world.connector.pass(true).await;
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 3);
    assert!(
        matches
            .iter()
            .any(|(found, _, _)| found == &external(65_089_253, 2))
    );
    assert_eq!(world.progress(BRACKET, "65089253"), "live");
}
