//! A fake organizer service answering the `http` adapter's contract
//! (ADAPTERS.md) under `/svc`: it checks the token and the contract
//! version, keeps its sets in memory and logs each request.
use std::sync::{Arc, Mutex};

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

pub struct ServiceState {
    /// Each set as the service answers it, with its report once made.
    pub sets: Vec<Value>,
    pub reports: Vec<(String, Value)>,
    pub log: Vec<Logged>,
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
    let path = uri.path().trim_start_matches("/svc");
    let parts: Vec<&str> = path.trim_start_matches('/').split('/').collect();
    let find =
        |state: &mut ServiceState, id: &str| state.sets.iter_mut().position(|set| set["id"] == id);
    match (method, parts.as_slice()) {
        (Method::GET, ["sets"]) => {
            assert_eq!(uri.query(), Some("state=open"));
            let open: Vec<Value> = state
                .sets
                .iter()
                .filter(|set| set["status"] == "open")
                .cloned()
                .collect();
            json_answer(json!({ "sets": open }))
        }
        (Method::GET, ["sets", id]) => match find(&mut state, id) {
            Some(index) => json_answer(json!({ "set": state.sets[index] })),
            None => StatusCode::NOT_FOUND.into_response(),
        },
        (Method::POST, ["sets", id, "underway"]) => match find(&mut state, id) {
            Some(index) => {
                state.sets[index]["underway"] = json!(true);
                StatusCode::NO_CONTENT.into_response()
            }
            None => StatusCode::NOT_FOUND.into_response(),
        },
        (Method::POST, ["sets", id, "report"]) => match find(&mut state, id) {
            Some(index) if state.sets[index]["status"] == "open" => {
                state.sets[index]["status"] = json!("decided");
                state.sets[index]["winner"] = body["winner"].clone();
                state.reports.push(((*id).to_owned(), body));
                StatusCode::NO_CONTENT.into_response()
            }
            Some(_) => StatusCode::CONFLICT.into_response(),
            None => StatusCode::NOT_FOUND.into_response(),
        },
        // `/standing` is optional; this service has none.
        _ => StatusCode::NOT_FOUND.into_response(),
    }
}

impl FakeService {
    pub async fn start(sets: Vec<Value>) -> Self {
        let state = Arc::new(Mutex::new(ServiceState {
            sets,
            reports: Vec::new(),
            log: Vec::new(),
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
}
