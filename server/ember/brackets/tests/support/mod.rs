//! Fake bracket sites (Challonge, start.gg, an organizer's own service), a
//! real bridge on a loopback port, and a connector between them. All keys
//! and tokens here are throwaway test values.
#![allow(dead_code)]

pub mod bridge;
pub mod challonge;
pub mod service;
pub mod startgg;

use std::path::{Path, PathBuf};

use axum::{
    Router,
    http::{Method, StatusCode},
};
use ember_brackets::{
    Adapter, Bridge, Clock, Config, Connector, Credential, CredentialKind, Hold, Results,
    RoundRule, Rounds, Side, Tournament,
};
use ember_protocol::webhook::Secret;
use serde_json::{Value, json};
use tokio::net::TcpListener;

use bridge::TestBridge;
use challonge::{FakeChallonge, FakeMatch};

pub const TOURNAMENT: &str = "ember_weekly";
pub const CHALLONGE_KEY: &str = "test-challonge-api-key";
pub const CONNECTION: &str = "brackets";

/// Serves `app` on a loopback port of its own; returns its origin.
pub async fn serve(app: Router) -> String {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let origin = format!("http://{}", listener.local_addr().unwrap());
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    origin
}

/// Page `page` (from 1) of `per_page` items, and how many pages there are.
pub fn page<T: Clone>(items: &[T], page: usize, per_page: usize) -> (Vec<T>, usize) {
    let slice = items
        .iter()
        .skip(page.saturating_sub(1) * per_page)
        .take(per_page)
        .cloned()
        .collect();
    (slice, items.len().div_ceil(per_page).max(1))
}

/// A connector's configuration over `dir`, with no tournament yet: its
/// provider credential, result secret and Challonge key written there.
pub fn config(dir: &Path, bridge_api: &str, provider: &str) -> Config {
    std::fs::write(dir.join("provider.token"), format!("{provider}\n")).unwrap();
    std::fs::write(dir.join("result.secret"), result_secret().reveal().as_str()).unwrap();
    std::fs::write(dir.join("challonge.key"), CHALLONGE_KEY).unwrap();
    Config {
        bridge: Bridge {
            api: bridge_api.into(),
            credential_file: dir.join("provider.token"),
            connection_id: CONNECTION.into(),
            build_id: "test-build".into(),
        },
        results: Results {
            listen: "127.0.0.1:0".into(),
            path: "/results".into(),
            secret_file: dir.join("result.secret"),
            previous_secret_file: None,
        },
        database: dir.join("brackets.sqlite3"),
        poll_secs: 120,
        max_poll_secs: 600,
        monthly_request_budget: 500,
        allow_loopback_http: true,
        tournaments: Vec::new(),
    }
}

/// A connector over `config`, with start.gg's rate raised so that no test
/// waits on it.
pub fn connector(config: &Config, clock: &Clock) -> Connector {
    Connector::with_startgg_rate(config.clone(), clock.clone(), (10_000, 60)).unwrap()
}

/// The Challonge tournament, on the key `config` wrote.
pub fn challonge_tournament(dir: &Path, kind: CredentialKind, api_base: &str) -> Tournament {
    Tournament {
        adapter: Adapter::Challonge,
        bridge_site: None,
        tournament: TOURNAMENT.into(),
        credential: Credential {
            kind,
            file: dir.join("challonge.key"),
        },
        rounds: rounds(),
        hold: Hold::default(),
        active_from: None,
        active_until: None,
        mark_underway: true,
        api_base: Some(api_base.into()),
    }
}

