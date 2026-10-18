//! The start.gg adapter (docs/design/challonge/STARTGG_API.md): one event,
//! read and reported through start.gg's GraphQL API with an organizer's
//! token (an account with a reporter role on the tournament).
//!
//! Entrants are found by the start.gg user behind them (`user.id`, looked
//! up on the bridge site given) and, failing that, by the Discord account
//! they linked on start.gg. Sets with a bye or a `preview` ID are never
//! offered: start.gg replaces preview sets when their phase starts. Every
//! request waits its turn under start.gg's rate limit, shared by everything
//! that uses the same token.
use std::{collections::HashMap, sync::Mutex};

use reqwest::{
    StatusCode,
    header::{AUTHORIZATION, CONTENT_TYPE, HeaderMap, HeaderValue},
};
use serde::{Deserialize, Deserializer, de::DeserializeOwned};
use serde_json::{Value, json};
use tokio::sync::OnceCell;

use crate::{
    http::{bounded_body, http_client},
    limit::Limiter,
    site::{Account, Entry, Purpose, Report, Set, Site, SiteError, SiteFuture, Standing, Status},
};

/// start.gg's API origin. The organizer's token goes nowhere else
/// (`config::is_site_origin`).
pub const API_ORIGIN: &str = "https://api.start.gg";
const API_PATH: &str = "/gql/alpha";
/// start.gg allows 80 requests in any 60 seconds per token; this leaves a
/// margin for the organizer's own tools.
pub const RATE: (usize, u64) = (70, 60);
/// Sets and entrants per page: well under start.gg's 1000 objects a
/// request with the fields asked for here.
const PER_PAGE: u32 = 40;
/// A list of more pages than this is refused rather than read forever.
const MAX_PAGES: u32 = 50;
const MAX_ANSWER: usize = 2 * 1024 * 1024;

/// `Set.state` values (section 3.4): not started, started, done, called.
const CREATED: i64 = 1;
const STARTED: i64 = 2;
const COMPLETED: i64 = 3;
const CALLED: i64 = 6;

const EVENT: &str = "query Event($slug: String) { event(slug: $slug) { id state } }";
const ENTRANTS: &str = "query Entrants($id: ID!, $page: Int!, $perPage: Int!) { event(id: $id) { entrants(query: {page: $page, perPage: $perPage}) { pageInfo { totalPages } nodes { id participants { user { id authorizations(types: [DISCORD]) { externalId } } } } } } }";
const SETS: &str = "query Sets($id: ID!, $page: Int!, $perPage: Int!) { event(id: $id) { sets(page: $page, perPage: $perPage, sortType: ROUND, filters: {state: [1, 2, 6], hideEmpty: true}) { pageInfo { totalPages } nodes { id state round fullRoundText winnerId slots { slotIndex entrant { id } } } } } }";
const SET: &str = "query Set($id: ID!) { set(id: $id) { id state round fullRoundText winnerId slots { slotIndex entrant { id } } } }";
const UNDERWAY: &str =
    "mutation Underway($setId: ID!) { markSetInProgress(setId: $setId) { id state } }";
const REPORT: &str = "mutation Report($setId: ID!, $winnerId: ID!, $gameData: [BracketSetGameDataInput]) { reportBracketSet(setId: $setId, winnerId: $winnerId, gameData: $gameData) { id state } }";

pub(crate) struct Startgg {
    http: reqwest::Client,
    url: String,
    headers: HeaderMap,
    limiter: Limiter,
    slug: String,
    bridge_site: String,
    /// The event's ID and state, read once.
    event: OnceCell<String>,
    /// Entrant ID to the accounts behind it, refreshed when a set names an
    /// entrant not in it.
    entrants: Mutex<HashMap<String, Vec<Account>>>,
}

/// An ID given as a string or as a number.
#[derive(Deserialize)]
#[serde(untagged)]
enum WireId {
    Number(i64),
    Text(String),
}

