//! Each bracket set's Ember match as the connector keeps it, the results it
//! receives, and the pure decisions about them: what a read of the bracket
//! does with an open set, and what a result does. `bracket.rs` carries them
//! out.
use std::fmt;

use ember_protocol::{EmberId, partner::Outcome};
use serde::Deserialize;

use crate::site::{Set, Status};

/// Ember matches made for one set before the organizer has to decide it:
/// a set that restarts or expires this often is not going to be played.
pub const MAX_ATTEMPTS: u32 = 5;

/// `<site>:<tournament>:<set>:<attempt>`, the `external_match_id` of each
/// attempt at a set. A new attempt is a new match, never a changed one.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ExternalId {
    /// `<site>:<tournament>` (`config::Tournament::key`).
    pub bracket: String,
    pub set: String,
    pub attempt: u32,
}

impl ExternalId {
    pub fn parse(text: &str) -> Option<Self> {
        let mut parts = text.rsplitn(3, ':');
        let attempt = parts.next()?.parse().ok().filter(|&attempt| attempt >= 1)?;
        let set = parts.next().filter(|set| !set.is_empty())?;
        let bracket = parts.next().filter(|bracket| bracket.contains(':'))?;
        Some(Self {
            bracket: bracket.to_owned(),
            set: set.to_owned(),
            attempt,
        })
    }
}

impl fmt::Display for ExternalId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}:{}:{}", self.bracket, self.set, self.attempt)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Progress {
    /// Attempt `attempt` has no Ember match recorded yet: its players are
    /// not found, or the bridge has not answered its creation.
    Waiting,
    /// `match_id` is being played.
    Live,
    /// The score went back to the site.
    Reported,
    /// The organizer reported, changed or removed the set on the site.
    Closed,
    /// Left to the organizer: `MAX_ATTEMPTS` ended without a result, the
    /// site refused its score, or the result's players were not the set's.
    Stuck,
}

impl Progress {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Waiting => "waiting",
            Self::Live => "live",
            Self::Reported => "reported",
            Self::Closed => "closed",
            Self::Stuck => "stuck",
        }
    }

    pub fn parse(text: &str) -> Option<Self> {
        [
            Self::Waiting,
            Self::Live,
            Self::Reported,
            Self::Closed,
            Self::Stuck,
        ]
        .into_iter()
        .find(|progress| progress.as_str() == text)
    }
}

/// One set, as stored.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Tracked {
    /// The site's participant IDs of player 1 and player 2, Ember slots 0
    /// and 1.
    pub entries: [String; 2],
    /// The Ember players of this attempt's match, by slot, once its
    /// creation was sent.
    pub players: Option<[EmberId; 2]>,
    pub attempt: u32,
    pub match_id: Option<String>,
    pub progress: Progress,
}

impl Tracked {
    /// Whether a result for `attempt` is about the set's current match. A
    /// waiting attempt counts: its match may exist without being recorded,
    /// if the connector stopped between creating and saving it.
    pub fn is_current(&self, attempt: u32) -> bool {
        self.attempt == attempt && matches!(self.progress, Progress::Waiting | Progress::Live)
    }

    pub fn entry_ids(&self) -> [&str; 2] {
        [&self.entries[0], &self.entries[1]]
    }
}

/// A match result, as received from the bridge or read back from it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Received {
    pub id: ExternalId,
    pub match_id: String,
    /// The match's players, by slot.
    pub players: [EmberId; 2],
    pub ended: Ended,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Ended {
    /// Games won by slot, and the winning slot.
    Completed { games: [u8; 2], winner: usize },
    /// Cancelled: play it again.
    Restart,
    /// Nobody played it in time.
    Expired,
}

/// A result body as the connector reads it: the fields it uses, and
/// anything else ignored, so a field a later bridge adds does not make
/// every result unreadable. The signature still covers the whole body.
#[derive(Debug, Deserialize)]
pub struct ResultView {
    #[serde(rename = "type")]
    pub kind: String,
    pub connection_id: String,
    pub match_id: String,
    pub external_match_id: String,
    pub outcome: Outcome,
    pub participants: Vec<SeatView>,
    #[serde(default)]
    pub winner_participant_id: Option<String>,
}

#[derive(Debug, Deserialize)]
pub struct SeatView {
    pub participant_id: String,
    pub ember_id: EmberId,
    pub slot: u8,
    /// Games won that count.
    pub score: u8,
}

