//! A fake bracket service answering the `http` adapter's contract
//! (ADAPTERS.md) under `/tournaments/<TOURNAMENT>/ember`, as the bracket
//! site does: it checks the token and the contract version, keeps its sets
//! in memory, moves winners on to the sets they feed, and logs each
//! request. Knobs make it fail the way a real service behind a proxy can.
use std::{
    collections::{HashMap, HashSet, VecDeque},
    sync::{Arc, Mutex},
};

use axum::{
    Router,
    body::Bytes,
    extract::State,
    http::{HeaderMap, Method, StatusCode, Uri, header},
    response::{IntoResponse, Response},
};
use serde_json::{Value, json};

use super::TOURNAMENT;

pub const SERVICE_TOKEN: &str = "test-service-token";

#[derive(Debug)]
pub struct Logged {
    pub method: Method,
    pub path: String,
    pub body: Value,
}

/// A report the service recorded: the set, its Idempotency-Key and body.
#[derive(Clone, Debug, PartialEq)]
pub struct Recorded {
    pub set: String,
    pub key: String,
    pub body: Value,
}

#[derive(Default)]
pub struct ServiceState {
    /// Each set as the service answers it.
    pub sets: Vec<Value>,
    pub reports: Vec<Recorded>,
    pub log: Vec<Logged>,
    /// The next requests are answered with these statuses, whatever they
    /// ask, as a proxy or a rate limit in front of the service would.
    pub fail_next: VecDeque<StatusCode>,
    /// Sets left out of lists and answered with a bare 404, as a route the
    /// service no longer mounts would be.
    pub hidden: HashSet<String>,
    /// Sets left out of lists only, as a list cached for a moment would
    /// leave out a set read on its own.
    pub unlisted: HashSet<String>,
    /// Reports are recorded without changing the sets read, as a service
    /// whose reads are cached for a while would answer.
    pub stale_reads: bool,
    /// The next report is recorded and then answered 503, as if its answer
    /// were lost.
    pub lose_report_answer: bool,
    /// Reports of these sets are deferred (423 with `{"retry": true}`),
    /// with these reasons.
    pub deferred_reports: HashMap<String, String>,
    /// The next reports are answered with these statuses, and not recorded.
    pub fail_reports: VecDeque<StatusCode>,
    /// Reports and `/underway` are answered `200` with a text body.
    pub text_answers: bool,
    /// What `/standing` answers; without it, a bare 404, as a service
    /// without the route answers.
    pub standing: Option<&'static str>,
}

/// Player `n`'s Discord account, which their entry names.
pub fn discord(n: u8) -> String {
    format!("2742203425587561{n:02}")
}

/// Player `n`'s entry: `player<n>`, by their Discord account.
pub fn entry(n: u8) -> Value {
    json!({ "id": format!("player{n}"), "accounts": [{ "discord": discord(n) }] })
}

/// An open set between players `a` and `b`.
pub fn open(id: &str, round: i32, [a, b]: [u8; 2]) -> Value {
    json!({ "id": id, "round": round, "status": "open", "entries": [entry(a), entry(b)] })
}

/// A pending set whose entries are the winners of the sets `from`.
// ponytail: `fed_by` is the fake's own bookkeeping; the connector ignores it.
pub fn fed_by(id: &str, round: i32, from: [&str; 2]) -> Value {
    json!({ "id": id, "round": round, "status": "pending", "fed_by": from })
}

impl ServiceState {
    fn index(&self, id: &str) -> usize {
        self.sets.iter().position(|set| set["id"] == id).unwrap()
    }