impl WireId {
    fn into_text(self) -> String {
        match self {
            Self::Number(number) => number.to_string(),
            Self::Text(text) => text,
        }
    }
}

fn id<'de, D: Deserializer<'de>>(deserializer: D) -> Result<String, D::Error> {
    WireId::deserialize(deserializer).map(WireId::into_text)
}

fn optional_id<'de, D: Deserializer<'de>>(deserializer: D) -> Result<Option<String>, D::Error> {
    Ok(Option::<WireId>::deserialize(deserializer)?.map(WireId::into_text))
}

/// A real set's ID: decimal digits. Preview sets carry other IDs.
fn is_set_id(id: &str) -> bool {
    !id.is_empty() && id.len() <= 20 && id.bytes().all(|byte| byte.is_ascii_digit())
}

#[derive(Deserialize)]
struct EventData {
    event: Option<EventNode>,
}

#[derive(Deserialize)]
struct EventNode {
    #[serde(deserialize_with = "id")]
    id: String,
    #[serde(default)]
    state: Option<String>,
}

#[derive(Deserialize)]
struct Page<T> {
    #[serde(rename = "pageInfo")]
    page_info: PageInfo,
    #[serde(default = "Vec::new")]
    nodes: Vec<T>,
}

#[derive(Deserialize)]
struct PageInfo {
    #[serde(rename = "totalPages", default)]
    total_pages: Option<u32>,
}

#[derive(Deserialize)]
struct EntrantsData {
    event: Option<EntrantsEvent>,
}

#[derive(Deserialize)]
struct EntrantsEvent {
    entrants: Page<WireEntrant>,
}

#[derive(Deserialize)]
struct WireEntrant {
    #[serde(deserialize_with = "id")]
    id: String,
    #[serde(default)]
    participants: Vec<WireParticipant>,
}

#[derive(Deserialize)]
struct WireParticipant {
    #[serde(default)]
    user: Option<WireUser>,
}

#[derive(Deserialize)]
struct WireUser {
    #[serde(deserialize_with = "id")]
    id: String,
    #[serde(default)]
    authorizations: Option<Vec<WireAuthorization>>,
}

#[derive(Deserialize)]
struct WireAuthorization {
    #[serde(rename = "externalId", default)]
    external_id: Option<String>,
}

#[derive(Deserialize)]
struct SetsData {
    event: Option<SetsEvent>,
}

#[derive(Deserialize)]
struct SetsEvent {
    sets: Page<WireSet>,
}

#[derive(Deserialize)]
struct SetData {
    set: Option<WireSet>,
}

#[derive(Deserialize)]
struct WireSet {
    #[serde(deserialize_with = "id")]
    id: String,
    #[serde(default)]
    state: Option<i64>,
    #[serde(default)]
    round: Option<i32>,
    #[serde(rename = "fullRoundText", default)]
    full_round_text: Option<String>,
    #[serde(rename = "winnerId", default, deserialize_with = "optional_id")]
    winner_id: Option<String>,
    #[serde(default)]
    slots: Vec<WireSlot>,
}

#[derive(Deserialize)]
struct WireSlot {
    #[serde(rename = "slotIndex", default)]
    slot_index: Option<u32>,
    #[serde(default)]
    entrant: Option<WireEntrantId>,
}

#[derive(Deserialize)]
struct WireEntrantId {
    #[serde(deserialize_with = "id")]
    id: String,
}

/// A GraphQL answer, or start.gg's flat refusal (rate limit, complexity).
#[derive(Deserialize)]
struct Envelope {
    #[serde(default)]
    data: Option<Value>,
    #[serde(default)]
    errors: Vec<GraphqlError>,
    #[serde(default)]
    success: Option<bool>,
    #[serde(default)]
    message: Option<String>,
}

#[derive(Deserialize)]
struct GraphqlError {
    #[serde(default)]
    message: String,
}

