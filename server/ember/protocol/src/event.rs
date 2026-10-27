//! CloudEvents 1.0 envelope for bridge events (spec 20.2), and the typed
//! data of the kinds a service announces.
use std::collections::BTreeMap;

use serde::{Deserialize, Deserializer, Serialize, de::DeserializeOwned};

use crate::{
    EmberId, Error, Result,
    encoding::{Counter, OriginPolicy, check_origin, is_prefixed_id},
    lobby::Rotation,
    matches::{MatchState, Resolution, Score},
    report::Outcome,
    rooms::ConnectionRoom,
    tournament::{Format, Side},
};

pub const SPEC_VERSION: &str = "1.0";
pub const NAMESPACE: &str = "io.ember.";
pub const CONTENT_TYPE: &str = "application/json";
/// `dataschema` prefix for events this bridge writes. A URN keeps the value
/// valid whatever origin the bridge is deployed at.
pub const SCHEMA_PREFIX: &str = "urn:ember:identity-tournament:v1:";
const MAX_TYPE: usize = 160;
const MAX_SUBJECT: usize = 256;

/// The frozen v1 event families. `full()` gives the wire type.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Kind {
    LinkPending,
    LinkCompleted,
    LinkRemoved,
    MatchCreated,
    AssignmentChanged,
    RoomReady,
    PlayerPresent,
    PlayerReady,
    MatchStarted,
    AttemptAuthorized,
    GameReported,
    GameConfirmed,
    ScoreChanged,
    NeedsReview,
    MatchCompleted,
    MatchCancelled,
    MatchFailed,
    MatchExpired,
    MatchCorrected,
    DeliverySucceeded,
    DeliveryFailed,
    DeliveryAmbiguous,
    LobbyCreated,
    LobbyQueueChanged,
    LobbySetStarted,
    LobbySetCompleted,
    LobbyClosed,
    TournamentCreated,
    TournamentEntrantsChanged,
    TournamentStarted,
    TournamentMatchStarted,
    TournamentMatchCompleted,
    TournamentMatchReopened,
    TournamentCompleted,
    TournamentCancelled,
    RoomCreated,
    RoomOpened,
    RoomChanged,
    RoomClosed,
}

impl Kind {
    pub const ALL: [Kind; 39] = [
        Self::LinkPending,
        Self::LinkCompleted,
        Self::LinkRemoved,
        Self::MatchCreated,
        Self::AssignmentChanged,
        Self::RoomReady,
        Self::PlayerPresent,
        Self::PlayerReady,
        Self::MatchStarted,
        Self::AttemptAuthorized,
        Self::GameReported,
        Self::GameConfirmed,
        Self::ScoreChanged,
        Self::NeedsReview,
        Self::MatchCompleted,
        Self::MatchCancelled,
        Self::MatchFailed,
        Self::MatchExpired,
        Self::MatchCorrected,
        Self::DeliverySucceeded,
        Self::DeliveryFailed,
        Self::DeliveryAmbiguous,
        Self::LobbyCreated,
        Self::LobbyQueueChanged,
        Self::LobbySetStarted,
        Self::LobbySetCompleted,
        Self::LobbyClosed,
        Self::TournamentCreated,
        Self::TournamentEntrantsChanged,
        Self::TournamentStarted,
        Self::TournamentMatchStarted,
        Self::TournamentMatchCompleted,
        Self::TournamentMatchReopened,
        Self::TournamentCompleted,
        Self::TournamentCancelled,
        Self::RoomCreated,
        Self::RoomOpened,
        Self::RoomChanged,
        Self::RoomClosed,
    ];

