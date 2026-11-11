//! The bridge routes the connector calls with its connection's provider
//! credential: finding players by the Discord account a site entry names,
//! creating a set's match, reading how one ended, and cancelling one.
use std::{collections::HashMap, fmt, sync::Mutex};

use ember_protocol::{
    EmberId,
    api::ErrorCode,
    client::{BridgeClient as Client, Failure},
    json,
    matches::{CreateMatch, DeliveryState, MatchState},
    partner::{LOOKUP_PATH, Lookup, LookupAnswer, MAX_LOOKUP},
};
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use zeroize::Zeroizing;

use crate::{
    http::http_client,
    sets::Ended,
    site::Entry,
};

const MAX_ANSWER: usize = 256 * 1024;
/// How long a refusal of Discord lookups is taken as read: a change on the
/// bridge is seen within this, without a restart.
const BRIDGE_FRESH_SECS: u64 = 15 * 60;

#[derive(Debug)]
pub(crate) enum BridgeError {
    /// The bridge answered with its error shape.
    Refused { code: ErrorCode, message: String },
    /// It could not be reached, or its answer was not one the bridge sends.
    Unavailable,
}

impl fmt::Display for BridgeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Refused { code, message } => {
                write!(f, "{} ({message})", code.as_str())
            }
            Self::Unavailable => f.write_str("the bridge could not be reached"),
        }
    }
}

/// A player the bridge found, linked on the connection.
#[derive(Clone, Debug)]
pub(crate) struct Player {
    pub participant_id: String,
    pub ember_id: EmberId,
}

/// A match that is over, as the bridge records it.
pub(crate) struct Over {
    /// Its players, by slot.
    pub players: [EmberId; 2],
    pub ended: Ended,
    /// How the bridge's delivery of its result is going.
    pub delivery: DeliveryState,
}

/// The players found for a lookup's Discord accounts, by user ID, and
/// whether the connection may not look Discord accounts up at all.
#[derive(Default)]
pub(crate) struct Found {
    players: HashMap<String, Player>,
    refused: bool,
}

impl Found {
    /// The entry's player, by its Discord account.
    pub fn player(&self, entry: &Entry) -> Option<&Player> {
        self.players.get(entry.discord.as_deref()?)
    }

    /// Why the entry's player could not be asked for, if they could not.
    pub fn not_asked(&self) -> Option<&'static str> {
        self.refused
            .then_some("this connection may not look up Discord accounts")
    }
}

/// A refusal that names no one account: the connection may not look up
/// Discord accounts.
fn is_forbidden(error: &BridgeError) -> bool {
    matches!(
        error,
        BridgeError::Refused {
            code: ErrorCode::Forbidden,
            ..
        }
    )
}

/// The part of a match the connector reads.
#[derive(Deserialize)]
struct MatchView {
    match_id: String,
    state: MatchState,
    revision: String,
    #[serde(default)]
    participants: Vec<Seat>,
    #[serde(default)]
    scores: Vec<Wins>,
    #[serde(default)]
    provider_delivery_state: Option<DeliveryState>,
    #[serde(default)]
    attempts: Vec<Attempt>,
}

/// One game of a match.
#[derive(Deserialize)]
struct Attempt {
    seq: u64,
    state: String,
    #[serde(default)]
    outcome: Option<String>,
    /// What both fighters' games said of it besides the result, when a
    /// bridge keeps it (`reports.facts`).
    #[serde(default)]
    facts: Option<serde_json::Value>,
}

#[derive(Deserialize)]
struct Seat {
    slot: u8,
    ember_id: EmberId,
}

#[derive(Deserialize)]
struct Wins {
    ember_id: EmberId,
    wins: u8,
}

impl MatchView {
    /// The winning slot of each game that counts, in the order they were
    /// played, with its facts. A drawn game counts for nobody.
    fn sequence(mut self) -> Vec<(usize, Option<serde_json::Value>)> {
        self.attempts.sort_by_key(|attempt| attempt.seq);
        self.attempts
            .into_iter()
            .filter(|attempt| attempt.state == "accepted")
            .filter_map(|attempt| match attempt.outcome.as_deref() {
                Some("p1_win") => Some((0, attempt.facts)),
                Some("p2_win") => Some((1, attempt.facts)),
                _ => None,
            })
            .collect()
    }

    /// How a match that is over ended, as its result says it; `None` while
    /// it runs, or for a roster that is not two players in slots 0 and 1.
    fn over(self) -> Option<Over> {
        let delivery = self.provider_delivery_state?;
        let ended = match self.state {
            MatchState::Completed => None,
            MatchState::Cancelled | MatchState::Failed => Some(Ended::Restart),
            MatchState::Expired => Some(Ended::Expired),
            _ => return None,
        };
        let mut seats = self.participants;
        seats.sort_by_key(|seat| seat.slot);
        let [one, two] = <[Seat; 2]>::try_from(seats).ok()?;
        if [one.slot, two.slot] != [0, 1] {
            return None;
        }
        let players = [one.ember_id, two.ember_id];
        let ended = match ended {
            Some(ended) => ended,
            None => {
                let wins = |player: &EmberId| {
                    self.scores
                        .iter()
                        .find(|score| score.ember_id == *player)
                        .map(|score| score.wins)
                };
                let games = [wins(&players[0])?, wins(&players[1])?];
                // The one with more games won: also a winner the organizer
                // adjudicated short of the set length.
                let winner = match games {
                    [one, two] if one > two => 0,
                    [one, two] if two > one => 1,
                    _ => return None,
                };
                Ended::Completed { games, winner }
            }
        };
        Some(Over {
            players,
            ended,
            delivery,
        })
    }
}