    /// Opens each pending set whose two feeding sets are decided, with
    /// their winners.
    fn advance(&mut self) {
        let winner = |sets: &[Value], id: &Value| {
            let set = sets.iter().find(|set| set["id"] == *id)?;
            let winner = set["winner"].as_str()?;
            set["entries"]
                .as_array()?
                .iter()
                .find(|entry| entry["id"] == winner)
                .cloned()
        };
        for index in 0..self.sets.len() {
            let set = &self.sets[index];
            if set["status"] != "pending" || !set["fed_by"].is_array() {
                continue;
            }
            if let (Some(a), Some(b)) = (
                winner(&self.sets, &set["fed_by"][0]),
                winner(&self.sets, &set["fed_by"][1]),
            ) {
                self.sets[index]["entries"] = json!([a, b]);
                self.sets[index]["status"] = json!("open");
            }
        }
    }

    /// The organizer decides a set by hand for the entry in `slot`.
    pub fn decide(&mut self, id: &str, slot: usize) {
        let index = self.index(id);
        let set = &mut self.sets[index];
        set["winner"] = set["entries"][slot]["id"].clone();
        set["status"] = json!("decided");
        self.advance();
    }

    /// The organizer reopens a set to correct it: open again with no
    /// winner, and every set after it that it fed pending again, without
    /// entries. Their reports are forgotten.
    pub fn reopen(&mut self, id: &str) {
        let set = self.set(id).as_object_mut().unwrap();
        set.insert("status".into(), json!("open"));
        set.remove("winner");
        set.remove("underway");
        let mut reset = vec![json!(id)];
        let mut index = 0;
        while let Some(fed) = reset.get(index).cloned() {
            index += 1;
            for set in &mut self.sets {
                if set["fed_by"]
                    .as_array()
                    .is_some_and(|from| from.contains(&fed))
                {
                    let set = set.as_object_mut().unwrap();
                    set.insert("status".into(), json!("pending"));
                    for field in ["entries", "winner", "underway"] {
                        set.remove(field);
                    }
                    reset.push(set["id"].clone());
                }
            }
        }
        self.reports
            .retain(|report| !reset.contains(&json!(report.set)));
    }

    /// Set `id` as the service now has it.
    pub fn set(&mut self, id: &str) -> &mut Value {
        let index = self.index(id);
        &mut self.sets[index]
    }
}

pub struct FakeService {
    /// Where it listens, as a desk service's base URL names it.
    pub origin: String,
    /// The base URL the connector is given.
    pub base: String,
    pub state: Arc<Mutex<ServiceState>>,
}

/// Where the service answers, as the bracket site serves a tournament.
fn prefix() -> String {
    format!("/tournaments/{TOURNAMENT}/ember")
}

fn json_answer(body: Value) -> Response {
    (
        StatusCode::OK,
        [(header::CONTENT_TYPE, "application/json")],
        body.to_string(),
    )
        .into_response()
}

/// A write's answer: `204`, or `200` with a text body.
fn written(state: &ServiceState) -> Response {
    if state.text_answers {
        (
            StatusCode::OK,
            [(header::CONTENT_TYPE, "text/plain")],
            "OK, recorded",
        )
            .into_response()
    } else {
        StatusCode::NO_CONTENT.into_response()
    }
}

