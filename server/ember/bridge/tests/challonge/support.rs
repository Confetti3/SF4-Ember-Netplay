//! The stand-in Challonge, the bridge configured for it, and the requests the
//! Challonge sign-in tests make, on the sign-in helpers both sites share
//! (`common::sign_in`).
use std::sync::Arc;

use ember_bridge::{config, integrations::Secrets};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};
use zeroize::Zeroizing;

pub use crate::common::sign_in::{Gate, User, confirmation, player, send};
use crate::common::{
    Bridge, Player,
    sign_in::{self, CHALLONGE, Code, FakeSite},
};

pub const KATE: User = ("1001", "Kate_99");
pub const SAM: User = ("1002", "sam");
/// Another Challonge account, which took Kate's name in another case after
/// she gave it up.
pub const RIVAL: User = ("1004", "KATE_99");
pub const CLIENT_ID: &str = "ember-test-client";
pub const CLIENT_SECRET: &str = "challonge-test-secret";
/// Finds players by Challonge username only.
pub const BRACKETS: &str = "bracket-bot";
/// Finds players by Discord account only.
pub const DISCORD_ONLY: &str = "discord-bot";
/// Finds players either way.
pub const BOTH: &str = "both-bot";

pub static HELD: Gate = Gate::new();

/// A stand-in for Challonge's token and `/me` endpoints. Codes `good-kate`,
/// `good-sam` and `good-rival` sign those users in, `held-kate` waits for
/// `HELD`, `busy` answers 429, `odd-name` signs in a user whose username
/// lookups cannot match, and anything else is refused. `/me` answers only
/// with Challonge's `Authorization-Type: v2`, giving Kate's ID as a string
/// and the others' as numbers, both of which Challonge's API uses.
pub fn fake_challonge() -> Arc<FakeSite> {
    FakeSite::new(
        "/oauth/token",
        "/v2.1/me.json",
        (CLIENT_ID, CLIENT_SECRET),
        "/v1/challonge/callback",
        vec![
            ("good-kate", Code::SignsIn(KATE)),
            ("good-sam", Code::SignsIn(SAM)),
            ("good-rival", Code::SignsIn(RIVAL)),
            ("odd-name", Code::SignsIn(("1003", "has space"))),
            ("held-kate", Code::Held(&HELD, KATE)),
            ("busy", Code::Fails(429)),
        ],
        &[("authorization-type", "v2")],
        |(id, username)| {
            let id = if id == KATE.0 {
                json!(id)
            } else {
                json!(id.parse::<u64>().unwrap())
            };
            json!({ "data": { "id": id, "type": "user",
                     "attributes": { "username": username, "email": "x@example.com" } } })
        },
    )
}

pub fn direct(id: &str, discord_lookup: bool, challonge_lookup: bool) -> config::Connection {
    config::Connection {
        id: id.into(),
        kind: "direct".into(),
        environment: "staging".into(),
        display_name: format!("Direct {id}"),
        enabled: true,
        api_base: None,
        discord_lookup,
        challonge_lookup,
        disputes: None,
        results_url: None,
        rooms: None,
    }
}

/// The integration secrets with Challonge's client secret.
pub fn with_secret() -> Secrets {
    Secrets {
        challonge_client_secret: Some(Zeroizing::new(CLIENT_SECRET.into())),
        ..Default::default()
    }
}

/// A bridge with Challonge sign-in against `fake`, with these other
/// `challonge` settings and integration secrets, and connections that find
/// players by Challonge username (whose tenant is named Cups), by Discord
/// account, or both. The clock is frozen.
pub async fn bridge_with(fake: &Arc<FakeSite>, settings: Json, secrets: Secrets) -> Bridge {
    let api_base = fake.serve().await;
    let bridge = Bridge::start_with(
        |config| {
            let mut challonge = json!({ "client_id": CLIENT_ID, "api_base": api_base });
            challonge
                .as_object_mut()
                .unwrap()
                .extend(settings.as_object().unwrap().clone());
            config.challonge = Some(serde_json::from_value(challonge).unwrap());
            config.integration_secrets = Some("unused-in-tests.json".into());
            config.tenants.push(config::Tenant {
                id: "cups".into(),
                name: "Cups".into(),
                connections: vec![
                    direct(BRACKETS, false, true),
                    direct(DISCORD_ONLY, true, false),
                    direct(BOTH, true, true),
                ],
            });
        },
        secrets,
    )
    .await;
    bridge.clock.freeze();
    bridge
}

pub async fn bridge(fake: &Arc<FakeSite>) -> Bridge {
    bridge_with(fake, json!({}), with_secret()).await
}

pub async fn try_start(bridge: &Bridge, player: &Player) -> (StatusCode, Json) {
    sign_in::try_start(bridge, player, &CHALLONGE).await
}