    /// The short family name from spec 20.2.
    pub fn name(self) -> &'static str {
        match self {
            Self::LinkPending => "identity.link.pending",
            Self::LinkCompleted => "identity.link.completed",
            Self::LinkRemoved => "identity.link.removed",
            Self::MatchCreated => "match.created",
            Self::AssignmentChanged => "match.assignment.changed",
            Self::RoomReady => "match.room.ready",
            Self::PlayerPresent => "match.player.present",
            Self::PlayerReady => "match.player.ready",
            Self::MatchStarted => "match.started",
            Self::AttemptAuthorized => "match.attempt.authorized",
            Self::GameReported => "match.game.reported",
            Self::GameConfirmed => "match.game.confirmed",
            Self::ScoreChanged => "match.score.changed",
            Self::NeedsReview => "match.needs_review",
            Self::MatchCompleted => "match.completed",
            Self::MatchCancelled => "match.cancelled",
            Self::MatchFailed => "match.failed",
            Self::MatchExpired => "match.expired",
            Self::MatchCorrected => "match.corrected",
            Self::DeliverySucceeded => "provider.delivery.succeeded",
            Self::DeliveryFailed => "provider.delivery.failed",
            Self::DeliveryAmbiguous => "provider.delivery.ambiguous",
            Self::LobbyCreated => "lobby.created",
            Self::LobbyQueueChanged => "lobby.queue.changed",
            Self::LobbySetStarted => "lobby.set.started",
            Self::LobbySetCompleted => "lobby.set.completed",
            Self::LobbyClosed => "lobby.closed",
            Self::TournamentCreated => "tournament.created",
            Self::TournamentEntrantsChanged => "tournament.entrants.changed",
            Self::TournamentStarted => "tournament.started",
            Self::TournamentMatchStarted => "tournament.match.started",
            Self::TournamentMatchCompleted => "tournament.match.completed",
            Self::TournamentMatchReopened => "tournament.match.reopened",
            Self::TournamentCompleted => "tournament.completed",
            Self::TournamentCancelled => "tournament.cancelled",
            Self::RoomCreated => "room.created",
            Self::RoomOpened => "room.opened",
            Self::RoomChanged => "room.changed",
            Self::RoomClosed => "room.closed",
        }
    }

    /// `io.ember.tournament.<name>.v1`.
    pub fn full(self) -> String {
        format!("{NAMESPACE}tournament.{}.v1", self.name())
    }

    pub fn from_full(text: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|kind| kind.full() == text)
    }

    /// Identity events are private to the linked player and the provider
    /// connection; they never go to general match subscribers.
    pub fn is_identity(self) -> bool {
        matches!(
            self,
            Self::LinkPending | Self::LinkCompleted | Self::LinkRemoved
        )
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Event {
    pub specversion: String,
    pub id: String,
    pub source: String,
    #[serde(rename = "type")]
    pub kind: String,
    pub subject: String,
    pub time: String,
    pub datacontenttype: String,
    pub dataschema: String,
    pub emberseq: Counter,
    pub data: serde_json::Map<String, serde_json::Value>,
}

impl Event {
    pub fn check(&self, policy: OriginPolicy) -> Result<()> {
        check_origin(&self.source, policy)?;
        let fields = [
            (self.specversion == SPEC_VERSION, "specversion"),
            (is_prefixed_id(&self.id, "evt"), "id"),
            (is_event_type(&self.kind), "type"),
            (
                !self.subject.is_empty() && self.subject.len() <= MAX_SUBJECT,
                "subject",
            ),
            (is_rfc3339_utc(&self.time), "time"),
            (self.datacontenttype == CONTENT_TYPE, "datacontenttype"),
            (
                self.dataschema.len() <= 512
                    && (self.dataschema.starts_with("https://")
                        || self.dataschema.starts_with("urn:")),
                "dataschema",
            ),
            (self.emberseq.is_positive(), "emberseq"),
        ];
        match fields.iter().find(|(ok, _)| !ok) {
            Some((_, field)) => Err(Error::InvalidField(field)),
            None => Ok(()),
        }
    }

    pub fn kind(&self) -> Option<Kind> {
        Kind::from_full(&self.kind)
    }

    /// The data read as one kind's typed data, or `None` when it does not
    /// have that shape. The data types ignore keys they do not know.
    pub fn data_as<T: DeserializeOwned>(&self) -> Option<T> {
        serde_json::from_value(serde_json::Value::Object(self.data.clone())).ok()
    }
}

// The typed data of the kinds a service announces. The bridge builds its
// event data from these, and readers decode them with `Event::data_as`.
//
// The wire rules they keep: a key the bridge writes as `null` is a plain
// `Option` that serializes `null`, never skipped; a key the bridge leaves out
// of some events is skipped when `None`. Events are stored JCS-canonical, so
// the order of the fields here never reaches the wire.
//
// The rules for reading: no type refuses an unknown key, so a newer bridge
// does not stop an older reader. The keys an announcement cannot do without
// are required; every other key has a default, and an enum a reader does not
// need is an `Option` so that its absence does not hide the event. Such an
// enum is also read with `lenient`, so that a value a newer bridge added
// reads as `None` rather than hiding the event; only an enum a reader does
// read (a tournament's format in its created and started events, a lobby's
// rotation in its created event) can stop an announcement. The bridge always
// writes those keys.

/// Reads a key that is absent (`None`), `null` (`Some(None)`) or a value.
fn present<'de, D, T>(deserializer: D) -> std::result::Result<Option<Option<T>>, D::Error>
where
    D: Deserializer<'de>,
    T: Deserialize<'de>,
{
    Option::<T>::deserialize(deserializer).map(Some)
}

/// Reads an optional enum no reader depends on: absent, `null` or a value
/// this build does not know all read as `None`.
fn lenient<'de, D, T>(deserializer: D) -> std::result::Result<Option<T>, D::Error>
where
    D: Deserializer<'de>,
    T: DeserializeOwned,
{
    Ok(Option::<serde_json::Value>::deserialize(deserializer)?
        .and_then(|value| serde_json::from_value(value).ok()))
}

fn first_to_one() -> u8 {
    1
}

/// A player named only by Ember ID: no account handles.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct EventPlayer {
    pub ember_id: EmberId,
}

