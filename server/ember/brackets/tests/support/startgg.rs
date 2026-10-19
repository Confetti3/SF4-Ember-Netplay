//! A fake start.gg GraphQL endpoint for one event: it answers the queries
//! and mutations the connector sends, by operation name, checks the token,
//! and logs each operation with its variables. For the bridge it also signs
//! users in (`oauth`, JSON token bodies, refresh at its own path), and
//! answers `currentUser` and the events its admin runs.
use std::{
    collections::{HashMap, HashSet, VecDeque},
    sync::{Arc, Mutex},
};

use axum::{
    Router,
    body::Bytes,
    extract::{RawQuery, State},
    http::{HeaderMap, StatusCode, header},
    response::{IntoResponse, Response},
    routing::post,
};
use serde_json::{Value, json};

use super::{Extra, Setup, World, oauth::OAuth};

pub const PLAYER1_USER: u64 = 5_001;
pub const STARTGG_TOKEN: &str = "test-startgg-token";
pub const EVENT_SLUG: &str = "tournament/ember-weekly/event/usf4";
const EVENT_ID: u64 = 769_488;
pub const CLIENT_ID: &str = "ember-startgg-client";
pub const CLIENT_SECRET: &str = "startgg-client-secret";

#[derive(Clone, Debug)]
pub struct FakeEntrant {
    pub id: u64,
    /// The start.gg user behind it, and the Discord account that user linked.
    pub user: Option<u64>,
    pub discord: Option<String>,
}

#[derive(Clone, Debug)]
pub struct FakeSet {
    pub id: u64,
    pub state: i64,
    pub round: i32,
    pub entrants: [Option<u64>; 2],
    pub winner: Option<u64>,
    /// Each reported game's winning entrant, in order.
    pub games: Vec<u64>,
    /// Merged into each game as start.gg answers it: what else the
    /// organizer entered for it.
    pub game_extra: Value,
    /// Its phase's ID and name.
    pub phase: Option<(u64, &'static str)>,
    /// The name of the stream it is assigned to.
    pub stream: Option<&'static str>,
}

impl FakeSet {
    pub fn open(id: u64, round: i32, entrants: [u64; 2]) -> Self {
        Self {
            id,
            state: 1,
            round,
            entrants: entrants.map(Some),
            winner: None,
            games: Vec::new(),
            game_extra: json!({}),
            phase: None,
            stream: None,
        }
    }

