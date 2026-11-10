//! A fake bracket service answering the `http` adapter's contract, a real
//! bridge on a loopback port, and a connector between them. All keys and
//! tokens here are throwaway test values.
#![allow(dead_code)]

pub mod bridge;
pub mod service;

use std::path::{Path, PathBuf};

use axum::{
    Router,
    http::{Method, StatusCode},
};
use ember_brackets::{
    Adapter, Bridge, Clock, Config, Connector, Credential, CredentialKind, Desk, DeskSite, Results,
    RoundRule, Rounds, Side, Tournament,
};
use ember_protocol::webhook::Secret;
use serde_json::{Value, json};
use tokio::net::TcpListener;

use bridge::TestBridge;
use service::{FakeService, SERVICE_TOKEN, fed_by, open};

pub const TOURNAMENT: &str = "weekly";
/// The tournament's key, as the connector's database and logs name it.
pub const BRACKET: &str = "http:weekly";
/// The bridge site the desk runs the fake service's events on.
pub const DESK_SITE: &str = "ember";
pub const CONNECTION: &str = "brackets";

/// Serves `app` on a loopback port of its own; returns its origin.
pub async fn serve(app: Router) -> String {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let origin = format!("http://{}", listener.local_addr().unwrap());
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    origin
}

/// A connector's configuration over `dir`, with no tournament yet: its
/// provider credential, result secret and service token written there.
pub fn config(dir: &Path, bridge_api: &str, provider: &str) -> Config {
    std::fs::write(dir.join("provider.token"), format!("{provider}\n")).unwrap();
    std::fs::write(dir.join("result.secret"), result_secret().reveal().as_str()).unwrap();
    std::fs::write(dir.join("service.token"), SERVICE_TOKEN).unwrap();
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
        allow_loopback_http: true,
        tournaments: Vec::new(),
        desk: None,
    }
}

pub fn connector(config: &Config, clock: &Clock) -> Connector {
    Connector::new(config.clone(), clock.clone()).unwrap()
}

/// The tournament at `api_base`, on the token `config` wrote.
pub fn tournament(dir: &Path, api_base: &str) -> Tournament {
    Tournament {
        adapter: Adapter::Http,
        tournament: TOURNAMENT.into(),
        credential: Credential {
            kind: CredentialKind::Bearer,
            file: dir.join("service.token"),
        },
        rounds: rounds(),
        active_from: None,
        active_until: None,
        mark_underway: true,
        api_base: Some(api_base.into()),
    }
}

pub fn temp(name: &str) -> PathBuf {
    ember_bridge::testing::temp_dir(&format!("ember-brackets-{name}"))
}

pub fn result_secret() -> Secret {
    Secret::from_bytes([7; 32])
}

pub struct World {
    pub service: FakeService,
    pub bridge: TestBridge,
    pub connector: Connector,
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

pub struct Setup {
    /// Whether the connector runs the tournament as a configured one. The
    /// desk runs it as an event instead.
    pub configured: bool,
    /// The tournament's base URL and token, when it is not the fake
    /// service's.
    pub base: Option<(String, &'static str)>,
    /// Whether the desk runs events on the fake service (`DESK_SITE`).
    pub desk: bool,
    /// A bracket site whose live tournaments the desk runs, with its
    /// connector token.
    pub desk_site: Option<(String, &'static str)>,
    pub active_from: Option<String>,
    pub active_until: Option<String>,
    /// Whether the connector answers on its results URL. Without it every
    /// delivery fails, as one lost in the network would.
    pub serve_results: bool,
    pub rounds: Rounds,
    /// The fake service's sets.
    pub sets: Vec<Value>,
}

impl Default for Setup {
    /// Players 1 and 2 play s-1, players 3 and 4 play s-2, and their
    /// winners meet in s-3.
    fn default() -> Self {
        Self {
            configured: true,
            base: None,
            desk: false,
            desk_site: None,
            active_from: None,
            active_until: None,
            serve_results: true,
            rounds: rounds(),
            sets: vec![
                open("s-1", 1, [1, 2]),
                open("s-2", 1, [3, 4]),
                fed_by("s-3", 2, ["s-1", "s-2"]),
            ],
        }
    }
}

impl World {
    pub async fn start(setup: Setup) -> Self {
        let dir = temp("connector");
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let results_url = format!("http://{}/results", listener.local_addr().unwrap());
        let service = FakeService::start(setup.sets).await;
        let bridge = TestBridge::start(&results_url).await;
        let provider = bridge.provider().await;
        let organizer = bridge.organizer().await;
        let mut config = config(&dir, &bridge.origin, &provider);
        if setup.configured {
            let (base, token) = setup
                .base
                .unwrap_or_else(|| (service.base.clone(), SERVICE_TOKEN));
            std::fs::write(dir.join("service.token"), token).unwrap();
            config.tournaments.push(Tournament {
                active_from: setup.active_from,
                active_until: setup.active_until,
                rounds: setup.rounds,
                ..tournament(&dir, &base)
            });
        }
        if setup.desk {
            config.desk = Some(Desk {
                services: [(DESK_SITE.to_owned(), service.origin.clone())].into(),
                ..Desk::default()
            });
        }
        if let Some((base, token)) = setup.desk_site {
            let credential_file = dir.join("desk-site.token");
            std::fs::write(&credential_file, token).unwrap();
            config.desk = Some(Desk {
                services: [(DESK_SITE.to_owned(), base)].into(),
                site: Some(DeskSite {
                    service: DESK_SITE.into(),
                    credential_file,
                }),
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
            service,
            bridge,
            connector,
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
            found["metadata"].clone(),
        );
        self.stage_waiting(bracket, set_id, entries, &command);
    }

    /// The organizer, byte 9, runs the fake service's tournament as a desk
    /// event on `DESK_SITE`, as the bridge's organizer routes would leave
    /// it (the bridge's own tests cover those), public or not.
    pub async fn run_event(&self, settings: Value, public: bool) {
        let organizer = self.bridge.connect_site(9, DESK_SITE, "9009").await;
        let state = self.bridge.state();
        let now = self.bridge.clock.now();
        let sealed = state.keys.seal(
            json!({
                "access": SERVICE_TOKEN,
                "refresh": null,
                "scope": "",
                "expires_at": now + 604_800,
            })
            .to_string()
            .as_bytes(),
        );
        let ember_id = organizer.as_str().to_owned();
        state
            .db
            .write(move |tx| {
                tx.execute(
                    "INSERT INTO organizer_grants (site, user_id, sealed, refreshed_at)
                     VALUES (?1, '9009', ?2, ?3)",
                    rusqlite::params![DESK_SITE, sealed, now],
                )?;
                tx.execute(
                    "INSERT INTO organizer_events
                         (id, ember_id, title, status, site, tournament, name, settings,
                          created_at, public)
                     VALUES ('evt_1', ?1, 'Weekly', 'ready', ?2, ?3, 'Weekly', ?4, ?5, ?6)",
                    rusqlite::params![
                        ember_id,
                        DESK_SITE,
                        TOURNAMENT,
                        settings.to_string(),
                        now,
                        public
                    ],
                )?;
                Ok(())
            })
            .await
            .unwrap();
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
pub fn creation(external: &str, players: &[Value], games_to_win: u64, metadata: Value) -> Value {
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
        "metadata": metadata,
    })
}
