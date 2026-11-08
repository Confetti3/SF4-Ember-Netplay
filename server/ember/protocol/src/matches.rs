//! Provider-neutral match objects (spec 13).
use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};

use crate::{
    EmberId, Error, Result,
    encoding::{Counter, is_prefixed_id},
    wire_enum::wire_enum,
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

wire_enum! {
    serde
    /// The game an organised match, lobby or tournament is for: the games of
    /// `rooms::GAMES`, by the same keys (a test keeps them equal). A command
    /// names it as a string, so an unknown one is `game`'s `invalid_request`
    /// as it always was; this is what the string is once checked.
    #[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, Serialize, Deserialize)]
    pub enum Game {
        /// Ultra Street Fighter IV, SF4 Ember's: every command from before
        /// games existed, and the game of a configuration that names none.
        #[default]
        Usf4 = "usf4",
        /// Street Fighter V, SFV Ember's.
        Sfv = "sfv",
    }
}

/// The `edition_policy` a game with no editions to choose is written with
/// (`Rules::standard`). Such a game does not read the field.
pub const NO_EDITION_POLICY: &str = "not_applicable";

impl Game {
    /// The game a command names, or `game`'s `InvalidField`.
    pub fn of(game: &str) -> Result<Self> {
        Self::parse(game).ok_or(Error::InvalidField("game"))
    }

    /// The edition a set of this game must be played in, where the game has
    /// editions: USF4's sets are Ultra only. SFV has one edition, so its
    /// sets are asked none and their `edition_policy` is not read.
    pub fn edition_policy(self) -> Option<&'static str> {
        match self {
            Self::Usf4 => Some("ultra_only"),
            Self::Sfv => None,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Participant {
    pub participant_id: String,
    pub ember_id: EmberId,
    pub slot: u8,
}

/// Set rules. Only the named policies below exist in v1; anything else is
/// `unsupported_rules`, never an approximation. `edition_policy` is the
/// game's own (`Game::edition_policy`) and is not read for a game without one.
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
    /// A first-to-`games_to_win` set of `game` under `native_rules_profile`,
    /// with the only policies v1 has. USF4's are what they were before games
    /// existed, to the byte, since every binding carries their digest.
    pub fn standard(game: Game, games_to_win: u8, native_rules_profile: &str) -> Self {
        Self {
            games_to_win,
            draw_policy: "replay_no_score".into(),
            native_rules_profile: native_rules_profile.into(),
            edition_policy: game.edition_policy().unwrap_or(NO_EDITION_POLICY).into(),
            character_policy: "unrestricted_between_games".into(),
            stage_policy: "p1_selects".into(),
            input_delay_policy: "ember_existing_ready_policy".into(),
        }
    }

    /// The rules of a set of `game`.
    pub fn check(&self, game: Game) -> Result<()> {
        let edition_valid = game
            .edition_policy()
            .is_none_or(|policy| self.edition_policy == policy);
        let valid = [
            (games_to_win_valid(self.games_to_win), "games_to_win"),
            (self.draw_policy == "replay_no_score", "draw_policy"),
            (
                !self.native_rules_profile.is_empty() && self.native_rules_profile.len() <= 64,
                "native_rules_profile",
            ),
            (edition_valid, "edition_policy"),
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
        let game = check_provider_head(&self.external_match_id, "external_match_id", &self.game)?;
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
        self.rules.check(game)?;
        if self.observer_policy != "authorized_only" {
            return Err(Error::InvalidField("observer_policy"));
        }
        if self.result_policy != "two_player_agreement_or_review" {
            return Err(Error::InvalidField("result_policy"));
        }
        check_provider_tail(&self.required_build_id, &self.metadata)
    }

    /// Participants ordered by slot.
    pub fn by_slot(&self) -> [&Participant; 2] {
        let [a, b] = [&self.participants[0], &self.participants[1]];
        if a.slot == 0 { [a, b] } else { [b, a] }
    }
}

/// The first two checks of every provider create command (a match, a lobby
/// or a tournament): the provider's own ID for it, and the game, which is
/// returned.
pub(crate) fn check_provider_head(
    external_id: &str,
    external_field: &'static str,
    game: &str,
) -> Result<Game> {
    if external_id.is_empty() || external_id.len() > MAX_EXTERNAL_ID {
        return Err(Error::InvalidField(external_field));
    }
    Game::of(game)
}

/// The last two checks of every provider create command: the required build,
/// by length only because it is the provider's label and the bridge never
/// compares it with a build, then the metadata.
pub(crate) fn check_provider_tail(
    required_build_id: &str,
    metadata: &BTreeMap<String, String>,
) -> Result<()> {
    if required_build_id.is_empty() || required_build_id.len() > 128 {
        return Err(Error::InvalidField("required_build_id"));
    }
    check_metadata(metadata)
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

wire_enum! {
    serde
    /// Set-level lifecycle (spec 17.1). This branch reaches only the states that
    /// need no native integration; the rest are named so the wire form is fixed.
    #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
    pub enum MatchState {
        Created = "created",
        AwaitingPlayers = "awaiting_players",
        Provisioning = "provisioning",
        Ready = "ready",
        Running = "running",
        BetweenGames = "between_games",
        AwaitingReports = "awaiting_reports",
        NeedsReview = "needs_review",
        Completed = "completed",
        Cancelled = "cancelled",
        Failed = "failed",
        /// Nobody played it before its `expires_at`. Unlike `cancelled`, nothing
        /// called it off, and it is not a request to play it again.
        Expired = "expired",
    }
}

impl MatchState {
    pub fn is_terminal(self) -> bool {
        matches!(
            self,
            Self::Completed | Self::Cancelled | Self::Failed | Self::Expired
        )
    }
}

wire_enum! {
    serde
    #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
    pub enum DeliveryState {
        NotRequired = "not_required",
        Queued = "queued",
        Delivering = "delivering",
        Delivered = "delivered",
        Retrying = "retrying",
        Failed = "failed",
        Ambiguous = "ambiguous",
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
    /// Wins each slot had when the set started, P1 first, counted in
    /// `scores` but won in an earlier match its platform cut short. Left out
    /// at 0-0.
    #[serde(default, skip_serializing_if = "starts_at_zero")]
    pub starting_wins: [u8; 2],
}

/// Whether a set starts from 0-0, which leaves `starting_wins` out of its
/// wire form.
pub(crate) fn starts_at_zero(wins: &[u8; 2]) -> bool {
    *wins == [0, 0]
}

impl MatchCompleted {
    /// One winner at exactly N, the other below N, and one accepted attempt
    /// per counted win past the starting wins.
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
            && self.starting_wins.iter().all(|&wins| wins < n)
            && a.wins >= self.starting_wins[0]
            && b.wins >= self.starting_wins[1]
            && usize::from(a.wins + b.wins - self.starting_wins[0] - self.starting_wins[1])
                == self.accepted_attempt_ids.len()
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{play::PROFILE, rooms};

    /// The specification's example create command, for `game` and with
    /// `edition_policy`.
    fn command(game: &str, edition_policy: &str) -> CreateMatch {
        let mut command: CreateMatch = crate::json::parse_as(
            include_bytes!("../../../../docs/design/identity-bridge/examples/create-match.json"),
            crate::json::MAX_BODY,
        )
        .unwrap();
        command.game = game.into();
        command.rules.edition_policy = edition_policy.into();
        command
    }

    #[test]
    fn the_games_of_matches_are_the_games_of_rooms() {
        assert_eq!(Game::ALL.map(Game::as_str), rooms::GAMES);
        assert_eq!(Game::default().as_str(), rooms::DEFAULT_GAME);
        for game in Game::ALL {
            assert_eq!(Game::of(game.as_str()), Ok(game));
        }
        for other in ["", "USF4", "sf4", "sf6"] {
            assert_eq!(Game::of(other), Err(Error::InvalidField("game")));
        }
    }

    #[test]
    fn usf4s_standard_rules_are_what_they_were_before_games() {
        // Every binding, permit and report carries the digest of these.
        assert_eq!(
            serde_json::to_string(&Rules::standard(Game::Usf4, 2, PROFILE)).unwrap(),
            format!(
                r#"{{"games_to_win":2,"draw_policy":"replay_no_score","native_rules_profile":"{PROFILE}","edition_policy":"ultra_only","character_policy":"unrestricted_between_games","stage_policy":"p1_selects","input_delay_policy":"ember_existing_ready_policy"}}"#
            )
        );
        let sfv = Rules::standard(Game::Sfv, 2, PROFILE);
        assert_eq!(sfv.edition_policy, NO_EDITION_POLICY);
        assert_eq!(sfv.check(Game::Sfv), Ok(()));
    }

    #[test]
    fn an_edition_is_asked_of_usf4_alone() {
        assert_eq!(command("usf4", "ultra_only").check(), Ok(()));
        assert_eq!(
            command("usf4", NO_EDITION_POLICY).check(),
            Err(Error::InvalidField("edition_policy"))
        );
        for edition in ["ultra_only", NO_EDITION_POLICY, "anything"] {
            assert_eq!(command("sfv", edition).check(), Ok(()), "{edition}");
        }
        for other in ["", "sf6", "USF4"] {
            assert_eq!(
                command(other, "ultra_only").check(),
                Err(Error::InvalidField("game"))
            );
        }
    }
}