    fn to_json(&self) -> Value {
        json!({
            // Numbers, as start.gg answers most IDs.
            "id": self.id,
            "state": self.state,
            "round": self.round,
            "fullRoundText": if self.round < 0 { format!("Losers Round {}", -self.round) } else { format!("Winners Round {}", self.round) },
            "winnerId": self.winner,
            "slots": self.entrants.iter().enumerate().map(|(index, entrant)| json!({
                "slotIndex": index,
                "entrant": entrant.map(|id| json!({ "id": id.to_string() })),
            })).collect::<Vec<_>>(),
            "games": self.games.iter().enumerate().map(|(index, winner)| {
                let mut game = json!({ "orderNum": index + 1, "winnerId": winner });
                for (key, value) in self.game_extra.as_object().into_iter().flatten() {
                    game[key] = value.clone();
                }
                game
            }).collect::<Vec<_>>(),
            "phaseGroup": self.phase.map(|(id, name)| json!({ "phase": { "id": id, "name": name } })),
            "stream": self.stream.map(|name| json!({ "id": 501, "streamName": name })),
        })
    }
}

#[derive(Debug)]
pub struct Logged {
    pub operation: String,
    pub variables: Value,
}

pub struct StartggState {
    pub oauth: OAuth,
    /// The user who runs the event.
    pub admin: Option<String>,
    /// Every request's operation and query text, kept when `log` is taken.
    pub asked: Vec<(String, String)>,
    pub entrants: Vec<FakeEntrant>,
    pub sets: Vec<FakeSet>,
    pub log: Vec<Logged>,
    /// The organizer reports this set by hand (the first entrant winning)
    /// just as the connector reads it again before reporting.
    pub decide_on_reread: Option<u64>,
    /// The next reports are refused with these HTTP statuses and error
    /// texts, and not recorded.
    pub report_errors: VecDeque<(StatusCode, String)>,
    /// Reports of these sets are refused with these texts, every time.
    pub refused_sets: HashMap<u64, String>,
    /// The next reports are answered with this `data` instead of the set,
    /// and recorded only when the flag says so.
    pub report_answers: VecDeque<(Value, bool)>,
    /// This set turns pending (state 5, its entrants kept) just as the
    /// connector reads it again before reporting.
    pub pend_on_reread: Option<u64>,
    /// Sets start.gg no longer has, as after the organizer reset their
    /// phase group: left out of lists and answered "Set not found".
    pub gone_sets: HashSet<u64>,
    /// Discord authorizations are private to the token: each comes back
    /// null, with an error naming the field.
    pub private_discord: bool,
    /// Its players start this set on start.gg just after a list of sets
    /// shows it not started.
    pub start_after_list: Option<u64>,
}

impl StartggState {
    fn find(&mut self, id: &Value) -> Option<&mut FakeSet> {
        let id = id.as_str()?.parse::<u64>().ok()?;
        self.sets.iter_mut().find(|set| set.id == id)
    }
}

pub struct FakeStartgg {
    pub origin: String,
    pub state: Arc<Mutex<StartggState>>,
}

fn page<T: Clone>(items: &[T], variables: &Value) -> (Vec<T>, usize) {
    let number = |key: &str, default: u64| variables[key].as_u64().unwrap_or(default) as usize;
    super::page(items, number("page", 1), number("perPage", 40))
}

fn answer(status: StatusCode, body: Value) -> Response {
    (
        status,
        [(header::CONTENT_TYPE, "application/json")],
        body.to_string(),
    )
        .into_response()
}

async fn graphql(
    State(state): State<Arc<Mutex<StartggState>>>,
    headers: HeaderMap,
    body: Bytes,
) -> Response {
    let request: Value = serde_json::from_slice(&body).unwrap();
    let mut state = state.lock().unwrap();
    let signed_in = state.oauth.bearer(&headers).cloned();
    let authorized = headers
        .get("authorization")
        .and_then(|value| value.to_str().ok())
        == Some(&format!("Bearer {STARTGG_TOKEN}"))
        || signed_in.is_some();
    if !authorized {
        return answer(
            StatusCode::UNAUTHORIZED,
            json!({ "success": false, "message": "Invalid authentication token" }),
        );
    }
    let query = request["query"].as_str().unwrap_or_default();
    let operation = query
        .split(['(', '{', ' '])
        .filter(|word| !word.is_empty())
        .nth(1)
        .unwrap_or_default()
        .to_owned();
    let variables = request["variables"].clone();
    // A signed-in user's token reports only with the reporter scope.
    let reporting = query.trim_start().starts_with("mutation");
    if reporting && signed_in.is_some() && !state.oauth.allows(&headers, "tournament.reporter") {
        return answer(
            StatusCode::OK,
            json!({ "data": null, "errors": [{ "message": "Your token does not have permission" }] }),
        );
    }
    state.log.push(Logged {
        operation: operation.clone(),
        variables: variables.clone(),
    });
    state.asked.push((operation.clone(), query.to_owned()));
    let data = match operation.as_str() {
        "CurrentUser" => {
            let user = signed_in.map(|((id, tag), _)| {
                json!({ "id": id.parse::<u64>().unwrap(), "player": { "gamerTag": tag } })
            });
            json!({ "currentUser": user })
        }
        "OrganizerEvents" => {
            let user = signed_in.map(|((id, _), _)| id);
            let nodes = if user.is_some() && user == state.admin {
                json!([{ "name": "Ember Weekly", "events": [{ "name": "USF4", "slug": EVENT_SLUG }] }])
            } else {
                json!([])
            };
            json!({ "currentUser": { "tournaments": { "nodes": nodes } } })
        }
        "Event" => {
            assert_eq!(variables["slug"], EVENT_SLUG);
            let over = state.sets.iter().all(|set| set.state == 3);
            json!({ "event": { "id": EVENT_ID, "state": if over { "COMPLETED" } else { "ACTIVE" } } })
        }
        "Entrants" => {
            assert_eq!(variables["id"], EVENT_ID.to_string());
            let nodes: Vec<Value> = state
                .entrants
                .iter()
                .map(|entrant| {
                    let user = entrant.user.map(|id| {
                        let authorizations: Vec<Value> = entrant
                            .discord
                            .iter()
                            .map(|external| json!({ "externalId": external }))
                            .collect();
                        let authorizations = if state.private_discord {
                            Value::Null
                        } else {
                            json!(authorizations)
                        };
                        json!({ "id": id, "authorizations": authorizations })
                    });
                    json!({ "id": entrant.id, "participants": [{ "user": user }] })
                })
                .collect();
            let (nodes, total) = page(&nodes, &variables);
            let data = json!({ "event": { "entrants": { "pageInfo": { "totalPages": total }, "nodes": nodes } } });
            if state.private_discord {
                return answer(
                    StatusCode::OK,
                    json!({
                        "data": data,
                        "errors": [{ "message": "Cannot query field externalId", "path": ["event", "entrants", "nodes", 0, "participants", 0, "user", "authorizations"] }],
                    }),
                );
            }
            data
        }
        "Sets" => {
            let open: Vec<Value> = state
                .sets
                .iter()
                .filter(|set| {
                    [1, 2, 6].contains(&set.state)
                        && set.entrants.iter().any(Option::is_some)
                        && !state.gone_sets.contains(&set.id)
                })
                .map(FakeSet::to_json)
                .collect();
            let (nodes, total) = page(&open, &variables);
            if let Some(id) = state.start_after_list.take()
                && let Some(set) = state.sets.iter_mut().find(|set| set.id == id)
            {
                set.state = 2;
            }
            json!({ "event": { "sets": { "pageInfo": { "totalPages": total }, "nodes": nodes } } })
        }
        "Set" => {
            if let Some(id) = state.decide_on_reread.take()
                && let Some(set) = state.sets.iter_mut().find(|set| set.id == id)
            {
                set.winner = set.entrants[0];
                set.state = 3;
            }
            if let Some(id) = state.pend_on_reread.take()
                && let Some(set) = state.sets.iter_mut().find(|set| set.id == id)
            {
                set.state = 5;
            }
            let id = variables["id"].as_str().unwrap_or_default().to_owned();
            if id
                .parse::<u64>()
                .is_ok_and(|id| state.gone_sets.contains(&id))
            {
                return answer(
                    StatusCode::OK,
                    json!({
                        "data": { "set": null },
                        "errors": [{ "message": format!("Set not found for id: {id}"), "path": ["set"] }],
                    }),
                );
            }
            let found = state.find(&variables["id"]).map(|set| set.to_json());
            json!({ "set": found })
        }
        "Underway" => match state.find(&variables["setId"]) {
            Some(set) if set.state == 2 => {
                return answer(
                    StatusCode::OK,
                    json!({ "data": null, "errors": [{ "message": "Set is already started" }] }),
                );
            }
            Some(set) => {
                set.state = 2;
                json!({ "markSetInProgress": { "id": set.id, "state": 2 } })
            }
            None => Value::Null,
        },
        "Report" => {
            if let Some((status, message)) = state.report_errors.pop_front() {
                return answer(status, json!({ "errors": [{ "message": message }] }));
            }
            let id = variables["setId"]
                .as_str()
                .and_then(|id| id.parse::<u64>().ok());
            if let Some(message) = id.and_then(|id| state.refused_sets.get(&id)) {
                return answer(
                    StatusCode::OK,
                    json!({ "errors": [{ "message": message }] }),
                );
            }
            let winner = variables["winnerId"]
                .as_str()
                .and_then(|id| id.parse().ok());
            let games: Vec<u64> = variables["gameData"]
                .as_array()
                .unwrap()
                .iter()
                .map(|game| game["winnerId"].as_str().unwrap().parse().unwrap())
                .collect();
            let odd = state.report_answers.pop_front();
            let Some(set) = state.find(&variables["setId"]) else {
                return answer(
                    StatusCode::OK,
                    json!({ "errors": [{ "message": "Set not found" }] }),
                );
            };
            if odd.as_ref().is_none_or(|(_, recorded)| *recorded) {
                set.winner = winner;
                set.games = games;
                set.state = 3;
            }
            match odd {
                Some((data, _)) => data,
                None => {
                    json!({ "reportBracketSet": [{ "id": set.id, "state": 3, "winnerId": set.winner }] })
                }
            }
        }
        other => panic!("unexpected start.gg operation {other}"),
    };
    answer(
        StatusCode::OK,
        json!({ "data": data, "extensions": { "queryComplexity": 4 } }),
    )
}

async fn authorize(
    State(state): State<Arc<Mutex<StartggState>>>,
    RawQuery(query): RawQuery,
) -> Response {
    state
        .lock()
        .unwrap()
        .oauth
        .authorize(&query.unwrap_or_default())
}

async fn token(
    State(state): State<Arc<Mutex<StartggState>>>,
    headers: HeaderMap,
    body: Bytes,
) -> Response {
    state.lock().unwrap().oauth.token(&headers, &body)
}

impl FakeStartgg {
    pub async fn start(entrants: Vec<FakeEntrant>, sets: Vec<FakeSet>) -> Self {
        let state = Arc::new(Mutex::new(StartggState {
            oauth: OAuth::new(CLIENT_ID, CLIENT_SECRET),
            admin: None,
            asked: Vec::new(),
            entrants,
            sets,
            log: Vec::new(),
            decide_on_reread: None,
            report_errors: VecDeque::new(),
            refused_sets: HashMap::new(),
            report_answers: VecDeque::new(),
            pend_on_reread: None,
            gone_sets: HashSet::new(),
            private_discord: false,
            start_after_list: None,
        }));
        let app = Router::new()
            .route("/gql/alpha", post(graphql))
            .route("/oauth/authorize", axum::routing::get(authorize))
            .route("/oauth/access_token", post(token))
            .route("/oauth/refresh", post(token))
            .with_state(state.clone());
        let origin = super::serve(app).await;
        Self { origin, state }
    }

