//! What a platform gets beyond the generic API when its connection asks for
//! it (`docs/design/INTEGRATION_PATHS.md`): finding players by their
//! connected Discord or Challonge account, and each finished match's result
//! sent to it.
use serde::{Deserialize, Serialize};

use crate::EmberId;

pub const LOOKUP_PATH: &str = "/v1/players/lookup";
/// Capabilities features: the bridge has the lookup route, and sends results
/// to a connection's `results_url`.
pub const LOOKUP_FEATURE: &str = "players.lookup";
pub const RESULTS_FEATURE: &str = "results.signed";
/// Accounts one lookup may name, Discord and Challonge together.
pub const MAX_LOOKUP: usize = 32;

/// `POST /v1/players/lookup`. Each list needs its own connection setting
/// (`discord_lookup`, `challonge_lookup`); an empty list asks nothing. A
/// lookup names at least one list: `{}` is refused, as it was before
/// `challonge` existed.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(try_from = "NamedLists")]
pub struct Lookup {
    pub discord: Vec<String>,
    /// Challonge usernames, as a tournament entry shows them. Compared
    /// without case (`challonge::username_key`).
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub challonge: Vec<String>,
}

/// `Lookup` as it arrives, before it is checked to name a list. A list left
/// out is `None`; a list given as `null` is refused, as it was before
/// `challonge` existed.
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct NamedLists {
    #[serde(default, deserialize_with = "list")]
    discord: Option<Vec<String>>,
    #[serde(default, deserialize_with = "list")]
    challonge: Option<Vec<String>>,
}

/// A list that is there, which `null` is not.
fn list<'de, D: serde::Deserializer<'de>>(lists: D) -> Result<Option<Vec<String>>, D::Error> {
    Vec::deserialize(lists).map(Some)
}

impl TryFrom<NamedLists> for Lookup {
    type Error = &'static str;

    fn try_from(lists: NamedLists) -> Result<Self, Self::Error> {
        if lists.discord.is_none() && lists.challonge.is_none() {
            return Err("a lookup names discord, challonge or both");
        }
        Ok(Self {
            discord: lists.discord.unwrap_or_default(),
            challonge: lists.challonge.unwrap_or_default(),
        })
    }
}

/// The players found, in the order asked. An account with no connected
/// Ember ID, or whose link the player removed, is left out.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct LookupAnswer {
    pub players: Vec<FoundPlayer>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub challonge: Vec<FoundChallongePlayer>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FoundPlayer {
    pub discord_user_id: String,
    pub ember_id: EmberId,
    pub participant_id: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FoundChallongePlayer {
    /// The username as the lookup named it.
    pub username: String,
    pub challonge_user_id: String,
    pub ember_id: EmberId,
    pub participant_id: String,
    /// When the account connected, which is when the bridge last saw that
    /// it had this username. A connector may refuse an old one: Challonge
    /// lets an account rename, and the bridge does not see that.
    pub connected_at: u64,
}

/// The `type` of a result the bridge sends to a connection's `results_url`.
pub const RESULT_TYPE: &str = "io.ember.tournament.match.result.v1";
/// `webhook-id` of a match's result: the same on every attempt.
pub fn result_id(match_id: &str) -> String {
    format!("res_{match_id}")
}

/// How a match ended, for the platform: a result, play it again, or never
/// played before it expired. `expired` is not a request to restart; what to
/// do about it is the platform's decision.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Outcome {
    Completed,
    Restart,
    Expired,
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_lookup_names_at_least_one_list() {
        let parse = |text: &str| serde_json::from_str::<Lookup>(text);
        assert!(parse("{}").is_err());
        assert!(parse(r#"{"discord": null, "challonge": ["Kate_99"]}"#).is_err());
        assert!(parse(r#"{"discord": [], "challonge": null}"#).is_err());
        assert!(parse(r#"{"discord": [], "email": []}"#).is_err());
        assert_eq!(parse(r#"{"discord": []}"#).unwrap(), Lookup::default());
        let challonge = parse(r#"{"challonge": ["Kate_99"]}"#).unwrap();
        assert_eq!((challonge.discord.len(), challonge.challonge.len()), (0, 1));
        // What a client sends always names `discord`, so it reads back.
        let sent = serde_json::to_string(&Lookup::default()).unwrap();
        assert_eq!(sent, r#"{"discord":[]}"#);
        assert_eq!(parse(&sent).unwrap(), Lookup::default());
    }
}
