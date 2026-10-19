//! The Challonge v2.1 adapter (docs/design/CHALLONGE_API.md).
//!
//! Challonge's documents mix string and integer IDs and disagree on where a
//! match's players sit, so both are read leniently. Every request is counted
//! against the credential's monthly budget before it is sent: scheduled
//! reads and underway marks can wait for next month, everything a result
//! needs cannot.
use std::collections::HashMap;

use reqwest::{
    Method,
    header::{ACCEPT, AUTHORIZATION, CONTENT_TYPE, HeaderMap, HeaderValue},
};
use serde::{Deserialize, Deserializer, de::DeserializeOwned};
use serde_json::{Value, json};
use zeroize::Zeroizing;

use crate::{
    Clock,
    budget::{Budget, Priority},
    config::{CredentialKind, is_slug},
    http::{self, http_client},
    roster::Roster,
    site::{
        Account, Entry, Purpose, Report, Reported, Set, Site, SiteError, SiteFuture, Standing,
        Status,
    },
};

/// Challonge's API origin. The organizer's credential goes nowhere else
/// (`config::is_site_origin`).
pub const API_ORIGIN: &str = "https://api.challonge.com";
const API_PATH: &str = "/v2.1";
/// Challonge's documented default page size, which it therefore serves in
/// full; a shorter page is the last. Its maximum is not documented.
const PER_PAGE: usize = 25;
/// Matches were once paginated without end (changelog, 2026-09-14), so a
/// list that keeps going is refused rather than read forever.
const MAX_PAGES: u32 = 40;
const MAX_ANSWER: usize = 1024 * 1024;
/// Tournament states after which no set is played. Not in the documented
/// schema, so a tournament without one is judged from its matches. Before
/// the organizer finalizes (`awaiting_review`) a set can still be reopened.
const OVER: &[&str] = &["complete", "ended"];
const IN_REVIEW: &str = "awaiting_review";

pub(crate) struct Challonge {
    http: reqwest::Client,
    /// `<api>/tournaments/<tournament>`.
    base: String,
    /// Every request's headers, with the credential marked sensitive.
    headers: HeaderMap,
    budget: Budget,
    /// Participant ID to the Challonge account behind it, read again when
    /// a set names a participant not in it: an entry the organizer gives an
    /// account later is found after a restart.
    roster: Roster,
    /// The bridge sign-in site players' Challonge accounts are under.
    bridge_site: String,
}

fn priority(purpose: Purpose) -> Priority {
    match purpose {
        Purpose::Scheduled => Priority::CanWait,
        Purpose::Result => Priority::Always,
    }
}

/// `path` with `query` added after `?` or `&`, as it needs.
fn with_query(path: &str, query: &str) -> String {
    let separator = if path.contains('?') { '&' } else { '?' };
    format!("{path}{separator}{query}")
}

impl Challonge {
    /// `origin` is Challonge's API origin, checked by the configuration.
    pub fn new(
        origin: &str,
        tournament: &str,
        bridge_site: &str,
        kind: CredentialKind,
        credential: &str,
        budget: Budget,
        clock: Clock,
    ) -> Result<Self, String> {
        let (version, value) = match kind {
            CredentialKind::V1Key => ("v1", Zeroizing::new(credential.to_owned())),
            CredentialKind::Bearer => ("v2", Zeroizing::new(format!("Bearer {credential}"))),
        };
        let authorization = http::secret_header(&value, "the Challonge credential")?;
        let mut headers = HeaderMap::new();
        headers.insert(
            CONTENT_TYPE,
            HeaderValue::from_static("application/vnd.api+json"),
        );
        headers.insert(ACCEPT, HeaderValue::from_static("application/json"));
        headers.insert("authorization-type", HeaderValue::from_static(version));
        headers.insert(AUTHORIZATION, authorization);
        Ok(Self {
            http: http_client()?,
            base: format!("{origin}{API_PATH}/tournaments/{tournament}"),
            headers,
            budget,
            roster: Roster::new(clock, None),
            bridge_site: bridge_site.to_owned(),
        })
    }

