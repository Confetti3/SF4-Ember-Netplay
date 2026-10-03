//! What a direct connection can ask for beyond the generic API (docs/design/
//! INTEGRATION_PATHS.md): finding players by Discord account, results sent to
//! its `results_url` signed with its result secret, and disputes that restart
//! instead of waiting for review. The configuration rules for all three are
//! tested in `src/config.rs`.
mod common;

use common::{
    Bridge, Player, code,
    receiver::{Receiver, receiver},
    results,
};
use ember_bridge::{AppState, Config, Keys, config, integrations::Secrets};
use ember_protocol::{
    challenge::{Action, Method},
    partner::{LookupAnswer, MatchResult, Outcome, RESULT_TYPE, result_id},
    webhook::{self, Secret},
};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};

const KATE: &str = "274220342558756145";
const SAM: &str = "1725265127372152396";

/// The result secret of every connection that has one here.
fn secret() -> Secret {
    Secret::from_bytes([7; 32])
}

fn secrets_for(connections: &[&str]) -> Secrets {
    Secrets {
        result_secrets: connections
            .iter()
            .map(|connection| ((*connection).to_owned(), secret()))
            .collect(),
        ..Default::default()
    }
}

fn direct(
    id: &str,
    lookup: bool,
    disputes: Option<config::Disputes>,
    results_url: Option<&str>,
) -> config::Connection {
    config::Connection {
        id: id.into(),
        kind: "direct".into(),
        environment: "local".into(),
        display_name: format!("Direct {id}"),
        enabled: true,
        api_base: None,
        discord_lookup: lookup,
        disputes,
        results_url: results_url.map(str::to_owned),
        rooms: None,
    }
}

/// A bridge with, besides the mock connections:
/// - `night-bot`: Discord lookup, restarting disputes, results sent to the
///   stand-in (it has its secret);
/// - `quiet-bot`: results sent to the stand-in, with no secret yet;
/// - `late-bot`: no `results_url`;
/// - `blumint-test`: BluMint's own kind.
async fn start() -> (Bridge, Receiver, String) {
    let (url, receiver) = receiver().await;
    let results_url = url.clone();
    let bridge = Bridge::start_with(
        move |config| {
            config.tenants.push(config::Tenant {
                id: "bot".into(),
                name: "Bots".into(),
                connections: vec![
                    direct(
                        "night-bot",
                        true,
                        Some(config::Disputes::Restart),
                        Some(&results_url),
                    ),
                    direct("quiet-bot", false, None, Some(&results_url)),
                    direct("late-bot", false, None, None),
                ],
            });
            config.tenants.push(config::Tenant {
                id: "bm".into(),
                name: "BluMint".into(),
                connections: vec![config::Connection {
                    kind: "blumint".into(),
                    environment: "staging".into(),
                    ..direct("blumint-test", false, None, None)
                }],
            });
        },
        secrets_for(&["night-bot"]),
    )
    .await;
    (bridge, receiver, url)
}

/// Writes the Discord accounts `connect` would, as tests/blumint.rs does
/// (tests/discord.rs covers the sign-in itself).
async fn connect_discord(bridge: &Bridge, accounts: &[(&str, &Player)]) {
    for (user_id, player) in accounts {
        let (user_id, ember_id) = ((*user_id).to_owned(), player.id().as_str().to_owned());
        bridge
            .state()
            .db
            .write(move |tx| {
                tx.execute(
                    "INSERT INTO discord_accounts (user_id, ember_id, username, connected_at) VALUES (?1, ?2, 'name', 1)",
                    rusqlite::params![user_id, ember_id],
                )?;
                Ok(())
            })
            .await
            .unwrap();
    }
}

async fn lookup(bridge: &Bridge, token: &str, ids: &[&str]) -> (StatusCode, Json) {
    bridge
        .post(token, "/v1/players/lookup", json!({ "discord": ids }))
        .await
}