    /// The operations sent since the last call.
    pub fn log(&self) -> Vec<Logged> {
        std::mem::take(&mut self.state.lock().unwrap().log)
    }

    pub fn operations(&self) -> Vec<String> {
        self.log()
            .into_iter()
            .map(|logged| logged.operation)
            .collect()
    }

    /// The query text of the last request for `operation`.
    pub fn query(&self, operation: &str) -> String {
        self.state
            .lock()
            .unwrap()
            .asked
            .iter()
            .rev()
            .find(|(asked, _)| asked == operation)
            .map(|(_, query)| query.clone())
            .unwrap_or_default()
    }

    pub fn set(&self, id: u64) -> FakeSet {
        self.state
            .lock()
            .unwrap()
            .sets
            .iter()
            .find(|set| set.id == id)
            .unwrap()
            .clone()
    }
}

/// The fake as a tournament of the connector's.
pub fn extra(fake: &FakeStartgg) -> Extra {
    Extra {
        adapter: ember_brackets::Adapter::Startgg,
        tournament: EVENT_SLUG.into(),
        api_base: fake.origin.clone(),
        token: STARTGG_TOKEN,
    }
}

/// Four entrants with start.gg accounts connected: the first two play set
/// 65089253, the other two set 65089255.
pub async fn four(sets: Vec<FakeSet>, setup: Setup) -> (FakeStartgg, World) {
    let entrant = |id: u64, user: u64| FakeEntrant {
        id,
        user: Some(user),
        discord: None,
    };
    let fake = FakeStartgg::start(
        vec![
            entrant(2_401_411, PLAYER1_USER),
            entrant(2_401_412, 5_002),
            entrant(2_401_414, 5_003),
            entrant(2_401_415, 5_004),
        ],
        sets,
    )
    .await;
    let world = World::start(Setup {
        challonge: false,
        extra: vec![extra(&fake)],
        ..setup
    })
    .await;
    world.bridge.connect_startgg(1, PLAYER1_USER).await;
    world.bridge.connect_startgg(2, 5_002).await;
    world.bridge.connect_startgg(3, 5_003).await;
    world.bridge.connect_startgg(4, 5_004).await;
    (fake, world)
}