    /// One request. `None` when Challonge answers 404.
    async fn call(
        &self,
        priority: Priority,
        method: Method,
        path: &str,
        body: Option<Value>,
    ) -> Result<Option<Value>, SiteError> {
        self.budget.spend(priority)?;
        let mut request = self
            .http
            .request(method, format!("{}{path}", self.base))
            .headers(self.headers.clone());
        if let Some(body) = body {
            request = request.body(body.to_string());
        }
        let (status, body) = http::exchange(request, MAX_ANSWER, "Challonge").await?;
        if status.is_success() {
            return if body.is_empty() {
                Ok(Some(Value::Null))
            } else {
                serde_json::from_slice(&body)
                    .map(Some)
                    .map_err(|_| unexpected())
            };
        }
        let message = match error_detail(&body) {
            Some(detail) => format!("Challonge answered {}: {detail}", status.as_u16()),
            None => format!("Challonge answered {}", status.as_u16()),
        };
        match status.as_u16() {
            404 => Ok(None),
            // A credential Challonge refuses is the operator's to fix, and a
            // request it never served does not count.
            401 | 403 => {
                self.budget.refund();
                Err(SiteError::Denied(message))
            }
            // 406 and 415 say the request's format was wrong, not this set:
            // waited out, so a fixed connector still reports.
            406 | 415 | 429 => Err(SiteError::Unavailable(message)),
            _ if status.is_server_error() => Err(SiteError::Unavailable(message)),
            _ => Err(SiteError::Refused(message)),
        }
    }

    /// A request whose 404 means the request itself is wrong.
    async fn call_found(
        &self,
        priority: Priority,
        method: Method,
        path: &str,
        body: Option<Value>,
    ) -> Result<Value, SiteError> {
        self.call(priority, method, path, body)
            .await?
            .ok_or_else(|| SiteError::Refused("Challonge answered 404".into()))
    }

    /// Every item of a list, page by page until a short page.
    async fn list<T: DeserializeOwned>(
        &self,
        priority: Priority,
        path: &str,
    ) -> Result<Vec<T>, SiteError> {
        let mut items = Vec::new();
        for page in 1..=MAX_PAGES {
            let path = with_query(path, &format!("page={page}&per_page={PER_PAGE}"));
            let answer = self.call_found(priority, Method::GET, &path, None).await?;
            let Data { data: page }: Data<Vec<T>> = parse(answer)?;
            let last = page.len() < PER_PAGE;
            items.extend(page);
            if last {
                return Ok(items);
            }
        }
        Err(SiteError::Unavailable(format!(
            "Challonge listed more than {MAX_PAGES} pages"
        )))
    }

    /// Whether a list has any item, in one request of one item.
    async fn any(&self, priority: Priority, path: &str) -> Result<bool, SiteError> {
        let path = with_query(path, "page=1&per_page=1");
        let answer = self.call_found(priority, Method::GET, &path, None).await?;
        let Data { data: items }: Data<Vec<Value>> = parse(answer)?;
        Ok(!items.is_empty())
    }

    /// Reads the participants: each one's Challonge username, when it has
    /// one. The bridge's Challonge site decides which usernames it looks
    /// up; one it refuses holds back only its own entry.
    async fn refresh_participants(
        &self,
        priority: Priority,
        asked: &[&String],
    ) -> Result<(), SiteError> {
        let participants: Vec<Resource<ParticipantAttributes>> =
            self.list(priority, "/participants.json").await?;
        let accounts: HashMap<String, Vec<Account>> = participants
            .into_iter()
            .map(|participant| {
                let account = participant
                    .attributes
                    .username
                    .filter(|name| !name.is_empty())
                    .map(|username| Account::Site {
                        site: self.bridge_site.clone(),
                        id: username,
                    });
                (participant.id, account.into_iter().collect())
            })
            .collect();
        self.roster.replace(accounts, asked.iter().copied());
        Ok(())
    }

    fn set_of(&self, raw: RawSet) -> Set {
        let entries = self.roster.entries(raw.players.as_ref());
        raw.into_set(entries)
    }

    async fn read_open_sets(&self, purpose: Purpose) -> Result<Vec<Set>, SiteError> {
        let priority = priority(purpose);
        let matches: Vec<Resource<MatchAttributes>> =
            self.list(priority, "/matches.json?state=open").await?;
        let raw: Vec<RawSet> = matches.into_iter().map(RawSet::from).collect();
        let ids: Vec<&String> = raw
            .iter()
            .flat_map(|set| set.players.iter().flatten())
            .collect();
        if self.roster.stale(ids.iter().copied()) {
            self.refresh_participants(priority, &ids).await?;
        }
        Ok(raw.into_iter().map(|raw| self.set_of(raw)).collect())
    }