async fn found(bridge: &Bridge, token: &str, ids: &[&str]) -> LookupAnswer {
    let (status, body) = lookup(bridge, token, ids).await;
    assert_eq!(status, StatusCode::OK, "{body}");
    serde_json::from_value(body).unwrap()
}

/// The approval and state of the player's link on the connection.
async fn link_of(bridge: &Bridge, connection: &str, player: &Player) -> (String, bool) {
    let (connection, ember_id) = (connection.to_owned(), player.id().as_str().to_owned());
    bridge
        .state()
        .db
        .read(move |tx| {
            Ok(tx.query_row(
                "SELECT approved_via, revoked_at IS NULL FROM links WHERE connection_id = ?1 AND ember_id = ?2",
                [&connection, &ember_id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )?)
        })
        .await
        .unwrap()
}

#[tokio::test]
async fn lookup_is_for_connections_that_ask_for_it_and_links_what_it_finds() {
    let (bridge, _, _) = start().await;
    let (mut kate, mut sam) = (bridge.player(1), bridge.player(2));
    bridge.open_session(&mut kate).await;
    bridge.open_session(&mut sam).await;
    connect_discord(&bridge, &[(KATE, &kate), (SAM, &sam)]).await;

    // A connection that did not ask for it, an organizer, and a player.
    for token in [
        bridge.provider("mock-a").await,
        bridge.provider("quiet-bot").await,
        bridge.organizer("bot").await,
        kate.token().to_owned(),
    ] {
        let (status, body) = lookup(&bridge, &token, &[KATE]).await;
        assert_eq!(
            (status, code(&body)),
            (StatusCode::FORBIDDEN, "forbidden"),
            "{body}"
        );
    }

    // Answered in the order asked; an ID nobody connected is skipped.
    let provider = bridge.provider("night-bot").await;
    let answer = found(&bridge, &provider, &[SAM, "99999999999999999", KATE]).await;
    let players: Vec<_> = answer
        .players
        .iter()
        .map(|player| (player.discord_user_id.as_str(), player.ember_id.as_str()))
        .collect();
    assert_eq!(
        players,
        [(SAM, sam.id().as_str()), (KATE, kate.id().as_str())]
    );
    assert!(
        answer
            .players
            .iter()
            .all(|player| !player.participant_id.is_empty())
    );
    // Each is linked on the connection, by their Discord sign-in.
    for player in [&kate, &sam] {
        assert_eq!(
            link_of(&bridge, "night-bot", player).await,
            ("discord".to_owned(), true)
        );
    }
    // Asking again gives the same participants.
    assert_eq!(
        found(&bridge, &provider, &[SAM, KATE]).await.players,
        answer.players
    );
    // And the connection can put them in a match.
    let (status, created) = bridge
        .post_keyed(
            &provider,
            "/v1/matches",
            match_body(
                "found-1",
                "organizer-reported-v1",
                1,
                [
                    (&answer.players[0].participant_id, sam.id().as_str()),
                    (&answer.players[1].participant_id, kate.id().as_str()),
                ],
            ),
            Some("found-1"),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED, "{created}");

    // A lookup names at most 32; the 32-ID form is accepted.
    let many: Vec<String> = (0..33)
        .map(|n| (10_000_000_000_000_000u64 + n).to_string())
        .collect();
    let many: Vec<&str> = many.iter().map(String::as_str).collect();
    let (status, body) = lookup(&bridge, &provider, &many).await;
    assert_eq!(
        (status, code(&body)),
        (StatusCode::BAD_REQUEST, "invalid_request"),
        "{body}"
    );
    assert_eq!(found(&bridge, &provider, &many[..32]).await.players, []);
}

#[tokio::test]
async fn a_link_the_player_removed_is_not_made_again_by_a_lookup() {
    let (bridge, _, _) = start().await;
    let (mut kate, mut sam) = (bridge.player(1), bridge.player(2));
    bridge.open_session(&mut kate).await;
    bridge.open_session(&mut sam).await;
    connect_discord(&bridge, &[(KATE, &kate), (SAM, &sam)]).await;
    let provider = bridge.provider("night-bot").await;
    assert_eq!(
        found(&bridge, &provider, &[KATE, SAM]).await.players.len(),
        2
    );

    let (_, links) = bridge.get(kate.token(), "/v1/links").await;
    let link = links["links"][0]["link_id"].as_str().unwrap().to_owned();
    let path = format!("/v1/links/{link}");
    let body = bridge
        .prove(
            &kate,
            Action::LinkRemove,
            Method::Delete,
            &path,
            json!({ "link_id": link }),
        )
        .await;
    let (status, removed) = bridge.send_proof(&kate, Method::Delete, &path, body).await;
    assert_eq!(status, StatusCode::OK, "{removed}");

    // Kate is not found again, however often it is asked; Sam still is.
    for _ in 0..2 {
        let answer = found(&bridge, &provider, &[KATE, SAM]).await;
        let ids: Vec<_> = answer
            .players
            .iter()
            .map(|player| player.discord_user_id.as_str())
            .collect();
        assert_eq!(ids, [SAM]);
    }
    assert!(!link_of(&bridge, "night-bot", &kate).await.1);
}

#[tokio::test]
async fn blumints_own_lookup_still_answers_as_before() {
    let (bridge, _, _) = start().await;
    let (mut kate, mut sam) = (bridge.player(1), bridge.player(2));
    bridge.open_session(&mut kate).await;
    bridge.open_session(&mut sam).await;
    connect_discord(&bridge, &[(KATE, &kate)]).await;
    let provider = bridge.provider("blumint-test").await;
    let (status, answer) = bridge
        .post(
            &provider,
            "/v1/blumint/lookup",
            json!({ "email": ["kate@example.com"], "discord": [KATE, SAM] }),
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{answer}");
    assert_eq!(answer, json!({ "discord": [kate.id()] }));
    // Its kind includes the generic lookup, which names the same player.
    let answer = found(&bridge, &provider, &[KATE, SAM]).await;
    let ids: Vec<_> = answer
        .players
        .iter()
        .map(|player| player.ember_id.as_str())
        .collect();
    assert_eq!(ids, [kate.id().as_str()]);
    // Another kind of connection cannot use BluMint's route.
    let (status, _) = bridge
        .post(
            &bridge.provider("night-bot").await,
            "/v1/blumint/lookup",
            json!({ "discord": [KATE] }),
        )
        .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
}

struct Fixture {
    bridge: Bridge,
    receiver: Receiver,
    url: String,
    provider: String,
    organizer: String,
    kate: Player,
    sam: Player,
    links: [Json; 2],
}

/// Players linked on `connection` (as a provider links its own accounts).
async fn fixture(connection: &str) -> Fixture {
    let (bridge, receiver, url) = start().await;
    let tenant = if connection.starts_with("mock") {
        "t1"
    } else {
        "bot"
    };
    let provider = bridge.provider(connection).await;
    let organizer = bridge.organizer(tenant).await;
    let (mut kate, mut sam) = (bridge.player(31), bridge.player(32));
    bridge.open_session(&mut kate).await;
    bridge.open_session(&mut sam).await;
    let links = [
        bridge
            .link(&provider, &kate, connection, "kate-account")
            .await,
        bridge
            .link(&provider, &sam, connection, "sam-account")
            .await,
    ];
    Fixture {
        bridge,
        receiver,
        url,
        provider,
        organizer,
        kate,
        sam,
        links,
    }
}

fn match_body(external: &str, profile: &str, games_to_win: u8, roster: [(&str, &str); 2]) -> Json {
    json!({
        "external_match_id": external,
        "game": "usf4",
        "participants": [
            { "participant_id": roster[0].0, "ember_id": roster[0].1, "slot": 0 },
            { "participant_id": roster[1].0, "ember_id": roster[1].1, "slot": 1 },
        ],
        "rules": {
            "games_to_win": games_to_win,
            "draw_policy": "replay_no_score",
            "native_rules_profile": profile,
            "edition_policy": "ultra_only",
            "character_policy": "unrestricted_between_games",
            "stage_policy": "p1_selects",
            "input_delay_policy": "ember_existing_ready_policy",
        },
        "observer_policy": "authorized_only",
        "result_policy": "two_player_agreement_or_review",
        "required_build_id": "test-build",
        "metadata": {},
    })
}

impl Fixture {
    fn participant(&self, slot: usize) -> &str {
        self.links[slot]["participant_id"].as_str().unwrap()
    }

    async fn create(&self, external: &str, profile: &str, games_to_win: u8) -> String {
        let body = match_body(
            external,
            profile,
            games_to_win,
            [
                (self.participant(0), self.kate.id().as_str()),
                (self.participant(1), self.sam.id().as_str()),
            ],
        );
        let (status, created) = self
            .bridge
            .post_keyed(&self.provider, "/v1/matches", body, Some(external))
            .await;
        assert_eq!(status, StatusCode::CREATED, "{created}");
        created["match_id"].as_str().unwrap().to_owned()
    }

    async fn snapshot(&self, id: &str) -> Json {
        let (status, found) = self
            .bridge
            .get(&self.provider, &format!("/v1/matches/{id}"))
            .await;
        assert_eq!(status, StatusCode::OK, "{found}");
        found
    }

    /// The organizer records `slot` as the winner of a game.
    async fn win(&self, id: &str, slot: u8, key: &str) {
        results::win(&self.bridge, &self.provider, &self.organizer, id, slot, key).await;
    }

    /// Voids the match's last accepted game, as an organizer correcting it.
    async fn void_last(&self, id: &str, key: &str) -> (StatusCode, Json) {
        results::void_last(&self.bridge, &self.provider, &self.organizer, id, key).await
    }

    /// An organizer-reported first-to-1 match that slot 0 has won.
    async fn finished(&self, external: &str) -> String {
        let id = self.create(external, "organizer-reported-v1", 1).await;
        self.win(&id, 0, &format!("{external}-game-1")).await;
        id
    }

    /// A match's `delivery_state` and attempts.
    async fn delivery(&self, id: &str) -> (String, u32) {
        results::delivery(&self.bridge, id).await
    }

    /// Runs delivery passes until the match's delivery is `done`.
    async fn until(&self, id: &str, done: impl Fn((String, u32)) -> bool) {
        results::until(&self.bridge, id, done).await;
    }

    /// The results the stand-in received, verified and parsed, for `id`.
    fn results(&self, id: &str) -> Vec<MatchResult> {
        self.receiver
            .received()
            .into_iter()
            .filter(|(headers, _)| headers.id == result_id(id))
            .map(|(headers, body)| {
                // Checked as of the send: the clock may have moved on since.
                let sent_at = headers.timestamp.parse().unwrap();
                webhook::verify(&[&secret()], &headers, &body, sent_at).unwrap();
                serde_json::from_slice(&body).unwrap()
            })
            .collect()
    }

    /// A bridge over the same database whose configuration and secrets are
    /// these, as after the operator changed them.
    fn reconfigured(&self, edit: impl FnOnce(&mut Config), secrets: Secrets) -> AppState {
        let state = self.bridge.state();
        let mut config = (*state.config).clone();
        edit(&mut config);
        AppState::new(
            config,
            Keys::generate(),
            secrets,
            state.db.clone(),
            self.bridge.clock.clone(),
        )
    }
}

#[tokio::test]
async fn a_completed_match_is_sent_once_signed_with_the_result_secret() {
    let f = fixture("night-bot").await;
    let id = f.create("set-1", "organizer-reported-v1", 2).await;
    for (slot, key) in [(0, "game-1"), (1, "game-2"), (0, "game-3")] {
        f.win(&id, slot, key).await;
    }
    f.until(&id, |(state, _)| state == "delivered").await;
    ember_bridge::deliver_results(f.bridge.state()).await;
    assert_eq!(f.delivery(&id).await, ("delivered".into(), 1));
    assert_eq!(
        f.snapshot(&id).await["provider_delivery_state"],
        "delivered"
    );

    let received = f.receiver.received();
    assert_eq!(received.len(), 1);
    let (headers, body) = &received[0];
    assert_eq!(headers.id, format!("res_{id}"));
    let now = f.bridge.clock.now();
    webhook::verify(&[&secret()], headers, body, now).unwrap();
    // Signed with that secret, over exactly that body.
    assert!(webhook::verify(&[&Secret::from_bytes([8; 32])], headers, body, now).is_err());
    assert!(webhook::verify(&[&secret()], headers, b"{}", now).is_err());

    let result: MatchResult = serde_json::from_slice(body).unwrap();
    assert_eq!(result.kind, RESULT_TYPE);
    assert_eq!(
        (
            result.bridge_id.as_str(),
            result.connection_id.as_str(),
            result.match_id.as_str()
        ),
        (f.bridge.bridge_id.as_str(), "night-bot", id.as_str())
    );
    assert_eq!(result.external_match_id, "set-1");
    assert_eq!(result.outcome, Outcome::Completed);
    assert_eq!(
        result.revision,
        f.snapshot(&id).await["revision"].as_str().unwrap()
    );
    let roster: Vec<_> = result
        .participants
        .iter()
        .map(|p| {
            (
                p.participant_id.as_str(),
                p.ember_id.as_str(),
                p.slot,
                p.score,
            )
        })
        .collect();
    assert_eq!(
        roster,
        [
            (f.participant(0), f.kate.id().as_str(), 0, 2),
            (f.participant(1), f.sam.id().as_str(), 1, 1),
        ]
    );
    assert_eq!(
        result.winner_participant_id.as_deref(),
        Some(f.participant(0))
    );
}

#[tokio::test]
async fn a_cancelled_match_is_sent_as_a_restart_without_a_winner() {
    let f = fixture("night-bot").await;
    let id = f.create("set-1", "organizer-reported-v1", 2).await;
    f.win(&id, 1, "game-1").await;
    let (status, cancelled) = f
        .bridge
        .post_keyed(
            &f.provider,
            &format!("/v1/matches/{id}/cancel"),
            json!({ "reason": "No show", "expected_revision": f.snapshot(&id).await["revision"] }),
            Some("cancel-1"),
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{cancelled}");
    f.until(&id, |(state, _)| state == "delivered").await;

    let received = f.receiver.received();
    assert_eq!(received.len(), 1);
    let raw: Json = serde_json::from_slice(&received[0].1).unwrap();
    assert_eq!(raw["outcome"], "restart");
    assert!(raw.get("winner_participant_id").is_none(), "{raw}");
    let result = &f.results(&id)[0];
    assert_eq!(result.outcome, Outcome::Restart);
    // The score is what was accepted before it ended.
    let scores: Vec<_> = result.participants.iter().map(|p| p.score).collect();
    assert_eq!(scores, [0, 1]);
}

#[tokio::test]
async fn the_platforms_answer_settles_retries_or_refuses_a_result() {
    let f = fixture("night-bot").await;

    // 409: it already has the result, which settles it.
    f.receiver.answer_with(&[409]);
    let first = f.finished("set-1").await;
    f.until(&first, |(state, _)| state == "delivered").await;
    assert_eq!(f.delivery(&first).await, ("delivered".into(), 1));

    // 500: sent again, under the same ID.
    f.receiver.answer_with(&[500]);
    let second = f.finished("set-2").await;
    f.until(&second, |(state, attempts)| {
        state == "retrying" && attempts == 1
    })
    .await;
    f.bridge.clock.advance(11);
    f.until(&second, |(state, _)| state == "delivered").await;
    ember_bridge::deliver_results(f.bridge.state()).await;
    assert_eq!(f.delivery(&second).await, ("delivered".into(), 2));
    let sent = f.results(&second);
    assert_eq!(sent.len(), 2);
    assert_eq!(sent[0], sent[1]);

    // 400: a refusal, never retried.
    f.receiver.answer_with(&[400]);
    let third = f.finished("set-3").await;
    f.until(&third, |(state, _)| state == "failed").await;
    f.bridge.clock.advance(3600);
    ember_bridge::deliver_results(f.bridge.state()).await;
    assert_eq!(f.delivery(&third).await, ("failed".into(), 1));
    assert_eq!(
        f.snapshot(&third).await["provider_delivery_state"],
        "failed"
    );
    assert_eq!(f.results(&third).len(), 1);
}

#[tokio::test]
async fn a_result_once_it_is_being_sent_is_final() {
    let f = fixture("night-bot").await;
    f.receiver.answer_with(&[500]);
    let id = f.finished("set-1").await;
    f.until(&id, |(state, attempts)| {
        state == "retrying" && attempts == 1
    })
    .await;
    // Sending has started, and has not got through.
    let (status, refused) = f.void_last(&id, "void-1").await;
    assert_eq!(
        (status, code(&refused)),
        (StatusCode::CONFLICT, "lease_conflict"),
        "{refused}"
    );
    f.bridge.clock.advance(11);
    f.until(&id, |(state, _)| state == "delivered").await;
    let (status, refused) = f.void_last(&id, "void-2").await;
    assert_eq!(
        (status, code(&refused)),
        (StatusCode::CONFLICT, "lease_conflict"),
        "{refused}"
    );
    assert_eq!(f.snapshot(&id).await["state"], "completed");
    assert_eq!(f.results(&id).len(), 2);
}

#[tokio::test]
async fn without_a_result_secret_nothing_is_sent_and_the_match_waits() {
    let f = fixture("quiet-bot").await;
    let id = f.finished("set-1").await;
    ember_bridge::deliver_results(f.bridge.state()).await;
    assert!(f.receiver.received().is_empty());
    assert_eq!(f.delivery(&id).await, ("queued".into(), 0));
    assert_eq!(f.snapshot(&id).await["provider_delivery_state"], "queued");

    // Nothing has been sent, so a correction still reopens the match.
    let (status, voided) = f.void_last(&id, "void-1").await;
    assert_eq!(status, StatusCode::CREATED, "{voided}");
    assert_ne!(f.snapshot(&id).await["state"], "completed");
    f.win(&id, 1, "game-2").await;

    // Once the secret is there, the result as it now stands goes out.
    let keyed = f.reconfigured(|_| {}, secrets_for(&["quiet-bot"]));
    ember_bridge::deliver_results(&keyed).await;
    assert_eq!(f.delivery(&id).await, ("delivered".into(), 1));
    let sent = f.results(&id);
    assert_eq!(sent.len(), 1);
    assert_eq!(sent[0].outcome, Outcome::Completed);
    assert_eq!(
        sent[0].winner_participant_id.as_deref(),
        Some(f.participant(1))
    );
}

#[tokio::test]
async fn a_match_made_before_the_connection_had_a_results_url_is_never_sent() {
    let f = fixture("late-bot").await;
    let id = f.finished("set-1").await;
    assert_eq!(f.delivery(&id).await, ("not_required".into(), 0));
    assert_eq!(
        f.snapshot(&id).await["provider_delivery_state"],
        "not_required"
    );

    let url = f.url.clone();
    let configured = f.reconfigured(
        |config| {
            for tenant in &mut config.tenants {
                for connection in &mut tenant.connections {
                    if connection.id == "late-bot" {
                        connection.results_url = Some(url.clone());
                    }
                }
            }
        },
        secrets_for(&["late-bot"]),
    );
    ember_bridge::deliver_results(&configured).await;
    assert!(f.receiver.received().is_empty());
    assert_eq!(f.delivery(&id).await, ("not_required".into(), 0));
}

/// Gives the match a permitted game nobody reports, and lets it time out.
async fn let_a_game_go_silent(f: &Fixture, id: &str) {
    let id = id.to_owned();
    f.bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO attempts (id, match_id, assignment_generation, seq, source, state, created_at, start_by, permit_id)
                 VALUES ('att_silent', ?1, 1, 1, 'player_agreement', 'permitted', 0, 0, 'pmt_silent')",
                [&id],
            )?;
            Ok(())
        })
        .await
        .unwrap();
    f.bridge.clock.advance(31 * 60);
    ember_bridge::maintain(f.bridge.state()).await;
}