impl Received {
    /// A result body's meaning, or `None` when it is not two participants in
    /// slots 0 and 1 with a winner among them exactly when it completed.
    pub fn from_result(result: &ResultView) -> Option<Self> {
        let id = ExternalId::parse(&result.external_match_id)?;
        let mut participants: Vec<_> = result.participants.iter().collect();
        participants.sort_by_key(|participant| participant.slot);
        let [one, two] = participants.as_slice() else {
            return None;
        };
        if [one.slot, two.slot] != [0, 1] {
            return None;
        }
        let winner = result.winner_participant_id.as_deref().map(|winner| {
            [one, two]
                .iter()
                .position(|participant| participant.participant_id == winner)
        });
        let ended = match (result.outcome, winner) {
            (Outcome::Completed, Some(Some(winner))) => Ended::Completed {
                games: [one.score, two.score],
                winner,
            },
            (Outcome::Restart, None) => Ended::Restart,
            (Outcome::Expired, None) => Ended::Expired,
            _ => return None,
        };
        Some(Self {
            id,
            match_id: result.match_id.clone(),
            players: [one.ember_id.clone(), two.ember_id.clone()],
            ended,
        })
    }
}

/// What a site's set says about a tracked one, judged only from the set
/// itself: never from its absence in a list.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Seen {
    /// Open, with the same two entries and no result.
    Unchanged,
    /// Decided with the same two entries; the winner's index when it is one
    /// of them.
    Decided { winner: Option<usize> },
    /// Other entries, waiting again, or removed.
    Changed,
}

pub fn seen(set: Option<&Set>, entries: &[String; 2]) -> Seen {
    let Some(set) = set.filter(|set| set.entry_ids() == Some([&entries[0], &entries[1]])) else {
        return Seen::Changed;
    };
    match set.status {
        Status::Open => Seen::Unchanged,
        Status::Decided => Seen::Decided {
            winner: set
                .winner
                .as_ref()
                .and_then(|winner| entries.iter().position(|entry| entry == winner)),
        },
        Status::Pending => Seen::Changed,
    }
}

/// Whether each game's winning slot, in the order played, adds up to the
/// score by slot.
pub fn adds_up(sequence: &[usize], games: [u8; 2]) -> bool {
    [0, 1].map(|slot| sequence.iter().filter(|&&won| won == slot).count()) == games.map(usize::from)
}

/// What a read does with a set the site lists as playable.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OnOpen {
    Leave,
    /// Create the Ember match of this attempt.
    Start(u32),
    GiveUp,
}

/// What a result does with its set.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OnResult {
    /// Report this score: games won by slot, and the winning slot.
    Report {
        games: [u8; 2],
        winner: usize,
    },
    /// The site already has this score: a report whose answer was lost.
    AlreadyReported,
    /// Play the set again as this attempt.
    Replay(u32),
    GiveUp,
    /// The organizer decided or changed the set.
    Close,
    /// The result's players are not the ones this attempt was made for.
    Mismatch,
}

fn next_attempt(attempt: u32) -> Option<u32> {
    (attempt < MAX_ATTEMPTS).then_some(attempt + 1)
}

/// A set the site lists as playable, with these entries. A live set whose
/// entries changed was cancelled before this is asked, so a live one is the
/// same set.
pub fn on_open(tracked: Option<&Tracked>, entries: [&str; 2]) -> OnOpen {
    let Some(tracked) = tracked else {
        return OnOpen::Start(1);
    };
    match tracked.progress {
        Progress::Waiting => OnOpen::Start(tracked.attempt),
        Progress::Live | Progress::Stuck => OnOpen::Leave,
        // Open again, after a report or the organizer's decision, for the
        // same two players: the organizer reopened it to correct it, and
        // decides it (on Challonge, reopening a set reopens the sets after
        // it too). Ember plays it again only for other players. A set whose
        // match ended because the set changed is `Waiting` instead, and is
        // played again whoever its players are.
        Progress::Reported | Progress::Closed if tracked.entry_ids() == entries => OnOpen::Leave,
        Progress::Reported | Progress::Closed => {
            next_attempt(tracked.attempt).map_or(OnOpen::GiveUp, OnOpen::Start)
        }
    }
}

/// A result for the set's current attempt, against what its set shows.
pub fn on_result(received: &Received, tracked: &Tracked, seen: Seen) -> OnResult {
    let players_match = tracked.players.as_ref() == Some(&received.players);
    match (seen, received.ended) {
        (Seen::Changed, _) => OnResult::Close,
        (Seen::Decided { winner }, Ended::Completed { winner: won, .. })
            if players_match && winner == Some(won) =>
        {
            OnResult::AlreadyReported
        }
        (Seen::Decided { .. }, _) => OnResult::Close,
        (Seen::Unchanged, Ended::Completed { games, winner }) if players_match => {
            OnResult::Report { games, winner }
        }
        (Seen::Unchanged, Ended::Completed { .. }) => OnResult::Mismatch,
        (Seen::Unchanged, Ended::Restart | Ended::Expired) => {
            next_attempt(tracked.attempt).map_or(OnResult::GiveUp, OnResult::Replay)
        }
    }
}

