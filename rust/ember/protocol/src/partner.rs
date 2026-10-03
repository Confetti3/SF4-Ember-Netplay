//! What a platform gets beyond the generic API when its connection asks for
//! it (`docs/design/INTEGRATION_PATHS.md`): finding players by their
//! connected Discord account, and each finished match's result sent to it.
use serde::{Deserialize, Serialize};

use crate::EmberId;

pub const LOOKUP_PATH: &str = "/v1/players/lookup";
/// Capabilities features: the bridge has the lookup route, and sends results
/// to a connection's `results_url`.
pub const LOOKUP_FEATURE: &str = "players.lookup";
pub const RESULTS_FEATURE: &str = "results.signed";
/// Discord user IDs one lookup may name.
pub const MAX_LOOKUP: usize = 32;

/// `POST /v1/players/lookup`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Lookup {
    pub discord: Vec<String>,
}

/// The players found, in the order asked. An ID with no connected account,
/// or whose link the player removed, is left out.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct LookupAnswer {
    pub players: Vec<FoundPlayer>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FoundPlayer {
    pub discord_user_id: String,
    pub ember_id: EmberId,
    pub participant_id: String,
}

/// The `type` of a result the bridge sends to a connection's `results_url`.
pub const RESULT_TYPE: &str = "io.ember.tournament.match.result.v1";
/// `webhook-id` of a match's result: the same on every attempt.
pub fn result_id(match_id: &str) -> String {
    format!("res_{match_id}")
}

/// How a match ended, for the platform: a result, or play it again.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Outcome {
    Completed,
    Restart,
}

/// One finished match's result.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct MatchResult {
    #[serde(rename = "type")]
    pub kind: String,
    pub bridge_id: String,
    pub connection_id: String,
    pub match_id: String,
    pub external_match_id: String,
    pub outcome: Outcome,
    /// The match revision, as a decimal string.
    pub revision: String,
    pub participants: Vec<ResultParticipant>,
    /// Set when `outcome` is `completed`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub winner_participant_id: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ResultParticipant {
    pub participant_id: String,
    pub ember_id: EmberId,
    pub slot: u8,
    /// Games won that count.
    pub score: u8,
}
