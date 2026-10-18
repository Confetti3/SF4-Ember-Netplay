//! The `http` adapter: a bracket run by the organizer's own service, which
//! answers a small JSON contract (ADAPTERS.md, "The http adapter"), so any
//! service in any language can be played in Ember without code here.
//!
//! The service names each entry's accounts itself, site and all, and keeps
//! its own request limits; the connector adds everything else the contract
//! promises (one live match per set, evidence, retries, verified results).
use std::{collections::HashSet, sync::Mutex};

use reqwest::{
    Method, StatusCode,
    header::{ACCEPT, AUTHORIZATION, HeaderMap, HeaderValue},
};
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use serde_json::{Value, json};

use crate::{
    config::is_slug,
    http::{bounded_body, http_client},
    log,
    site::{Account, Entry, Purpose, Report, Set, Site, SiteError, SiteFuture, Standing, Status},
};

/// The contract version, sent on every request. A service that changes a
/// shape in a way an older connector would misread answers a request of an
/// older version with 400.
pub const CONTRACT: &str = "1";
const CONTRACT_HEADER: &str = "ember-brackets-contract";
/// The same on every delivery of one result.
const IDEMPOTENCY_HEADER: &str = "idempotency-key";
const MAX_ANSWER: usize = 1024 * 1024;

pub(crate) struct Service {
    http: reqwest::Client,
    /// The service's base URL, without a trailing `/`.
    base: String,
    headers: HeaderMap,
    /// Sets left out of a read for their shape, each said once.
    left_out: Mutex<HashSet<String>>,
}

/// `GET {base}/sets?state=open`. Each set is read on its own, so one out of
/// shape leaves only itself out.
#[derive(Deserialize)]
struct SetList {
    sets: Vec<Value>,
}

/// `GET {base}/sets/{id}`: `null` when the service has no such set.
#[derive(Deserialize)]
struct OneSet {
    set: Option<WireSet>,
}

#[derive(Deserialize)]
struct WireSet {
    id: String,
    #[serde(default)]
    round: i32,
    #[serde(default)]
    round_label: Option<String>,
    status: WireStatus,
    #[serde(default)]
    entries: Option<[WireEntry; 2]>,
    #[serde(default)]
    underway: bool,
    /// The winning entry's ID, once decided.
    #[serde(default)]
    winner: Option<String>,
}

#[derive(Deserialize)]
#[serde(rename_all = "snake_case")]
enum WireStatus {
    Pending,
    Open,
    Decided,
}

#[derive(Deserialize)]
struct WireEntry {
    id: String,
    #[serde(default)]
    accounts: Vec<WireAccount>,
}

/// `{"site": ..., "id": ...}` or `{"discord": ...}`.
#[derive(Deserialize)]
#[serde(untagged)]
enum WireAccount {
    Site { site: String, id: String },
    Discord { discord: String },
}

/// `GET {base}/standing`.
#[derive(Deserialize)]
struct WireStanding {
    standing: StandingName,
}

#[derive(Deserialize)]
#[serde(rename_all = "snake_case")]
enum StandingName {
    Finished,
    PlayedOut,
    Running,
}

/// `POST {base}/sets/{id}/report`.
#[derive(Serialize)]
struct WireReport<'a> {
    winner: &'a str,
    scores: [WireScore<'a>; 2],
    /// The winning entry of each game, in the order played.
    games: Vec<&'a str>,
}

#[derive(Serialize)]
struct WireScore<'a> {
    entry: &'a str,
    games: u8,
}

impl<'a> WireReport<'a> {
    fn of(report: &'a Report) -> Self {
        let entry = |index: usize| report.entries[index].as_str();
        Self {
            winner: entry(report.winner),
            scores: [0, 1].map(|index| WireScore {
                entry: entry(index),
                games: report.games[index],
            }),
            games: report.sequence.iter().map(|&index| entry(index)).collect(),
        }
    }
}

impl From<WireAccount> for Account {
    fn from(account: WireAccount) -> Self {
        match account {
            WireAccount::Site { site, id } => Self::Site { site, id },
            WireAccount::Discord { discord } => Self::Discord { user_id: discord },
        }
    }
}

impl WireSet {
    /// The set as the connector sees it, or `None` when its IDs could not
    /// go back into a URL or a match's external ID.
    fn into_set(self) -> Option<Set> {
        let ids_valid = is_slug(&self.id)
            && self
                .entries
                .iter()
                .flatten()
                .all(|entry| is_slug(&entry.id));
        if !ids_valid {
            return None;
        }
        let entry = |wire: WireEntry| Entry {
            participant_id: wire.id,
            accounts: wire.accounts.into_iter().map(Account::from).collect(),
        };
        Some(Set {
            round_label: self
                .round_label
                .unwrap_or_else(|| format!("Round {}", self.round)),
            id: self.id,
            round: self.round,
            entries: self.entries.map(|entries| entries.map(entry)),
            status: match self.status {
                WireStatus::Pending => Status::Pending,
                WireStatus::Open => Status::Open,
                WireStatus::Decided => Status::Decided,
            },
            underway: self.underway,
            winner: self.winner,
        })
    }
}

