//! What an event is announced as, shared by Discord and Twitch.
use std::collections::BTreeMap;

use ember_protocol::{
    EmberId,
    event::{
        Event, EventScore, GameConfirmedData, Kind, LobbyCreatedData, LobbySetCompletedData,
        MatchCreatedData, RoomEventData, ScoreChangedData, TournamentCompletedData,
        TournamentCreatedData, TournamentMatchCompletedData, TournamentStartedData,
    },
    lobby::Rotation,
    report::Outcome,
    rooms::{CLOSED_BY_CONNECTION, ENDED},
    tournament::Format,
};
use rusqlite::OptionalExtension;
use serde::Deserialize;

use crate::Notifier;

/// The match, lobby or tournament an event belongs to, for the kinds whose
/// announcement needs nothing else.
#[derive(Debug, Default, Deserialize)]
struct Ids {
    #[serde(default)]
    match_id: String,
    #[serde(default)]
    lobby_id: Option<String>,
    #[serde(default)]
    tournament_id: Option<String>,
}

/// What the announcement of `match.completed` reads. `MatchCompleted`
/// itself refuses unknown keys, so it is not used to read the event.
#[derive(Debug, Deserialize)]
struct MatchCompletedNews {
    winner_id: EmberId,
    #[serde(default)]
    scores: Vec<EventScore>,
}

impl Notifier {
    fn name(&self, id: &str) -> String {
        self.0.config.names.get(id).cloned().unwrap_or_else(|| {
            EmberId::parse(id).map_or_else(|_| "Unknown player".into(), |id| id.fingerprint())
        })
    }

    fn label(&self, match_id: &str) -> String {
        self.0
            .db
            .lock()
            .ok()
            .and_then(|db| {
                db.query_row(
                    "SELECT label FROM matches WHERE id = ?1",
                    [match_id],
                    |row| row.get(0),
                )
                .optional()
                .ok()
                .flatten()
            })
            .unwrap_or_else(|| "Match".into())
    }

    fn scores(&self, scores: &[EventScore]) -> String {
        scores
            .iter()
            .map(|score| format!("{} {}", self.name(score.ember_id.as_str()), score.wins))
            .collect::<Vec<_>>()
            .join(" – ")
    }

