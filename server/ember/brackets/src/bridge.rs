//! The bridge routes the connector calls with its connection's provider
//! credential: finding players by the account a site entry stands for,
//! creating a set's match, reading how one ended, and cancelling one.
//! Bridge requests cost nothing against a site's budget.
use std::{
    collections::{HashMap, HashSet},
    fmt,
    sync::Mutex,
};

use ember_protocol::{
    EmberId,
    api::ErrorCode,
    client::{BridgeClient as Client, Failure},
    json,
    matches::{CreateMatch, DeliveryState, MatchState},
    partner::{LOOKUP_PATH, Lookup, LookupAnswer, MAX_LOOKUP},
    sites::SiteAccount,
};
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use zeroize::Zeroizing;

use crate::{
    http::http_client,
    sets::Ended,
    site::{Account, Entry},
};

const MAX_ANSWER: usize = 256 * 1024;
/// How long the bridge's sign-in sites, and a kind of account the
/// connection may not look up, are taken as read: a change on the bridge
/// is seen within this, without a restart.
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

/// The accounts the bridge's connection settings allow or refuse as one:
/// Discord's (`discord_lookup`), or one site's (`lookup_sites`).
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
enum Kind {
    Discord,
    Site(String),
}

impl Kind {
    fn of(account: &Account) -> Self {
        match account {
            Account::Site { site, .. } => Self::Site(site.clone()),
            Account::Discord { .. } => Self::Discord,
        }
    }
}

/// The players found for a lookup's accounts, by the account as asked,
/// and why the others could not be asked: a site the bridge does not have,
/// or a kind of account the connection may not look up.
#[derive(Default)]
pub(crate) struct Found {
    players: HashMap<Account, Player>,
    unknown: HashSet<Account>,
    refused: HashSet<Account>,
}

impl Found {
    /// The entry's player, by the first of its accounts that was found.
    pub fn player<'a>(&'a self, entry: &'a Entry) -> Option<(&'a Account, &'a Player)> {
        entry
            .accounts
            .iter()
            .find_map(|account| Some((account, self.players.get(account)?)))
    }

    /// Why this account was not asked, if it was not.
    pub fn not_asked(&self, account: &Account) -> Option<String> {
        if self.unknown.contains(account) {
            return Some(format!("the bridge has no sign-in site for {account}"));
        }
        self.refused
            .contains(account)
            .then(|| match Kind::of(account) {
                Kind::Discord => "this connection may not look up Discord accounts".to_owned(),
                Kind::Site(site) => format!("this connection may not look up {site} accounts"),
            })
    }
}

/// What the client knows of the bridge for a while: its sign-in sites, and
/// the kinds of account the connection was refused, each with when it was
/// learnt.
#[derive(Default)]
struct Known {
    sites: Option<(HashMap<String, bool>, u64)>,
    refused: HashMap<Kind, u64>,
}