/// What a set that is not a preview or a bye says, before its entrants'
/// accounts are attached.
struct RawSet {
    id: String,
    round: i32,
    round_label: String,
    status: Status,
    underway: bool,
    winner: Option<String>,
    /// Entrant IDs, slot 0 first.
    entrants: Option<[String; 2]>,
}

impl WireSet {
    fn into_raw(self) -> Option<RawSet> {
        if !is_set_id(&self.id) {
            return None;
        }
        let mut slots: Vec<(u32, Option<String>)> = self
            .slots
            .into_iter()
            .map(|slot| {
                (
                    slot.slot_index.unwrap_or(u32::MAX),
                    slot.entrant.map(|entrant| entrant.id),
                )
            })
            .collect();
        slots.sort_by_key(|(index, _)| *index);
        let entrants = match slots.as_slice() {
            [(0, Some(one)), (1, Some(two))] => Some([one.clone(), two.clone()]),
            _ => None,
        };
        let state = self.state.unwrap_or(0);
        let status = match (&self.winner_id, state) {
            (Some(_), _) | (None, COMPLETED) => Status::Decided,
            (None, CREATED | STARTED | CALLED) if entrants.is_some() => Status::Open,
            _ => Status::Pending,
        };
        let round = self.round.unwrap_or(0);
        Some(RawSet {
            round_label: self
                .full_round_text
                .filter(|text| !text.is_empty())
                .unwrap_or_else(|| format!("Round {round}")),
            id: self.id,
            round,
            status,
            underway: state == STARTED,
            winner: self.winner_id,
            entrants,
        })
    }
}

impl WireEntrant {
    /// The accounts behind a one-player entrant: its start.gg user, then
    /// the Discord account that user linked. A team entrant has none.
    fn accounts(self, bridge_site: &str) -> (String, Vec<Account>) {
        let user = match <[WireParticipant; 1]>::try_from(self.participants) {
            Ok([participant]) => participant.user,
            Err(_) => None,
        };
        let accounts = user.map_or_else(Vec::new, |user| {
            let discord = user
                .authorizations
                .unwrap_or_default()
                .into_iter()
                .filter_map(|authorization| authorization.external_id)
                .filter(|external| !external.is_empty())
                .map(|user_id| Account::Discord { user_id });
            std::iter::once(Account::Site {
                site: bridge_site.to_owned(),
                id: user.id,
            })
            .chain(discord.take(1))
            .collect()
        });
        (self.id, accounts)
    }
}

impl Startgg {
    /// `origin` is start.gg's API origin, checked by the configuration.
    pub fn new(
        origin: &str,
        slug: &str,
        bridge_site: &str,
        token: &str,
        limiter: Limiter,
    ) -> Result<Self, String> {
        let mut authorization = HeaderValue::from_str(&format!("Bearer {token}"))
            .map_err(|_| "the start.gg token is not a valid header value")?;
        authorization.set_sensitive(true);
        let mut headers = HeaderMap::new();
        headers.insert(CONTENT_TYPE, HeaderValue::from_static("application/json"));
        headers.insert(AUTHORIZATION, authorization);
        Ok(Self {
            http: http_client()?,
            url: format!("{origin}{API_PATH}"),
            headers,
            limiter,
            slug: slug.to_owned(),
            bridge_site: bridge_site.to_owned(),
            event: OnceCell::new(),
            entrants: Mutex::default(),
        })
    }