    async fn read_set(&self, id: &str, purpose: Purpose) -> Result<Option<Set>, SiteError> {
        if !is_slug(id) {
            return Ok(None);
        }
        let path = format!("/matches/{id}.json");
        let Some(answer) = self
            .call(priority(purpose), Method::GET, &path, None)
            .await?
        else {
            return Ok(None);
        };
        let Data { data: resource }: Data<Resource<MatchAttributes>> = parse(answer)?;
        Ok(Some(self.set_of(RawSet::from(resource))))
    }

    /// The tournament's `state` decides when Challonge gives it; otherwise
    /// the matches say whether everything looks played.
    async fn read_standing(&self) -> Result<Standing, SiteError> {
        let answer = self
            .call_found(Priority::CanWait, Method::GET, ".json", None)
            .await?;
        let Data { data: tournament }: Data<Resource<TournamentAttributes>> = parse(answer)?;
        if let Some(state) = tournament.attributes.state {
            return Ok(match state.as_str() {
                state if OVER.contains(&state) => Standing::Finished,
                IN_REVIEW => Standing::PlayedOut,
                _ => Standing::Running,
            });
        }
        let played_out = self
            .any(Priority::CanWait, "/matches.json?state=complete")
            .await?
            && !self
                .any(Priority::CanWait, "/matches.json?state=pending")
                .await?;
        Ok(if played_out {
            Standing::PlayedOut
        } else {
            Standing::Running
        })
    }

    async fn send_underway(&self, id: &str) -> Result<(), SiteError> {
        let path = format!("/matches/{}/change_state.json", checked(id)?);
        let body = json!({ "data": { "type": "MatchState", "attributes": { "state": "mark_as_underway" } } });
        self.call_found(Priority::CanWait, Method::PUT, &path, Some(body))
            .await?;
        Ok(())
    }

    /// Sends the score. It counts as sent only when Challonge's answer (the
    /// match, as Challonge documents) shows the match decided for this
    /// winner, or, when the answer does not, a fresh read of the match does.
    async fn send_report(&self, id: &str, report: &Report) -> Result<(), SiteError> {
        let path = format!("/matches/{}.json", checked(id)?);
        let scored: Vec<Value> = (0..2)
            .map(|index| {
                let won = index == report.winner;
                json!({
                    "participant_id": report.entries[index],
                    "score_set": report.games[index].to_string(),
                    "rank": if won { 1 } else { 2 },
                    "advancing": won,
                })
            })
            .collect();
        let body = json!({ "data": { "type": "match", "attributes": { "match": scored } } });
        let answer = self
            .call_found(Priority::Always, Method::PUT, &path, Some(body))
            .await?;
        let winner = &report.entries[report.winner];
        let decided_for = |set: &RawSet| {
            set.id == id && set.status == Status::Decided && set.winner.as_ref() == Some(winner)
        };
        let answered = parse::<Data<Resource<MatchAttributes>>>(answer)
            .ok()
            .map(|found| RawSet::from(found.data));
        if answered.as_ref().is_some_and(decided_for) {
            return Ok(());
        }
        // No sign of the score in the answer: the match itself decides.
        match self.read_set(id, Purpose::Result).await? {
            Some(set) if set.status == Status::Decided && set.winner.as_ref() == Some(winner) => {
                Ok(())
            }
            Some(set)
                if set.status == Status::Open
                    && set.entry_ids()
                        == Some([report.entries[0].as_str(), report.entries[1].as_str()]) =>
            {
                Err(SiteError::Deferred(
                    "Challonge's answer did not show the score recorded, and the match is still open"
                        .into(),
                ))
            }
            _ => Err(SiteError::Refused(
                "after the score was sent, Challonge shows the match decided otherwise or changed"
                    .into(),
            )),
        }
    }
}

impl Site for Challonge {
    fn open_sets(&self, purpose: Purpose) -> SiteFuture<'_, Vec<Set>> {
        Box::pin(self.read_open_sets(purpose))
    }

    fn set<'a>(&'a self, id: &'a str, purpose: Purpose) -> SiteFuture<'a, Option<Set>> {
        Box::pin(self.read_set(id, purpose))
    }

    fn standing(&self) -> SiteFuture<'_, Standing> {
        Box::pin(self.read_standing())
    }

    fn mark_underway<'a>(&'a self, id: &'a str) -> SiteFuture<'a, ()> {
        Box::pin(self.send_underway(id))
    }

    fn report<'a>(&'a self, id: &'a str, report: &'a Report) -> SiteFuture<'a, Reported> {
        Box::pin(async move {
            self.send_report(id, report).await?;
            Ok(Reported::Sent)
        })
    }
}