/// A refusal that names no one account: the connection may not look up a
/// kind of account the lookup holds.
fn is_forbidden(error: &BridgeError) -> bool {
    matches!(
        error,
        BridgeError::Refused {
            code: ErrorCode::Forbidden,
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

/// `GET /v1/capabilities`, as far as the connector reads it.
#[derive(Deserialize)]
struct CapabilitiesView {
    #[serde(default)]
    sign_in_sites: Vec<SiteView>,
}

#[derive(Deserialize)]
struct SiteView {
    id: String,
    lookup_by: String,
}

/// The bridge's sign-in sites, by ID, and whether each looks accounts up
/// by username (a name an account can change).
fn sites_of(capabilities: CapabilitiesView) -> HashMap<String, bool> {
    capabilities
        .sign_in_sites
        .into_iter()
        .map(|site| (site.id, site.lookup_by == "username"))
        .collect()
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

pub(crate) struct BridgeClient {
    client: Client,
    known: Mutex<Known>,
}

impl BridgeClient {
    pub fn new(api: &str, credential: Zeroizing<String>) -> Result<Self, String> {
        Ok(Self {
            client: Client::new(http_client()?, api, credential),
            known: Mutex::default(),
        })
    }

    fn get(&self, path: &str) -> reqwest::RequestBuilder {
        self.client.get(path)
    }

    fn post(&self, path: &str) -> reqwest::RequestBuilder {
        self.client.post(path)
    }

    /// The players connected to these accounts, asked for in lookups of at
    /// most `MAX_LOOKUP` accounts, Discord's and sites' together. The
    /// bridge finds nothing for an ID its site's accounts cannot have. It
    /// refuses a whole lookup that names a site it does not have, which is
    /// therefore not asked, or a kind of account the connection may not
    /// look up (Discord's, or a site's): a refused lookup is asked again
    /// kind by kind, and a refused kind is left out for a while. Either way
    /// only the entries with such an account wait. Bridge requests cost no
    /// site budget.
    pub async fn lookup(&self, accounts: &[&Account], now: u64) -> Result<Found, BridgeError> {
        let mut asked = accounts.to_vec();
        asked.sort_by_key(|account| account.to_string());
        asked.dedup();
        let mut found = Found::default();
        let sites = self.sign_in_sites(now).await;
        {
            let known = self.known();
            asked.retain(|&account| {
                if let (Account::Site { site, .. }, Some(sites)) = (account, &sites)
                    && !sites.contains_key(site)
                {
                    found.unknown.insert(account.clone());
                    return false;
                }
                let refused = known
                    .refused
                    .get(&Kind::of(account))
                    .is_some_and(|&at| now.saturating_sub(at) < BRIDGE_FRESH_SECS);
                if refused {
                    found.refused.insert(account.clone());
                }
                !refused
            });
        }
        for chunk in asked.chunks(MAX_LOOKUP) {
            match self.lookup_once(chunk).await {
                Ok(answer) => found.players.extend(found_in(answer)),
                Err(error) if is_forbidden(&error) => {
                    // The bridge's sites may have changed too.
                    self.known().sites = None;
                    let mut kinds: Vec<Kind> = Vec::new();
                    for &account in chunk {
                        let kind = Kind::of(account);
                        if !kinds.contains(&kind) {
                            kinds.push(kind);
                        }
                    }
                    for kind in kinds {
                        let group: Vec<&Account> = chunk
                            .iter()
                            .copied()
                            .filter(|&account| Kind::of(account) == kind)
                            .collect();
                        match self.lookup_once(&group).await {
                            Ok(answer) => found.players.extend(found_in(answer)),
                            Err(error) if is_forbidden(&error) => {
                                self.known().refused.insert(kind, now);
                                found.refused.extend(group.into_iter().cloned());
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

    fn known(&self) -> std::sync::MutexGuard<'_, Known> {
        self.known
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    /// The bridge's sign-in sites, and whether each looks accounts up by
    /// username, from its capabilities: read again every
    /// `BRIDGE_FRESH_SECS` and after a refused lookup. `None` while they
    /// cannot be read.
    pub async fn sign_in_sites(&self, now: u64) -> Option<HashMap<String, bool>> {
        if let Some((sites, at)) = &self.known().sites
            && now.saturating_sub(*at) < BRIDGE_FRESH_SECS
        {
            return Some(sites.clone());
        }
        let capabilities: CapabilitiesView =
            self.answer(self.get("/v1/capabilities")).await.ok()?;
        let sites = sites_of(capabilities);
        self.known().sites = Some((sites.clone(), now));
        Some(sites)
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
        Ok(self.read_match(match_id).await?.over())
    }

    async fn read_match(&self, match_id: &str) -> Result<MatchView, BridgeError> {
        self.answer(self.get(&format!("/v1/matches/{match_id}")))
            .await
    }

    /// The winning slot of each game of the match that counts, in the order
    /// they were played.
    pub async fn sequence(&self, match_id: &str) -> Result<Vec<usize>, BridgeError> {
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
            "accounts": [{ "site": "startgg", "id": "77", "user_id": "77", "ember_id": player1, "participant_id": "epl_b", "connected_at": 5, "verified": true }],
            "next": null,
        }))
        .unwrap();
        assert_eq!(found_in(answer).count(), 2);
        // A Discord-only answer leaves `accounts` out.
        let answer: LookupAnswer = serde_json::from_value(json!({ "players": [] })).unwrap();
        assert_eq!(found_in(answer).count(), 0);
        let capabilities: CapabilitiesView = serde_json::from_value(json!({
            "api_version": "1",
            "sign_in_sites": [
                { "id": "challonge", "lookup_by": "username", "display_name": "Challonge" },
                { "id": "startgg", "lookup_by": "user_id" },
            ],
        }))
        .unwrap();
        assert_eq!(
            sites_of(capabilities),
            HashMap::from([
                ("challonge".to_owned(), true),
                ("startgg".to_owned(), false)
            ])
        );
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
        // Why an account was not asked.
        let unknown = Account::Site {
            site: "smashgg".into(),
            id: "12".into(),
        };
        found.unknown.insert(unknown.clone());
        found.refused.insert(discord.clone());
        assert_eq!(
            found.not_asked(&unknown).as_deref(),
            Some("the bridge has no sign-in site for smashgg account 12")
        );
        assert_eq!(
            found.not_asked(&discord).as_deref(),
            Some("this connection may not look up Discord accounts")
        );
        assert_eq!(found.not_asked(&site), None);
    }
}
