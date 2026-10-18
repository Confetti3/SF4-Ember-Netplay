//! The stand-in Discord, the bridge configured for it, and the requests the
//! Discord sign-in tests make, on the sign-in helpers both sites share
//! (`common::sign_in`).
use ember_bridge::{config, integrations::Secrets};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};
use zeroize::Zeroizing;

pub use crate::common::sign_in::{Gate, confirmation, player};
use crate::common::{
    Bridge, Player,
    sign_in::{self, Code, FakeSite, Routes},
};

pub const KATE: &str = "274220342558756145";
pub const SAM: &str = "1725265127372152396";
/// A platform that finds players by Discord account.
pub const BLUMINT: &str = "bm-partner";

pub static SLOW: Gate = Gate::new();
pub static HELD: Gate = Gate::new();
pub static LATE: Gate = Gate::new();

/// A stand-in for Discord's token and user endpoints, and its API base.
/// Codes `good-kate`, `good-sam` and the gated `slow-kate`, `held-kate` and
/// `late-kate` sign in those users; anything else is refused.
pub async fn fake_discord() -> String {
    let (kate, sam) = ((KATE, "kate"), (SAM, "sam"));
    let site = FakeSite::new(
        "/api/oauth2/token",
        "/api/users/@me",
        ("1546980049692135514", "test-secret"),
        "/v1/discord/callback",
        vec![
            ("good-kate", Code::SignsIn(kate)),
            ("good-sam", Code::SignsIn(sam)),
            ("slow-kate", Code::Held(&SLOW, kate)),
            ("held-kate", Code::Held(&HELD, kate)),
            ("late-kate", Code::Held(&LATE, kate)),
        ],
        &[],
        |(id, username)| json!({ "id": id, "username": username }),
    );
    format!("{}/api", site.serve().await)
}

pub async fn bridge_with_discord() -> Bridge {
    bridge_with_discord_secret(Secrets {
        discord_client_secret: Some(Zeroizing::new("test-secret".into())),
        ..Default::default()
    })
    .await
}

/// A bridge configured for Discord and BluMint, with these secrets.
pub async fn bridge_with_discord_secret(secrets: Secrets) -> Bridge {
    bridge_with_discord_sites(secrets, Vec::new()).await
}

/// A site that finds players by Discord account and hands a disputed match
/// to its organizer, as a connection's disputes are by default.
pub fn reviewing_site(name: &str) -> config::Tenant {
    config::Tenant {
        id: "cup".into(),
        name: name.into(),
        connections: vec![config::Connection {
            id: "cup-site".into(),
            kind: "direct".into(),
            environment: "staging".into(),
            display_name: format!("{name} (test)"),
            enabled: true,
            api_base: None,
            discord_lookup: true,
            lookup_sites: Vec::new(),
            disputes: None,
            results_url: None,
            rooms: None,
        }],
    }
}

/// A bridge configured for Discord and BluMint, with these secrets, and
/// these other tenants.
pub async fn bridge_with_discord_sites(secrets: Secrets, others: Vec<config::Tenant>) -> Bridge {
    let api_base = fake_discord().await;
    Bridge::start_with(
        |config| {
            config.discord = Some(config::Discord {
                client_id: "1546980049692135514".into(),
                api_base,
            });
            config.integration_secrets = Some("unused-in-tests.json".into());
            config.tenants.push(config::Tenant {
                id: "bm".into(),
                name: "BluMint".into(),
                connections: vec![config::Connection {
                    id: BLUMINT.into(),
                    kind: "blumint".into(),
                    environment: "staging".into(),
                    display_name: "BluMint (test)".into(),
                    enabled: true,
                    api_base: None,
                    discord_lookup: false,
                    lookup_sites: Vec::new(),
                    disputes: None,
                    results_url: None,
                    rooms: None,
                }],
            });
            config.tenants.extend(others);
        },
        secrets,
    )
    .await
}

/// Starts a sign-in and returns the state Discord would hand back.
pub async fn start(bridge: &Bridge, player: &Player) -> String {
    let url = sign_in::start(bridge, player, &Routes::discord()).await;
    assert_eq!(url.path(), "/oauth2/authorize");
    let query = sign_in::query(&url);
    assert_eq!(query["scope"], "identify");
    assert_eq!(query["client_id"], "1546980049692135514");
    assert_eq!(query["redirect_uri"], bridge.url("/v1/discord/callback"));
    query["state"].clone()
}