impl Service {
    /// `base` is the service's base URL, checked by the configuration.
    pub fn new(base: &str, token: &str) -> Result<Self, String> {
        let mut authorization = HeaderValue::from_str(&format!("Bearer {token}"))
            .map_err(|_| "the service's token is not a valid header value")?;
        authorization.set_sensitive(true);
        let mut headers = HeaderMap::new();
        headers.insert(ACCEPT, HeaderValue::from_static("application/json"));
        headers.insert(CONTRACT_HEADER, HeaderValue::from_static(CONTRACT));
        headers.insert(AUTHORIZATION, authorization);
        Ok(Self {
            http: http_client()?,
            base: base.trim_end_matches('/').to_owned(),
            headers,
            left_out: Mutex::default(),
        })
    }

    /// One request. `None` when the service answers 404, which the caller
    /// reads: it is never taken as a set being gone.
    async fn call<T: DeserializeOwned>(
        &self,
        method: Method,
        path: &str,
        body: Option<Value>,
        idempotency_key: Option<&str>,
    ) -> Result<Option<T>, SiteError> {
        let mut request = self
            .http
            .request(method, format!("{}{path}", self.base))
            .headers(self.headers.clone());
        if let Some(body) = body {
            request = request.json(&body);
        }
        if let Some(key) = idempotency_key {
            request = request.header(IDEMPOTENCY_HEADER, key);
        }
        let response = request
            .send()
            .await
            .map_err(|_| SiteError::Unavailable("the service could not be reached".into()))?;
        let status = response.status();
        let body = bounded_body(response, MAX_ANSWER).await.ok_or_else(|| {
            SiteError::Unavailable("the service's answer was cut off or too long".into())
        })?;
        let message = || format!("the service answered {}", status.as_u16());
        match status {
            StatusCode::NOT_FOUND => Ok(None),
            status if status.is_success() => {
                // An answer with no body reads as JSON null, which `()` takes.
                let value: Value = if body.is_empty() {
                    Value::Null
                } else {
                    serde_json::from_slice(&body).map_err(|_| unexpected())?
                };
                serde_json::from_value(value)
                    .map(Some)
                    .map_err(|_| unexpected())
            }
            StatusCode::UNAUTHORIZED | StatusCode::FORBIDDEN | StatusCode::TOO_MANY_REQUESTS => {
                Err(SiteError::Unavailable(message()))
            }
            status if status.is_server_error() => Err(SiteError::Unavailable(message())),
            _ => Err(SiteError::Refused(message())),
        }
    }

    /// A request whose 404 is the service not answering as it should (a
    /// route moved, a proxy in between): try again later.
    async fn call_found<T: DeserializeOwned>(
        &self,
        method: Method,
        path: &str,
        body: Option<Value>,
        idempotency_key: Option<&str>,
    ) -> Result<T, SiteError> {
        self.call(method, path, body, idempotency_key)
            .await?
            .ok_or_else(|| SiteError::Unavailable(format!("the service answered 404 to {path}")))
    }

    /// The open sets. A set not in the contract's shape is left out, and
    /// said once, rather than failing the read for every other set.
    async fn read_open_sets(&self) -> Result<Vec<Set>, SiteError> {
        let list: SetList = self
            .call_found(Method::GET, "/sets?state=open", None, None)
            .await?;
        let mut left_out = self.left_out.lock().unwrap_or_else(|e| e.into_inner());
        Ok(list
            .sets
            .into_iter()
            .enumerate()
            .filter_map(|(index, value)| {
                let set = serde_json::from_value::<WireSet>(value.clone())
                    .ok()
                    .and_then(WireSet::into_set);
                if set.is_none() {
                    let name = value["id"]
                        .as_str()
                        .map_or_else(|| format!("number {}", index + 1), str::to_owned);
                    if left_out.insert(name.clone()) {
                        log!("the service's set {name} is not in the contract's shape; it is left out");
                    }
                }
                set
            })
            .collect())
    }

    /// The set, or `None` only when the service answers `{"set": null}`.
    async fn read_set(&self, id: &str) -> Result<Option<Set>, SiteError> {
        if !is_slug(id) {
            return Ok(None);
        }
        let found: OneSet = self
            .call_found(Method::GET, &format!("/sets/{id}"), None, None)
            .await?;
        match found.set {
            None => Ok(None),
            Some(set) => set.into_set().map(Some).ok_or_else(|| {
                SiteError::Unavailable(format!(
                    "the service's set {id} is not in the contract's shape"
                ))
            }),
        }
    }

