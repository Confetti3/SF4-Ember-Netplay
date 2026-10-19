//! A tournament run end to end, as an organizer and four players do it,
//! against fake Challonge and start.gg sites that keep their state: the
//! organizer connects both sites with organizer access in the game, signs in
//! to the web desk, creates a double elimination bracket on Challonge and
//! attaches an event on start.gg, and marks both ready. The connector calls
//! each set, the players play it in Ember and both report every game, and
//! the scores go back to the sites. A game cut off without a result counts
//! for nobody, a set the organizer corrects on Challonge is played again
//! with its new players, and the bracket finishes.
#[path = "../../bridge/tests/common/mod.rs"]
mod common;
mod support;

use std::collections::{BTreeMap, HashMap};

use common::{
    Bridge, Player,
    play::{self, BUILD, Table},
    sign_in::{self, Routes, definition},
    sites::{self, TestSite},
};
use ember_brackets::{Clock, Connector, Desk};
use ember_bridge::config::{Connection, Disputes};
use ember_protocol::{
    challenge::{Action, Method},
    report::Outcome::{self, Abort, P1Win, P2Win},
};
use reqwest::{StatusCode, header};
use serde_json::{Value as Json, json};
use support::{
    CONNECTION,
    challonge::{self, FakeChallonge, lines},
    oauth::approve,
    startgg::{self, EVENT_SLUG, FakeEntrant, FakeSet, FakeStartgg},
};

const CHALLONGE: TestSite = TestSite("challonge");
const STARTGG: TestSite = TestSite("startgg");
/// The organizer's accounts on Challonge and start.gg.
const ORGANIZER: (&str, &str) = ("9009", "organizer");
const ORGANIZER_STARTGG: &str = "5009";

/// The bridge, the connector on its desk connection, both fake sites and
/// the five people.
struct Flow {
    bridge: Bridge,
    connector: Connector,
    clock: Clock,
    challonge: FakeChallonge,
    startgg: FakeStartgg,
    organizer: Player,
    /// Player 1 to 4, by Ember ID.
    players: HashMap<String, Player>,
    /// The game counter each new game takes.
    generation: u64,
    dir: std::path::PathBuf,
}

impl Drop for Flow {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.dir);
    }
}

/// A set's two players at their table, player 1 first.
struct Seats<'a>(&'a Bridge, &'a Player, &'a Player);

impl Table for Seats<'_> {
    fn bridge(&self) -> &Bridge {
        self.0
    }

    fn a(&self) -> &Player {
        self.1
    }

    fn b(&self) -> &Player {
        self.2
    }
}

/// Signs in on the site's page the bridge sent the browser to, saying yes
/// when the bridge asks before connecting; the last page shown.
async fn sign_in_at(bridge: &Bridge, site: TestSite, authorize: &url::Url) -> String {
    let back = approve(authorize.as_str()).await;
    let page = site.back(bridge, &back).await;
    if !page.contains("name=\"confirmation\"") {
        return page;
    }
    site.decide(bridge, &sign_in::confirmation(&page), "connect")
        .await
}

/// The value of cookie `name` that `response` sets.
fn cookie(response: &reqwest::Response, name: &str) -> String {
    response
        .headers()
        .get_all(header::SET_COOKIE)
        .iter()
        .filter_map(|value| value.to_str().ok())
        .find_map(|value| value.strip_prefix(&format!("{name}=")))
        .map(|rest| rest.split(';').next().unwrap_or_default().to_owned())
        .unwrap()
}

