//! A fake Challonge v2.1 for one tournament: it serves the routes the
//! connector calls, checks their headers, logs every request, and advances
//! its bracket as Challonge does when a set is decided.
use std::{
    collections::{HashMap, HashSet},
    sync::{Arc, Mutex},
};

use axum::{
    Router,
    body::Bytes,
    extract::State,
    http::{HeaderMap, Method, StatusCode, Uri, header},
    response::{IntoResponse, Response},
};
use ember_brackets::CredentialKind;
use serde_json::{Value, json};

use super::{CHALLONGE_KEY, TOURNAMENT};

#[derive(Clone, Debug)]
pub struct FakeMatch {
    pub id: String,
    pub round: i32,
    pub state: &'static str,
    pub players: [Option<String>; 2],
    pub winner: Option<String>,
    pub score_sets: Option<[String; 2]>,
    pub underway: bool,
    /// The sets whose winners become player 1 and player 2.
    pub fed_by: Option<[String; 2]>,
}

impl FakeMatch {
    pub fn open(id: &str, round: i32, players: [&str; 2]) -> Self {
        Self {
            id: id.into(),
            round,
            state: "open",
            players: players.map(|player| Some(player.to_owned())),
            winner: None,
            score_sets: None,
            underway: false,
            fed_by: None,
        }
    }

    pub fn fed_by(id: &str, round: i32, from: [&str; 2]) -> Self {
        Self {
            id: id.into(),
            round,
            state: "pending",
            players: [None, None],
            winner: None,
            score_sets: None,
            underway: false,
            fed_by: Some(from.map(str::to_owned)),
        }
    }

    fn to_json(&self) -> Value {
        let player = |slot: &Option<String>| match slot {
            Some(id) => json!({ "data": { "id": id, "type": "participant" } }),
            None => json!({ "data": null }),
        };
        json!({
            "id": self.id,
            "type": "match",
            "attributes": {
                "state": self.state,
                "round": self.round,
                "identifier": "A",
                "scores": "",
                // An integer here, as Challonge's schema has it.
                "winner_id": self.winner.as_ref().map(|id| id.parse::<u64>().unwrap()),
                "underway_at": self.underway.then_some("2026-10-18T19:05:00.000Z"),
                "relationships": {
                    "player1": player(&self.players[0]),
                    "player2": player(&self.players[1]),
                },
            },
        })
    }
}

#[derive(Clone, Debug)]
pub struct Logged {
    pub method: Method,
    pub path: String,
    pub query: String,
    pub headers: HeaderMap,
    pub body: Value,
}

pub struct ChallongeState {
    pub authorization: (String, String),
    /// The tournament's `state`, which Challonge's schema leaves out.
    pub tournament_state: Option<&'static str>,
    pub participants: Vec<(String, Option<String>)>,
    pub matches: Vec<FakeMatch>,
    pub log: Vec<Logged>,
    /// Sets left out of list reads, as a page that shifted between two
    /// requests would leave them out. Read one by one, they are there.
    pub hidden: HashSet<String>,
    /// Every request is answered with this status.
    pub failing: Option<u16>,
    /// Scores are refused, as Challonge refuses one it will not take.
    pub refuse_reports: bool,
    /// Scores are recorded, and the answer is lost (a 502).
    pub lose_report_answers: bool,
    /// The next scores are answered with these statuses, and not recorded.
    pub report_failures: Vec<u16>,
    /// The next scores are answered 200 with this body instead of the
    /// match, and recorded only when the flag says so.
    pub odd_report_answers: Vec<(Value, bool)>,
    /// Sets listed twice, as a page that shifted between two requests can
    /// list a set on both.
    pub listed_twice: HashSet<String>,
}

impl ChallongeState {
    fn find(&mut self, id: &str) -> Option<&mut FakeMatch> {
        self.matches.iter_mut().find(|found| found.id == id)
    }

    /// Fills sets whose feeding sets are both decided, as Challonge
    /// advances a bracket.
    fn advance(&mut self) {
        let winners: HashMap<String, String> = self
            .matches
            .iter()
            .filter_map(|found| Some((found.id.clone(), found.winner.clone()?)))
            .collect();
        for found in &mut self.matches {
            if let (Some([a, b]), "pending") = (&found.fed_by, found.state)
                && let (Some(one), Some(two)) = (winners.get(a), winners.get(b))
            {
                found.players = [Some(one.clone()), Some(two.clone())];
                found.state = "open";
            }
        }
    }

