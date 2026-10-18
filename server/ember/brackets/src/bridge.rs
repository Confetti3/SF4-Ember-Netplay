//! The bridge routes the connector calls with its connection's provider
//! credential: finding players by the account a site entry stands for,
//! creating a set's match, reading how one ended, and cancelling one.
//! Bridge requests cost nothing against a site's budget.
use std::{collections::HashMap, fmt};

use ember_protocol::{
    EmberId,
    api::{ApiError, ErrorCode},
    challonge::{is_username, username_key},
    json,
    matches::{CreateMatch, MatchState},
    partner::{LOOKUP_PATH, Lookup, LookupAnswer, MAX_LOOKUP},
};
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use zeroize::Zeroizing;

use crate::{
    http::{bounded_body, http_client},
    sets::Ended,
    site::Account,
};

const MAX_ANSWER: usize = 256 * 1024;

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
                let code = serde_json::to_value(code).unwrap_or_default();
                write!(f, "{} ({message})", code.as_str().unwrap_or("refused"))
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

/// The players found for a lookup's accounts.
#[derive(Default)]
pub(crate) struct Found(HashMap<Account, Player>);

/// The form an account is matched in: Challonge usernames without case.
fn matched(account: &Account) -> Account {
    match account {
        Account::Challonge { username } => Account::Challonge {
            username: username_key(username),
        },
        Account::Discord { .. } => account.clone(),
    }
}

impl Found {
    pub fn get(&self, account: &Account) -> Option<&Player> {
        self.0.get(&matched(account))
    }
}

/// The part of a match the connector reads.
#[derive(Deserialize)]
struct MatchView {
    match_id: String,
    state: MatchState,
    revision: String,
    #[serde(default)]
    games_to_win: u8,
    #[serde(default)]
    participants: Vec<Seat>,
    #[serde(default)]
    scores: Vec<Wins>,
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
    /// How a match that is over ended, as its result says it; `None` while
    /// it runs, or for a roster that is not two players in slots 0 and 1.
    fn ended(self) -> Option<([EmberId; 2], Ended)> {
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
                let winner = games.iter().position(|&won| won == self.games_to_win)?;
                Ended::Completed { games, winner }
            }
        };
        Some((players, ended))
    }
}

#[derive(Serialize)]
struct Cancel<'a> {
    reason: &'a str,
    expected_revision: &'a str,
}

pub(crate) struct BridgeClient {
    http: reqwest::Client,
    api: String,
    credential: Zeroizing<String>,
}

impl BridgeClient {
    pub fn new(api: &str, credential: Zeroizing<String>) -> Result<Self, String> {
        Ok(Self {
            http: http_client()?,
            api: api.trim_end_matches('/').to_owned(),
            credential,
        })
    }

    fn get(&self, path: &str) -> reqwest::RequestBuilder {
        self.http
            .get(format!("{}{path}", self.api))
            .bearer_auth(self.credential.as_str())
    }

    fn post(&self, path: &str) -> reqwest::RequestBuilder {
        self.http
            .post(format!("{}{path}", self.api))
            .bearer_auth(self.credential.as_str())
    }

    /// The players connected to these accounts, asked for in lookups of at
    /// most `MAX_LOOKUP` accounts. A Challonge username the bridge would
    /// refuse is not asked for, so it is simply not found.
    pub async fn lookup(&self, accounts: &[&Account]) -> Result<Found, BridgeError> {
        let mut asked: Vec<Account> = accounts
            .iter()
            .map(|account| matched(account))
            .filter(|account| match account {
                Account::Challonge { username } => is_username(username),
                Account::Discord { .. } => true,
            })
            .collect();
        asked.sort_by_key(|account| account.to_string());
        asked.dedup();
        let mut found = Found::default();
        for chunk in asked.chunks(MAX_LOOKUP) {
            let mut body = Lookup::default();
            for account in chunk {
                match account {
                    Account::Challonge { username } => body.challonge.push(username.clone()),
                    Account::Discord { user_id } => body.discord.push(user_id.clone()),
                }
            }
            let answer: LookupAnswer = self.answer(self.post(LOOKUP_PATH).json(&body)).await?;
            let challonge = answer.challonge.into_iter().map(|player| {
                let account = Account::Challonge {
                    username: player.username,
                };
                (account, player.participant_id, player.ember_id)
            });
            let discord = answer.players.into_iter().map(|player| {
                let account = Account::Discord {
                    user_id: player.discord_user_id,
                };
                (account, player.participant_id, player.ember_id)
            });
            for (account, participant_id, ember_id) in challonge.chain(discord) {
                let player = Player {
                    participant_id,
                    ember_id,
                };
                found.0.insert(matched(&account), player);
            }
        }
        Ok(found)
    }

    /// Creates the match, or finds the one this `external_match_id` already
    /// made with the same request, and returns its ID.
    pub async fn create(&self, command: &CreateMatch) -> Result<String, BridgeError> {
        let request = self
            .post("/v1/matches")
            .header("idempotency-key", &command.external_match_id)
            .json(command);
        let created: MatchView = self.answer(request).await?;
        Ok(created.match_id)
    }

    /// How the match ended, if it has: its players by slot and its outcome,
    /// read the way the bridge's result would give them.
    pub async fn ended(
        &self,
        match_id: &str,
    ) -> Result<Option<([EmberId; 2], Ended)>, BridgeError> {
        let found: MatchView = self
            .answer(self.get(&format!("/v1/matches/{match_id}")))
            .await?;
        Ok(found.ended())
    }

    /// Cancels the match unless it has already ended.
    pub async fn cancel(&self, match_id: &str, reason: &str) -> Result<(), BridgeError> {
        let found: MatchView = self
            .answer(self.get(&format!("/v1/matches/{match_id}")))
            .await?;
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
        let response = request.send().await.map_err(|_| BridgeError::Unavailable)?;
        let success = response.status().is_success();
        let body = bounded_body(response, MAX_ANSWER)
            .await
            .ok_or(BridgeError::Unavailable)?;
        if success {
            return json::parse_as(&body, MAX_ANSWER).map_err(|_| BridgeError::Unavailable);
        }
        let refusal: ApiError =
            json::parse_as(&body, MAX_ANSWER).map_err(|_| BridgeError::Unavailable)?;
        Err(BridgeError::Refused {
            code: refusal.error.code,
            message: refusal.error.message.chars().take(200).collect(),
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
        }))
        .unwrap()
    }

    #[test]
    fn a_finished_match_reads_like_its_result() {
        assert_eq!(
            view("completed", [1, 2]).ended().map(|(_, ended)| ended),
            Some(Ended::Completed {
                games: [1, 2],
                winner: 1
            })
        );
        let outcomes: Vec<_> = ["cancelled", "failed", "expired", "running", "needs_review"]
            .into_iter()
            .map(|state| view(state, [1, 0]).ended().map(|(_, ended)| ended))
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
        // A completed match without a winner at the set length is not read.
        assert_eq!(view("completed", [1, 1]).ended(), None);
    }

    #[test]
    fn challonge_usernames_match_without_case() {
        let mut found = Found::default();
        let player = Player {
            participant_id: "epl_a".into(),
            ember_id: EmberId::parse("emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja")
                .unwrap(),
        };
        found.0.insert(
            matched(&Account::Challonge {
                username: "Kate".into(),
            }),
            player,
        );
        let asked = Account::Challonge {
            username: "KATE".into(),
        };
        assert!(found.get(&asked).is_some());
        let discord = Account::Discord {
            user_id: "274220342558756145".into(),
        };
        assert!(found.get(&discord).is_none());
    }
}
