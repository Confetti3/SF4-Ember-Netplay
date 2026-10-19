//! A fake Challonge v2.1 for one tournament: it serves the routes the
//! connector calls, checks their headers, logs every request, and advances
//! its bracket as Challonge does when a set is decided. For the bridge it
//! also signs users in (`oauth`), answers `/me`, lists and creates the
//! tournament, takes participants and starts a four-player double
//! elimination bracket, as an organizer does on Challonge.
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

use super::{CHALLONGE_KEY, TOURNAMENT, oauth::OAuth};

pub const CLIENT_ID: &str = "ember-challonge-client";
pub const CLIENT_SECRET: &str = "challonge-client-secret";

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
    /// Which of the two take their set's loser instead.
    pub losers: [bool; 2],
    /// The group of a group stage match.
    pub group_id: Option<Value>,
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
            losers: [false; 2],
            group_id: None,
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
            losers: [false; 2],
            group_id: None,
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
                "group_id": self.group_id,
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
    pub oauth: OAuth,
    /// The tournament's ID in paths, its name and the user who made it.
    pub tournament: String,
    pub name: String,
    pub owner: Option<String>,
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

    fn tournament_json(&self) -> Value {
        let mut attributes = json!({ "name": self.name, "tournament_type": "double elimination",
                                     "url": self.tournament });
        if let Some(tournament_state) = self.tournament_state {
            attributes["state"] = json!(tournament_state);
        }
        json!({ "id": self.tournament, "type": "tournament", "attributes": attributes })
    }

    /// Fills sets whose feeding sets are both decided, as Challonge
    /// advances a bracket.
    fn advance(&mut self) {
        // Each decided set's winner and loser.
        let decided: HashMap<String, [String; 2]> = self
            .matches
            .iter()
            .filter_map(|found| {
                let winner = found.winner.clone()?;
                let loser = found.players.iter().flatten().find(|id| **id != winner)?;
                Some((found.id.clone(), [winner, loser.clone()]))
            })
            .collect();
        for found in &mut self.matches {
            if let (Some([a, b]), "pending") = (&found.fed_by, found.state)
                && let (Some(one), Some(two)) = (decided.get(a), decided.get(b))
            {
                let [one_lost, two_lost] = found.losers.map(usize::from);
                found.players = [Some(one[one_lost].clone()), Some(two[two_lost].clone())];
                found.state = "open";
            }
        }
    }

    /// Starts a four-player double elimination bracket, seeds 1 v 4 and 2 v
    /// 3, with a grand final and no reset, as Challonge numbers its rounds
    /// (losers' rounds negative).
    fn start(&mut self) {
        let seeds: Vec<&str> = self
            .participants
            .iter()
            .map(|(id, _)| id.as_str())
            .collect();
        let [one, two, three, four] = seeds[..] else {
            panic!("the fake starts brackets of four");
        };
        let losers = |mut found: FakeMatch, losers| {
            found.losers = losers;
            found
        };
        self.matches = vec![
            FakeMatch::open("1", 1, [one, four]),
            FakeMatch::open("2", 1, [two, three]),
            FakeMatch::fed_by("3", 2, ["1", "2"]),
            losers(FakeMatch::fed_by("4", -1, ["1", "2"]), [true, true]),
            losers(FakeMatch::fed_by("5", -2, ["3", "4"]), [true, false]),
            FakeMatch::fed_by("6", 3, ["3", "5"]),
        ];
        self.tournament_state = Some("underway");
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
    body_bytes: Bytes,
) -> Response {
    let mut state = state.lock().unwrap();
    let body: Value = serde_json::from_slice(&body_bytes).unwrap_or(Value::Null);
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
    match (&method, uri.path()) {
        (&Method::GET, "/oauth/authorize") => {
            return state.oauth.authorize(uri.query().unwrap_or_default());
        }
        (&Method::POST, "/oauth/token") => return state.oauth.token(&headers, &body_bytes),
        _ => {}
    }
    let signed_in = get("authorization-type") == "v2" && state.oauth.bearer(&headers).is_some();
    if (get("authorization-type"), get("authorization")) != state.authorization && !signed_in {
        return refusal(401, "Unauthorized");
    }
    // A signed-in user's token reads and changes only what its scope allows.
    let scope = match (&method, uri.path()) {
        (_, "/v2.1/me.json") => "me",
        (&Method::GET, path) if path.contains("/matches") => "matches:read",
        (_, path) if path.contains("/matches") => "matches:write",
        (&Method::GET, path) if path.contains("/participants") => "participants:read",
        (_, path) if path.contains("/participants") => "participants:write",
        (&Method::GET, _) => "tournaments:read",
        _ => "tournaments:write",
    };
    if signed_in && !state.oauth.allows(&headers, scope) {
        return refusal(403, "Forbidden");
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
    let user = state.oauth.bearer(&headers).map(|((id, _), _)| id.clone());
    match (&method, uri.path()) {
        (&Method::GET, "/v2.1/me.json") => {
            let ((id, username), _) = state.oauth.bearer(&headers).unwrap();
            return answer(
                200,
                json!({ "data": { "id": id, "type": "user", "attributes": { "username": username } } }),
            );
        }
        (&Method::GET, "/v2.1/tournaments.json") => {
            let mine = state.owner.is_some() && state.owner == user;
            let listed = if mine {
                vec![state.tournament_json()]
            } else {
                Vec::new()
            };
            return answer(200, json!({ "data": listed }));
        }
        (&Method::POST, "/v2.1/tournaments.json") => {
            let attributes = &body["data"]["attributes"];
            let Some(name) = attributes["name"].as_str().filter(|name| !name.is_empty()) else {
                return refusal(422, "Name can't be blank");
            };
            if body["data"]["type"] != "tournaments"
                || attributes["tournament_type"] != "double elimination"
            {
                return refusal(422, "Tournament type is invalid");
            }
            state.name = name.to_owned();
            state.tournament = "30201".into();
            state.owner = user;
            return answer(200, json!({ "data": state.tournament_json() }));
        }
        (&Method::GET, path) if path == format!("/v2.1/tournaments/{}.json", state.tournament) => {
            return answer(200, json!({ "data": state.tournament_json() }));
        }
        _ => {}
    }
    let prefix = format!("/v2.1/tournaments/{}/", state.tournament);
    let Some(rest) = uri.path().strip_prefix(&prefix) else {
        return refusal(404, "Not found");
    };
    let rest = rest.to_owned();
    match (method, rest.as_str()) {
        (Method::POST, "participants.json") => {
            let attributes = &body["data"]["attributes"];
            let Some(name) = attributes["name"].as_str() else {
                return refusal(422, "Name can't be blank");
            };
            let id = (355 + state.participants.len()).to_string();
            let username = attributes["username"].as_str().map(str::to_owned);
            state.participants.push((id.clone(), username.clone()));
            answer(
                200,
                json!({ "data": { "id": id, "type": "participant", "attributes": {
                    "name": name, "username": username, "tournament_id": 30201,
                    "seed": state.participants.len() } } }),
            )
        }
        (Method::PUT, "change_state.json") => {
            match body["data"]["attributes"]["state"].as_str() {
                Some("start") if state.matches.is_empty() => state.start(),
                Some("finalize") if state.matches.iter().all(|found| found.state == "complete") => {
                    state.tournament_state = Some("complete");
                }
                _ => return refusal(422, "That state change is not possible"),
            }
            answer(200, json!({ "data": state.tournament_json() }))
        }
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
                "reopen" if found.state == "complete" => state.reopen(id),
                _ => return refusal(422, "That state change is not possible"),
            }
            let found = state.find(id).unwrap().to_json();
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
            oauth: OAuth::new(CLIENT_ID, CLIENT_SECRET),
            tournament: TOURNAMENT.into(),
            name: "Ember weekly".into(),
            owner: None,
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