impl Flow {
    async fn start() -> Self {
        let challonge = FakeChallonge::start(
            ember_brackets::CredentialKind::Bearer,
            Vec::new(),
            Vec::new(),
        )
        .await;
        let entrant = |id: u64, user: u64| FakeEntrant {
            id,
            user: Some(user),
            discord: None,
        };
        let startgg = FakeStartgg::start(
            vec![
                entrant(2_401_411, 5_001),
                entrant(2_401_412, 5_002),
                entrant(2_401_414, 5_003),
                entrant(2_401_415, 5_004),
            ],
            vec![
                FakeSet::open(65_089_253, 1, [2_401_411, 2_401_412]),
                FakeSet::open(65_089_255, 1, [2_401_414, 2_401_415]),
            ],
        )
        .await;
        startgg.state.lock().unwrap().admin = Some(ORGANIZER_STARTGG.into());

        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let results_url = format!("http://{}/results", listener.local_addr().unwrap());
        // The shipped definitions, pointed at the fakes; budgets are other
        // tests' business.
        let mut sites = vec![
            definition("challonge", &challonge.api, challonge::CLIENT_ID),
            definition("startgg", &startgg.origin, startgg::CLIENT_ID),
        ];
        for site in &mut sites {
            site.limits = None;
        }
        let mut secrets = sites::secrets(&[
            ("challonge", challonge::CLIENT_SECRET),
            ("startgg", startgg::CLIENT_SECRET),
        ]);
        secrets.result_secrets = [(CONNECTION.to_owned(), support::result_secret())].into();
        let connection = Connection {
            disputes: Some(Disputes::Restart),
            results_url: Some(results_url),
            ..sites::direct(CONNECTION, false, &["challonge", "startgg"])
        };
        let bridge =
            sites::bridge_with_desk(sites, secrets, vec![connection], Some(CONNECTION)).await;

        let dir = support::temp("flow");
        let provider = bridge.provider(CONNECTION).await;
        let mut config = support::config(&dir, &bridge.origin, &provider);
        config.bridge.build_id = BUILD.into();
        config.monthly_request_budget = 10_000;
        config.desk = Some(Desk {
            api_bases: BTreeMap::from([
                ("challonge".into(), challonge.api.clone()),
                ("startgg".into(), startgg.origin.clone()),
            ]),
        });
        let clock = Clock::frozen(bridge.clock.now());
        let connector = support::connector(&config, &clock);
        connector.serve(listener).unwrap();

        let organizer = sign_in::player(&bridge, 9).await;
        let mut flow = Self {
            bridge,
            connector,
            clock,
            challonge,
            startgg,
            organizer,
            players: HashMap::new(),
            generation: 0,
            dir,
        };
        for byte in 1..=4u8 {
            let player = sign_in::player(&flow.bridge, byte).await;
            flow.connect(
                &player,
                CHALLONGE,
                (format!("10{byte}"), format!("player{byte}")),
            )
            .await;
            flow.connect(
                &player,
                STARTGG,
                (format!("500{byte}"), format!("Player {byte}")),
            )
            .await;
            flow.players.insert(player.id().as_str().to_owned(), player);
        }
        flow
    }

    /// Who the browser is signed in to `site` as.
    fn browse_as(&self, site: TestSite, user: (String, String)) {
        match site.0 {
            "challonge" => self.challonge.state.lock().unwrap().oauth.browser = Some(user),
            _ => self.startgg.state.lock().unwrap().oauth.browser = Some(user),
        }
    }

    /// `player` connects their account `user` on `site` from the game.
    async fn connect(&self, player: &Player, site: TestSite, user: (String, String)) {
        self.browse_as(site, user);
        let authorize = sign_in::start(&self.bridge, player, &Routes::site(site.0)).await;
        let page = sign_in_at(&self.bridge, site, &authorize).await;
        assert!(page.contains("connected"), "{page}");
    }

    /// The organizer acting on Challonge itself, with their own token.
    async fn on_challonge(&self, method: reqwest::Method, path: &str, body: Json) -> Json {
        let token = self
            .challonge
            .state
            .lock()
            .unwrap()
            .oauth
            .token_of(ORGANIZER.0, "tournaments:write");
        let answer = self
            .bridge
            .client
            .request(method, format!("{}/v2.1/{path}", self.challonge.api))
            .header("authorization-type", "v2")
            .bearer_auth(token)
            .header("content-type", "application/vnd.api+json")
            .header("accept", "application/json")
            .body(body.to_string())
            .send()
            .await
            .unwrap();
        assert_eq!(answer.status(), StatusCode::OK);
        answer.json().await.unwrap()
    }

