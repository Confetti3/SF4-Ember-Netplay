//! Watching a match, as a watcher's helper and an organizer's tools do it, on
//! top of the fighters' play helpers. Sample players are `player1` and
//! `player2`, never real names.
use ember_protocol::{challenge::Action, play::SignedBinding};
use reqwest::{Method, StatusCode};
use serde_json::{Value as Json, json};

use super::{
    Bridge, Player,
    play::{BUILD, Table, act, bound_as, bridge_key, claim_as, endpoint},
    read,
};

/// Two fighters of one bridge, wherever the bridge is kept.
pub struct Pair<'a> {
    pub bridge: &'a Bridge,
    pub a: &'a Player,
    pub b: &'a Player,
}

impl Table for Pair<'_> {
    fn bridge(&self) -> &Bridge {
        self.bridge
    }

    fn a(&self) -> &Player {
        self.a
    }

    fn b(&self) -> &Player {
        self.b
    }
}

/// A bridge with one `ember-room-v1` match on `mock-a` of tenant `t1`.
pub struct Match {
    pub bridge: Bridge,
    pub provider: String,
    pub organizer: String,
    pub a: Player,
    pub b: Player,
    pub id: String,
}

impl Table for Match {
    fn bridge(&self) -> &Bridge {
        &self.bridge
    }

    fn a(&self) -> &Player {
        &self.a
    }

    fn b(&self) -> &Player {
        &self.b
    }
}

pub const CONNECTION: &str = "mock-a";

pub async fn fixture() -> Match {
    let bridge = Bridge::start().await;
    let provider = bridge.provider(CONNECTION).await;
    let organizer = bridge.organizer("t1").await;
    let (mut a, mut b) = (bridge.player(31), bridge.player(32));
    let id = make_match(
        &bridge,
        &provider,
        [&mut a, &mut b],
        "watch-1",
        "ember-room-v1",
    )
    .await;
    Match {
        bridge,
        provider,
        organizer,
        a,
        b,
        id,
    }
}

/// Opens both players' sessions, links them on `mock-a` and makes a first-to-2
/// match of `profile` between them; returns its ID.
pub async fn make_match(
    bridge: &Bridge,
    provider: &str,
    players: [&mut Player; 2],
    external: &str,
    profile: &str,
) -> String {
    let [a, b] = players;
    bridge.open_session(a).await;
    bridge.open_session(b).await;
    let links = [
        bridge
            .link(provider, a, CONNECTION, &format!("{external}-a"))
            .await,
        bridge
            .link(provider, b, CONNECTION, &format!("{external}-b"))
            .await,
    ];
    let (status, created) = bridge
        .post_keyed(
            provider,
            "/v1/matches",
            json!({
                "external_match_id": external,
                "game": "usf4",
                "participants": [
                    { "participant_id": links[0]["participant_id"], "ember_id": a.id(), "slot": 0 },
                    { "participant_id": links[1]["participant_id"], "ember_id": b.id(), "slot": 1 },
                ],
                "rules": {
                    "games_to_win": 2,
                    "draw_policy": "replay_no_score",
                    "native_rules_profile": profile,
                    "edition_policy": "ultra_only",
                    "character_policy": "unrestricted_between_games",
                    "stage_policy": "p1_selects",
                    "input_delay_policy": "ember_existing_ready_policy",
                },
                "observer_policy": "authorized_only",
                "result_policy": "two_player_agreement_or_review",
                "required_build_id": "ember",
                "metadata": {},
            }),
            Some(external),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED, "{created}");
    created["match_id"].as_str().unwrap().to_owned()
}

/// A player who is not a fighter, with a session and no link.
pub async fn viewer(bridge: &Bridge, byte: u8) -> Player {
    let mut player = bridge.player(byte);
    bridge.open_session(&mut player).await;
    player
}

fn command(endpoint: &str, build: &str) -> Json {
    json!({
        "endpoint_id": endpoint,
        "helper_instance_id": "ins_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a02",
        "build_id": build,
    })
}

/// The watcher's helper asks to watch from its own endpoint on the fighters'
/// build.
pub async fn ask(f: &impl Table, id: &str, who: &Player) -> (StatusCode, Json) {
    ask_from(f, id, who, &endpoint(who), BUILD).await
}

pub async fn ask_from(
    f: &impl Table,
    id: &str,
    who: &Player,
    endpoint: &str,
    build: &str,
) -> (StatusCode, Json) {
    act(
        f,
        who,
        Action::MatchWatch,
        &format!("/v1/matches/{id}/watch"),
        command(endpoint, build),
    )
    .await
}

pub async fn leave(f: &impl Table, id: &str, who: &Player) -> (StatusCode, Json) {
    act(
        f,
        who,
        Action::MatchUnwatch,
        &format!("/v1/matches/{id}/watch/leave"),
        json!({}),
    )
    .await
}

/// A watcher placed in the room: its ask answers `room`.
pub async fn admitted(f: &impl Table, id: &str, who: &Player) -> Json {
    let (status, answer) = ask(f, id, who).await;
    assert_eq!(
        (status, answer["state"].as_str()),
        (StatusCode::OK, Some("room")),
        "{answer}"
    );
    answer
}

pub async fn waiting(f: &impl Table, id: &str, who: &Player) -> String {
    let (status, answer) = ask(f, id, who).await;
    assert_eq!(
        (status, answer["state"].as_str()),
        (StatusCode::OK, Some("waiting")),
        "{answer}"
    );
    answer["reason"].as_str().unwrap().to_owned()
}

/// A claim from a helper that reads watchers, as a current Ember's does.
pub async fn claim_watching(
    f: &impl Table,
    player: &Player,
    id: &str,
    endpoint: &str,
    build: &str,
) -> (StatusCode, Json) {
    claim_as(f, player, id, endpoint, build, true).await
}