    /// The organizer reopens a set to correct it: it is open again with no
    /// result, and every set after it that it fed goes back to pending,
    /// without its players, as Challonge resets a bracket.
    pub fn reopen(&mut self, id: &str) {
        let found = self.find(id).unwrap();
        found.state = "open";
        found.winner = None;
        found.score_sets = None;
        found.underway = false;
        let mut reset = vec![id.to_owned()];
        while let Some(fed) = reset.pop() {
            for found in &mut self.matches {
                if found
                    .fed_by
                    .as_ref()
                    .is_some_and(|from| from.contains(&fed))
                {
                    found.state = "pending";
                    found.players = [None, None];
                    found.winner = None;
                    found.score_sets = None;
                    found.underway = false;
                    reset.push(found.id.clone());
                }
            }
        }
    }

    /// The organizer reports a set by hand.
    pub fn decide(&mut self, id: &str, winner: usize) {
        let found = self.find(id).unwrap();
        found.winner = found.players[winner].clone();
        found.state = "complete";
        self.advance();
    }
}

pub struct FakeChallonge {
    pub api: String,
    pub state: Arc<Mutex<ChallongeState>>,
}

fn page<T: Clone>(items: &[T], query: &HashMap<String, String>) -> Vec<T> {
    let number = |key: &str, default: usize| {
        query
            .get(key)
            .and_then(|value| value.parse().ok())
            .unwrap_or(default)
    };
    super::page(items, number("page", 1), number("per_page", 25)).0
}

fn answer(status: u16, body: Value) -> Response {
    (
        StatusCode::from_u16(status).unwrap(),
        [(header::CONTENT_TYPE, "application/json")],
        body.to_string(),
    )
        .into_response()
}

fn refusal(status: u16, detail: &str) -> Response {
    answer(
        status,
        json!({ "errors": [{ "status": status, "detail": detail }] }),
    )
}

async fn challonge(
    State(state): State<Arc<Mutex<ChallongeState>>>,
    method: Method,
    uri: Uri,
    headers: HeaderMap,
    body: Bytes,
) -> Response {
    let mut state = state.lock().unwrap();
    let body = serde_json::from_slice(&body).unwrap_or(Value::Null);
    state.log.push(Logged {
        method: method.clone(),
        path: uri.path().to_owned(),
        query: uri.query().unwrap_or_default().to_owned(),
        headers: headers.clone(),
        body: body.clone(),
    });
    let get = |name: &str| {
        headers
            .get(name)
            .and_then(|value| value.to_str().ok())
            .unwrap_or_default()
            .to_owned()
    };
    if let Some(status) = state.failing {
        return refusal(status, "Failing");
    }
    if (get("authorization-type"), get("authorization")) != state.authorization {
        return refusal(401, "Unauthorized");
    }
    if get("content-type") != "application/vnd.api+json" || get("accept") != "application/json" {
        return refusal(415, "Unsupported media type");
    }
    let query: HashMap<String, String> = uri
        .query()
        .unwrap_or_default()
        .split('&')
        .filter_map(|pair| pair.split_once('='))
        .map(|(key, value)| (key.to_owned(), value.to_owned()))
        .collect();
    if (&method, uri.path())
        == (
            &Method::GET,
            format!("/v2.1/tournaments/{TOURNAMENT}.json").as_str(),
        )
    {
        let mut attributes =
            json!({ "name": "Ember weekly", "tournament_type": "double elimination" });
        if let Some(tournament_state) = state.tournament_state {
            attributes["state"] = json!(tournament_state);
        }
        return answer(
            200,
            json!({ "data": { "id": "30201", "type": "tournament", "attributes": attributes } }),
        );
    }
    let prefix = format!("/v2.1/tournaments/{TOURNAMENT}/");
    let Some(rest) = uri.path().strip_prefix(&prefix) else {
        return refusal(404, "Not found");
    };
    let rest = rest.to_owned();
    match (method, rest.as_str()) {
        (Method::GET, "matches.json") => {
            let listed: Vec<Value> = state
                .matches
                .iter()
                .filter(|found| {
                    query
                        .get("state")
                        .is_none_or(|wanted| wanted == found.state)
                        && !state.hidden.contains(&found.id)
                })
                .flat_map(|found| {
                    let times = if state.listed_twice.contains(&found.id) {
                        2
                    } else {
                        1
                    };
                    std::iter::repeat_n(found.to_json(), times)
                })
                .collect();
            answer(200, json!({ "data": page(&listed, &query) }))
        }
        (Method::GET, "participants.json") => {
            let listed: Vec<Value> = state
                .participants
                .iter()
                .map(|(id, username)| {
                    json!({
                        "id": id,
                        "type": "participant",
                        "attributes": { "name": format!("Entry {id}"), "username": username, "tournament_id": 21 },
                    })
                })
                .collect();
            answer(200, json!({ "data": page(&listed, &query) }))
        }
        (Method::GET, path) => {
            let id = path
                .trim_start_matches("matches/")
                .trim_end_matches(".json");
            match state.find(id) {
                Some(found) => answer(200, json!({ "data": found.to_json() })),
                None => refusal(404, "Match not found"),
            }
        }
        (Method::PUT, path) if path.ends_with("/change_state.json") => {
            let id = path
                .trim_start_matches("matches/")
                .trim_end_matches("/change_state.json");
            let change = body["data"]["attributes"]["state"]
                .as_str()
                .unwrap_or_default();
            let Some(found) = state.find(id) else {
                return refusal(404, "Match not found");
            };
            match change {
                "mark_as_underway" if found.state == "open" => found.underway = true,
                _ => return refusal(422, "That state change is not possible"),
            }
            let found = found.to_json();
            answer(200, json!({ "data": found }))
        }
        (Method::PUT, path) => {
            let id = path
                .trim_start_matches("matches/")
                .trim_end_matches(".json");
            let scored = body["data"]["attributes"]["match"]
                .as_array()
                .cloned()
                .unwrap_or_default();
            if state.refuse_reports {
                return refusal(422, "Scores cannot be reported for this match");
            }
            if !state.report_failures.is_empty() {
                let status = state.report_failures.remove(0);
                return refusal(status, "Unsupported Media Type");
            }
            let odd =
                (!state.odd_report_answers.is_empty()).then(|| state.odd_report_answers.remove(0));
            if let Some((body, false)) = &odd {
                return answer(200, body.clone());
            }
            let lose_answer = state.lose_report_answers;
            let Some(found) = state.find(id) else {
                return refusal(404, "Match not found");
            };
            if found.state != "open" {
                return refusal(422, "Match is not open");
            }
            let score_of = |id: &Option<String>| {
                scored
                    .iter()
                    .find(|entry| entry["participant_id"].as_str() == id.as_deref())
                    .and_then(|entry| entry["score_set"].as_str())
                    .map(str::to_owned)
            };
            let (Some(one), Some(two)) = (score_of(&found.players[0]), score_of(&found.players[1]))
            else {
                return refusal(422, "Both participants need a score");
            };
            found.score_sets = Some([one, two]);
            found.winner = scored
                .iter()
                .find(|entry| entry["advancing"] == true)
                .and_then(|entry| entry["participant_id"].as_str())
                .map(str::to_owned);
            found.state = "complete";
            let found = found.to_json();
            state.advance();
            if lose_answer {
                return refusal(502, "Bad gateway");
            }
            match odd {
                Some((body, _)) => answer(200, body),
                None => answer(200, json!({ "data": found })),
            }
        }
        _ => refusal(404, "Not found"),
    }
}