pub fn temp(name: &str) -> PathBuf {
    let mut bytes = [0u8; 8];
    getrandom::fill(&mut bytes).unwrap();
    let dir = std::env::temp_dir().join(format!(
        "ember-brackets-{name}-{}",
        ember_protocol::encoding::b64u(&bytes)
    ));
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

pub fn result_secret() -> Secret {
    Secret::from_bytes([7; 32])
}

pub struct World {
    pub challonge: FakeChallonge,
    pub bridge: TestBridge,
    pub connector: Connector,
    /// Where the connector receives results.
    pub results_url: String,
    /// The connector's clock, frozen at the same second as the bridge's;
    /// `advance` moves both.
    pub clock: Clock,
    pub provider: String,
    pub organizer: String,
    pub config: Config,
    dir: PathBuf,
}

impl Drop for World {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.dir);
    }
}

/// First to 2, first to 3 from round 2 on.
pub fn rounds() -> Rounds {
    Rounds {
        games_to_win: 2,
        rules: vec![RoundRule {
            phase: None,
            from_round: 2,
            to_round: None,
            side: Side::Any,
            games_to_win: 3,
        }],
    }
}

/// A tournament on another fake site, beside or instead of the Challonge
/// one.
pub struct Extra {
    pub adapter: Adapter,
    pub tournament: String,
    pub api_base: String,
    pub token: &'static str,
}

pub struct Setup {
    /// Whether the connector runs the fake Challonge tournament.
    pub challonge: bool,
    pub extra: Vec<Extra>,
    pub kind: CredentialKind,
    pub budget: u32,
    pub active_from: Option<String>,
    pub active_until: Option<String>,
    /// Whether the connector answers on its results URL. Without it every
    /// delivery fails, as one lost in the network would.
    pub serve_results: bool,
    /// Whether the bridge connection may look players up by Discord account.
    pub discord_lookup: bool,
    /// Every tournament's set lengths and held sets.
    pub rounds: Rounds,
    pub hold: Hold,
    pub participants: Vec<(String, Option<String>)>,
    pub matches: Vec<FakeMatch>,
    /// Whether the fake Challonge sends matches in the shape the published
    /// schema describes (players in `relationships`) instead of the live
    /// API's (players in `points_by_participant`), which is the default.
    pub documented_shape: bool,
}

impl Default for Setup {
    /// Player 1 (355) and Player 2 (354) play set 1001; Player 3 (353) and an entry with
    /// no account (352) play 1002; their winners meet in 1003.
    fn default() -> Self {
        Self {
            challonge: true,
            extra: Vec::new(),
            kind: CredentialKind::V1Key,
            budget: 500,
            active_from: None,
            active_until: None,
            serve_results: true,
            discord_lookup: true,
            rounds: rounds(),
            hold: Hold::default(),
            participants: [
                ("355", Some("Player1")),
                ("354", Some("player_2")),
                ("353", Some("player3")),
                ("352", None),
            ]
            .map(|(id, username)| (id.to_owned(), username.map(str::to_owned)))
            .into(),
            matches: vec![
                FakeMatch::open("1001", 1, ["355", "354"]),
                FakeMatch::open("1002", 1, ["353", "352"]),
                FakeMatch::fed_by("1003", 2, ["1001", "1002"]),
            ],
            documented_shape: false,
        }
    }
}

impl World {
    pub async fn start(setup: Setup) -> Self {
        let dir = temp("connector");
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let results_url = format!("http://{}/results", listener.local_addr().unwrap());
        let challonge = FakeChallonge::start(setup.kind, setup.participants, setup.matches).await;
        challonge.state.lock().unwrap().documented_shape = setup.documented_shape;
        let bridge = TestBridge::start(&results_url, setup.discord_lookup).await;
        let provider = bridge.provider().await;
        let organizer = bridge.organizer().await;
        let mut config = config(&dir, &bridge.origin, &provider);
        config.monthly_request_budget = setup.budget;
        if setup.challonge {
            config.tournaments.push(Tournament {
                active_from: setup.active_from.clone(),
                active_until: setup.active_until.clone(),
                rounds: setup.rounds.clone(),
                hold: setup.hold,
                ..challonge_tournament(&dir, setup.kind, &challonge.api)
            });
        }
        for extra in setup.extra {
            let file = dir.join(format!("{}.token", extra.adapter.name()));
            std::fs::write(&file, extra.token).unwrap();
            config.tournaments.push(Tournament {
                adapter: extra.adapter,
                bridge_site: None,
                tournament: extra.tournament,
                credential: Credential {
                    kind: CredentialKind::Bearer,
                    file,
                },
                rounds: setup.rounds.clone(),
                hold: setup.hold,
                active_from: setup.active_from.clone(),
                active_until: setup.active_until.clone(),
                mark_underway: true,
                api_base: Some(extra.api_base),
            });
        }
        // Both clocks stand still from here until a test moves them.
        bridge.clock.freeze();
        let clock = Clock::frozen(bridge.clock.now());
        let connector = connector(&config, &clock);
        if setup.serve_results {
            connector.serve(listener).unwrap();
        }
        Self {
            challonge,
            bridge,
            connector,
            results_url,
            clock,
            provider,
            organizer,
            config,
            dir,
        }
    }