/// Claims and publishes the room with both fighters' helpers reading watchers.
pub async fn bound_watching(f: &impl Table, id: &str) -> Json {
    bound_as(f, id, true).await
}

/// The binding the fighter's claim answers with, its signature checked. The
/// fighter's helper reads watchers.
pub async fn binding_for(f: &impl Table, id: &str, fighter: &Player) -> Json {
    let (status, answer) = claim_watching(f, fighter, id, &endpoint(fighter), BUILD).await;
    verified_binding(f, &status, &answer).await
}

/// The binding of a claim's answer, its signature checked.
pub async fn verified_binding(f: &impl Table, status: &StatusCode, answer: &Json) -> Json {
    assert_eq!(*status, StatusCode::OK, "{answer}");
    let signed: SignedBinding = serde_json::from_value(answer["binding"].clone()).unwrap();
    let (key, kid) = bridge_key(f).await;
    serde_json::to_value(signed.verify(&key, &kid).unwrap()).unwrap()
}

/// A binding's `binding_revision`.
pub fn revision(binding: &Json) -> u64 {
    binding["binding_revision"]
        .as_str()
        .unwrap()
        .parse()
        .unwrap()
}

/// The Ember IDs a binding names as watchers, in its order.
pub fn watchers(binding: &Json) -> Vec<String> {
    binding["watchers"]
        .as_array()
        .map(|list| {
            list.iter()
                .map(|w| w["ember_id"].as_str().unwrap().to_owned())
                .collect()
        })
        .unwrap_or_default()
}

pub async fn send(
    bridge: &Bridge,
    token: &str,
    method: Method,
    path: &str,
    body: Option<Json>,
) -> (StatusCode, Json) {
    let mut request = bridge
        .client
        .request(method, bridge.url(path))
        .bearer_auth(token);
    if let Some(body) = body {
        request = request
            .header("content-type", "application/json")
            .body(serde_json::to_vec(&body).unwrap());
    }
    read(request.send().await.unwrap()).await
}

pub async fn add_staff(f: &Match, who: &Player) -> StatusCode {
    let path = format!("/v1/connections/{CONNECTION}/watch/staff/{}", who.id());
    send(&f.bridge, &f.organizer, Method::PUT, &path, Some(json!({})))
        .await
        .0
}

pub async fn remove_staff(f: &Match, who: &Player) -> StatusCode {
    let path = format!("/v1/connections/{CONNECTION}/watch/staff/{}", who.id());
    send(&f.bridge, &f.organizer, Method::DELETE, &path, None)
        .await
        .0
}

pub async fn open_to_link_holders(f: &Match, on: bool) -> StatusCode {
    let path = format!("/v1/connections/{CONNECTION}/watch");
    let body = Some(json!({ "link_holders": on }));
    send(&f.bridge, &f.organizer, Method::PUT, &path, body)
        .await
        .0
}

pub async fn deny(f: &Match, who: &Player) -> StatusCode {
    let path = format!("/v1/matches/{}/watch/denials/{}", f.id, who.id());
    send(&f.bridge, &f.organizer, Method::PUT, &path, Some(json!({})))
        .await
        .0
}

pub async fn clear_denial(f: &Match, who: &Player) -> StatusCode {
    let path = format!("/v1/matches/{}/watch/denials/{}", f.id, who.id());
    send(&f.bridge, &f.organizer, Method::DELETE, &path, None)
        .await
        .0
}

/// `binding_revision`, `fighters_revision` and the stored watcher set of the
/// match's room.
pub async fn room_row(bridge: &Bridge, id: &str) -> (u64, u64, Option<String>) {
    let id = id.to_owned();
    bridge
        .state()
        .db
        .read(move |tx| {
            Ok(tx.query_row(
                "SELECT binding_revision, fighters_revision, bound_watchers FROM match_rooms WHERE match_id = ?1",
                [&id],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
            )?)
        })
        .await
        .unwrap()
}

/// The organizer audit entries of watch changes, oldest first, as action and
/// reason.
pub async fn audited(bridge: &Bridge) -> Vec<(String, String)> {
    bridge
        .state()
        .db
        .read(|tx| {
            Ok(tx
                .prepare(
                    "SELECT action, COALESCE(reason, '') FROM audit_entries
                     WHERE action LIKE 'watch.%' AND action != 'watch.endpoint_conflict' ORDER BY id",
                )?
                .query_map([], |row| Ok((row.get(0)?, row.get(1)?)))?
                .collect::<rusqlite::Result<_>>()?)
        })
        .await
        .unwrap()
}

/// Rows of `sql`'s single integer column, for counting what a test left behind.
pub async fn count(bridge: &Bridge, sql: &'static str) -> i64 {
    bridge
        .state()
        .db
        .read(move |tx| Ok(tx.query_row(sql, [], |row| row.get(0))?))
        .await
        .unwrap()
}

/// The `details.reason` of an error body.
pub fn reason(body: &Json) -> &str {
    body["error"]["details"]["reason"].as_str().unwrap_or("")
}

/// A set between the two players of the BluMint fixture, made through
/// BluMint's own API; returns its ID.
pub async fn blumint_match(f: &super::blumint::Fixture) -> String {
    use super::blumint::{KATE as FIRST, SAM as SECOND, create, lookup};
    lookup(f, json!({ "discord": [FIRST, SECOND] })).await;
    let (status, created) = create(f, "watch-1", 2).await;
    assert_eq!(status, StatusCode::OK, "{created}");
    created["matchId"].as_str().unwrap().to_owned()
}