#[cfg(test)]
mod tests {
    use ember_protocol::partner::RESULT_TYPE;

    use super::*;
    use crate::site::Entry;

    fn player(byte: u8) -> EmberId {
        let seed = zeroize::Zeroizing::new([byte; 32]);
        ember_protocol::SigningIdentity::from_seed(&seed)
            .unwrap()
            .ember_id()
            .clone()
    }

    fn ember() -> EmberId {
        player(1)
    }

    fn tracked(progress: Progress, attempt: u32) -> Tracked {
        Tracked {
            entries: ["355".into(), "354".into()],
            players: Some([ember(), player(2)]),
            attempt,
            match_id: Some("emt_1".into()),
            progress,
        }
    }

    fn received(ended: Ended) -> Received {
        Received {
            id: ExternalId::parse("challonge:t:1001:1").unwrap(),
            match_id: "emt_1".into(),
            players: [ember(), player(2)],
            ended,
        }
    }

    fn set(players: [&str; 2], status: Status, winner: Option<&str>) -> Set {
        Set {
            id: "1001".into(),
            round: 1,
            round_label: "Round 1".into(),
            entries: Some(players.map(|id| Entry {
                participant_id: id.into(),
                accounts: Vec::new(),
            })),
            status,
            underway: false,
            winner: winner.map(str::to_owned),
        }
    }

    #[test]
    fn external_ids_round_trip() {
        let id = ExternalId {
            bracket: "challonge:ember_weekly-12".into(),
            set: "8008135".into(),
            attempt: 3,
        };
        let text = id.to_string();
        assert_eq!(text, "challonge:ember_weekly-12:8008135:3");
        assert_eq!(ExternalId::parse(&text), Some(id));
        for text in [
            "challonge:t:8008135:0",
            "challonge:t:8008135:x",
            "challonge:t::1",
            "t:8008135:1",
            "8008135:1",
            "",
        ] {
            assert_eq!(ExternalId::parse(text), None, "{text}");
        }
    }

    #[test]
    fn a_read_starts_new_sets_and_reopened_ones_and_leaves_the_rest() {
        let same = ["355", "354"];
        let other = ["355", "353"];
        assert_eq!(on_open(None, same), OnOpen::Start(1));
        let cases = [
            (Progress::Waiting, 2, same, OnOpen::Start(2)),
            (Progress::Live, 1, same, OnOpen::Leave),
            (Progress::Stuck, 5, other, OnOpen::Leave),
            // Reopened for the same players: the organizer's.
            (Progress::Reported, 1, same, OnOpen::Leave),
            (Progress::Closed, 4, same, OnOpen::Leave),
            // Open for other players: a new attempt.
            (Progress::Reported, 1, other, OnOpen::Start(2)),
            (Progress::Closed, 4, other, OnOpen::Start(5)),
            (Progress::Closed, MAX_ATTEMPTS, other, OnOpen::GiveUp),
        ];
        for (progress, attempt, entries, step) in cases {
            assert_eq!(
                on_open(Some(&tracked(progress, attempt)), entries),
                step,
                "{progress:?} {entries:?}"
            );
        }
    }

    #[test]
    fn a_set_is_judged_by_what_it_shows() {
        let entries = ["355".to_owned(), "354".to_owned()];
        let cases = [
            (
                Some(set(["355", "354"], Status::Open, None)),
                Seen::Unchanged,
            ),
            (
                Some(set(["355", "354"], Status::Decided, Some("354"))),
                Seen::Decided { winner: Some(1) },
            ),
            (
                Some(set(["355", "354"], Status::Decided, None)),
                Seen::Decided { winner: None },
            ),
            (Some(set(["355", "353"], Status::Open, None)), Seen::Changed),
            (
                Some(set(["355", "354"], Status::Pending, None)),
                Seen::Changed,
            ),
            (None, Seen::Changed),
        ];
        for (shown, expected) in cases {
            assert_eq!(seen(shown.as_ref(), &entries), expected, "{shown:?}");
        }
    }