async fn handle(
    State(state): State<Arc<Mutex<ServiceState>>>,
    method: Method,
    uri: Uri,
    headers: HeaderMap,
    body: Bytes,
) -> Response {
    let header = |name: &str| headers.get(name).and_then(|value| value.to_str().ok());
    if header("authorization") != Some(&format!("Bearer {SERVICE_TOKEN}")) {
        return StatusCode::UNAUTHORIZED.into_response();
    }
    // A version mismatch, answered as the contract says.
    if header("ember-brackets-contract") != Some("1") {
        return StatusCode::UPGRADE_REQUIRED.into_response();
    }
    let body: Value = serde_json::from_slice(&body).unwrap_or(Value::Null);
    let mut state = state.lock().unwrap();
    state.log.push(Logged {
        method: method.clone(),
        path: uri.to_string().trim_start_matches(&prefix()).to_owned(),
        body: body.clone(),
    });
    if let Some(status) = state.fail_next.pop_front() {
        return status.into_response();
    }
    let path = uri.path().trim_start_matches(&prefix());
    let parts: Vec<&str> = path.trim_start_matches('/').split('/').collect();
    if let [_, id, ..] = parts.as_slice()
        && state.hidden.contains(*id)
    {
        return StatusCode::NOT_FOUND.into_response();
    }
    let find =
        |state: &mut ServiceState, id: &str| state.sets.iter_mut().position(|set| set["id"] == id);
    match (method, parts.as_slice()) {
        (Method::GET, ["sets"]) => {
            assert_eq!(uri.query(), Some("state=open"));
            let open: Vec<Value> = state
                .sets
                .iter()
                .filter(|set| set["status"] == "open")
                .filter(|set| {
                    let id = set["id"].as_str().unwrap_or_default();
                    !state.hidden.contains(id) && !state.unlisted.contains(id)
                })
                .cloned()
                .collect();
            json_answer(json!({ "sets": open }))
        }
        // A set the service does not have is said to be gone in so many
        // words.
        (Method::GET, ["sets", id]) => {
            let found = find(&mut state, id).map(|index| state.sets[index].clone());
            json_answer(json!({ "set": found }))
        }
        (Method::POST, ["sets", id, "underway"]) => match find(&mut state, id) {
            Some(index) => {
                state.sets[index]["underway"] = json!(true);
                written(&state)
            }
            None => StatusCode::NOT_FOUND.into_response(),
        },
        (Method::POST, ["sets", id, "report"]) => {
            if let Some(status) = state.fail_reports.pop_front() {
                return status.into_response();
            }
            if let Some(reason) = state.deferred_reports.get(*id) {
                return (
                    StatusCode::LOCKED,
                    [(header::CONTENT_TYPE, "application/json")],
                    json!({ "retry": true, "reason": reason }).to_string(),
                )
                    .into_response();
            }
            let recorded = Recorded {
                set: (*id).to_owned(),
                key: header("idempotency-key").unwrap_or_default().to_owned(),
                body,
            };
            // A repeat of the report recorded is answered as the first was;
            // a different one is a conflict.
            if let Some(earlier) = state.reports.iter().find(|earlier| earlier.set == *id) {
                return if *earlier == recorded {
                    StatusCode::NO_CONTENT
                } else {
                    StatusCode::CONFLICT
                }
                .into_response();
            }
            let Some(index) = find(&mut state, id) else {
                return StatusCode::NOT_FOUND.into_response();
            };
            if state.sets[index]["status"] != "open" {
                return StatusCode::CONFLICT.into_response();
            }
            if !state.stale_reads {
                state.sets[index]["status"] = json!("decided");
                state.sets[index]["winner"] = recorded.body["winner"].clone();
                state.advance();
            }
            state.reports.push(recorded);
            if std::mem::take(&mut state.lose_report_answer) {
                return StatusCode::SERVICE_UNAVAILABLE.into_response();
            }
            written(&state)
        }
        (Method::GET, ["standing"]) if state.standing.is_some() => {
            json_answer(json!({ "standing": state.standing }))
        }
        _ => StatusCode::NOT_FOUND.into_response(),
    }
}

impl FakeService {
    pub async fn start(sets: Vec<Value>) -> Self {
        let state = Arc::new(Mutex::new(ServiceState {
            sets,
            ..ServiceState::default()
        }));
        let app = Router::new().fallback(handle).with_state(state.clone());
        let origin = super::serve(app).await;
        let base = format!("{origin}{}", prefix());
        Self {
            origin,
            base,
            state,
        }
    }

    /// `METHOD path` of each request since the last call.
    pub fn log(&self) -> Vec<String> {
        std::mem::take(&mut self.state.lock().unwrap().log)
            .into_iter()
            .map(|logged| format!("{} {}", logged.method, logged.path))
            .collect()
    }

    pub fn reports(&self) -> Vec<Recorded> {
        self.state.lock().unwrap().reports.clone()
    }

    pub fn with<T>(&self, change: impl FnOnce(&mut ServiceState) -> T) -> T {
        change(&mut self.state.lock().unwrap())
    }
}