#[tokio::test]
async fn a_connection_that_restarts_disputes_cancels_what_would_wait_for_review() {
    // `night-bot` restarts: the match is cancelled and the platform told.
    let f = fixture("night-bot").await;
    let id = f.create("set-1", "ember-room-v1", 2).await;
    let_a_game_go_silent(&f, &id).await;
    assert_eq!(f.snapshot(&id).await["state"], "cancelled");
    f.until(&id, |(state, _)| state == "delivered").await;
    let sent = f.results(&id);
    assert_eq!(sent.len(), 1);
    assert_eq!(
        (sent[0].outcome, sent[0].winner_participant_id.clone()),
        (Outcome::Restart, None)
    );
    // Its players are free to play again.
    f.create("set-1-again", "ember-room-v1", 2).await;

    // The default connection reviews: the match waits for an organizer.
    let f = fixture("mock-a").await;
    let id = f.create("set-1", "ember-room-v1", 2).await;
    let_a_game_go_silent(&f, &id).await;
    assert_eq!(f.snapshot(&id).await["state"], "needs_review");
    let (status, refused) = f
        .bridge
        .post_keyed(
            &f.provider,
            "/v1/matches",
            match_body(
                "set-1-again",
                "ember-room-v1",
                2,
                [
                    (f.participant(0), f.kate.id().as_str()),
                    (f.participant(1), f.sam.id().as_str()),
                ],
            ),
            Some("set-1-again"),
        )
        .await;
    assert_eq!(status, StatusCode::CONFLICT, "{refused}");
}