impl FakeChallonge {
    pub async fn start(
        kind: CredentialKind,
        participants: Vec<(String, Option<String>)>,
        matches: Vec<FakeMatch>,
    ) -> Self {
        let authorization = match kind {
            CredentialKind::V1Key => ("v1".to_owned(), CHALLONGE_KEY.to_owned()),
            CredentialKind::Bearer => ("v2".to_owned(), format!("Bearer {CHALLONGE_KEY}")),
        };
        let state = Arc::new(Mutex::new(ChallongeState {
            authorization,
            tournament_state: None,
            participants,
            matches,
            hidden: HashSet::new(),
            failing: None,
            refuse_reports: false,
            lose_report_answers: false,
            report_failures: Vec::new(),
            odd_report_answers: Vec::new(),
            listed_twice: HashSet::new(),
            log: Vec::new(),
        }));
        let app = Router::new().fallback(challonge).with_state(state.clone());
        let api = super::serve(app).await;
        Self { api, state }
    }

    pub fn log(&self) -> Vec<Logged> {
        std::mem::take(&mut self.state.lock().unwrap().log)
    }

    pub fn find(&self, id: &str) -> FakeMatch {
        self.state.lock().unwrap().find(id).unwrap().clone()
    }

    pub fn decide(&self, id: &str, winner: usize) {
        self.state.lock().unwrap().decide(id, winner);
    }

    pub fn reopen(&self, id: &str) {
        self.state.lock().unwrap().reopen(id);
    }
}

/// `METHOD path?query` of each request, for comparing.
pub fn lines(log: &[Logged]) -> Vec<String> {
    log.iter()
        .map(|request| {
            let path = request
                .path
                .trim_start_matches(&format!("/v2.1/tournaments/{TOURNAMENT}"));
            match request.query.as_str() {
                "" => format!("{} {path}", request.method),
                query => format!("{} {path}?{query}", request.method),
            }
        })
        .collect()
}