    /// Results go to the connector, and it reads both brackets and acts on
    /// them.
    async fn pass(&self) {
        ember_bridge::deliver_results(self.bridge.state()).await;
        self.connector.pass(true).await;
    }

    /// Moves the bridge's and the connector's clocks on together.
    fn advance(&self, seconds: i64) {
        self.bridge.clock.advance(seconds);
        self.clock.advance(seconds);
    }

    async fn snapshot(&self, match_id: &str) -> Json {
        let provider = self.bridge.provider(CONNECTION).await;
        let (status, found) = self
            .bridge
            .get(&provider, &format!("/v1/matches/{match_id}"))
            .await;
        assert_eq!(status, StatusCode::OK, "{found}");
        found
    }

    /// Every Ember match, oldest first: external ID (`challonge:30201:3:1`)
    /// and match ID.
    async fn matches(&self) -> Vec<(String, String)> {
        let matches = self.bridge.state().db.read(|tx| {
            let mut query =
                tx.prepare("SELECT external_match_id, id FROM matches ORDER BY created_at, rowid")?;
            let rows = query
                .query_map([], |row| Ok((row.get(0)?, row.get(1)?)))?
                .collect::<rusqlite::Result<Vec<(String, String)>>>()?;
            Ok(rows)
        });
        matches.await.unwrap()
    }

    /// The Ember match of `external`'s latest attempt (`challonge:30201:3`).
    async fn match_of(&self, external: &str) -> String {
        self.matches()
            .await
            .into_iter()
            .rev()
            .find(|(id, _)| id.starts_with(&format!("{external}:")))
            .unwrap_or_else(|| panic!("no match for {external}"))
            .1
    }

    /// The set's two players play its games in Ember, each reporting every
    /// one and sending the replay of each that counts. An `Abort` is a game
    /// cut off before its end, which both report so.
    /// Each set starts a while after the last, its players signed in to
    /// Ember afresh.
    async fn play(&mut self, external: &str, games: &[Outcome]) {
        self.advance(61);
        let match_id = self.match_of(external).await;
        let found = self.snapshot(&match_id).await;
        let seated: Vec<String> = [0, 1]
            .map(|slot| {
                let participants = found["participants"].as_array().unwrap();
                let player = participants.iter().find(|player| player["slot"] == slot);
                player.unwrap()["ember_id"].as_str().unwrap().to_owned()
            })
            .into();
        for id in &seated {
            let player = self.players.get_mut(id).unwrap();
            self.bridge.open_session(player).await;
        }
        let seats = Seats(
            &self.bridge,
            &self.players[&seated[0]],
            &self.players[&seated[1]],
        );
        let binding = play::bound(&seats, &match_id).await;
        for (n, &game) in games.iter().enumerate() {
            let replay = play::replay(n as u8);
            self.generation += 1;
            let permit = play::permit(&seats, &match_id, &binding, self.generation).await;
            for player in [seats.1, seats.2] {
                let n = (2 * n + usize::from(player.id() == seats.2.id())) as u8;
                let (status, sent) =
                    play::report(&seats, player, player, &match_id, &permit, n, game).await;
                assert_eq!(status, StatusCode::CREATED, "{sent}");
                // Each fighter's game sends the replay of a game that counts.
                if game != Abort {
                    let status = play::send_replay(&seats, player, &match_id, &replay, 1).await;
                    assert!(status.is_success(), "{status}");
                }
            }
        }
        let played = self.snapshot(&match_id).await;
        assert_eq!(played["state"], "completed", "{played}");
    }
}

/// The organizer's web desk session: its cookie and CSRF token.
struct WebDesk {
    client: reqwest::Client,
    origin: String,
    cookie: String,
    csrf: String,
}

impl WebDesk {
    async fn post(&self, path: &str, body: Json) -> (StatusCode, Json) {
        let answer = self
            .client
            .post(format!("{}{path}", self.origin))
            .header("cookie", &self.cookie)
            .header("x-ember-csrf", &self.csrf)
            .header("content-type", "application/json")
            .body(body.to_string())
            .send()
            .await
            .unwrap();
        common::read(answer).await
    }