/// Starts a sign-in; the state Challonge would hand back.
pub async fn start(bridge: &Bridge, player: &Player) -> String {
    let url = sign_in::start(bridge, player, &CHALLONGE).await;
    sign_in::query(&url)["state"].clone()
}

/// What the browser sees when Challonge sends it back.
pub async fn back(bridge: &Bridge, query: &str) -> String {
    sign_in::back(bridge, &CHALLONGE, query).await
}

/// The player's answer on the page that asked.
pub async fn decide(bridge: &Bridge, confirmation: &str, choice: &str) -> String {
    sign_in::decide(bridge, &CHALLONGE, confirmation, choice).await
}

/// Signs `player` in with code `code`; the first page the browser shows.
pub async fn sign_in(bridge: &Bridge, player: &Player, code: &str) -> String {
    let state = start(bridge, player).await;
    back(bridge, &format!("code={code}&state={state}")).await
}

/// Signs `player` in as `who` (`kate`, `sam` or `rival`), saying yes when
/// the page asks before connecting an account no other Ember ID has.
pub async fn connect(bridge: &Bridge, player: &Player, who: &str) {
    let state = start(bridge, player).await;
    connect_with(bridge, &state, who).await;
}

/// Finishes the sign-in `state` as `who`, as `connect` does.
pub async fn connect_with(bridge: &Bridge, state: &str, who: &str) {
    let mut page = back(bridge, &format!("code=good-{who}&state={state}")).await;
    if page.contains("Connect this Challonge account?") {
        page = decide(bridge, &confirmation(&page), "connect").await;
    }
    assert!(page.contains("Challonge connected"), "{page}");
}

pub async fn cancel(bridge: &Bridge, player: &Player) -> (StatusCode, Json) {
    sign_in::cancel(bridge, player, &CHALLONGE).await
}

pub async fn disconnect(bridge: &Bridge, player: &Player) -> (StatusCode, Json) {
    sign_in::disconnect(bridge, player, &CHALLONGE).await
}

/// The Challonge user ID connected to the player's Ember ID, or None.
pub async fn account(bridge: &Bridge, player: &Player) -> Option<String> {
    sign_in::account(bridge, player, &CHALLONGE).await["user_id"]
        .as_str()
        .map(str::to_owned)
}

/// A lookup on `connection` for these Challonge usernames and Discord IDs.
pub async fn lookup(
    bridge: &Bridge,
    connection: &str,
    challonge: &[&str],
    discord: &[&str],
) -> (StatusCode, Json) {
    let provider = bridge.provider(connection).await;
    bridge
        .post(
            &provider,
            "/v1/players/lookup",
            json!({ "challonge": challonge, "discord": discord }),
        )
        .await
}

/// The Challonge players a lookup on `connection` found: (username asked,
/// user ID, Ember ID).
pub async fn found(
    bridge: &Bridge,
    connection: &str,
    usernames: &[&str],
) -> Vec<(String, String, String)> {
    let (status, answer) = lookup(bridge, connection, usernames, &[]).await;
    assert_eq!(status, StatusCode::OK, "{answer}");
    answer["challonge"]
        .as_array()
        .map(|players| {
            players
                .iter()
                .map(|player| {
                    (
                        player["username"].as_str().unwrap().to_owned(),
                        player["challonge_user_id"].as_str().unwrap().to_owned(),
                        player["ember_id"].as_str().unwrap().to_owned(),
                    )
                })
                .collect()
        })
        .unwrap_or_default()
}

/// The player's active link on `connection`: how it was approved and its
/// claim.
pub async fn link_of(
    bridge: &Bridge,
    connection: &str,
    player: &Player,
) -> Option<(String, String)> {
    let (connection, ember_id) = (connection.to_owned(), player.id().as_str().to_owned());
    bridge
        .state()
        .db
        .read(move |tx| {
            use rusqlite::OptionalExtension;
            Ok(tx
                .query_row(
                    "SELECT approved_via, claim_id FROM links
                     WHERE connection_id = ?1 AND ember_id = ?2 AND revoked_at IS NULL",
                    [&connection, &ember_id],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )
                .optional()?)
        })
        .await
        .unwrap()
}

/// Writes the Discord account `connect` would for Discord, as
/// tests/partner.rs does.
pub async fn connect_discord(bridge: &Bridge, user_id: &str, player: &Player) {
    let (user_id, ember_id) = (user_id.to_owned(), player.id().as_str().to_owned());
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO discord_accounts (user_id, ember_id, username, connected_at) VALUES (?1, ?2, 'kate', 1)",
                [&user_id, &ember_id],
            )?;
            Ok(())
        })
        .await
        .unwrap();
}