    /// One GraphQL request, waiting its turn under the rate limit. Errors in
    /// the answer are the caller's to read.
    async fn post(&self, query: &str, variables: Value) -> Result<Envelope, SiteError> {
        self.limiter.admit().await;
        let response = self
            .http
            .post(&self.url)
            .headers(self.headers.clone())
            .json(&json!({ "query": query, "variables": variables }))
            .send()
            .await
            .map_err(|_| SiteError::Unavailable("start.gg could not be reached".into()))?;
        let status = response.status();
        let body = bounded_body(response, MAX_ANSWER).await.ok_or_else(|| {
            SiteError::Unavailable("start.gg's answer was cut off or too long".into())
        })?;
        let envelope: Option<Envelope> = serde_json::from_slice(&body).ok();
        if let Some(Envelope {
            success: Some(false),
            message,
            ..
        }) = &envelope
        {
            let message = message.as_deref().unwrap_or("refused");
            return Err(SiteError::Unavailable(format!("start.gg: {message}")));
        }
        match (status, envelope) {
            (status, Some(envelope)) if status.is_success() => Ok(envelope),
            (StatusCode::BAD_REQUEST, Some(envelope)) if !envelope.errors.is_empty() => {
                Ok(envelope)
            }
            (status, _) => Err(SiteError::Unavailable(format!(
                "start.gg answered {}",
                status.as_u16()
            ))),
        }
    }

    /// A query's data; any error in the answer is start.gg failing.
    async fn query<T: DeserializeOwned>(
        &self,
        query: &str,
        variables: Value,
    ) -> Result<T, SiteError> {
        let envelope = self.post(query, variables).await?;
        if let Some(error) = envelope.errors.first() {
            return Err(SiteError::Unavailable(format!(
                "start.gg: {}",
                error.message
            )));
        }
        envelope
            .data
            .and_then(|data| serde_json::from_value(data).ok())
            .ok_or_else(unexpected)
    }

    /// A mutation; `Ok(Err(message))` when start.gg refused it.
    async fn mutate(&self, query: &str, variables: Value) -> Result<Result<(), String>, SiteError> {
        let envelope = self.post(query, variables).await?;
        Ok(match envelope.errors.into_iter().next() {
            Some(error) if error.message == "An unknown error has occurred" => {
                return Err(SiteError::Unavailable(format!(
                    "start.gg: {}",
                    error.message
                )));
            }
            Some(error) => Err(error.message),
            None => Ok(()),
        })
    }

    async fn event_id(&self) -> Result<&str, SiteError> {
        self.event
            .get_or_try_init(|| async {
                let found: EventData = self.query(EVENT, json!({ "slug": self.slug })).await?;
                found.event.map(|event| event.id).ok_or_else(|| {
                    SiteError::Refused(format!("start.gg has no event {}", self.slug))
                })
            })
            .await
            .map(String::as_str)
    }

    /// Every item of a paged list under the event.
    async fn pages<T: DeserializeOwned, P: DeserializeOwned>(
        &self,
        query: &str,
        page_of: impl Fn(P) -> Option<Page<T>>,
    ) -> Result<Vec<T>, SiteError> {
        let event = self.event_id().await?.to_owned();
        let mut items = Vec::new();
        for page in 1..=MAX_PAGES {
            let variables = json!({ "id": event, "page": page, "perPage": PER_PAGE });
            let data: P = self.query(query, variables).await?;
            let found = page_of(data).ok_or_else(|| {
                SiteError::Refused(format!("start.gg has no event {}", self.slug))
            })?;
            let last = found
                .page_info
                .total_pages
                .is_none_or(|total| page >= total)
                || found.nodes.is_empty();
            items.extend(found.nodes);
            if last {
                return Ok(items);
            }
        }
        Err(SiteError::Unavailable(format!(
            "start.gg listed more than {MAX_PAGES} pages"
        )))
    }

    async fn refresh_entrants(&self) -> Result<(), SiteError> {
        let entrants: Vec<WireEntrant> = self
            .pages(ENTRANTS, |data: EntrantsData| {
                data.event.map(|event| event.entrants)
            })
            .await?;
        let accounts = entrants
            .into_iter()
            .map(|entrant| entrant.accounts(&self.bridge_site))
            .collect();
        *self.entrants.lock().unwrap_or_else(|e| e.into_inner()) = accounts;
        Ok(())
    }

    fn knows(&self, id: &str) -> bool {
        self.entrants
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .contains_key(id)
    }