/// What the browser sees when Discord sends it back.
pub async fn back(bridge: &Bridge, query: &str) -> String {
    sign_in::back(bridge, &Routes::discord(), query).await
}

pub async fn account(bridge: &Bridge, player: &Player) -> Json {
    sign_in::account(bridge, player, &Routes::discord()).await
}

/// Signs `player` in as `who` (`kate` or `sam`), whose account no other
/// Ember ID has, answering the page's question when it asks.
pub async fn connect(bridge: &Bridge, player: &Player, who: &str) {
    let state = start(bridge, player).await;
    connect_with(bridge, &state, who).await;
}

/// The page a sign-in ends on once the player says yes to a first
/// connection, or the page itself when it did not ask.
pub async fn answered(bridge: &Bridge, page: String) -> String {
    if !page.contains("Connect this Discord account?") {
        return page;
    }
    decide(bridge, &confirmation(&page), "connect").await
}

/// The player's answer on the page that asks before moving an account.
pub async fn decide(bridge: &Bridge, confirmation: &str, choice: &str) -> String {
    sign_in::decide(bridge, &Routes::discord(), confirmation, choice).await
}

/// Signs `player` in as `who`, whose account another Ember ID has, and moves it.
pub async fn move_here(bridge: &Bridge, player: &Player, who: &str) {
    let state = start(bridge, player).await;
    let page = back(bridge, &format!("code=good-{who}&state={state}")).await;
    assert!(page.contains("Move this Discord account?"), "{page}");
    let page = decide(bridge, &confirmation(&page), "move").await;
    assert!(page.contains("Discord connected"), "{page}");
}

/// Ends the player's sign-ins in flight, as Ember's Cancel does.
pub async fn cancel(bridge: &Bridge, player: &Player) -> (StatusCode, Json) {
    sign_in::cancel(bridge, player, &Routes::discord()).await
}

pub async fn disconnect(bridge: &Bridge, player: &Player) -> (StatusCode, Json) {
    sign_in::disconnect(bridge, player, &Routes::discord()).await
}

/// BluMint's player lookup for these Discord users.
pub async fn lookup(bridge: &Bridge, provider: &str, users: &[&str]) -> Json {
    let (status, found) = bridge
        .post(provider, "/v1/blumint/lookup", json!({ "discord": users }))
        .await;
    assert_eq!(status, StatusCode::OK, "{found}");
    found
}

/// BluMint's request for a first to 2 between these two Ember IDs.
pub async fn blumint_match(
    bridge: &Bridge,
    provider: &str,
    one: &str,
    two: &str,
) -> (StatusCode, Json) {
    let team = |id: &str| json!({ "players": [ { "identities": [ { "inGameId": id } ] } ] });
    bridge
        .post_keyed(
            provider,
            "/v1/blumint/matches",
            json!({ "teams": [team(one), team(two)], "spectators": [], "matchSettings": {}, "series": { "gamesToWin": 2 } }),
            Some(&format!("{one}-{two}")),
        )
        .await
}

/// The active links on BluMint's connection: Ember ID and the claim that
/// approved it, by Ember ID.
pub async fn platform_links(bridge: &Bridge) -> Vec<(String, String)> {
    bridge
        .state()
        .db
        .read(|tx| {
            Ok(tx
                .prepare(
                    "SELECT ember_id, claim_id FROM links
                     WHERE connection_id = ?1 AND revoked_at IS NULL ORDER BY ember_id",
                )?
                .query_map([BLUMINT], |row| Ok((row.get(0)?, row.get(1)?)))?
                .collect::<rusqlite::Result<_>>()?)
        })
        .await
        .unwrap()
}

/// Finishes the sign-in `state` as `who`, whose account no other Ember ID has.
pub async fn connect_with(bridge: &Bridge, state: &str, who: &str) {
    let page = back(bridge, &format!("code=good-{who}&state={state}")).await;
    let page = answered(bridge, page).await;
    assert!(page.contains("Discord connected"), "{page}");
}
