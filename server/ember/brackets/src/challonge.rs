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
use serde::{
    Deserialize, Deserializer,
    de::{DeserializeOwned, IgnoredAny},
};
use serde_json::{Value, json};
use zeroize::Zeroizing;

use crate::{
    Clock,
    budget::{Budget, Priority},
    config::{CredentialKind, is_slug},
    http::{self, http_client},
    roster::Roster,
    site::{
        Account, Entry, Extras, Phase, Purpose, Report, Reported, Set, Site, SiteError, SiteFuture,
        Standing, Status,
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

    /// One request, answered with the body of a success, however it reads.
    /// `None` when Challonge answers 404.
    async fn fetch(
        &self,
        priority: Priority,
        method: Method,
        path: &str,
        body: Option<Value>,
    ) -> Result<Option<Vec<u8>>, SiteError> {
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
            return Ok(Some(body));
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

    /// One request, its answer read as JSON. `None` when Challonge answers 404.
    async fn call(
        &self,
        priority: Priority,
        method: Method,
        path: &str,
        body: Option<Value>,
    ) -> Result<Option<Value>, SiteError> {
        let Some(body) = self.fetch(priority, method, path, body).await? else {
            return Ok(None);
        };
        if body.is_empty() {
            Ok(Some(Value::Null))
        } else {
            serde_json::from_slice(&body)
                .map(Some)
                .map_err(|_| unexpected())
        }
    }

    /// A request whose 404 means the request itself is wrong, its answer read
    /// as a `T`.
    async fn fetch_as<T: DeserializeOwned>(
        &self,
        priority: Priority,
        method: Method,
        path: &str,
        body: Option<Value>,
    ) -> Result<T, SiteError> {
        let body = self
            .fetch(priority, method, path, body)
            .await?
            .ok_or_else(|| SiteError::Refused("Challonge answered 404".into()))?;
        parse_body(&body)
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
            let Data { data: page }: Data<Vec<T>> =
                self.fetch_as(priority, Method::GET, &path, None).await?;
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
        let Data { data: items }: Data<Vec<IgnoredAny>> =
            self.fetch_as(priority, Method::GET, &path, None).await?;
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
            .fetch(priority(purpose), Method::GET, &path, None)
            .await?
        else {
            return Ok(None);
        };
        let Data { data: resource }: Data<Resource<MatchAttributes>> = parse_body(&answer)?;
        Ok(Some(self.set_of(RawSet::from(resource))))
    }

    /// The tournament's `state` decides when Challonge gives it; otherwise
    /// the matches say whether everything looks played.
    async fn read_standing(&self) -> Result<Standing, SiteError> {
        let Data { data: tournament }: Data<Resource<TournamentAttributes>> = self
            .fetch_as(Priority::CanWait, Method::GET, ".json", None)
            .await?;
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

fn parse_body<T: DeserializeOwned>(body: &[u8]) -> Result<T, SiteError> {
    serde_json::from_slice(body).map_err(|_| unexpected())
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
    /// Not in the documented schema; read when Challonge sends it. The live
    /// answer has it in `timestamps`, which is read too.
    #[serde(default)]
    underway_at: Option<Value>,
    #[serde(default)]
    timestamps: Option<Value>,
    /// Each participant of the match, first player first. The live answer
    /// has no `player1` and `player2`; this is where it names the players
    /// (`listed_players`). A loose value on purpose: a strict type would
    /// fail the whole page of matches on one entry it did not expect, and
    /// `listed_players` checks what it needs.
    #[serde(default)]
    points_by_participant: Option<Value>,
    /// Set on a group stage match of a two stage tournament, as Challonge's
    /// upgrading notes say. Not in the documented schema, whose default is
    /// placeholder text, so only a number, or a string of digits, counts:
    /// unverified live (`in_group`).
    #[serde(default)]
    group_id: Option<Value>,
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
    /// A group stage match, as `group_id` says.
    in_group: bool,
}

/// The stages of a Challonge tournament as round rules name them: the
/// group stage of a two stage tournament, and the final stage (or the only
/// one).
pub const GROUP_STAGE: &str = "group";
pub const FINAL_STAGE: &str = "final";

/// Whether a match's `group_id` says it is in a group: a number, or a string
/// of digits. Anything else, such as the schema's placeholder text, is not.
fn in_group(group_id: Option<&Value>) -> bool {
    match group_id {
        Some(Value::Number(_)) => true,
        Some(Value::String(text)) => {
            !text.is_empty() && text.bytes().all(|byte| byte.is_ascii_digit())
        }
        _ => false,
    }
}

/// The two players a match's `points_by_participant` lists, first player
/// first: exactly two entries, each with a participant ID Challonge can have
/// (a number or a string of one), and two different players. A match whose
/// players are not known yet lists fewer, or none with an ID (not read live:
/// only a match with both players has been).
fn listed_players(points: Option<&Value>) -> Option<[String; 2]> {
    let entries = points?.as_array()?;
    let id = |entry: &Value| {
        match entry.get("participant_id")? {
            Value::Number(number) => number.as_u64().map(|id| id.to_string()),
            Value::String(text) => Some(text.clone()),
            _ => None,
        }
        .filter(|text| is_slug(text))
    };
    match entries.as_slice() {
        [one, two] => distinct([id(one)?, id(two)?]),
        _ => None,
    }
}

/// The pair when it names two different players: a set is never a player
/// against themselves.
fn distinct(pair: [String; 2]) -> Option<[String; 2]> {
    (pair[0] != pair[1]).then_some(pair)
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
                distinct([id(players.player1)?, id(players.player2)?])
            })
            .or_else(|| listed_players(attributes.points_by_participant.as_ref()));
        let in_group = in_group(attributes.group_id.as_ref());
        let underway = attributes.underway_at.is_some()
            || attributes
                .timestamps
                .as_ref()
                .and_then(|timestamps| timestamps.get("underway_at"))
                .is_some_and(|at| !at.is_null());
        Self {
            id: resource.id,
            round: attributes.round.unwrap_or(0),
            status,
            underway,
            winner: attributes.winner_id,
            players,
            in_group,
        }
    }
}

impl RawSet {
    fn into_set(self, entries: Option<[Entry; 2]>) -> Set {
        let (round_label, stage) = if self.in_group {
            (
                format!("Group stage, {}", round_label(self.round)),
                GROUP_STAGE,
            )
        } else {
            (round_label(self.round), FINAL_STAGE)
        };
        Set {
            round_label,
            id: self.id,
            round: self.round,
            entries,
            status: self.status,
            underway: self.underway,
            winner: self.winner,
            extras: Extras {
                phase: Some(Phase::named(stage)),
                ..Extras::default()
            },
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
        // The live answer keeps it in `timestamps`.
        assert!(underway(
            json!({ "state": "open", "timestamps": { "underway_at": "2026-10-19T17:20:00.000Z" } })
        ));
        assert!(!underway(
            json!({ "state": "open", "timestamps": { "underway_at": null } })
        ));
    }

    /// A match as Challonge's v2.1 API answered for a started single
    /// elimination tournament (read live, ids changed): the players are only
    /// in `points_by_participant`, and `underway_at` is in `timestamps`.
    #[test]
    fn the_live_answer_names_its_players_in_points_by_participant() {
        let open = raw(json!({
            "id": "100200300",
            "type": "match",
            "attributes": {
                "state": "open",
                "round": 1,
                "identifier": "A",
                "scores": "0 - 0",
                "suggested_play_order": 1,
                "location": null,
                "score_in_sets": [],
                "points_by_participant": [
                    { "participant_id": 555001, "scores": [] },
                    { "participant_id": 555002, "scores": [] },
                ],
                "timestamps": {
                    "started_at": "2026-10-19T17:17:55.208Z",
                    "created_at": "2026-10-19T17:17:55.175Z",
                    "updated_at": "2026-10-19T17:17:55.208Z",
                    "underway_at": null,
                },
                "winner_id": null,
                "tie": false,
                "scheduled_time": null,
            },
            "relationships": { "attachments": { "data": [] } },
        }));
        assert_eq!(open.players, Some(["555001".into(), "555002".into()]));
        assert_eq!(
            (open.status, open.underway, open.round),
            (Status::Open, false, 1)
        );

        // A match not fed yet lists fewer players, or none with an ID.
        for points in [
            json!([]),
            json!([{ "participant_id": 555001, "scores": [] }]),
            json!([{ "participant_id": 555001 }, { "participant_id": null }]),
            json!([{ "participant_id": 555001 }, { "participant_id": "a/b" }]),
            // The same player twice is no set.
            json!([{ "participant_id": 5 }, { "participant_id": "5" }]),
            json!([{ "participant_id": 1 }, { "participant_id": 2 }, { "participant_id": 3 }]),
            json!("555001"),
        ] {
            let pending = raw(json!({
                "id": "2",
                "attributes": { "state": "pending", "points_by_participant": points },
            }));
            assert_eq!(pending.players, None, "{points}");
        }
        // The documented relationships win when a document has both.
        let both = raw(json!({
            "id": "3",
            "attributes": {
                "state": "open",
                "relationships": { "player1": { "data": { "id": "7" } }, "player2": { "data": { "id": "8" } } },
                "points_by_participant": [{ "participant_id": 9 }, { "participant_id": 10 }],
            },
        }));
        assert_eq!(both.players, Some(["7".into(), "8".into()]));
        // The same player twice in the relationships is no set either.
        let twice = raw(json!({
            "id": "4",
            "attributes": {
                "state": "open",
                "relationships": { "player1": { "data": { "id": "7" } }, "player2": { "data": { "id": "7" } } },
            },
        }));
        assert_eq!(twice.players, None);
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
    fn a_match_is_in_a_group_only_when_its_group_id_is_a_number() {
        let in_group = |value: Value| in_group(Some(&value));
        assert!(in_group(json!(7001)));
        assert!(in_group(json!("7001")));
        // Null, the schema's placeholder text, and anything else.
        assert!(!in_group(Value::Null));
        assert!(!in_group(json!("")));
        assert!(!in_group(json!("Group ID")));
        assert!(!in_group(json!("70 01")));
        assert!(!in_group(json!(true)));
        assert!(!in_group(json!({ "id": 7 })));
        assert!(!super::in_group(None));
        // And it goes into the label and the stage the rules read.
        let set = |group: Value| {
            raw(json!({ "id": 1, "type": "match", "attributes": { "state": "open", "round": 2, "group_id": group } }))
                .into_set(None)
        };
        let grouped = set(json!("7001"));
        assert_eq!(grouped.round_label, "Group stage, Round 2");
        assert_eq!(grouped.extras.phase, Some(Phase::named(GROUP_STAGE)));
        let placeholder = set(json!("Group ID"));
        assert_eq!(placeholder.round_label, "Round 2");
        assert_eq!(placeholder.extras.phase, Some(Phase::named(FINAL_STAGE)));
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
