//! Provider-neutral match objects (spec 13).
use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};

use crate::{
    EmberId, Error, Result,
    encoding::{Counter, is_prefixed_id},
};

pub const MAX_EXTERNAL_ID: usize = 256;
pub const MAX_METADATA_ENTRIES: usize = 16;
pub const MAX_METADATA_VALUE: usize = 1024;
pub const MAX_METADATA_TOTAL: usize = 8 * 1024;
/// The longest set: first to 10 (best of 19).
pub const MAX_GAMES_TO_WIN: u8 = 10;

/// Whether a set of first to `games` is one Ember plays: 1 to `MAX_GAMES_TO_WIN`.
pub fn games_to_win_valid(games: u8) -> bool {
    (1..=MAX_GAMES_TO_WIN).contains(&games)
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Participant {
    pub participant_id: String,
    pub ember_id: EmberId,
    pub slot: u8,
}

/// Set rules. Only the named policies below exist in v1; anything else is
/// `unsupported_rules`, never an approximation.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Rules {
    pub games_to_win: u8,
    pub draw_policy: String,
    pub native_rules_profile: String,
    pub edition_policy: String,
    pub character_policy: String,
    pub stage_policy: String,
    pub input_delay_policy: String,
}

impl Rules {
    /// A first-to-`games_to_win` set under `native_rules_profile`, with the
    /// only policies v1 has.
    pub fn standard(games_to_win: u8, native_rules_profile: &str) -> Self {
        Self {
            games_to_win,
            draw_policy: "replay_no_score".into(),
            native_rules_profile: native_rules_profile.into(),
            edition_policy: "ultra_only".into(),
            character_policy: "unrestricted_between_games".into(),
            stage_policy: "p1_selects".into(),
            input_delay_policy: "ember_existing_ready_policy".into(),
        }
    }

    pub fn check(&self) -> Result<()> {
        let valid = [
            (games_to_win_valid(self.games_to_win), "games_to_win"),
            (self.draw_policy == "replay_no_score", "draw_policy"),
            (
                !self.native_rules_profile.is_empty() && self.native_rules_profile.len() <= 64,
                "native_rules_profile",
            ),
            (self.edition_policy == "ultra_only", "edition_policy"),
            (
                self.character_policy == "unrestricted_between_games",
                "character_policy",
            ),
            (self.stage_policy == "p1_selects", "stage_policy"),
            (
                self.input_delay_policy == "ember_existing_ready_policy",
                "input_delay_policy",
            ),
        ];
        match valid.iter().find(|(ok, _)| !ok) {
            Some((_, field)) => Err(Error::InvalidField(field)),
            None => Ok(()),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CreateMatch {
    pub external_match_id: String,
    pub game: String,
    pub participants: Vec<Participant>,
    pub rules: Rules,
    pub observer_policy: String,
    pub result_policy: String,
    pub required_build_id: String,
    pub metadata: BTreeMap<String, String>,
}

impl CreateMatch {
    /// Shape only. Whether each participant's Ember ID is that participant's
    /// current approved link is the bridge's check.
    pub fn check(&self) -> Result<()> {
        if self.external_match_id.is_empty() || self.external_match_id.len() > MAX_EXTERNAL_ID {
            return Err(Error::InvalidField("external_match_id"));
        }
        if self.game != "usf4" {
            return Err(Error::InvalidField("game"));
        }
        let [a, b] = self.participants.as_slice() else {
            return Err(Error::InvalidField("participants"));
        };
        for participant in [a, b] {
            if !is_prefixed_id(&participant.participant_id, "epl") || participant.slot > 1 {
                return Err(Error::InvalidField("participants"));
            }
        }
        if a.slot == b.slot || a.ember_id == b.ember_id || a.participant_id == b.participant_id {
            return Err(Error::Mismatch(
                "participants must be two distinct fighters",
            ));
        }
        self.rules.check()?;
        if self.observer_policy != "authorized_only" {
            return Err(Error::InvalidField("observer_policy"));
        }
        if self.result_policy != "two_player_agreement_or_review" {
            return Err(Error::InvalidField("result_policy"));
        }
        if self.required_build_id.is_empty() || self.required_build_id.len() > 128 {
            return Err(Error::InvalidField("required_build_id"));
        }
        check_metadata(&self.metadata)
    }

    /// Participants ordered by slot.
    pub fn by_slot(&self) -> [&Participant; 2] {
        let [a, b] = [&self.participants[0], &self.participants[1]];
        if a.slot == 0 { [a, b] } else { [b, a] }
    }
}

/// Display-only metadata: lowercase keys, bounded values, never policy.
pub fn check_metadata(metadata: &BTreeMap<String, String>) -> Result<()> {
    let keys_valid = metadata.keys().all(|key| {
        let bytes = key.as_bytes();
        !bytes.is_empty()
            && bytes.len() <= 64
            && bytes[0].is_ascii_lowercase()
            && bytes
                .iter()
                .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || *b == b'_')
    });
    let total: usize = metadata.iter().map(|(k, v)| k.len() + v.len()).sum();
    if metadata.len() > MAX_METADATA_ENTRIES
        || !keys_valid
        || metadata
            .values()
            .any(|value| value.len() > MAX_METADATA_VALUE)
        || total > MAX_METADATA_TOTAL
    {
        return Err(Error::InvalidField("metadata"));
    }
    Ok(())
}

/// Set-level lifecycle (spec 17.1). This branch reaches only the states that
/// need no native integration; the rest are named so the wire form is fixed.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum MatchState {
    Created,
    AwaitingPlayers,
    Provisioning,
    Ready,
    Running,
    BetweenGames,
    AwaitingReports,
    NeedsReview,
    Completed,
    Cancelled,
    Failed,
    /// Nobody played it before its `expires_at`. Unlike `cancelled`, nothing
    /// called it off, and it is not a request to play it again.
    Expired,
}

impl MatchState {
    pub fn is_terminal(self) -> bool {
        matches!(
            self,
            Self::Completed | Self::Cancelled | Self::Failed | Self::Expired
        )
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Self::Created => "created",
            Self::AwaitingPlayers => "awaiting_players",
            Self::Provisioning => "provisioning",
            Self::Ready => "ready",
            Self::Running => "running",
            Self::BetweenGames => "between_games",
            Self::AwaitingReports => "awaiting_reports",
            Self::NeedsReview => "needs_review",
            Self::Completed => "completed",
            Self::Cancelled => "cancelled",
            Self::Failed => "failed",
            Self::Expired => "expired",
        }
    }