    /// The announcement for an event, or `None` for events not announced.
    pub fn render(&self, event: &Event) -> Option<Message> {
        let kind = event.kind()?;
        let ids: Ids = event.data_as().unwrap_or_default();
        let match_id = ids.match_id.as_str();
        let (title, text, color) = match kind {
            Kind::MatchCreated => {
                let data: MatchCreatedData = event.data_as()?;
                let names: Vec<String> = data
                    .participants
                    .iter()
                    .map(|p| self.name(p.ember_id.as_str()))
                    .collect();
                (
                    match_label(&data.metadata),
                    format!(
                        "{} vs {}, first to {}",
                        names.first()?,
                        names.get(1)?,
                        data.games_to_win
                    ),
                    0x5865F2,
                )
            }
            Kind::ScoreChanged => {
                let data: ScoreChangedData = event.data_as()?;
                (self.label(match_id), self.scores(&data.scores), 0x3BA55C)
            }
            Kind::GameConfirmed
                if event
                    .data_as::<GameConfirmedData>()
                    .and_then(|data| data.result())
                    == Some(Outcome::Draw) =>
            {
                (
                    self.label(match_id),
                    "Draw. The game is replayed.".into(),
                    0x99AAB5,
                )
            }
            Kind::MatchCompleted => {
                let data: MatchCompletedNews = event.data_as()?;
                let winner = self.name(data.winner_id.as_str());
                (
                    self.label(match_id),
                    format!("{winner} wins the set. {}", self.scores(&data.scores)),
                    0xF1C40F,
                )
            }
            Kind::MatchCancelled => (self.label(match_id), "Match cancelled.".into(), 0xED4245),
            Kind::MatchExpired => (
                self.label(match_id),
                "Match expired, not played.".into(),
                0x99AAB5,
            ),
            Kind::NeedsReview => (
                self.label(match_id),
                "Waiting for an organizer to review the result.".into(),
                0xFAA61A,
            ),
            Kind::MatchCorrected => (
                self.label(match_id),
                "The organizer corrected the result.".into(),
                0xFAA61A,
            ),
            Kind::TournamentCreated => {
                let data: TournamentCreatedData = event.data_as()?;
                let first_to = data.games_to_win;
                let finals = data
                    .finals_games_to_win
                    .filter(|finals| *finals != first_to)
                    .map(|finals| format!(", finals first to {finals}"))
                    .unwrap_or_default();
                (
                    title_of(&data.metadata, "Tournament").to_owned(),
                    format!(
                        "Registration is open: {}, first to {first_to}{finals}.",
                        format_name(data.format)
                    ),
                    0x5865F2,
                )
            }
            Kind::TournamentStarted => {
                let data: TournamentStartedData = event.data_as()?;
                (
                    self.label(data.tournament_id.as_deref()?),
                    format!(
                        "The bracket is set: {} players, {}.",
                        data.entrants.len(),
                        format_name(data.format)
                    ),
                    0x5865F2,
                )
            }
            // The set's own match events announce who played and the score;
            // this adds where the players go.
            Kind::TournamentMatchCompleted => {
                let data: TournamentMatchCompletedData = event.data_as()?;
                let tournament_id = data.tournament_id.as_deref()?;
                let winner = self.name(data.winner.as_ref()?.ember_id.as_str());
                let label = data.label.as_deref().unwrap_or("the set");
                let mut text = match (data.walkover, &data.winner_next) {
                    (true, Some(next)) => format!("{winner} advances to {next} by walkover."),
                    (true, None) => format!("{winner} wins {label} by walkover."),
                    (false, Some(next)) => format!("{winner} advances to {next}."),
                    (false, None) => format!("{winner} wins {label}."),
                };
                if let Some(loser) = &data.loser {
                    let loser = self.name(loser.ember_id.as_str());
                    if data.eliminated.is_some() {
                        text.push_str(&format!(" {loser} is out."));
                    } else if let Some(Some(next)) = &data.loser_next {
                        text.push_str(&format!(" {loser} drops to {next}."));
                    }
                }
                (self.label(tournament_id), text, 0xF1C40F)
            }
            Kind::TournamentCompleted => {
                let data: TournamentCompletedData = event.data_as()?;
                let tournament_id = data.tournament_id.as_deref()?;
                let mut places: BTreeMap<u32, Vec<String>> = BTreeMap::new();
                for entry in data.placements.iter().filter(|entry| entry.placement <= 3) {
                    places
                        .entry(entry.placement)
                        .or_default()
                        .push(self.name(entry.ember_id.as_str()));
                }
                let text = places
                    .iter()
                    .map(|(place, names)| {
                        let label = match place {
                            1 => "Champion".to_owned(),
                            2 => "2nd".to_owned(),
                            _ => "3rd".to_owned(),
                        };
                        format!("{label}: {}.", names.join(" and "))
                    })
                    .collect::<Vec<_>>()
                    .join(" ");
                (self.label(tournament_id), text, 0xF1C40F)
            }
            Kind::TournamentCancelled => (
                self.label(ids.tournament_id.as_deref()?),
                "Tournament cancelled.".into(),
                0xED4245,
            ),
            Kind::LobbyCreated => {
                let data: LobbyCreatedData = event.data_as()?;
                let rotation = match data.rotation {
                    Rotation::WinnerStays => "winner stays",
                    Rotation::LoserStays => "loser stays",
                    Rotation::BothRotate => "both players rotate",
                };
                (
                    title_of(&data.metadata, "Lobby").to_owned(),
                    format!(
                        "Lobby open: first to {}, {rotation}. Ask to join the queue to play.",
                        data.games_to_win
                    ),
                    0x5865F2,
                )
            }
            // The set's own match events announce who plays and the score;
            // this adds what the rotation did.
            Kind::LobbySetCompleted => {
                let data: LobbySetCompletedData = event.data_as()?;
                let winner = self.name(data.winner_id.as_str());
                let streak = data
                    .streak
                    .as_ref()
                    .map(|streak| streak.sets)
                    .filter(|sets| *sets >= 2)
                    .map(|sets| format!(", {sets} sets in a row"))
                    .unwrap_or_default();
                let next: Vec<String> = data
                    .queue
                    .iter()
                    .take(3)
                    .map(|id| self.name(id.as_str()))
                    .collect();
                let queue = if next.is_empty() {
                    "Nobody is waiting in the queue.".to_owned()
                } else {
                    format!("Next in the queue: {}.", next.join(", "))
                };
                (
                    self.label(&data.lobby_id),
                    format!("{winner} wins the set{streak}. {queue}"),
                    0xF1C40F,
                )
            }
            Kind::LobbyClosed => (
                self.label(ids.lobby_id.as_deref()?),
                "Lobby closed.".into(),
                0x99AAB5,
            ),
            Kind::RoomCreated | Kind::RoomOpened | Kind::RoomClosed => {
                return self.render_room(kind, event.data_as()?);
            }
            _ => return None,
        };
        Some(Message { title, text, color })
    }