    /// A service without `/standing` (404) is taken as running.
    async fn read_standing(&self) -> Result<Standing, SiteError> {
        let found: Option<WireStanding> = self.call(Method::GET, "/standing", None, None).await?;
        Ok(match found.map(|found| found.standing) {
            Some(StandingName::Finished) => Standing::Finished,
            Some(StandingName::PlayedOut) => Standing::PlayedOut,
            Some(StandingName::Running) | None => Standing::Running,
        })
    }

    async fn post(
        &self,
        id: &str,
        action: &str,
        body: Value,
        idempotency_key: Option<&str>,
    ) -> Result<(), SiteError> {
        if !is_slug(id) {
            return Err(SiteError::Refused(format!("{id:?} is not a set ID")));
        }
        let _: Value = self
            .call_found(
                Method::POST,
                &format!("/sets/{id}/{action}"),
                Some(body),
                idempotency_key,
            )
            .await?;
        Ok(())
    }

    /// Sent at least once: a report whose answer is lost is sent again
    /// under the same key, which the service answers as it did the first.
    async fn send_report(&self, id: &str, report: &Report) -> Result<(), SiteError> {
        let body = serde_json::to_value(WireReport::of(report)).map_err(|_| unexpected())?;
        self.post(id, "report", body, Some(&report.key)).await
    }
}

impl Site for Service {
    /// The service keeps its own limits, so a read for a result and a
    /// scheduled one are the same request.
    fn open_sets(&self, _purpose: Purpose) -> SiteFuture<'_, Vec<Set>> {
        Box::pin(self.read_open_sets())
    }

    fn set<'a>(&'a self, id: &'a str, _purpose: Purpose) -> SiteFuture<'a, Option<Set>> {
        Box::pin(self.read_set(id))
    }

    fn standing(&self) -> SiteFuture<'_, Standing> {
        Box::pin(self.read_standing())
    }

    fn wants_games(&self) -> bool {
        true
    }

    fn mark_underway<'a>(&'a self, id: &'a str) -> SiteFuture<'a, ()> {
        Box::pin(self.post(id, "underway", json!({}), None))
    }

    fn report<'a>(&'a self, id: &'a str, report: &'a Report) -> SiteFuture<'a, ()> {
        Box::pin(self.send_report(id, report))
    }
}

fn unexpected() -> SiteError {
    SiteError::Unavailable("the service answered something unexpected".into())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn set(value: Value) -> Option<Set> {
        serde_json::from_value::<WireSet>(value).unwrap().into_set()
    }

    #[test]
    fn a_set_reads_with_its_accounts_in_order() {
        let read = set(json!({
            "id": "s-12",
            "round": -2,
            "status": "open",
            "entries": [
                { "id": "e1", "accounts": [{ "site": "startgg", "id": "1234" }, { "discord": "274220342558756145" }] },
                { "id": "e2" },
            ],
            "some_future_field": true,
        }))
        .unwrap();
        assert_eq!(read.round_label, "Round -2");
        assert_eq!(read.playable_ids(), Some(["e1", "e2"]));
        let [one, two] = read.entries.unwrap();
        assert_eq!(
            one.accounts,
            [
                Account::Site {
                    site: "startgg".into(),
                    id: "1234".into()
                },
                Account::Discord {
                    user_id: "274220342558756145".into()
                },
            ]
        );
        assert!(two.accounts.is_empty());
    }

    #[test]
    fn ids_that_cannot_go_into_a_url_are_left_out() {
        assert!(set(json!({ "id": "a/b", "status": "open" })).is_none());
        assert!(
            set(
                json!({ "id": "s", "status": "open", "entries": [{ "id": "e 1" }, { "id": "e2" }] })
            )
            .is_none()
        );
        assert_eq!(
            set(json!({ "id": "s", "status": "decided", "winner": "e2", "round_label": "Grand final" }))
                .map(|set| (set.status, set.winner, set.round_label)),
            Some((Status::Decided, Some("e2".into()), "Grand final".into()))
        );
    }

    #[test]
    fn a_report_names_entries_and_games_in_order() {
        let report = Report {
            entries: ["e1".into(), "e2".into()],
            games: [1, 2],
            winner: 1,
            sequence: vec![1, 0, 1],
            key: "emt_1".into(),
        };
        assert_eq!(
            serde_json::to_value(WireReport::of(&report)).unwrap(),
            json!({
                "winner": "e2",
                "scores": [{ "entry": "e1", "games": 1 }, { "entry": "e2", "games": 2 }],
                "games": ["e2", "e1", "e2"],
            })
        );
    }
}