    #[test]
    fn a_result_reports_replays_closes_or_waits_for_the_organizer() {
        let won = Ended::Completed {
            games: [2, 1],
            winner: 0,
        };
        let live = tracked(Progress::Live, 1);
        let cases = [
            (
                won,
                Seen::Unchanged,
                OnResult::Report {
                    games: [2, 1],
                    winner: 0,
                },
            ),
            (
                won,
                Seen::Decided { winner: Some(0) },
                OnResult::AlreadyReported,
            ),
            (won, Seen::Decided { winner: Some(1) }, OnResult::Close),
            (won, Seen::Decided { winner: None }, OnResult::Close),
            (won, Seen::Changed, OnResult::Close),
            (Ended::Restart, Seen::Unchanged, OnResult::Replay(2)),
            (Ended::Expired, Seen::Unchanged, OnResult::Replay(2)),
            (
                Ended::Restart,
                Seen::Decided { winner: Some(0) },
                OnResult::Close,
            ),
            (Ended::Expired, Seen::Changed, OnResult::Close),
        ];
        for (ended, shown, expected) in cases {
            assert_eq!(
                on_result(&received(ended), &live, shown),
                expected,
                "{ended:?} {shown:?}"
            );
        }
        let last = tracked(Progress::Live, MAX_ATTEMPTS);
        assert_eq!(
            on_result(&received(Ended::Restart), &last, Seen::Unchanged),
            OnResult::GiveUp
        );
        // Another pair of players, or none recorded, is never reported.
        let mut other = live.clone();
        other.players = None;
        assert_eq!(
            on_result(&received(won), &other, Seen::Unchanged),
            OnResult::Mismatch
        );
        assert_eq!(
            on_result(&received(won), &other, Seen::Decided { winner: Some(0) }),
            OnResult::Close
        );
    }

    #[test]
    fn games_add_up_to_the_score_or_not() {
        assert!(adds_up(&[1, 0, 1], [1, 2]));
        assert!(adds_up(&[], [0, 0]));
        assert!(!adds_up(&[1, 1], [1, 2]));
        assert!(!adds_up(&[0, 1, 1], [2, 1]));
    }

    #[test]
    fn only_the_current_attempt_is_current() {
        assert!(tracked(Progress::Live, 2).is_current(2));
        assert!(tracked(Progress::Waiting, 2).is_current(2));
        assert!(!tracked(Progress::Live, 2).is_current(1));
        for progress in [Progress::Reported, Progress::Closed, Progress::Stuck] {
            assert!(!tracked(progress, 2).is_current(2), "{progress:?}");
        }
        for progress in ["waiting", "live", "reported", "closed", "stuck"] {
            assert_eq!(Progress::parse(progress).unwrap().as_str(), progress);
        }
        assert_eq!(Progress::parse("open"), None);
    }

    #[test]
    fn a_result_body_is_read_past_fields_a_later_bridge_adds() {
        let body = serde_json::json!({
            "type": RESULT_TYPE, "bridge_id": "brg_x", "connection_id": "brackets",
            "match_id": "emt_1", "external_match_id": "challonge:t:1001:1",
            "outcome": "completed", "revision": "5", "ended_at": "2026-10-18T00:00:00Z",
            "participants": [
                { "participant_id": "epl_a", "ember_id": ember(), "slot": 0, "score": 2, "rating": 1500 },
                { "participant_id": "epl_b", "ember_id": player(2), "slot": 1, "score": 0 },
            ],
            "winner_participant_id": "epl_a",
        });
        let result: ResultView = serde_json::from_value(body).unwrap();
        assert_eq!(
            Received::from_result(&result).unwrap().ended,
            Ended::Completed {
                games: [2, 0],
                winner: 0
            }
        );
    }

    #[test]
    fn a_result_body_is_read_by_slot_once() {
        let second = player(2);
        let mut result = ResultView {
            kind: RESULT_TYPE.into(),
            connection_id: "brackets".into(),
            match_id: "emt_1".into(),
            external_match_id: "challonge:t:1001:2".into(),
            outcome: Outcome::Completed,
            // Listed slot 1 first: order on the wire does not matter.
            participants: vec![
                SeatView {
                    participant_id: "epl_b".into(),
                    ember_id: second.clone(),
                    slot: 1,
                    score: 2,
                },
                SeatView {
                    participant_id: "epl_a".into(),
                    ember_id: ember(),
                    slot: 0,
                    score: 1,
                },
            ],
            winner_participant_id: Some("epl_b".into()),
        };
        let read = Received::from_result(&result).unwrap();
        assert_eq!(read.id.attempt, 2);
        assert_eq!(read.players, [ember(), second]);
        assert_eq!(
            read.ended,
            Ended::Completed {
                games: [1, 2],
                winner: 1
            }
        );

        // A winner that is not a participant, a winner on a restart, no
        // winner on a completion, or slots other than 0 and 1.
        result.winner_participant_id = Some("epl_c".into());
        assert_eq!(Received::from_result(&result), None);
        result.outcome = Outcome::Restart;
        result.winner_participant_id = Some("epl_a".into());
        assert_eq!(Received::from_result(&result), None);
        result.winner_participant_id = None;
        assert_eq!(
            Received::from_result(&result).unwrap().ended,
            Ended::Restart
        );
        result.outcome = Outcome::Completed;
        assert_eq!(Received::from_result(&result), None);
        result.outcome = Outcome::Restart;
        result.participants[0].slot = 2;
        assert_eq!(Received::from_result(&result), None);
    }
}
