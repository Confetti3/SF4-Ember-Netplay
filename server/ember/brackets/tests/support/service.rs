//! A fake organizer service answering the `http` adapter's contract
//! (ADAPTERS.md) under `/svc`: it checks the token and the contract
//! version, keeps its sets in memory and logs each request. Knobs make it
//! fail the way a real service behind a proxy can.
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
use tokio::net::TcpListener;

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
    /// Reports are recorded without changing the sets read, as a service
    /// whose reads are cached for a while would answer.
    pub stale_reads: bool,
    /// The next report is recorded and then answered 503, as if its answer
    /// were lost.
    pub lose_report_answer: bool,
    /// Reports of these sets are deferred (423 with `{"retry": true}`),
    /// with these reasons.
    pub deferred_reports: HashMap<String, String>,
}

pub struct FakeService {
    /// The base URL the connector is given.
    pub base: String,
    pub state: Arc<Mutex<ServiceState>>,
}

fn json_answer(body: Value) -> Response {
    (
        StatusCode::OK,
        [(header::CONTENT_TYPE, "application/json")],
        body.to_string(),
    )
        .into_response()
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
    if header("ember-brackets-contract") != Some("1") {
        return StatusCode::BAD_REQUEST.into_response();
    }
    let body: Value = serde_json::from_slice(&body).unwrap_or(Value::Null);
    let mut state = state.lock().unwrap();
    state.log.push(Logged {
        method: method.clone(),
        path: uri.to_string(),
        body: body.clone(),
    });
    if let Some(status) = state.fail_next.pop_front() {
        return status.into_response();
    }
    let path = uri.path().trim_start_matches("/svc");
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
                    !state
                        .hidden
                        .contains(set["id"].as_str().unwrap_or_default())
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
                StatusCode::NO_CONTENT.into_response()
            }
            None => StatusCode::NOT_FOUND.into_response(),
        },
        (Method::POST, ["sets", id, "report"]) => {
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
            }
            state.reports.push(recorded);
            if std::mem::take(&mut state.lose_report_answer) {
                return StatusCode::SERVICE_UNAVAILABLE.into_response();
            }
            StatusCode::NO_CONTENT.into_response()
        }
        // `/standing` is optional; this service has none.
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
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let base = format!("http://{}/svc", listener.local_addr().unwrap());
        tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
        Self { base, state }
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
