//! The start.gg adapter (docs/design/STARTGG_API.md): one event,
//! read and reported through start.gg's GraphQL API with an organizer's
//! token (an account with a reporter role on the tournament).
//!
//! Entrants are found by the start.gg user behind them (`user.id`, looked
//! up on the bridge site given) and, failing that, by the Discord account
//! they linked on start.gg. Sets with a bye or a `preview` ID are never
//! offered: start.gg replaces preview sets when their phase starts. Every
//! request waits its turn under start.gg's rate limit, shared by everything
//! that uses the same token.
//!
//! A report start.gg refuses with a text it does not explain is not given
//! up at once: the set is read again, and while it is still open with the
//! same entrants that one result waits and is sent again (`Deferred`), as
//! the connector counts. Only a set that is gone, changed or decided
//! otherwise is left to the organizer at once.
mod wire;

use std::collections::HashMap;

use reqwest::{
    StatusCode,
    header::{AUTHORIZATION, CONTENT_TYPE, HeaderMap, HeaderValue},
};
use serde::de::DeserializeOwned;
use serde_json::{Value, json};
use tokio::sync::OnceCell;

use crate::{
    Clock,
    http::{bearer, exchange, http_client},
    limit::Limiter,
    roster::Roster,
    site::{
        Account, Purpose, Report, Reported, Set, Site, SiteError, SiteFuture, Standing, Status,
    },
};
use wire::{
    COMPLETED, ENTRANTS, EVENT, EntrantsData, Envelope, EventData, Page, REPORT, RawSet, SET,
    SetData, SetsData, UNDERWAY, WireEntrant, WireSet, is_set_id, is_set_not_found, is_whole,
    sets_query, shows_recorded,
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
/// How often the entrants are read again for accounts linked since: a
/// player who links Discord on start.gg after the first read is found
/// within this. A few requests, against 70 a minute.
const ENTRANTS_FRESH_SECS: u64 = 15 * 60;

/// What a tournament's configuration asks of each set beyond its players:
/// the fields only those that use them pay for.
#[derive(Clone, Copy, Debug, Default)]
pub struct Wanted {
    /// A round rule names a phase.
    pub phases: bool,
    /// Sets on a stream are held.
    pub streams: bool,
}

pub(crate) struct Startgg {
    http: reqwest::Client,
    url: String,
    headers: HeaderMap,
    /// The query for a page of open sets, with what `Wanted` asks for.
    sets_query: String,
    /// The same list with the decided sets: a desk event's whole bracket.
    whole_query: String,
    limiter: Limiter,
    slug: String,
    bridge_site: String,
    /// The event's ID, read once.
    event: OnceCell<String>,
    /// Entrant ID to the accounts behind it, read again when a set names an
    /// entrant not in it, or every `ENTRANTS_FRESH_SECS`.
    roster: Roster,
}

/// How a query's answer is read when it carries `errors`.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Errors {
    /// Any error is the query failing.
    Fail,
    /// The data is used when every field asked at the top is there and it
    /// parses: an error on one field deeper down (an entrant's Discord
    /// authorizations, whose visibility STARTGG_API.md does not state)
    /// nulls only that field, and the rest still counts.
    Partial,
}

impl Startgg {
    /// `origin` is start.gg's API origin, checked by the configuration.
    pub fn new(
        origin: &str,
        slug: &str,
        bridge_site: &str,
        token: &str,
        limiter: Limiter,
        clock: Clock,
        wanted: Wanted,
    ) -> Result<Self, String> {
        let authorization = bearer(token, "the start.gg token")?;
        let mut headers = HeaderMap::new();
        headers.insert(CONTENT_TYPE, HeaderValue::from_static("application/json"));
        headers.insert(AUTHORIZATION, authorization);
        Ok(Self {
            http: http_client()?,
            url: format!("{origin}{API_PATH}"),
            headers,
            sets_query: sets_query(wanted.phases, wanted.streams, false),
            whole_query: sets_query(wanted.phases, wanted.streams, true),
            limiter,
            slug: slug.to_owned(),
            bridge_site: bridge_site.to_owned(),
            event: OnceCell::new(),
            roster: Roster::new(clock, Some(ENTRANTS_FRESH_SECS)),
        })
    }