    /// A room a connection opened: created, open to anyone, or closed. The
    /// data is the room as the connection sees it, plus `reason` when it
    /// closed. A closed room's link is no longer worth posting.
    fn render_room(&self, kind: Kind, data: RoomEventData) -> Option<Message> {
        let room = data.room;
        let players = format!("{}/{} players", room.room.members, room.room.capacity);
        let (text, color) = match kind {
            Kind::RoomCreated => (
                format!(
                    "{} opened a room, {players}. Join: {}",
                    self.name(room.creator_ember_id.as_str()),
                    room.join_url
                ),
                0x5865F2,
            ),
            Kind::RoomOpened => (
                format!("The room is open, {players}. Join: {}", room.join_url),
                0x3BA55C,
            ),
            _ => (
                match data.reason.as_deref() {
                    Some(CLOSED_BY_CONNECTION) => "The room was closed by its organizer.",
                    Some(ENDED) => "The room ended.",
                    _ => "The room is closed.",
                }
                .to_owned(),
                0x99AAB5,
            ),
        };
        Some(Message {
            title: room.room.name,
            text,
            color,
        })
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Message {
    pub title: String,
    pub text: String,
    pub color: u32,
}

/// A match's display label: its round label (after its tournament's title
/// for a bracket set), or its lobby and set number.
pub(crate) fn match_label(metadata: &BTreeMap<String, String>) -> String {
    let field = |key: &str| metadata.get(key).map(String::as_str);
    match (
        field("round_label"),
        field("lobby_set"),
        field("tournament_node"),
    ) {
        (Some(label), _, Some(_)) => match field("title") {
            Some(title) => format!("{title}, {label}"),
            None => label.to_owned(),
        },
        (Some(label), _, None) => label.to_owned(),
        (None, Some(set), _) => format!("{}, set {set}", field("title").unwrap_or("Lobby")),
        (None, None, _) => "Match".to_owned(),
    }
}

pub(crate) fn title_of<'a>(metadata: &'a BTreeMap<String, String>, fallback: &'a str) -> &'a str {
    metadata.get("title").map_or(fallback, String::as_str)
}

fn format_name(format: Option<Format>) -> &'static str {
    match format {
        Some(Format::DoubleElimination) => "double elimination",
        Some(Format::RoundRobin) => "round robin",
        Some(Format::SingleElimination) | None => "single elimination",
    }
}

/// Keeps names and labels from being read as Discord formatting.
pub(crate) fn escape_markdown(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for ch in text.chars() {
        if matches!(
            ch,
            '\\' | '*' | '_' | '~' | '`' | '|' | '>' | '[' | ']' | '(' | ')' | '#' | '@'
        ) {
            out.push('\\');
        }
        out.push(ch);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn markdown_is_escaped() {
        assert_eq!(
            escape_markdown("**Kate** @everyone"),
            "\\*\\*Kate\\*\\* \\@everyone"
        );
    }
}