    async fn get(&self, path: &str) -> Json {
        let answer = self
            .client
            .get(format!("{}{path}", self.origin))
            .header("cookie", &self.cookie)
            .send()
            .await
            .unwrap();
        let (status, body) = common::read(answer).await;
        assert_eq!(status, StatusCode::OK, "{body}");
        body
    }
}

/// The organizer signs in to the web desk with their Challonge account.
async fn web_sign_in(flow: &Flow) -> WebDesk {
    // The organizer opens the desk from the game: a site sign-in alone may
    // not create or ready an event.
    let bridge = &flow.bridge;
    let (status, made) = sign_in::send(
        bridge,
        &flow.organizer,
        Action::SiteConnect,
        Method::Post,
        "/v1/web-sessions/handoff",
        json!({}),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED, "{made}");
    let signed_in = bridge
        .client
        .get(bridge.url(&format!("/desk/h/{}", made["token"].as_str().unwrap())))
        .send()
        .await
        .unwrap();
    assert_eq!(signed_in.status(), StatusCode::SEE_OTHER);
    assert_eq!(signed_in.headers()["location"], "/desk");
    let mut desk = WebDesk {
        client: bridge.client.clone(),
        origin: bridge.origin.clone(),
        cookie: format!("ember_desk={}", cookie(&signed_in, "ember_desk")),
        csrf: String::new(),
    };
    desk.csrf = desk.get("/web/session").await["csrf"]
        .as_str()
        .unwrap()
        .to_owned();
    desk
}