fn unexpected() -> SiteError {
    SiteError::Unavailable("Challonge answered something unexpected".into())
}

fn parse<T: DeserializeOwned>(value: Value) -> Result<T, SiteError> {
    serde_json::from_value(value).map_err(|_| unexpected())
}

/// A set ID taken from Challonge, before it goes back into a URL.
fn checked(id: &str) -> Result<&str, SiteError> {
    if is_slug(id) {
        Ok(id)
    } else {
        Err(SiteError::Refused(format!("{id:?} is not a Challonge ID")))
    }
}

/// The `detail`s of a JSON:API error answer, which Challonge documents both
/// as `{"errors": [...]}` and as a bare array.
fn error_detail(body: &[u8]) -> Option<String> {
    let value: Value = serde_json::from_slice(body).ok()?;
    let errors = value.get("errors").unwrap_or(&value).as_array()?;
    let details: Vec<&str> = errors
        .iter()
        .filter_map(|error| error.get("detail")?.as_str())
        .collect();
    (!details.is_empty()).then(|| details.join("; ").chars().take(300).collect())
}

#[derive(Deserialize)]
struct Data<T> {
    data: T,
}

#[derive(Deserialize)]
struct Resource<A> {
    #[serde(deserialize_with = "id")]
    id: String,
    attributes: A,
    /// Where standard JSON:API puts a match's players.
    #[serde(default)]
    relationships: Option<Players>,
}

#[derive(Deserialize)]
struct MatchAttributes {
    state: String,
    #[serde(default)]
    round: Option<i32>,
    #[serde(default, deserialize_with = "optional_id")]
    winner_id: Option<String>,
    /// Not in the documented schema; read when Challonge sends it.
    #[serde(default)]
    underway_at: Option<Value>,
    /// Where Challonge's schema puts them.
    #[serde(default)]
    relationships: Option<Players>,
}

#[derive(Deserialize)]
struct Players {
    #[serde(default)]
    player1: Option<Related>,
    #[serde(default)]
    player2: Option<Related>,
}

#[derive(Deserialize)]
struct Related {
    #[serde(default)]
    data: Option<RelatedData>,
}

#[derive(Deserialize)]
struct RelatedData {
    #[serde(deserialize_with = "id")]
    id: String,
}

#[derive(Deserialize)]
struct TournamentAttributes {
    #[serde(default)]
    state: Option<String>,
}

#[derive(Deserialize)]
struct ParticipantAttributes {
    #[serde(default)]
    username: Option<String>,
}

/// A Challonge ID, string or integer, which goes back into URLs.
fn id<'de, D: Deserializer<'de>>(deserializer: D) -> Result<String, D::Error> {
    let text = http::id(deserializer)?;
    if is_slug(&text) {
        Ok(text)
    } else {
        Err(serde::de::Error::custom("not a Challonge ID"))
    }
}

fn optional_id<'de, D: Deserializer<'de>>(deserializer: D) -> Result<Option<String>, D::Error> {
    Ok(http::optional_id(deserializer)?.filter(|text| !text.is_empty()))
}

/// A match before its participants' usernames are attached.
struct RawSet {
    id: String,
    round: i32,
    status: Status,
    underway: bool,
    winner: Option<String>,
    players: Option<[String; 2]>,
}

impl From<Resource<MatchAttributes>> for RawSet {
    fn from(resource: Resource<MatchAttributes>) -> Self {
        let attributes = resource.attributes;
        let status = match (attributes.state.as_str(), &attributes.winner_id) {
            ("complete", _) | (_, Some(_)) => Status::Decided,
            ("open", None) => Status::Open,
            _ => Status::Pending,
        };
        let players = attributes
            .relationships
            .or(resource.relationships)
            .and_then(|players| {
                let id = |slot: Option<Related>| Some(slot?.data?.id);
                Some([id(players.player1)?, id(players.player2)?])
            });
        Self {
            id: resource.id,
            round: attributes.round.unwrap_or(0),
            status,
            underway: attributes.underway_at.is_some(),
            winner: attributes.winner_id,
            players,
        }
    }
}