/// One player's games won in a set.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct EventScore {
    pub ember_id: EmberId,
    pub wins: u8,
}

impl From<&Score> for EventScore {
    fn from(score: &Score) -> Self {
        Self {
            ember_id: score.ember_id.clone(),
            wins: score.wins,
        }
    }
}

/// One seat of a match's roster.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct EventParticipant {
    #[serde(default)]
    pub slot: u8,
    #[serde(default)]
    pub participant_id: String,
    pub ember_id: EmberId,
}

/// A bracket entrant and its seed, 1 first.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct EventSeed {
    pub ember_id: EmberId,
    pub seed: usize,
}

/// A tournament entrant and the place they finished, 1 first.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct EventPlacement {
    pub ember_id: EmberId,
    pub placement: u32,
}

/// Who holds a lobby's streak, as the lobby stored the ID, and for how many
/// sets.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct EventStreak {
    pub ember_id: String,
    pub sets: u64,
}

/// Data of `match.created`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct MatchCreatedData {
    #[serde(default)]
    pub match_id: String,
    #[serde(default)]
    pub external_match_id: String,
    #[serde(default)]
    pub match_revision: Counter,
    #[serde(default)]
    pub assignment_generation: Counter,
    #[serde(default, deserialize_with = "lenient")]
    pub state: Option<MatchState>,
    #[serde(default = "first_to_one")]
    pub games_to_win: u8,
    pub participants: Vec<EventParticipant>,
    #[serde(default)]
    pub metadata: BTreeMap<String, String>,
}

/// Data of `match.score.changed`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ScoreChangedData {
    #[serde(default)]
    pub match_id: String,
    #[serde(default)]
    pub match_revision: Counter,
    #[serde(default)]
    pub games_to_win: u8,
    #[serde(default)]
    pub scores: Vec<EventScore>,
}

/// Data of `match.game.confirmed`. An organizer's decision names its
/// `adjudication_id`; two agreeing fighters' reports name their `report_ids`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct GameConfirmedData {
    #[serde(default)]
    pub match_id: String,
    /// Set by the ledger when the game settles.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub match_revision: Option<Counter>,
    #[serde(default)]
    pub attempt_id: String,
    #[serde(default)]
    pub seq: u64,
    /// `p1_win`, `p2_win` or `draw`.
    #[serde(default)]
    pub outcome: String,
    /// `null` for a draw.
    #[serde(default)]
    pub winner_id: Option<EmberId>,
    #[serde(default, deserialize_with = "lenient")]
    pub resolution: Option<Resolution>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub adjudication_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub report_ids: Option<Vec<String>>,
}

impl GameConfirmedData {
    /// The outcome as the report's enum, or `None` for a name it does not
    /// have.
    pub fn result(&self) -> Option<Outcome> {
        Outcome::deserialize(
            serde::de::value::StrDeserializer::<serde::de::value::Error>::new(&self.outcome),
        )
        .ok()
    }
}

/// Data of `tournament.created`. Like every tournament event, its
/// `tournament_id` and `tournament_revision` are set by the tournament's
/// event writer.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct TournamentCreatedData {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tournament_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tournament_revision: Option<Counter>,
    #[serde(default)]
    pub external_tournament_id: String,
    #[serde(default)]
    pub format: Option<Format>,
    #[serde(default = "first_to_one")]
    pub games_to_win: u8,
    #[serde(default)]
    pub finals_games_to_win: Option<u8>,
    #[serde(default)]
    pub grand_final_reset: bool,
    #[serde(default)]
    pub metadata: BTreeMap<String, String>,
}

/// Data of `tournament.started`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct TournamentStartedData {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tournament_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tournament_revision: Option<Counter>,
    #[serde(default)]
    pub format: Option<Format>,
    #[serde(default)]
    pub entrants: Vec<EventSeed>,
    #[serde(default)]
    pub sets: usize,
}