#[derive(Serialize)]
struct Cancel<'a> {
    reason: &'a str,
    expected_revision: &'a str,
}

/// One of the desk's running events (`/v1/desk/brackets`).
#[derive(Deserialize)]
pub(crate) struct DeskEvent {
    pub id: String,
    /// The bridge sign-in site the event is on, which names its adapter.
    pub site: String,
    pub tournament: String,
    /// Its round rules, holds and window, as a configured tournament's.
    #[serde(default)]
    pub settings: serde_json::Map<String, serde_json::Value>,
    /// Kept in memory only, and dropped when a newer one comes.
    pub access_token: String,
}

impl Drop for DeskEvent {
    fn drop(&mut self) {
        zeroize::Zeroize::zeroize(&mut self.access_token);
    }
}

#[derive(Deserialize)]
struct DeskView {
    brackets: Vec<DeskEvent>,
}

pub(crate) struct BridgeClient {
    client: Client,
    /// When the connection was last refused Discord lookups.
    refused_at: Mutex<Option<u64>>,
}

impl BridgeClient {
    pub fn new(api: &str, credential: Zeroizing<String>) -> Result<Self, String> {
        Ok(Self {
            client: Client::new(http_client()?, api, credential),
            refused_at: Mutex::default(),
        })
    }

    fn get(&self, path: &str) -> reqwest::RequestBuilder {
        self.client.get(path)
    }

    fn post(&self, path: &str) -> reqwest::RequestBuilder {
        self.client.post(path)
    }

    /// The players connected to these Discord accounts, asked for in
    /// lookups of at most `MAX_LOOKUP`. A connection refused Discord
    /// lookups is not asked again for `BRIDGE_FRESH_SECS`; only the entries
    /// wait.
    pub async fn lookup(&self, discords: &[&str], now: u64) -> Result<Found, BridgeError> {
        let mut asked = discords.to_vec();
        asked.sort_unstable();
        asked.dedup();
        let mut found = Found::default();
        let refused_at = *self
            .refused_at
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        if refused_at.is_some_and(|at| now.saturating_sub(at) < BRIDGE_FRESH_SECS) {
            found.refused = true;
            return Ok(found);
        }
        for chunk in asked.chunks(MAX_LOOKUP) {
            let lookup = Lookup {
                discord: chunk.iter().map(|&id| id.to_owned()).collect(),
                ..Lookup::default()
            };
            match self.answer::<LookupAnswer>(self.post(LOOKUP_PATH).json(&lookup)).await {
                Ok(answer) => found.players.extend(answer.players.into_iter().map(|player| {
                    (
                        player.discord_user_id,
                        Player {
                            participant_id: player.participant_id,
                            ember_id: player.ember_id,
                        },
                    )
                })),
                Err(error) if is_forbidden(&error) => {
                    *self
                        .refused_at
                        .lock()
                        .unwrap_or_else(|poisoned| poisoned.into_inner()) = Some(now);
                    found.refused = true;
                    return Ok(found);
                }
                Err(error) => return Err(error),
            }
        }
        Ok(found)
    }

    /// The desk's running events, each with a fresh access token.
    pub async fn desk(&self) -> Result<Vec<DeskEvent>, BridgeError> {
        let view: DeskView = self.answer(self.get("/v1/desk/brackets")).await?;
        Ok(view.brackets)
    }

    /// Gives the bridge a desk event's sets (`bracket::snapshot`).
    pub async fn desk_sets(
        &self,
        event: &str,
        sets: &serde_json::Value,
    ) -> Result<(), BridgeError> {
        let request = self
            .client
            .put(&format!("/v1/desk/events/{event}/sets"))
            .json(sets);
        let _: serde_json::Value = self.answer(request).await?;
        Ok(())
    }

    /// Creates the match, or finds the one this `external_match_id` already
    /// made with the same request, and returns its ID.
    pub async fn create(&self, command: &CreateMatch) -> Result<String, BridgeError> {
        // An external ID can be longer than the 128 bytes the bridge takes
        // in an Idempotency-Key; its digest is as fixed and fits.
        let key = json::digest(&command.external_match_id).map_err(|_| BridgeError::Unavailable)?;
        let request = self
            .post("/v1/matches")
            .header("idempotency-key", format!("ext_{key}"))
            .json(command);
        let created: MatchView = self.answer(request).await?;
        Ok(created.match_id)
    }

