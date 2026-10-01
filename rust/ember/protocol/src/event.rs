//! CloudEvents 1.0 envelope for bridge events (spec 20.2).
use serde::{Deserialize, Serialize};

use crate::{
    Error, Result,
    encoding::{Counter, OriginPolicy, check_origin, is_prefixed_id},
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
}

impl Kind {
    pub const ALL: [Kind; 34] = [
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
