//! Each bracket set's Ember match as the connector keeps it, and the pure
//! decisions about it: what a read of the bracket does with an open set, and
//! what a match result does. `bracket.rs` carries them out.
use std::fmt;

use ember_protocol::partner::{MatchResult, Outcome};

use crate::site::Report;

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
    /// Attempt `attempt` has no Ember match yet: its players are not found,
    /// or the bridge refused it for now.
    Waiting,
    /// `match_id` is being played.
    Live,
    /// The score went back to the site.
    Reported,
    /// The organizer reported, changed or removed the set on the site.
    Closed,
    /// Left to the organizer: `MAX_ATTEMPTS` ended without a result, or the
    /// site refused its score.
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

/// What to do about a set.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Step {
    Leave,
    /// Create the Ember match of this attempt.
    Start(u32),
    Report,
    Close,
    GiveUp,
}

fn next_attempt(attempt: u32) -> Step {
    if attempt < MAX_ATTEMPTS {
        Step::Start(attempt + 1)
    } else {
        Step::GiveUp
    }
}

/// A set the site lists as playable now. A live set whose entries changed
/// was cancelled before this is asked, so a live one is the same set.
pub fn on_open(tracked: Option<&Tracked>) -> Step {
    match tracked {
        None => Step::Start(1),
        Some(tracked) => match tracked.progress {
            Progress::Waiting => Step::Start(tracked.attempt),
            Progress::Live | Progress::Stuck => Step::Leave,
            // Open again without a result: the organizer reopened or reset it.
            Progress::Reported | Progress::Closed => next_attempt(tracked.attempt),
        },
    }
}

/// A result for the set's current attempt. `unchanged`: the site still has
/// the set open, with the same two entries and no result.
pub fn on_result(attempt: u32, outcome: Outcome, unchanged: bool) -> Step {
    match (unchanged, outcome) {
        (false, _) => Step::Close,
        (true, Outcome::Completed) => Step::Report,
        (true, Outcome::Restart | Outcome::Expired) => next_attempt(attempt),
    }
}

/// The score of a completed result for the site: Ember slot 0 is the set's
/// player 1. `None` when the result has no winner among its participants.
pub fn report_of(entries: &[String; 2], result: &MatchResult) -> Option<Report> {
    let mut games = [0; 2];
    let mut winner = None;
    for participant in &result.participants {
        let slot = usize::from(participant.slot);
        *games.get_mut(slot)? = participant.score;
        if result.winner_participant_id.as_deref() == Some(participant.participant_id.as_str()) {
            winner = Some(slot);
        }
    }
    Some(Report {
        entries: entries.clone(),
        games,
        winner: winner?,
    })
}

#[cfg(test)]
mod tests {
    use ember_protocol::{
        EmberId,
        partner::{RESULT_TYPE, ResultParticipant},
    };

    use super::*;

    fn tracked(progress: Progress, attempt: u32) -> Tracked {
        Tracked {
            entries: ["355".into(), "354".into()],
            attempt,
            match_id: Some("emt_1".into()),
            progress,
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
    fn a_poll_starts_new_sets_and_reopened_ones_and_leaves_the_rest() {
        assert_eq!(on_open(None), Step::Start(1));
        assert_eq!(
            on_open(Some(&tracked(Progress::Waiting, 2))),
            Step::Start(2)
        );
        assert_eq!(on_open(Some(&tracked(Progress::Live, 1))), Step::Leave);
        assert_eq!(on_open(Some(&tracked(Progress::Stuck, 5))), Step::Leave);
        assert_eq!(
            on_open(Some(&tracked(Progress::Reported, 1))),
            Step::Start(2)
        );
        assert_eq!(on_open(Some(&tracked(Progress::Closed, 4))), Step::Start(5));
        assert_eq!(
            on_open(Some(&tracked(Progress::Closed, MAX_ATTEMPTS))),
            Step::GiveUp
        );
    }

    #[test]
    fn a_result_reports_restarts_or_closes() {
        assert_eq!(on_result(1, Outcome::Completed, true), Step::Report);
        assert_eq!(on_result(1, Outcome::Restart, true), Step::Start(2));
        assert_eq!(on_result(4, Outcome::Expired, true), Step::Start(5));
        assert_eq!(
            on_result(MAX_ATTEMPTS, Outcome::Restart, true),
            Step::GiveUp
        );
        for outcome in [Outcome::Completed, Outcome::Restart, Outcome::Expired] {
            assert_eq!(on_result(1, outcome, false), Step::Close);
        }
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
    fn a_score_goes_back_by_slot() {
        let ember_id =
            EmberId::parse("emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja").unwrap();
        let mut result = MatchResult {
            kind: RESULT_TYPE.into(),
            bridge_id: "brg_x".into(),
            connection_id: "brackets".into(),
            match_id: "emt_1".into(),
            external_match_id: "challonge:t:1:1".into(),
            outcome: Outcome::Completed,
            revision: "5".into(),
            participants: vec![
                ResultParticipant {
                    participant_id: "epl_b".into(),
                    ember_id: ember_id.clone(),
                    slot: 1,
                    score: 2,
                },
                ResultParticipant {
                    participant_id: "epl_a".into(),
                    ember_id,
                    slot: 0,
                    score: 1,
                },
            ],
            winner_participant_id: Some("epl_b".into()),
        };
        let entries = ["355".to_owned(), "354".to_owned()];
        let report = report_of(&entries, &result).unwrap();
        assert_eq!((report.games, report.winner), ([1, 2], 1));
        assert_eq!(report.entries, entries);
        result.winner_participant_id = Some("epl_c".into());
        assert_eq!(report_of(&entries, &result), None);
        result.winner_participant_id = Some("epl_a".into());
        result.participants[0].slot = 2;
        assert_eq!(report_of(&entries, &result), None);
    }
}