#[tokio::test]
async fn an_organizer_runs_a_double_elimination_bracket_and_a_startgg_event() {
    let mut flow = Flow::start().await;

    // The organizer connects both sites with organizer access in the game.
    let organizer = (ORGANIZER.0.to_owned(), ORGANIZER.1.to_owned());
    flow.browse_as(CHALLONGE, organizer);
    let authorize = CHALLONGE
        .start_organizing(&flow.bridge, &flow.organizer)
        .await;
    let page = sign_in_at(&flow.bridge, CHALLONGE, &authorize).await;
    assert!(page.contains("Organizer access for Challonge allowed"), "{page}");
    flow.browse_as(STARTGG, (ORGANIZER_STARTGG.into(), "TO".into()));
    let authorize = STARTGG
        .start_organizing(&flow.bridge, &flow.organizer)
        .await;
    sign_in_at(&flow.bridge, STARTGG, &authorize).await;
    for site in [CHALLONGE, STARTGG] {
        let grant = site.organizer(&flow.bridge, &flow.organizer).await;
        assert!(grant["scope"].is_string(), "{}: {grant}", site.0);
    }

    // A day before the tokens lapse, reading the grants refreshes them on
    // both sites; the connector works with the new ones from here.
    let token = |flow: &Flow| {
        flow.challonge
            .state
            .lock()
            .unwrap()
            .oauth
            .token_of(ORGANIZER.0, "tournaments:write")
    };
    let before = token(&flow);
    flow.advance(604_800 - 86_400 + 1);
    flow.bridge.open_session(&mut flow.organizer).await;
    for player in flow.players.values_mut() {
        flow.bridge.open_session(player).await;
    }
    for site in [CHALLONGE, STARTGG] {
        let grant = site.organizer(&flow.bridge, &flow.organizer).await;
        assert_eq!(grant["expires_at"], flow.bridge.clock.now() + 604_800);
    }
    assert_ne!(token(&flow), before);

    // On the web desk: a draft, created on Challonge as a double
    // elimination bracket, starting now, first to 2.
    let desk = web_sign_in(&flow).await;
    let (status, made) = desk
        .post("/v1/organizer/drafts", json!({ "title": "Ember Cup" }))
        .await;
    assert_eq!(status, StatusCode::CREATED, "{made}");
    let cup = made["id"].as_str().unwrap().to_owned();
    let (status, created) = desk
        .post(
            &format!("/v1/organizer/events/{cup}/create"),
            json!({ "site": "challonge", "fields": { "format": "double elimination" } }),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED, "{created}");
    assert_eq!(
        created,
        json!({ "tournament": "30201", "link": "https://challonge.com/30201" })
    );
    let now = flow.bridge.clock.now();
    let settings = json!({ "rounds": { "games_to_win": 2 } });
    let (status, changed) = desk
        .post(
            &format!("/v1/organizer/events/{cup}"),
            json!({ "starts_at": now, "settings": settings }),
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{changed}");

    // A second draft takes the start.gg event the organizer runs.
    let (_, made) = desk
        .post("/v1/organizer/drafts", json!({ "title": "Ember Weekly" }))
        .await;
    let weekly = made["id"].as_str().unwrap().to_owned();
    let (status, attached) = desk
        .post(
            &format!("/v1/organizer/events/{weekly}"),
            json!({ "site": "startgg", "tournament": EVENT_SLUG, "starts_at": now,
                    "settings": settings }),
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{attached}");

    // The organizer enters the four players on Challonge and starts it.
    for byte in 1..=4 {
        flow.on_challonge(
            reqwest::Method::POST,
            "tournaments/30201/participants.json",
            json!({ "data": { "type": "Participants", "attributes": {
                "name": format!("Player {byte}"), "username": format!("player{byte}") } } }),
        )
        .await;
    }
    flow.on_challonge(
        reqwest::Method::PUT,
        "tournaments/30201/change_state.json",
        json!({ "data": { "type": "TournamentState", "attributes": { "state": "start" } } }),
    )
    .await;

    // Nothing runs until it is ready. A player plays one set at a time, so
    // the start.gg event waits for the Challonge bracket to finish.
    flow.pass().await;
    assert!(flow.matches().await.is_empty());
    let ready = |event: String| {
        let desk = &desk;
        async move {
            let (status, ready) = desk
                .post(&format!("/v1/organizer/events/{event}/ready"), json!({}))
                .await;
            assert_eq!(status, StatusCode::OK, "{ready}");
        }
    };
    ready(cup.clone()).await;
    let events = desk.get("/v1/organizer/events").await;
    let status = |id: &str| {
        let events = events["events"].as_array().unwrap();
        events.iter().find(|event| event["id"] == id).unwrap()["status"].clone()
    };
    assert_eq!(
        (status(&cup), status(&weekly)),
        (json!("running"), json!("draft"))
    );

    // Ready, the first round's sets are made and called, with the
    // refreshed token.
    flow.pass().await;
    assert!(flow.challonge.find("1").underway && flow.challonge.find("2").underway);
    let called = flow
        .challonge
        .state
        .lock()
        .unwrap()
        .log
        .last()
        .unwrap()
        .clone();
    assert_eq!(
        called.headers["authorization"],
        format!("Bearer {}", token(&flow))
    );

    // Set 1: the second game is cut off and both say so; it counts for
    // nobody, and Player 1 wins 2-1 on the three games played to the end.
    flow.play("challonge:30201:1", &[P1Win, Abort, P2Win, P1Win])
        .await;
    flow.play("challonge:30201:2", &[P2Win, P2Win]).await;
    let first = flow
        .snapshot(&flow.match_of("challonge:30201:1").await)
        .await;
    assert_eq!(
        (&first["scores"][0]["wins"], &first["scores"][1]["wins"]),
        (&json!(2), &json!(1)),
        "{first}"
    );
    // Its pack holds the three games played to the end, in the order played.
    let p1 = &flow.players[first["participants"][0]["ember_id"].as_str().unwrap()];
    let seats = Seats(&flow.bridge, p1, p1);
    let (header, games) = play::pack(&seats, p1.token(), first["match_id"].as_str().unwrap()).await;
    assert_eq!(
        games,
        vec![play::replay(0), play::replay(2), play::replay(3)]
    );
    assert_eq!(
        (&header["score"], &header["rooms"]),
        (&json!([2, 1]), &json!(1)),
        "{header}"
    );
    assert_eq!(
        (&header["tournament"], &header["round_label"]),
        (&json!("Ember Cup"), &json!("Round 1")),
        "{header}"
    );
    flow.pass().await;
    let one = flow.challonge.find("1");
    assert_eq!(one.state, "complete");
    assert_eq!(one.winner.as_deref(), Some("355"));
    assert_eq!(one.score_sets, Some(["2".into(), "1".into()]));
    assert_eq!(flow.challonge.find("2").winner.as_deref(), Some("357"));

    // The winners' final and the first losers' set are made.
    flow.pass().await;
    let winners_final = flow.match_of("challonge:30201:3").await;
    let losers_first = flow.match_of("challonge:30201:4").await;

    // The organizer corrects set 1 on Challonge: Player 4 won it. The two
    // sets after it are cancelled and made again for their new players.
    flow.on_challonge(
        reqwest::Method::PUT,
        "tournaments/30201/matches/1/change_state.json",
        json!({ "data": { "type": "MatchState", "attributes": { "state": "reopen" } } }),
    )
    .await;
    flow.on_challonge(
        reqwest::Method::PUT,
        "tournaments/30201/matches/1.json",
        json!({ "data": { "type": "Match", "attributes": { "match": [
            { "participant_id": "355", "score_set": "1", "rank": 2, "advancing": false },
            { "participant_id": "358", "score_set": "2", "rank": 1, "advancing": true },
        ] } } }),
    )
    .await;
    flow.pass().await;
    for cancelled in [&winners_final, &losers_first] {
        assert_eq!(flow.snapshot(cancelled).await["state"], "cancelled");
    }
    assert_ne!(flow.match_of("challonge:30201:3").await, winners_final);
    assert_eq!(flow.challonge.find("3").players[0].as_deref(), Some("358"));

    // The rest is played out: Player 3 wins the winners' final, Player 4
    // comes back through the losers' bracket and takes the grand final.
    flow.play("challonge:30201:3", &[P2Win, P2Win]).await;
    flow.play("challonge:30201:4", &[P1Win, P1Win]).await;
    flow.pass().await;
    flow.pass().await;
    flow.play("challonge:30201:5", &[P1Win, P1Win]).await;
    flow.pass().await;
    flow.pass().await;
    flow.play("challonge:30201:6", &[P1Win, P2Win, P2Win]).await;
    flow.pass().await;
    let grand_final = flow.challonge.find("6");
    assert_eq!(grand_final.state, "complete");
    assert_eq!(grand_final.winner.as_deref(), Some("358"));

    // Each set's score went to Challonge once, the organizer's correction
    // aside; nothing was sent for the two cancelled matches.
    let mut scored: Vec<String> = lines(&flow.challonge.log())
        .iter()
        .filter_map(|line| line.strip_prefix("PUT /v2.1/tournaments/30201/matches/"))
        .filter_map(|rest| rest.strip_suffix(".json"))
        .filter(|set| !set.contains('/'))
        .map(str::to_owned)
        .collect();
    scored.sort();
    assert_eq!(scored, ["1", "1", "2", "3", "4", "5", "6"]);

    // The organizer finalizes it; the bracket is over.
    flow.on_challonge(
        reqwest::Method::PUT,
        "tournaments/30201/change_state.json",
        json!({ "data": { "type": "TournamentState", "attributes": { "state": "finalize" } } }),
    )
    .await;
    flow.pass().await;
    flow.challonge.log();
    flow.pass().await;
    assert!(flow.challonge.log().is_empty());

    // The start.gg event: its sets are called, played, and reported with
    // each game's winner.
    ready(weekly).await;
    flow.pass().await;
    assert_eq!(flow.startgg.set(65_089_253).state, 2);
    flow.play(&format!("startgg:{EVENT_SLUG}:65089253"), &[P1Win, P1Win])
        .await;
    flow.play(
        &format!("startgg:{EVENT_SLUG}:65089255"),
        &[P2Win, P1Win, P2Win],
    )
    .await;
    flow.pass().await;
    let reported = flow.startgg.set(65_089_253);
    assert_eq!(
        (reported.winner, reported.games),
        (Some(2_401_411), vec![2_401_411, 2_401_411])
    );
    let reported = flow.startgg.set(65_089_255);
    assert_eq!(
        (reported.winner, reported.games),
        (Some(2_401_415), vec![2_401_415, 2_401_414, 2_401_415])
    );
}