    /// Moves the connector's and the bridge's clocks on together.
    pub fn advance(&self, seconds: i64) {
        self.clock.advance(seconds);
        self.bridge.clock.advance(seconds);
    }

    /// The connector's database, as a test reaches into it to stage what a
    /// crash would leave behind.
    pub fn database(&self) -> rusqlite::Connection {
        rusqlite::Connection::open(&self.config.database).unwrap()
    }

    /// How the connector's attempt at a set stands (`waiting`, `live`,
    /// `reported`, `stuck`, ...).
    pub fn progress(&self, bracket: &str, set_id: &str) -> String {
        self.database()
            .query_row(
                "SELECT progress FROM sets WHERE bracket = ?1 AND set_id = ?2",
                [bracket, set_id],
                |row| row.get(0),
            )
            .unwrap()
    }

    /// A connector over the same files, as after a restart.
    pub fn restarted(&self) -> Connector {
        connector(&self.config, &self.clock)
    }

    /// The organizer records each slot in turn as a game's winner.
    pub async fn play(&self, match_id: &str, winners: &[u8]) {
        for (game, slot) in winners.iter().enumerate() {
            let (_, found) = self
                .bridge
                .send(
                    &self.provider,
                    Method::GET,
                    &format!("/v1/matches/{match_id}"),
                    None,
                    None,
                )
                .await;
            let (status, decided) = self
                .bridge
                .send(
                    &self.organizer,
                    Method::POST,
                    &format!("/v1/matches/{match_id}/adjudications"),
                    Some(json!({
                        "kind": "game_result", "winner_slot": slot, "reason": "test",
                        "expected_revision": found["revision"],
                    })),
                    Some(&format!("{match_id}-game-{game}")),
                )
                .await;
            assert_eq!(status, StatusCode::CREATED, "{decided}");
        }
    }

    /// Cancels an Ember match as its connection, which the bridge sends as a
    /// restart.
    pub async fn cancel(&self, match_id: &str) {
        let (_, found) = self
            .bridge
            .send(
                &self.provider,
                Method::GET,
                &format!("/v1/matches/{match_id}"),
                None,
                None,
            )
            .await;
        let (status, cancelled) = self
            .bridge
            .send(
                &self.provider,
                Method::POST,
                &format!("/v1/matches/{match_id}/cancel"),
                Some(json!({ "reason": "No show", "expected_revision": found["revision"] })),
                Some(&format!("test-cancel-{match_id}")),
            )
            .await;
        assert_eq!(status, StatusCode::OK, "{cancelled}");
    }

    /// Every match the bridge has, as it shows it, by external match ID.
    pub async fn snapshots(&self) -> Vec<(String, Value)> {
        let mut found = Vec::new();
        for (external, match_id, _) in self.bridge.matches().await {
            found.push((external, self.snapshot(&match_id).await));
        }
        found.sort_by(|a, b| a.0.cmp(&b.0));
        found
    }