    /// One GraphQL request, waiting its turn under the rate limit. Errors in
    /// the answer are the caller's to read.
    async fn post(&self, query: &str, variables: Value) -> Result<Envelope, SiteError> {
        self.limiter.admit().await;
        let request = self
            .http
            .post(&self.url)
            .headers(self.headers.clone())
            .json(&json!({ "query": query, "variables": variables }));
        let (status, body) = exchange(request, MAX_ANSWER, "start.gg").await?;
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
            // A 400 or 401 is how start.gg is seen to refuse a bad or expired
            // token (STARTGG_API.md section 4.4 states no text for it): the
            // organizer can fix it, so the whole tournament waits, and no
            // set is given up for it.
            (status, envelope) => {
                let error = envelope
                    .and_then(|envelope| envelope.errors.into_iter().next())
                    .map(|error| format!(": {}", error.message))
                    .unwrap_or_default();
                let message = format!("start.gg answered {}{error}", status.as_u16());
                Err(match status {
                    StatusCode::BAD_REQUEST | StatusCode::UNAUTHORIZED | StatusCode::FORBIDDEN => {
                        SiteError::Denied(format!(
                            "{message}; check the organizer's token and its access to the event"
                        ))
                    }
                    _ => SiteError::Unavailable(message),
                })
            }
        }
    }

    /// A query's data; any error in the answer is start.gg failing.
    async fn query<T: DeserializeOwned>(
        &self,
        query: &str,
        variables: Value,
    ) -> Result<T, SiteError> {
        self.query_as(query, variables, Errors::Fail)
            .await?
            .map_err(|message| SiteError::Unavailable(format!("start.gg: {message}")))
    }

    /// A query's data, or the first GraphQL error's message for the caller
    /// to read.
    async fn query_as<T: DeserializeOwned>(
        &self,
        query: &str,
        variables: Value,
        errors: Errors,
    ) -> Result<Result<T, String>, SiteError> {
        let envelope = self.post(query, variables).await?;
        let error = envelope
            .errors
            .into_iter()
            .next()
            .map(|error| error.message);
        let data = envelope
            .data
            .filter(|data| error.is_none() || (errors == Errors::Partial && is_whole(data)))
            .and_then(|data| serde_json::from_value(data).ok());
        match (data, error) {
            (Some(data), _) => Ok(Ok(data)),
            (None, Some(message)) => Ok(Err(message)),
            (None, None) => Err(unexpected()),
        }
    }

    /// A mutation's data, as start.gg answered it; `Ok(Err(message))` when
    /// start.gg refused it. Its one stated server fault is `Unavailable`.
    async fn mutate(
        &self,
        query: &str,
        variables: Value,
    ) -> Result<Result<Option<Value>, String>, SiteError> {
        let envelope = self.post(query, variables).await?;
        Ok(match envelope.errors.into_iter().next() {
            Some(error) if error.message == "An unknown error has occurred" => {
                return Err(SiteError::Unavailable(format!(
                    "start.gg: {}",
                    error.message
                )));
            }
            Some(error) => Err(error.message),
            None => Ok(envelope.data),
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
        errors: Errors,
        page_of: impl Fn(P) -> Option<Page<T>>,
    ) -> Result<Vec<T>, SiteError> {
        let event = self.event_id().await?.to_owned();
        let mut items = Vec::new();
        for page in 1..=MAX_PAGES {
            let variables = json!({ "id": event, "page": page, "perPage": PER_PAGE });
            let data: P = self
                .query_as(query, variables, errors)
                .await?
                .map_err(|message| SiteError::Unavailable(format!("start.gg: {message}")))?;
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

    async fn refresh_entrants(&self, asked: &[&String]) -> Result<(), SiteError> {
        let entrants: Vec<WireEntrant> = self
            .pages(ENTRANTS, Errors::Partial, |data: EntrantsData| {
                data.event.map(|event| event.entrants)
            })
            .await?;
        let accounts: HashMap<String, Vec<Account>> = entrants
            .into_iter()
            .map(|entrant| entrant.accounts(&self.bridge_site))
            .collect();
        self.roster.replace(accounts, asked.iter().copied());
        Ok(())
    }

    fn set_of(&self, raw: RawSet) -> Set {
        Set {
            entries: self.roster.entries(raw.entrants.as_ref()),
            id: raw.id,
            round: raw.round,
            round_label: raw.round_label,
            status: raw.status,
            underway: raw.underway,
            winner: raw.winner,
            extras: raw.extras,
        }
    }

    async fn read_open_sets(&self) -> Result<Vec<Set>, SiteError> {
        let sets = self.read_wire(&self.sets_query).await?;
        self.sets_of(sets).await
    }

    /// Every set, the open ones (as the open read lists them) and the rest.
    /// The decided sets take more pages as the bracket is played.
    async fn read_whole(&self) -> Result<(Vec<Set>, Vec<Set>), SiteError> {
        let (open, rest) = self
            .read_wire(&self.whole_query)
            .await?
            .into_iter()
            .partition(|set| set.state != Some(COMPLETED));
        Ok((self.sets_of(open).await?, self.sets_of(rest).await?))
    }

    async fn read_wire(&self, query: &str) -> Result<Vec<WireSet>, SiteError> {
        self.pages(query, Errors::Fail, |data: SetsData| {
            data.event.map(|event| event.sets)
        })
        .await
    }

    /// The sets, with their entrants' accounts read when the roster lacks
    /// them.
    async fn sets_of(&self, sets: Vec<WireSet>) -> Result<Vec<Set>, SiteError> {
        let raw: Vec<RawSet> = sets.into_iter().filter_map(WireSet::into_raw).collect();
        let ids: Vec<&String> = raw
            .iter()
            .flat_map(|set| set.entrants.iter().flatten())
            .collect();
        if self.roster.stale(ids.iter().copied()) {
            self.refresh_entrants(&ids).await?;
        }
        Ok(raw.into_iter().map(|raw| self.set_of(raw)).collect())
    }

    async fn read_raw(&self, id: &str) -> Result<Option<RawSet>, SiteError> {
        if !is_set_id(id) {
            return Ok(None);
        }
        match self
            .query_as::<SetData>(SET, json!({ "id": id }), Errors::Fail)
            .await?
        {
            Ok(found) => Ok(found.set.and_then(WireSet::into_raw)),
            // How start.gg answers for a set it no longer has: one of a
            // phase group the organizer reset or reseeded, whose sets get
            // new IDs. The text is the one STARTGG_API.md section 4.4 gives
            // for preview IDs; that start.gg answers a removed real set the
            // same way is not stated, and awaits a live check. Any other
            // error, or a `set` nulled beside one, is start.gg failing.
            Err(message) if is_set_not_found(&message, id) => Ok(None),
            Err(message) => Err(SiteError::Unavailable(format!("start.gg: {message}"))),
        }
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
            Ok(_) => Ok(()),
            // Started already, by its players or an earlier attempt.
            Err(message) if message == "Set is already started" => Ok(()),
            Err(message) => Err(SiteError::Refused(format!("start.gg: {message}"))),
        }
    }

    /// Reports the winner and each game's winner in the order played,
    /// after reading the set again: a set with a result, or with games
    /// entered that Ember's do not match, is never written over, since
    /// `reportBracketSet` replaces both. The score counts as sent only when
    /// start.gg's answer shows the set decided for this winner, or, when
    /// the answer does not, a fresh read of the set does.
    async fn send_report(&self, id: &str, report: &Report) -> Result<Reported, SiteError> {
        if !self.reportable(id, report).await? {
            return Ok(Reported::AlreadyHeld);
        }
        let game_data: Vec<Value> = report
            .sequence
            .iter()
            .enumerate()
            .map(|(index, &winner)| {
                json!({ "gameNum": index + 1, "winnerId": report.entries[winner] })
            })
            .collect();
        let winner = &report.entries[report.winner];
        let variables = json!({
            "setId": id,
            "winnerId": winner,
            "gameData": game_data,
        });
        match self.mutate(REPORT, variables).await? {
            Ok(answer) if shows_recorded(answer.as_ref(), id, winner) => Ok(Reported::Sent),
            // No error, but no sign of the score either (no set, another
            // set, a set without this winner): the set itself decides.
            Ok(_) => {
                if self.reportable(id, report).await? {
                    Err(SiteError::Deferred(
                        "start.gg's answer did not show the score recorded, and the set is still open"
                            .into(),
                    ))
                } else {
                    Ok(Reported::Sent)
                }
            }
            Err(message) => self.refused(id, report, &message).await,
        }
    }

    /// Whether the set, read now, still waits for this report: `false`
    /// when it already holds it.
    async fn reportable(&self, id: &str, report: &Report) -> Result<bool, SiteError> {
        let current = self
            .read_raw(id)
            .await?
            .ok_or_else(|| SiteError::Refused(format!("start.gg has no set {id}")))?;
        if current.entrants.as_ref() != Some(&report.entries) {
            return Err(SiteError::Refused("the set's entrants have changed".into()));
        }
        match current.status {
            Status::Open => {}
            Status::Decided if current.winner.as_ref() == Some(&report.entries[report.winner]) => {
                return Ok(false);
            }
            Status::Decided => {
                return Err(SiteError::Refused(
                    "the set already has a different result".into(),
                ));
            }
            // Not open now, though its entrants are the same: the next read
            // of the bracket decides what becomes of it.
            Status::Pending => {
                return Err(SiteError::Deferred(
                    "the set is not open on start.gg now".into(),
                ));
            }
        }
        // A report replaces every game entered. One that holds only its
        // winner, the same as Ember's, loses nothing; any other is the
        // organizer's to keep, so the set is left to them.
        for game in &current.games {
            let Some(order) = game.order else {
                return Err(SiteError::Refused(
                    "start.gg has a game entered with no number, which a report would replace"
                        .into(),
                ));
            };
            let Some(entered) = &game.winner else {
                return Err(SiteError::Refused(format!(
                    "start.gg has game {order} entered without a winner, which a report would replace"
                )));
            };
            if game.details {
                return Err(SiteError::Refused(format!(
                    "start.gg has game {order} entered with a score, stage or characters, which a report would replace"
                )));
            }
            let ours = (order as usize)
                .checked_sub(1)
                .and_then(|index| report.sequence.get(index))
                .map(|&slot| &report.entries[slot]);
            if ours != Some(entered) {
                return Err(SiteError::Refused(format!(
                    "start.gg already has game {order} entered with another winner than Ember's, which a report would replace"
                )));
            }
        }
        Ok(true)
    }

    /// A report start.gg refused with `message`. Only a preview ID is a
    /// stated permanent refusal; anything else (a token or role problem
    /// has no stated text) is checked against the set: one still open and
    /// unchanged defers this result alone, which the connector sends again
    /// a bounded number of times.
    async fn refused(
        &self,
        id: &str,
        report: &Report,
        message: &str,
    ) -> Result<Reported, SiteError> {
        if message.starts_with("Set not found") {
            return Err(SiteError::Refused(format!("start.gg: {message}")));
        }
        // The report may have been recorded after all.
        if !self.reportable(id, report).await? {
            return Ok(Reported::AlreadyHeld);
        }
        Err(SiteError::Deferred(format!(
            "start.gg refused the score ({message}), and the set is still open"
        )))
    }
}

impl Site for Startgg {
    /// start.gg counts requests by the minute, not the month: every request
    /// waits its turn, whatever it is for.
    fn open_sets(&self, _purpose: Purpose) -> SiteFuture<'_, Vec<Set>> {
        Box::pin(self.read_open_sets())
    }

    fn whole_sets(&self, _purpose: Purpose) -> SiteFuture<'_, Option<(Vec<Set>, Vec<Set>)>> {
        Box::pin(async { self.read_whole().await.map(Some) })
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

    fn report<'a>(&'a self, id: &'a str, report: &'a Report) -> SiteFuture<'a, Reported> {
        Box::pin(self.send_report(id, report))
    }

    fn wants_games(&self) -> bool {
        true
    }
}

fn unexpected() -> SiteError {
    SiteError::Unavailable("start.gg answered something unexpected".into())
}