#[tokio::test]
async fn a_connections_disputes_setting_cannot_change_once_recorded() {
    let f = fixture("night-bot").await;
    let with = |connection: &'static str, disputes: Option<config::Disputes>| {
        f.reconfigured(
            move |config| {
                for tenant in &mut config.tenants {
                    for found in &mut tenant.connections {
                        if found.id == connection {
                            found.disputes = disputes;
                        }
                    }
                }
            },
            Secrets::default(),
        )
    };
    // `night-bot` restarts, `mock-a` reviews; neither may be switched.
    assert!(
        ember_bridge::sync_config(&with("night-bot", None))
            .await
            .is_err()
    );
    assert!(
        ember_bridge::sync_config(&with("night-bot", Some(config::Disputes::Review)))
            .await
            .is_err()
    );
    assert!(
        ember_bridge::sync_config(&with("mock-a", Some(config::Disputes::Restart)))
            .await
            .is_err()
    );
    // Saying again what it already is changes nothing.
    ember_bridge::sync_config(&with("night-bot", Some(config::Disputes::Restart)))
        .await
        .unwrap();
    ember_bridge::sync_config(&with("mock-a", Some(config::Disputes::Review)))
        .await
        .unwrap();
    ember_bridge::sync_config(&with("mock-a", None))
        .await
        .unwrap();
}