    fn set_of(&self, raw: RawSet) -> Set {
        let known = self.entrants.lock().unwrap_or_else(|e| e.into_inner());
        let entry = |id: &String| Entry {
            participant_id: id.clone(),
            accounts: known.get(id).cloned().unwrap_or_default(),
        };
        Set {
            entries: raw.entrants.as_ref().map(|[a, b]| [entry(a), entry(b)]),
            id: raw.id,
            round: raw.round,
            round_label: raw.round_label,
            status: raw.status,
            underway: raw.underway,
            winner: raw.winner,
        }
    }

    async fn read_open_sets(&self) -> Result<Vec<Set>, SiteError> {
        let sets: Vec<WireSet> = self
            .pages(SETS, |data: SetsData| data.event.map(|event| event.sets))
            .await?;
        let raw: Vec<RawSet> = sets.into_iter().filter_map(WireSet::into_raw).collect();
        let unknown = raw
            .iter()
            .flat_map(|set| set.entrants.iter().flatten())
            .any(|id| !self.knows(id));
        if unknown {
            self.refresh_entrants().await?;
        }
        Ok(raw.into_iter().map(|raw| self.set_of(raw)).collect())
    }

    async fn read_raw(&self, id: &str) -> Result<Option<RawSet>, SiteError> {
        if !is_set_id(id) {
            return Ok(None);
        }
        let found: SetData = self.query(SET, json!({ "id": id })).await?;
        Ok(found.set.and_then(WireSet::into_raw))
    }

    async fn read_set(&self, id: &str) -> Result<Option<Set>, SiteError> {
        Ok(self.read_raw(id).await?.map(|raw| self.set_of(raw)))
    }

    /// start.gg says when an event is over; until then it is running.
    async fn read_standing(&self) -> Result<Standing, SiteError> {
        let found: EventData = self.query(EVENT, json!({ "slug": self.slug })).await?;
        Ok(match found.event.and_then(|event| event.state).as_deref() {
            Some("COMPLETED") => Standing::Finished,
            _ => Standing::Running,
        })
    }

    async fn send_underway(&self, id: &str) -> Result<(), SiteError> {
        match self.mutate(UNDERWAY, json!({ "setId": id })).await? {
            Ok(()) => Ok(()),
            // Started already, by its players or an earlier attempt.
            Err(message) if message == "Set is already started" => Ok(()),
            Err(message) => Err(SiteError::Refused(format!("start.gg: {message}"))),
        }
    }

    /// Reports the winner and each game's winner in the order played,
    /// after reading the set again: a set with a result is never written
    /// over, since `reportBracketSet` would replace it.
    async fn send_report(&self, id: &str, report: &Report) -> Result<(), SiteError> {
        let current = self
            .read_raw(id)
            .await?
            .ok_or_else(|| SiteError::Refused(format!("start.gg has no set {id}")))?;
        if current.status == Status::Decided {
            return Err(SiteError::Refused("the set already has a result".into()));
        }
        if current.entrants.as_ref() != Some(&report.entries) {
            return Err(SiteError::Refused("the set's entrants have changed".into()));
        }
        let game_data: Vec<Value> = report
            .sequence
            .iter()
            .enumerate()
            .map(|(index, &winner)| {
                json!({ "gameNum": index + 1, "winnerId": report.entries[winner] })
            })
            .collect();
        let variables = json!({
            "setId": id,
            "winnerId": report.entries[report.winner],
            "gameData": game_data,
        });
        self.mutate(REPORT, variables)
            .await?
            .map_err(|message| SiteError::Refused(format!("start.gg: {message}")))
    }
}

