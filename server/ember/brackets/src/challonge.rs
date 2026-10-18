//! The Challonge v2.1 adapter (docs/design/challonge/CHALLONGE_API.md).
//!
//! Challonge's documents mix string and integer IDs and disagree on where a
//! match's players sit, so both are read leniently. Every request is counted
//! against the credential's monthly budget before it is sent.
use std::{collections::HashMap, sync::Mutex};

use reqwest::{
    Method,
    header::{ACCEPT, AUTHORIZATION, CONTENT_TYPE, HeaderMap, HeaderValue},
};
use serde::{Deserialize, Deserializer, de::DeserializeOwned};
use serde_json::{Value, json};
use zeroize::Zeroizing;

use crate::{
    budget::Budget,
    config::CredentialKind,
    http::{bounded_body, http_client},
    site::{Entry, Need, Report, Set, Site, SiteError, SiteFuture, Status},
};

pub const API: &str = "https://api.challonge.com/v2.1";
/// Challonge's documented default page size, which it therefore serves in
/// full; a shorter page is the last. Its maximum is not documented.
const PER_PAGE: usize = 25;
/// Matches were once paginated without end (changelog, 2026-09-14), so a
/// list that keeps going is refused rather than read forever.
const MAX_PAGES: u32 = 40;
const MAX_ANSWER: usize = 1024 * 1024;
const MAX_ID: usize = 64;

pub(crate) struct Challonge {
    http: reqwest::Client,
    /// `<api>/tournaments/<tournament>`.
    base: String,
    /// Every request's headers, with the credential marked sensitive.
    headers: HeaderMap,
    budget: Budget,
    /// Participant ID to Challonge username, refreshed when a set names a
    /// participant not in it.
    usernames: Mutex<HashMap<String, Option<String>>>,
}

impl Challonge {
    pub fn new(
        api: &str,
        tournament: &str,
        kind: CredentialKind,
        credential: &str,
        budget: Budget,
    ) -> Result<Self, String> {
        let (version, value) = match kind {
            CredentialKind::V1Key => ("v1", Zeroizing::new(credential.to_owned())),
            CredentialKind::Bearer => ("v2", Zeroizing::new(format!("Bearer {credential}"))),
        };
        let mut authorization = HeaderValue::from_str(&value)
            .map_err(|_| "the Challonge credential is not a valid header value")?;
        authorization.set_sensitive(true);
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
            base: format!("{}/tournaments/{tournament}", api.trim_end_matches('/')),
            headers,
            budget,
            usernames: Mutex::default(),
        })
    }

    async fn call(
        &self,
        need: Need,
        method: Method,
        path: &str,
        body: Option<Value>,
    ) -> Result<Value, SiteError> {
        self.budget.spend(need)?;
        let mut request = self
            .http
            .request(method, format!("{}{path}", self.base))
            .headers(self.headers.clone());
        if let Some(body) = body {
            request = request.body(body.to_string());
        }
        let response = request
            .send()
            .await
            .map_err(|_| SiteError::Unavailable("Challonge could not be reached".into()))?;
        let status = response.status();
        let body = bounded_body(response, MAX_ANSWER).await.ok_or_else(|| {
            SiteError::Unavailable("Challonge's answer was cut off or too long".into())
        })?;
        if status.is_success() {
            return if body.is_empty() {
                Ok(Value::Null)
            } else {
                serde_json::from_slice(&body).map_err(|_| unexpected())
            };
        }
        let message = match error_detail(&body) {
            Some(detail) => format!("Challonge answered {}: {detail}", status.as_u16()),
            None => format!("Challonge answered {}", status.as_u16()),
        };
        Err(match status.as_u16() {
            404 => SiteError::Gone,
            // A credential Challonge refuses is the operator's to fix; until
            // then nothing is given up.
            401 | 403 | 429 => SiteError::Unavailable(message),
            _ if status.is_server_error() => SiteError::Unavailable(message),
            _ => SiteError::Refused(message),
        })
    }

    /// Every item of a list, page by page until a short page.
    async fn list<T: DeserializeOwned>(&self, need: Need, path: &str) -> Result<Vec<T>, SiteError> {
        let separator = if path.contains('?') { '&' } else { '?' };
        let mut items = Vec::new();
        for page in 1..=MAX_PAGES {
            let path = format!("{path}{separator}page={page}&per_page={PER_PAGE}");
            let answer = self.call(need, Method::GET, &path, None).await?;
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
    async fn any(&self, need: Need, path: &str) -> Result<bool, SiteError> {
        let answer = self
            .call(
                need,
                Method::GET,
                &format!("{path}&page=1&per_page=1"),
                None,
            )
            .await?;
        let Data { data: items }: Data<Vec<Value>> = parse(answer)?;
        Ok(!items.is_empty())
    }

    async fn refresh_usernames(&self, need: Need) -> Result<(), SiteError> {
        let participants: Vec<Resource<ParticipantAttributes>> =
            self.list(need, "/participants.json").await?;
        let usernames = participants
            .into_iter()
            .map(|participant| {
                let username = participant
                    .attributes
                    .username
                    .filter(|name| !name.is_empty());
                (participant.id, username)
            })
            .collect();
        *self.usernames.lock().unwrap_or_else(|e| e.into_inner()) = usernames;
        Ok(())
    }

    fn entries_of(&self, raw: &RawSet) -> Option<[Entry; 2]> {
        let usernames = self.usernames.lock().unwrap_or_else(|e| e.into_inner());
        let entry = |id: &String| Entry {
            participant_id: id.clone(),
            username: usernames.get(id).cloned().flatten(),
        };
        raw.players.as_ref().map(|[a, b]| [entry(a), entry(b)])
    }

    fn knows(&self, id: &str) -> bool {
        self.usernames
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .contains_key(id)
    }

    async fn read_open_sets(&self, need: Need) -> Result<Vec<Set>, SiteError> {
        let matches: Vec<Resource<MatchAttributes>> =
            self.list(need, "/matches.json?state=open").await?;
        let raw: Vec<RawSet> = matches.into_iter().map(RawSet::from).collect();
        let unknown = raw
            .iter()
            .flat_map(|set| set.players.iter().flatten())
            .any(|id| !self.knows(id));
        if unknown {
            self.refresh_usernames(need).await?;
        }
        Ok(raw
            .into_iter()
            .map(|raw| {
                let entries = self.entries_of(&raw);
                raw.into_set(entries)
            })
            .collect())
    }

    /// The tournament's `state` decides, when Challonge gives it (the
    /// documented schema does not); otherwise matches have been decided and
    /// none waits for an earlier one.
    async fn read_finished(&self, need: Need) -> Result<bool, SiteError> {
        let answer = self.call(need, Method::GET, ".json", None).await?;
        let Data { data: tournament }: Data<Resource<TournamentAttributes>> = parse(answer)?;
        if let Some(state) = tournament.attributes.state {
            return Ok(matches!(
                state.as_str(),
                "complete" | "awaiting_review" | "ended"
            ));
        }
        Ok(self.any(need, "/matches.json?state=complete").await?
            && !self.any(need, "/matches.json?state=pending").await?)
    }

    async fn change_state(&self, need: Need, id: &str, state: &str) -> Result<(), SiteError> {
        let path = format!("/matches/{}/change_state.json", checked(id)?);
        let body = json!({ "data": { "type": "MatchState", "attributes": { "state": state } } });
        self.call(need, Method::PUT, &path, Some(body)).await?;
        Ok(())
    }

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
        self.call(Need::Result, Method::PUT, &path, Some(body))
            .await?;
        Ok(())
    }
}

