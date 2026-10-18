//! The bridge routes the connector calls with its connection's provider
//! credential: finding players by the account a site entry stands for,
//! creating a set's match, reading how one ended, and cancelling one.
//! Bridge requests cost nothing against a site's budget.
use std::{collections::HashMap, fmt};

use ember_protocol::{
    EmberId,
    api::{ApiError, ErrorCode},
    json,
    matches::{CreateMatch, DeliveryState, MatchState},
    partner::{LOOKUP_PATH, Lookup, LookupAnswer, MAX_LOOKUP},
    sites::SiteAccount,
};
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use zeroize::Zeroizing;

use crate::{
    http::{bounded_body, http_client},
    sets::Ended,
    site::{Account, Entry},
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
    /// When a site account was connected: the last time the bridge saw
    /// it under the name it was found by, which a site may let it change.
    pub connected_at: Option<u64>,
}

/// A match that is over, as the bridge records it.
pub(crate) struct Over {
    /// Its players, by slot.
    pub players: [EmberId; 2],
    pub ended: Ended,
    /// How the bridge's delivery of its result is going.
    pub delivery: DeliveryState,
}

/// The players found for a lookup's accounts, by the account as asked,
/// and the accounts the bridge would not look up, with its reason.
#[derive(Default)]
pub(crate) struct Found {
    players: HashMap<Account, Player>,
    refused: HashMap<Account, String>,
}

impl Found {
    /// The entry's player, by the first of its accounts that was found.
    pub fn player<'a>(&'a self, entry: &'a Entry) -> Option<(&'a Account, &'a Player)> {
        entry
            .accounts
            .iter()
            .find_map(|account| Some((account, self.players.get(account)?)))
    }

    /// The first of the entry's accounts the bridge would not look up, and
    /// why.
    pub fn refusal<'a>(&'a self, entry: &'a Entry) -> Option<(&'a Account, &'a str)> {
        entry
            .accounts
            .iter()
            .find_map(|account| Some((account, self.refused.get(account)?.as_str())))
    }
}

/// A refusal of a whole lookup for what one of its accounts is: an ID not
/// in its site's form, or a site the connection may not find players by.
fn names_an_account(error: &BridgeError) -> bool {
    matches!(
        error,
        BridgeError::Refused {
            code: ErrorCode::InvalidRequest | ErrorCode::Forbidden,
            ..
        }
    )
}

/// A lookup of these accounts, in the bridge's request form.
fn lookup_of(accounts: &[&Account]) -> Lookup {
    let mut lookup = Lookup::default();
    for account in accounts {
        match account {
            Account::Site { site, id } => lookup.accounts.push(SiteAccount {
                site: site.clone(),
                id: id.clone(),
            }),
            Account::Discord { user_id } => lookup.discord.push(user_id.clone()),
        }
    }
    lookup
}

/// The players a lookup answer names, by the account they were asked as.
fn found_in(answer: LookupAnswer) -> impl Iterator<Item = (Account, Player)> {
    let sites = answer.accounts.into_iter().map(|found| {
        let account = Account::Site {
            site: found.site,
            id: found.id,
        };
        let player = Player {
            participant_id: found.participant_id,
            ember_id: found.ember_id,
            connected_at: Some(found.connected_at),
        };
        (account, player)
    });
    let discord = answer.players.into_iter().map(|found| {
        let account = Account::Discord {
            user_id: found.discord_user_id,
        };
        let player = Player {
            participant_id: found.participant_id,
            ember_id: found.ember_id,
            connected_at: None,
        };
        (account, player)
    });
    sites.chain(discord)
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
    /// played. A drawn game counts for nobody.
    fn sequence(mut self) -> Vec<usize> {
        self.attempts.sort_by_key(|attempt| attempt.seq);
        self.attempts
            .iter()
            .filter(|attempt| attempt.state == "accepted")
            .filter_map(|attempt| match attempt.outcome.as_deref() {
                Some("p1_win") => Some(0),
                Some("p2_win") => Some(1),
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
                let winner = games.iter().position(|&won| won == self.games_to_win)?;
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
    /// most `MAX_LOOKUP` accounts, Discord's and sites' together. The
    /// bridge refuses a whole lookup that names one account it will not
    /// look up; that lookup's accounts are then asked one by one, so only
    /// that account's entry waits. Bridge requests cost no site budget.
    pub async fn lookup(&self, accounts: &[&Account]) -> Result<Found, BridgeError> {
        let mut asked = accounts.to_vec();
        asked.sort_by_key(|account| account.to_string());
        asked.dedup();
        let mut found = Found::default();
        for chunk in asked.chunks(MAX_LOOKUP) {
            match self.lookup_once(chunk).await {
                Ok(answer) => found.players.extend(found_in(answer)),
                Err(error) if names_an_account(&error) => {
                    for &account in chunk {
                        match self.lookup_once(&[account]).await {
                            Ok(answer) => found.players.extend(found_in(answer)),
                            Err(error) if names_an_account(&error) => {
                                found.refused.insert(account.clone(), error.to_string());
                            }
                            Err(error) => return Err(error),
                        }
                    }
                }
                Err(error) => return Err(error),
            }
        }
        Ok(found)
    }

    async fn lookup_once(&self, accounts: &[&Account]) -> Result<LookupAnswer, BridgeError> {
        let request = self.post(LOOKUP_PATH).json(&lookup_of(accounts));
        self.answer(request).await
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
        let found: MatchView = self
            .answer(self.get(&format!("/v1/matches/{match_id}")))
            .await?;
        Ok(found.over())
    }

    /// The winning slot of each game of the match that counts, in the order
    /// they were played.
    pub async fn sequence(&self, match_id: &str) -> Result<Vec<usize>, BridgeError> {
        let found: MatchView = self
            .answer(self.get(&format!("/v1/matches/{match_id}")))
            .await?;
        Ok(found.sequence())
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
        // A completed match without a winner at the set length is not read.
        assert!(view("completed", [1, 1]).over().is_none());
    }

    #[test]
    fn an_entry_is_found_by_its_first_account_found() {
        let site = Account::Site {
            site: "startgg".into(),
            id: "1234".into(),
        };
        let discord = Account::Discord {
            user_id: "274220342558756145".into(),
        };
        let player = |participant: &str| Player {
            participant_id: participant.into(),
            ember_id: EmberId::parse("emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja")
                .unwrap(),
            connected_at: None,
        };
        let entry = Entry {
            participant_id: "355".into(),
            accounts: vec![site.clone(), discord.clone()],
        };
        let mut found = Found::default();
        found.players.insert(discord.clone(), player("epl_discord"));
        assert_eq!(found.player(&entry).unwrap().0, &discord);
        found.players.insert(site.clone(), player("epl_site"));
        let (account, by_site) = found.player(&entry).unwrap();
        assert_eq!(
            (account, by_site.participant_id.as_str()),
            (&site, "epl_site")
        );
        let none = Entry {
            participant_id: "354".into(),
            accounts: Vec::new(),
        };
        assert!(found.player(&none).is_none());
        // An account the bridge would not look up is the reason given.
        let refused = Account::Site {
            site: "startgg".into(),
            id: "kate".into(),
        };
        found
            .refused
            .insert(refused.clone(), "invalid_request (no ID)".into());
        let waiting = Entry {
            participant_id: "353".into(),
            accounts: vec![refused.clone()],
        };
        assert!(found.player(&waiting).is_none());
        assert_eq!(
            found.refusal(&waiting),
            Some((&refused, "invalid_request (no ID)"))
        );
    }
}