    /// Every match's first-to length, by external match ID.
    pub async fn lengths(&self) -> Vec<(String, u64)> {
        self.snapshots()
            .await
            .into_iter()
            .map(|(external, found)| (external, found["games_to_win"].as_u64().unwrap()))
            .collect()
    }

    /// Why the set waits, as the connector keeps it (and logs it once).
    pub fn note(&self, bracket: &str, set_id: &str) -> Option<String> {
        self.database()
            .query_row(
                "SELECT note FROM sets WHERE bracket = ?1 AND set_id = ?2",
                [bracket, set_id],
                |row| row.get(0),
            )
            .unwrap()
    }

    /// What a crash leaves when it comes after a creation was saved and
    /// before the bridge's answer was: the set's attempt 1 waiting, with
    /// its creation, which is sent again as it is.
    pub fn stage_waiting(&self, bracket: &str, set_id: &str, entries: [&str; 2], command: &Value) {
        let players = &command["participants"];
        self.database()
            .execute(
                "INSERT INTO sets (bracket, set_id, player1, player2, ember1, ember2, attempt,
                     match_id, progress, command)
                 VALUES (?6, ?7, ?1, ?2, ?3, ?4, 1, NULL, 'waiting', ?5)
                 ON CONFLICT (bracket, set_id) DO UPDATE SET player1 = ?1, player2 = ?2,
                     ember1 = ?3, ember2 = ?4, attempt = 1, match_id = NULL,
                     progress = 'waiting', command = ?5",
                rusqlite::params![
                    entries[0],
                    entries[1],
                    players[0]["ember_id"].as_str().unwrap(),
                    players[1]["ember_id"].as_str().unwrap(),
                    command.to_string(),
                    bracket,
                    set_id,
                ],
            )
            .unwrap();
    }

    /// The same, for a match the bridge did make: its creation is rebuilt
    /// from the match, as the connector built it, and saved with the set
    /// waiting, as if the bridge's answer had been lost.
    pub async fn stage_unanswered(
        &self,
        bracket: &str,
        set_id: &str,
        entries: [&str; 2],
        match_id: &str,
    ) {
        let external = self
            .bridge
            .matches()
            .await
            .into_iter()
            .find(|(_, id, _)| id == match_id)
            .unwrap()
            .0;
        let found = self.snapshot(match_id).await;
        let players: Vec<Value> = found["participants"]
            .as_array()
            .unwrap()
            .iter()
            .map(|player| {
                json!({ "participant_id": player["participant_id"], "ember_id": player["ember_id"], "slot": player["slot"] })
            })
            .collect();
        let command = creation(
            &external,
            &players,
            found["games_to_win"].as_u64().unwrap(),
            found["metadata"]["round_label"].as_str().unwrap(),
        );
        self.stage_waiting(bracket, set_id, entries, &command);
    }

    pub async fn snapshot(&self, match_id: &str) -> Value {
        let (status, found) = self
            .bridge
            .send(
                &self.provider,
                Method::GET,
                &format!("/v1/matches/{match_id}"),
                None,
                None,
            )
            .await;
        assert_eq!(status, StatusCode::OK, "{found}");
        found
    }
}

/// The creation the connector sends for an attempt, rebuilt from what the
/// bridge has for its participants.
pub fn creation(external: &str, players: &[Value], games_to_win: u64, round_label: &str) -> Value {
    json!({
        "external_match_id": external,
        "game": "usf4",
        "participants": players,
        "rules": {
            "games_to_win": games_to_win,
            "draw_policy": "replay_no_score",
            "native_rules_profile": "ember-room-v1",
            "edition_policy": "ultra_only",
            "character_policy": "unrestricted_between_games",
            "stage_policy": "p1_selects",
            "input_delay_policy": "ember_existing_ready_policy",
        },
        "observer_policy": "authorized_only",
        "result_policy": "two_player_agreement_or_review",
        "required_build_id": "test-build",
        "metadata": { "round_label": round_label },
    })
}