impl Site for Startgg {
    /// start.gg counts requests by the minute, not the month: every request
    /// waits its turn, whatever it is for.
    fn open_sets(&self, _purpose: Purpose) -> SiteFuture<'_, Vec<Set>> {
        Box::pin(self.read_open_sets())
    }

    fn set<'a>(&'a self, id: &'a str, _purpose: Purpose) -> SiteFuture<'a, Option<Set>> {
        Box::pin(self.read_set(id))
    }

    fn standing(&self) -> SiteFuture<'_, Standing> {
        Box::pin(self.read_standing())
    }

    fn mark_underway<'a>(&'a self, id: &'a str) -> SiteFuture<'a, ()> {
        Box::pin(self.send_underway(id))
    }

    fn report<'a>(&'a self, id: &'a str, report: &'a Report) -> SiteFuture<'a, ()> {
        Box::pin(self.send_report(id, report))
    }
}

fn unexpected() -> SiteError {
    SiteError::Unavailable("start.gg answered something unexpected".into())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn raw(value: Value) -> Option<RawSet> {
        serde_json::from_value::<WireSet>(value).unwrap().into_raw()
    }

    #[test]
    fn ids_are_numbers_or_strings_and_previews_are_left_out() {
        let set = raw(json!({
            "id": 65089253, "state": 1, "round": -2, "fullRoundText": "Losers Round 2",
            "slots": [
                { "slotIndex": 1, "entrant": { "id": "14250090" } },
                { "slotIndex": 0, "entrant": { "id": 14259653 } },
            ],
        }))
        .unwrap();
        assert_eq!(set.id, "65089253");
        assert_eq!(set.entrants, Some(["14259653".into(), "14250090".into()]));
        assert_eq!(
            (set.status, set.round_label.as_str()),
            (Status::Open, "Losers Round 2")
        );
        assert!(raw(json!({ "id": "preview_5093733_1_0", "state": 1, "slots": [] })).is_none());
    }

    #[test]
    fn a_set_is_open_only_with_two_entrants_and_no_winner() {
        let status = |state: i64, winner: Value, slots: Value| {
            raw(json!({ "id": "1", "state": state, "winnerId": winner, "slots": slots }))
                .map(|set| (set.status, set.underway))
        };
        let both = json!([
            { "slotIndex": 0, "entrant": { "id": 1 } },
            { "slotIndex": 1, "entrant": { "id": 2 } },
        ]);
        assert_eq!(
            status(1, Value::Null, both.clone()),
            Some((Status::Open, false))
        );
        assert_eq!(
            status(2, Value::Null, both.clone()),
            Some((Status::Open, true))
        );
        assert_eq!(
            status(6, Value::Null, both.clone()),
            Some((Status::Open, false))
        );
        assert_eq!(
            status(3, Value::Null, both.clone()),
            Some((Status::Decided, false))
        );
        assert_eq!(status(2, json!(2), both), Some((Status::Decided, true)));
        // A bye or a slot still waiting on another set.
        let one = json!([
            { "slotIndex": 0, "entrant": { "id": 1 } },
            { "slotIndex": 1, "entrant": null },
        ]);
        assert_eq!(status(1, Value::Null, one), Some((Status::Pending, false)));
    }

    #[test]
    fn an_entrant_is_found_by_user_then_discord() {
        let entrant = |value: Value| {
            serde_json::from_value::<WireEntrant>(value)
                .unwrap()
                .accounts("startgg")
        };
        let (id, accounts) = entrant(json!({
            "id": 2401411,
            "participants": [{ "user": { "id": 1234567, "authorizations": [{ "externalId": "123456789012345678" }] } }],
        }));
        assert_eq!(id, "2401411");
        assert_eq!(
            accounts,
            [
                Account::Site {
                    site: "startgg".into(),
                    id: "1234567".into()
                },
                Account::Discord {
                    user_id: "123456789012345678".into()
                },
            ]
        );
        // No start.gg account behind it, or a team: nobody to find.
        assert!(
            entrant(json!({ "id": 1, "participants": [{ "user": null }] }))
                .1
                .is_empty()
        );
        assert!(
            entrant(json!({ "id": 1, "participants": [{ "user": { "id": 1 } }, { "user": { "id": 2 } }] }))
                .1
                .is_empty()
        );
    }
}