    /// How the match ended, if it has, read the way the bridge's result
    /// would give it, with how the result's delivery is going.
    pub async fn over(&self, match_id: &str) -> Result<Option<Over>, BridgeError> {
        Ok(self.read_match(match_id).await?.over())
    }

    async fn read_match(&self, match_id: &str) -> Result<MatchView, BridgeError> {
        self.answer(self.get(&format!("/v1/matches/{match_id}")))
            .await
    }

    /// The winning slot of each game of the match that counts, in the order
    /// they were played, with the game's facts when the bridge keeps them.
    pub async fn sequence(
        &self,
        match_id: &str,
    ) -> Result<Vec<(usize, Option<serde_json::Value>)>, BridgeError> {
        Ok(self.read_match(match_id).await?.sequence())
    }

    /// Cancels the match unless it has already ended.
    pub async fn cancel(&self, match_id: &str, reason: &str) -> Result<(), BridgeError> {
        let found = self.read_match(match_id).await?;
        if found.state.is_terminal() {
            return Ok(());
        }
        let request = self
            .post(&format!("/v1/matches/{match_id}/cancel"))
            .header(
                "idempotency-key",
                format!("cancel:{match_id}:{}", found.revision),
            )
            .json(&Cancel {
                reason,
                expected_revision: &found.revision,
            });
        let _: MatchView = self.answer(request).await?;
        Ok(())
    }

    async fn answer<T: DeserializeOwned>(
        &self,
        request: reqwest::RequestBuilder,
    ) -> Result<T, BridgeError> {
        self.client
            .answer(request, MAX_ANSWER)
            .await
            .map_err(|failure| match failure {
                Failure::Unavailable => BridgeError::Unavailable,
                Failure::Refused(error) => BridgeError::Refused {
                    code: error.code,
                    message: error.message.chars().take(200).collect(),
                },
            })
    }
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::*;

    fn view(state: &str, scores: [u8; 2]) -> MatchView {
        let one = "emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";
        let two = ember_protocol::SigningIdentity::from_seed(&Zeroizing::new([2; 32]))
            .unwrap()
            .ember_id()
            .to_string();
        serde_json::from_value(json!({
            "match_id": "emt_1", "state": state, "revision": "4", "games_to_win": 2,
            // Listed out of slot order: the slots decide.
            "participants": [
                { "slot": 1, "participant_id": "epl_b", "ember_id": two },
                { "slot": 0, "participant_id": "epl_a", "ember_id": one },
            ],
            "scores": [{ "ember_id": one, "wins": scores[0] }, { "ember_id": two, "wins": scores[1] }],
            "provider_delivery_state": "retrying",
        }))
        .unwrap()
    }

    #[test]
    fn a_finished_match_reads_like_its_result() {
        assert_eq!(
            view("completed", [1, 2]).over().map(|over| over.ended),
            Some(Ended::Completed {
                games: [1, 2],
                winner: 1
            })
        );
        let outcomes: Vec<_> = ["cancelled", "failed", "expired", "running", "needs_review"]
            .into_iter()
            .map(|state| view(state, [1, 0]).over().map(|over| over.ended))
            .collect();
        assert_eq!(
            outcomes,
            [
                Some(Ended::Restart),
                Some(Ended::Restart),
                Some(Ended::Expired),
                None,
                None
            ]
        );
        // A completed match without a leader is not read.
        assert!(view("completed", [1, 1]).over().is_none());
        // A winner short of the set length (an adjudicated forfeit), or past
        // it, as the bridge itself allows.
        assert_eq!(
            view("completed", [1, 0]).over().map(|over| over.ended),
            Some(Ended::Completed {
                games: [1, 0],
                winner: 0
            })
        );
        assert_eq!(
            view("completed", [1, 3]).over().map(|over| over.ended),
            Some(Ended::Completed {
                games: [1, 3],
                winner: 1
            })
        );
    }

    #[test]
    fn answers_are_read_past_fields_a_later_bridge_adds() {
        let player1 = "emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";
        let answer: LookupAnswer = serde_json::from_value(json!({
            "players": [{ "discord_user_id": "274220342558756145", "ember_id": player1, "participant_id": "epl_a", "since": 1 }],
            "next": null,
        }))
        .unwrap();
        assert_eq!(answer.players.len(), 1);
    }

    #[test]
    fn an_entry_is_found_by_its_discord_account() {
        let entry = |discord: Option<&str>| Entry {
            participant_id: "355".into(),
            name: None,
            discord: discord.map(str::to_owned),
        };
        let mut found = Found::default();
        found.players.insert(
            "274220342558756145".into(),
            Player {
                participant_id: "epl_discord".into(),
                ember_id: EmberId::parse(
                    "emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja",
                )
                .unwrap(),
            },
        );
        let found_one = found.player(&entry(Some("274220342558756145")));
        assert_eq!(found_one.unwrap().participant_id, "epl_discord");
        assert!(found.player(&entry(None)).is_none());
        assert_eq!(found.not_asked(), None);
        found.refused = true;
        assert_eq!(
            found.not_asked(),
            Some("this connection may not look up Discord accounts")
        );
    }
}