impl RawSet {
    fn into_set(self, entries: Option<[Entry; 2]>) -> Set {
        Set {
            round_label: round_label(self.round),
            id: self.id,
            round: self.round,
            entries,
            status: self.status,
            underway: self.underway,
            winner: self.winner,
        }
    }
}

/// Challonge has no round names; its losers rounds are numbered below zero.
fn round_label(round: i32) -> String {
    match round {
        0 => "Match".into(),
        round if round < 0 => format!("Losers round {}", round.unsigned_abs()),
        round => format!("Round {round}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn raw(value: Value) -> RawSet {
        RawSet::from(serde_json::from_value::<Resource<MatchAttributes>>(value).unwrap())
    }

    #[test]
    fn ids_are_strings_or_integers() {
        let set = raw(json!({
            "id": 8008135,
            "type": "match",
            "attributes": {
                "state": "open",
                "round": -2,
                "relationships": {
                    "player1": { "data": { "id": "355", "type": "participant" } },
                    "player2": { "data": { "id": 354, "type": "participant" } },
                },
            },
        }));
        assert_eq!(set.id, "8008135");
        assert_eq!(set.players, Some(["355".into(), "354".into()]));
        assert_eq!((set.round, set.status), (-2, Status::Open));

        // Ids that cannot go back into a URL are refused.
        for bad in [json!("12/34"), json!(""), json!(-1), json!(1.5)] {
            let parsed = serde_json::from_value::<Resource<MatchAttributes>>(
                json!({ "id": bad, "attributes": { "state": "open" } }),
            );
            assert!(parsed.is_err(), "{bad}");
        }
    }

    #[test]
    fn players_are_found_where_either_document_puts_them_or_are_missing() {
        let standard = raw(json!({
            "id": "1",
            "attributes": { "state": "open", "round": 1 },
            "relationships": {
                "player1": { "data": { "id": "7" } },
                "player2": { "data": { "id": "8" } },
            },
        }));
        assert_eq!(standard.players, Some(["7".into(), "8".into()]));
        let waiting = raw(json!({
            "id": "2",
            "attributes": {
                "state": "pending",
                "relationships": { "player1": { "data": { "id": "7" } }, "player2": { "data": null } },
            },
        }));
        assert_eq!((waiting.players, waiting.status), (None, Status::Pending));
    }

    #[test]
    fn a_winner_or_completion_is_a_result() {
        let decided =
            |attributes: Value| raw(json!({ "id": "1", "attributes": attributes })).status;
        assert_eq!(decided(json!({ "state": "complete" })), Status::Decided);
        assert_eq!(
            decided(json!({ "state": "open", "winner_id": 355 })),
            Status::Decided
        );
        assert_eq!(
            decided(json!({ "state": "open", "winner_id": null })),
            Status::Open
        );
    }

    #[test]
    fn underway_is_read_when_challonge_sends_it() {
        let underway =
            |attributes: Value| raw(json!({ "id": "1", "attributes": attributes })).underway;
        assert!(underway(
            json!({ "state": "open", "underway_at": "2026-10-18T19:05:00Z" })
        ));
        assert!(!underway(json!({ "state": "open", "underway_at": null })));
        assert!(!underway(json!({ "state": "open" })));
    }

    #[test]
    fn queries_join_paths_either_way() {
        assert_eq!(
            with_query("/matches.json", "page=1"),
            "/matches.json?page=1"
        );
        assert_eq!(
            with_query("/matches.json?state=open", "page=1"),
            "/matches.json?state=open&page=1"
        );
    }

    #[test]
    fn rounds_are_labelled_by_side() {
        assert_eq!(round_label(3), "Round 3");
        assert_eq!(round_label(-1), "Losers round 1");
        assert_eq!(round_label(0), "Match");
    }

    #[test]
    fn errors_are_read_in_either_documented_shape() {
        let wrapped =
            br#"{"errors":[{"status":422,"detail":"Match is not open"},{"detail":"Second"}]}"#;
        assert_eq!(
            error_detail(wrapped).as_deref(),
            Some("Match is not open; Second")
        );
        let bare = br#"[{"status":422,"detail":"Bare"}]"#;
        assert_eq!(error_detail(bare).as_deref(), Some("Bare"));
        assert_eq!(error_detail(b"<html>"), None);
    }
}
