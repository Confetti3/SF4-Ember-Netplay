//! What an event is announced as, shared by Discord and Twitch.
use std::collections::BTreeMap;

use ember_protocol::{
    EmberId,
    event::{Event, Kind},
    rooms::{CLOSED_BY_CONNECTION, ConnectionRoom, ENDED},
};
use rusqlite::OptionalExtension;
use serde_json::Value;

use crate::Notifier;

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

    fn scores(&self, data: &serde_json::Map<String, Value>) -> String {
        data.get("scores")
            .and_then(Value::as_array)
            .map(|scores| {
                scores
                    .iter()
                    .map(|score| {
                        format!(
                            "{} {}",
                            self.name(
                                score
                                    .get("ember_id")
                                    .and_then(Value::as_str)
                                    .unwrap_or_default()
                            ),
                            score.get("wins").and_then(Value::as_u64).unwrap_or(0)
                        )
                    })
                    .collect::<Vec<_>>()
                    .join(" – ")
            })
            .unwrap_or_default()
    }

    /// The announcement for an event, or `None` for events not announced.
    pub fn render(&self, event: &Event) -> Option<Message> {
        let data = &event.data;
        let match_id = data
            .get("match_id")
            .and_then(Value::as_str)
            .unwrap_or_default();
        let (title, text, color) = match event.kind()? {
            Kind::MatchCreated => {
                let names: Vec<String> = data
                    .get("participants")
                    .and_then(Value::as_array)?
                    .iter()
                    .map(|p| {
                        self.name(
                            p.get("ember_id")
                                .and_then(Value::as_str)
                                .unwrap_or_default(),
                        )
                    })
                    .collect();
                let label = match_label(data);
                let first_to = data
                    .get("games_to_win")
                    .and_then(Value::as_u64)
                    .unwrap_or(1);
                (
                    label,
                    format!(
                        "{} vs {}, first to {first_to}",
                        names.first()?,
                        names.get(1)?
                    ),
                    0x5865F2,
                )
            }
            Kind::ScoreChanged => (self.label(match_id), self.scores(data), 0x3BA55C),
            Kind::GameConfirmed if data.get("outcome").and_then(Value::as_str) == Some("draw") => (
                self.label(match_id),
                "Draw. The game is replayed.".into(),
                0x99AAB5,
            ),
            Kind::MatchCompleted => {
                let winner = self.name(data.get("winner_id").and_then(Value::as_str)?);
                (
                    self.label(match_id),
                    format!("{winner} wins the set. {}", self.scores(data)),
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
                let first_to = data
                    .get("games_to_win")
                    .and_then(Value::as_u64)
                    .unwrap_or(1);
                let finals = data
                    .get("finals_games_to_win")
                    .and_then(Value::as_u64)
                    .filter(|finals| *finals != first_to)
                    .map(|finals| format!(", finals first to {finals}"))
                    .unwrap_or_default();
                (
                    title_of(data, "Tournament").to_owned(),
                    format!(
                        "Registration is open: {}, first to {first_to}{finals}.",
                        format_name(data)
                    ),
                    0x5865F2,
                )
            }
            Kind::TournamentStarted => {
                let tournament_id = data.get("tournament_id").and_then(Value::as_str)?;
                let entrants = data
                    .get("entrants")
                    .and_then(Value::as_array)
                    .map_or(0, Vec::len);
                (
                    self.label(tournament_id),
                    format!(
                        "The bracket is set: {entrants} players, {}.",
                        format_name(data)
                    ),
                    0x5865F2,
                )
            }
            // The set's own match events announce who played and the score;
            // this adds where the players go.
            Kind::TournamentMatchCompleted => {
                let tournament_id = data.get("tournament_id").and_then(Value::as_str)?;
                let player = |key: &str| {
                    data.get(key)
                        .and_then(|player| player.get("ember_id"))
                        .and_then(Value::as_str)
                        .map(|id| self.name(id))
                };
                let winner = player("winner")?;
                let walkover = data.get("walkover").and_then(Value::as_bool) == Some(true);
                let field = |key: &str| data.get(key).and_then(Value::as_str);
                let mut text = match (walkover, field("winner_next")) {
                    (true, Some(next)) => format!("{winner} advances to {next} by walkover."),
                    (true, None) => format!(
                        "{winner} wins {} by walkover.",
                        field("label").unwrap_or("the set")
                    ),
                    (false, Some(next)) => format!("{winner} advances to {next}."),
                    (false, None) => {
                        format!("{winner} wins {}.", field("label").unwrap_or("the set"))
                    }
                };
                if let Some(loser) = player("loser") {
                    if data.get("eliminated").and_then(Value::as_str).is_some() {
                        text.push_str(&format!(" {loser} is out."));
                    } else if let Some(next) = field("loser_next") {
                        text.push_str(&format!(" {loser} drops to {next}."));
                    }
                }
                (self.label(tournament_id), text, 0xF1C40F)
            }
            Kind::TournamentCompleted => {
                let tournament_id = data.get("tournament_id").and_then(Value::as_str)?;
                let mut places: BTreeMap<u64, Vec<String>> = BTreeMap::new();
                for entry in data.get("placements").and_then(Value::as_array)? {
                    let place = entry.get("placement").and_then(Value::as_u64)?;
                    if place <= 3 {
                        let id = entry.get("ember_id").and_then(Value::as_str)?;
                        places.entry(place).or_default().push(self.name(id));
                    }
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
                self.label(data.get("tournament_id").and_then(Value::as_str)?),
                "Tournament cancelled.".into(),
                0xED4245,
            ),
            Kind::LobbyCreated => {
                let first_to = data
                    .get("games_to_win")
                    .and_then(Value::as_u64)
                    .unwrap_or(1);
                let rotation = match data.get("rotation").and_then(Value::as_str)? {
                    "winner_stays" => "winner stays",
                    "loser_stays" => "loser stays",
                    _ => "both players rotate",
                };
                (
                    title_of(data, "Lobby").to_owned(),
                    format!(
                        "Lobby open: first to {first_to}, {rotation}. Ask to join the queue to play."
                    ),
                    0x5865F2,
                )
            }
            // The set's own match events announce who plays and the score;
            // this adds what the rotation did.
            Kind::LobbySetCompleted => {
                let lobby_id = data.get("lobby_id").and_then(Value::as_str)?;
                let winner = self.name(data.get("winner_id").and_then(Value::as_str)?);
                let streak = data
                    .get("streak")
                    .and_then(|streak| streak.get("sets"))
                    .and_then(Value::as_u64)
                    .filter(|sets| *sets >= 2)
                    .map(|sets| format!(", {sets} sets in a row"))
                    .unwrap_or_default();
                let next: Vec<String> = data
                    .get("queue")
                    .and_then(Value::as_array)
                    .map(|queue| {
                        queue
                            .iter()
                            .take(3)
                            .filter_map(Value::as_str)
                            .map(|id| self.name(id))
                            .collect()
                    })
                    .unwrap_or_default();
                let queue = if next.is_empty() {
                    "Nobody is waiting in the queue.".to_owned()
                } else {
                    format!("Next in the queue: {}.", next.join(", "))
                };
                (
                    self.label(lobby_id),
                    format!("{winner} wins the set{streak}. {queue}"),
                    0xF1C40F,
                )
            }
            Kind::LobbyClosed => (
                self.label(data.get("lobby_id").and_then(Value::as_str)?),
                "Lobby closed.".into(),
                0x99AAB5,
            ),
            Kind::RoomCreated | Kind::RoomOpened | Kind::RoomClosed => {
                return self.render_room(event.kind()?, data);
            }
            _ => return None,
        };
        Some(Message { title, text, color })
    }

    /// A room a connection opened: created, open to anyone, or closed. The
    /// data is the room as the connection sees it, plus `reason` when it
    /// closed. A closed room's link is no longer worth posting.
    fn render_room(&self, kind: Kind, data: &serde_json::Map<String, Value>) -> Option<Message> {
        let mut data = data.clone();
        let reason = data.remove("reason");
        let room: ConnectionRoom = serde_json::from_value(Value::Object(data)).ok()?;
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
                match reason.as_ref().and_then(Value::as_str) {
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
pub(crate) fn match_label(data: &serde_json::Map<String, Value>) -> String {
    let metadata = data.get("metadata");
    let field = |key: &str| metadata.and_then(|m| m.get(key)).and_then(Value::as_str);
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

pub(crate) fn title_of<'a>(data: &'a serde_json::Map<String, Value>, fallback: &'a str) -> &'a str {
    data.get("metadata")
        .and_then(|metadata| metadata.get("title"))
        .and_then(Value::as_str)
        .unwrap_or(fallback)
}

fn format_name(data: &serde_json::Map<String, Value>) -> &'static str {
    match data.get("format").and_then(Value::as_str) {
        Some("double_elimination") => "double elimination",
        Some("round_robin") => "round robin",
        _ => "single elimination",
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