/// Data of `tournament.match.completed`: a set was played, or a walkover
/// decided it. A walkover has no `match_id`, `scores` or `loser_next`.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct TournamentMatchCompletedData {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tournament_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tournament_revision: Option<Counter>,
    #[serde(default)]
    pub node: usize,
    #[serde(default, deserialize_with = "lenient")]
    pub side: Option<Side>,
    #[serde(default)]
    pub round: u32,
    #[serde(default)]
    pub label: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub match_id: Option<String>,
    #[serde(default)]
    pub walkover: bool,
    #[serde(default)]
    pub winner: Option<EventPlayer>,
    #[serde(default)]
    pub loser: Option<EventPlayer>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub scores: Option<Vec<EventScore>>,
    /// The player who left the tournament, or `null`.
    #[serde(default)]
    pub eliminated: Option<EmberId>,
    /// The label of the winner's next set, or `null`.
    #[serde(default)]
    pub winner_next: Option<String>,
    /// Absent for a walkover; `null` when the loser has no next set.
    #[serde(
        default,
        skip_serializing_if = "Option::is_none",
        deserialize_with = "present"
    )]
    pub loser_next: Option<Option<String>>,
}

/// Data of `tournament.completed`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct TournamentCompletedData {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tournament_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tournament_revision: Option<Counter>,
    #[serde(default, deserialize_with = "lenient")]
    pub format: Option<Format>,
    pub placements: Vec<EventPlacement>,
}

/// Data of `lobby.created`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct LobbyCreatedData {
    #[serde(default)]
    pub lobby_id: String,
    #[serde(default)]
    pub external_lobby_id: String,
    #[serde(default)]
    pub lobby_revision: Counter,
    #[serde(default = "first_to_one")]
    pub games_to_win: u8,
    pub rotation: Rotation,
    #[serde(default)]
    pub metadata: BTreeMap<String, String>,
}

/// Data of `lobby.set.completed`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct LobbySetCompletedData {
    pub lobby_id: String,
    #[serde(default)]
    pub lobby_revision: Counter,
    #[serde(default)]
    pub match_id: String,
    #[serde(default)]
    pub set: u64,
    #[serde(default)]
    pub games_to_win: u8,
    #[serde(default, deserialize_with = "lenient")]
    pub rotation: Option<Rotation>,
    pub winner_id: EmberId,
    #[serde(default)]
    pub loser_id: Option<EmberId>,
    #[serde(default)]
    pub scores: Vec<EventScore>,
    /// `null` when nobody holds a streak.
    #[serde(default)]
    pub streak: Option<EventStreak>,
    #[serde(default)]
    pub removed: Vec<EmberId>,
    #[serde(default)]
    pub queue: Vec<EmberId>,
}

/// Data of the `room.*` kinds: the room as its connection sees it, and on
/// `room.closed` why it closed.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct RoomEventData {
    #[serde(flatten)]
    pub room: ConnectionRoom,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,
}

/// `^io\.ember\.[a-z_]+(\.[a-z_]+)+\.v1$`.
pub fn is_event_type(text: &str) -> bool {
    let Some(middle) = text
        .strip_prefix(NAMESPACE)
        .and_then(|rest| rest.strip_suffix(".v1"))
    else {
        return false;
    };
    let parts: Vec<&str> = middle.split('.').collect();
    text.len() <= MAX_TYPE
        && parts.len() >= 2
        && parts.iter().all(|part| {
            !part.is_empty() && part.bytes().all(|b| b.is_ascii_lowercase() || b == b'_')
        })
}

/// `YYYY-MM-DDTHH:MM:SSZ`, the only form the bridge writes.
pub fn is_rfc3339_utc(text: &str) -> bool {
    let b = text.as_bytes();
    b.len() == 20
        && b.iter().enumerate().all(|(index, &byte)| match index {
            4 | 7 => byte == b'-',
            10 => byte == b'T',
            13 | 16 => byte == b':',
            19 => byte == b'Z',
            _ => byte.is_ascii_digit(),
        })
}

/// Formats Unix seconds as `YYYY-MM-DDTHH:MM:SSZ`.
pub fn rfc3339(unix: u64) -> String {
    let days = unix / 86_400;
    let seconds = unix % 86_400;
    // Civil-from-days, Howard Hinnant's algorithm.
    let z = days as i64 + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    format!(
        "{year:04}-{month:02}-{day:02}T{:02}:{:02}:{:02}Z",
        seconds / 3600,
        seconds / 60 % 60,
        seconds % 60
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_kind_is_a_valid_type() {
        for kind in Kind::ALL {
            assert!(is_event_type(&kind.full()), "{}", kind.full());
            assert_eq!(Kind::from_full(&kind.full()), Some(kind));
        }
        assert!(!is_event_type("io.ember.match.v1"));
        assert!(!is_event_type("io.ember.Match.done.v1"));
        assert!(!is_event_type("com.ember.match.done.v1"));
    }

    #[test]
    fn formats_utc_times() {
        assert_eq!(rfc3339(0), "1970-01-01T00:00:00Z");
        assert_eq!(rfc3339(1_790_813_520), "2026-10-01T00:12:00Z");
        assert_eq!(rfc3339(951_782_400), "2000-02-29T00:00:00Z");
        assert!(is_rfc3339_utc(&rfc3339(4_102_444_800)));
    }
}