impl Site for Challonge {
    fn open_sets(&self, need: Need) -> SiteFuture<'_, Vec<Set>> {
        Box::pin(self.read_open_sets(need))
    }

    fn finished(&self, need: Need) -> SiteFuture<'_, bool> {
        Box::pin(self.read_finished(need))
    }

    fn mark_underway<'a>(&'a self, id: &'a str) -> SiteFuture<'a, ()> {
        Box::pin(self.change_state(Need::Poll, id, "mark_as_underway"))
    }

    fn report<'a>(&'a self, id: &'a str, report: &'a Report) -> SiteFuture<'a, ()> {
        Box::pin(self.send_report(id, report))
    }

    fn reopen<'a>(&'a self, id: &'a str) -> SiteFuture<'a, ()> {
        Box::pin(self.change_state(Need::Result, id, "reopen"))
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
    if is_id(id) {
        Ok(id)
    } else {
        Err(SiteError::Gone)
    }
}

fn is_id(text: &str) -> bool {
    !text.is_empty()
        && text.len() <= MAX_ID
        && text
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-'))
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

/// An ID given as a string or as an integer.
#[derive(Deserialize)]
#[serde(untagged)]
enum RawId {
    Text(String),
    Number(u64),
}

impl RawId {
    fn into_text(self) -> String {
        match self {
            Self::Text(text) => text,
            Self::Number(number) => number.to_string(),
        }
    }
}

fn id<'de, D: Deserializer<'de>>(deserializer: D) -> Result<String, D::Error> {
    let text = RawId::deserialize(deserializer)?.into_text();
    if is_id(&text) {
        Ok(text)
    } else {
        Err(serde::de::Error::custom("not a Challonge ID"))
    }
}

fn optional_id<'de, D: Deserializer<'de>>(deserializer: D) -> Result<Option<String>, D::Error> {
    Ok(Option::<RawId>::deserialize(deserializer)?
        .map(RawId::into_text)
        .filter(|text| !text.is_empty()))
}

/// A match before its participants' usernames are attached.
struct RawSet {
    id: String,
    round: i32,
    status: Status,
    underway: bool,
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
