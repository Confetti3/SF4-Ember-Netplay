//! What a platform gets beyond the generic API when its connection asks for
//! it (`docs/design/INTEGRATION_PATHS.md`): finding players by their
//! connected Discord account or sign-in site account, and each finished
//! match's result sent to it.
use serde::{Deserialize, Serialize};

use crate::{
    EmberId,
    sites::{FoundAccount, SiteAccount},
};

pub const LOOKUP_PATH: &str = "/v1/players/lookup";
/// Capabilities features: the bridge has the lookup route, and sends results
/// to a connection's `results_url`.
pub const LOOKUP_FEATURE: &str = "players.lookup";
pub const RESULTS_FEATURE: &str = "results.signed";
/// Accounts one lookup may name, Discord's and sites' together.
pub const MAX_LOOKUP: usize = 32;

/// `POST /v1/players/lookup`. Each list needs its connection's setting
/// (`discord_lookup`; `lookup_sites` for each site in `accounts`). A list
/// left out or empty asks nothing.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Lookup {
    #[serde(default)]
    pub discord: Vec<String>,
    /// Accounts on sign-in sites, each named the way its site is looked up
    /// (`sites::SignInSite::lookup_by`).
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub accounts: Vec<SiteAccount>,
}

/// The players found, in the order asked. An account with no connected
/// Ember ID, or whose link the player removed, is left out.
///
/// Its readers refuse fields they do not know (`deny_unknown_fields`, as
/// ember-brackets reads it), so a field added later must be left out while
/// empty, as `accounts` is, or every older reader stops matching players.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct LookupAnswer {
    pub players: Vec<FoundPlayer>,
    /// Left out when empty, so an answer to a Discord-only lookup is what it
    /// was before `accounts` existed.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub accounts: Vec<FoundAccount>,
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
    fn a_lookup_names_its_lists() {
        let parse = |text: &str| serde_json::from_str::<Lookup>(text);
        assert_eq!(parse("{}").unwrap(), Lookup::default());
        assert_eq!(parse(r#"{"discord": []}"#).unwrap(), Lookup::default());
        for refused in [
            r#"{"discord": null}"#,
            r#"{"discord": [], "accounts": null}"#,
            r#"{"discord": [], "challonge": []}"#,
            r#"{"accounts": [{"site": "startgg", "id": "1", "extra": 1}]}"#,
        ] {
            assert!(parse(refused).is_err(), "{refused}");
        }
        let sites = parse(r#"{"accounts": [{"site": "startgg", "id": "1234"}]}"#).unwrap();
        assert_eq!(
            sites.accounts,
            [SiteAccount {
                site: "startgg".into(),
                id: "1234".into()
            }]
        );
        // What a Discord-only client sends always names `discord`, and reads
        // as it did before `accounts` existed.
        let sent = serde_json::to_string(&Lookup::default()).unwrap();
        assert_eq!(sent, r#"{"discord":[]}"#);
        assert_eq!(parse(&sent).unwrap(), Lookup::default());
        let answer = LookupAnswer {
            players: Vec::new(),
            accounts: Vec::new(),
        };
        assert_eq!(serde_json::to_string(&answer).unwrap(), r#"{"players":[]}"#);
    }
}