    pub fn parse(text: &str) -> Option<Self> {
        serde_json::from_value(serde_json::Value::String(text.to_owned())).ok()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum DeliveryState {
    NotRequired,
    Queued,
    Delivering,
    Delivered,
    Retrying,
    Failed,
    Ambiguous,
}

impl DeliveryState {
    pub fn parse(text: &str) -> Option<Self> {
        serde_json::from_value(serde_json::Value::String(text.to_owned())).ok()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Resolution {
    PlayerAgreement,
    OrganizerAdjudication,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Score {
    pub ember_id: EmberId,
    pub wins: u8,
}

/// Data of `match.completed`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct MatchCompleted {
    pub match_id: String,
    pub match_revision: Counter,
    pub assignment_generation: Counter,
    pub state: MatchState,
    pub games_to_win: u8,
    pub scores: Vec<Score>,
    pub winner_id: EmberId,
    pub resolution: Resolution,
    pub accepted_attempt_ids: Vec<String>,
    pub evidence_report_ids: Vec<String>,
    pub provider_delivery_state: DeliveryState,
}

impl MatchCompleted {
    /// One winner at exactly N, the other below N, and one accepted attempt
    /// per counted win.
    pub fn check(&self) -> Result<()> {
        let [a, b] = self.scores.as_slice() else {
            return Err(Error::InvalidField("scores"));
        };
        let n = self.games_to_win;
        let (winner, loser) = if a.ember_id == self.winner_id {
            (a, b)
        } else {
            (b, a)
        };
        let consistent = self.state == MatchState::Completed
            && games_to_win_valid(n)
            && a.ember_id != b.ember_id
            && winner.ember_id == self.winner_id
            && winner.wins == n
            && loser.wins < n
            && usize::from(a.wins + b.wins) == self.accepted_attempt_ids.len()
            && is_prefixed_id(&self.match_id, "emt")
            && self.match_revision.is_positive()
            && self.assignment_generation.is_positive()
            && self
                .accepted_attempt_ids
                .iter()
                .all(|id| is_prefixed_id(id, "ega"))
            && self
                .evidence_report_ids
                .iter()
                .all(|id| is_prefixed_id(id, "rpt"));
        if consistent {
            Ok(())
        } else {
            Err(Error::Mismatch("set score"))
        }
    }
}
