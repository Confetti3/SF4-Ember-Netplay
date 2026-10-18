//! The stand-in Challonge, the bridge configured with the shipped Challonge
//! definition pointed at it, and the requests the Challonge sign-in tests
//! make, on the shared sign-in helpers (`common::sign_in`, `common::sites`).
use std::sync::Arc;

use ember_bridge::{config::SignInSite, integrations::Secrets};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};

use crate::common::{
    Bridge, Player,
    sign_in::{self, Code, FakeSite, Routes, definition},
    sites::{self, bridge_with_sites, with_limits},
};
pub use crate::common::{
    sign_in::{Gate, User, confirmation, player, send},
    sites::{connect_discord, database_holds, direct, insert_account, link_of, secrets},
};

/// The site ID the definition gives Challonge.
pub const SITE: &str = "challonge";
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
        "/v1/sites/challonge/callback",
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

/// Challonge as `deploy/sites/challonge.json` defines it, pointed at a
/// stand-in served at `origin`.
pub fn challonge_site(origin: &str) -> SignInSite {
    definition("challonge", origin, CLIENT_ID)
}

/// The integration secrets with Challonge's client secret.
pub fn with_secret() -> Secrets {
    secrets(&[(SITE, CLIENT_SECRET)])
}

/// A bridge with Challonge sign-in against `fake`, with these `limits`
/// changed and these integration secrets, and connections that find players
/// by Challonge username (whose tenant is named Cups), by Discord account,
/// or both. The clock is frozen.
pub async fn bridge_with(fake: &Arc<FakeSite>, limits: Json, secrets: Secrets) -> Bridge {
    let site = with_limits(challonge_site(&fake.serve().await), limits);
    bridge_with_sites(
        vec![site],
        secrets,
        vec![
            direct(BRACKETS, false, &[SITE]),
            direct(DISCORD_ONLY, true, &[]),
            direct(BOTH, true, &[SITE]),
        ],
    )
    .await
}

pub async fn bridge(fake: &Arc<FakeSite>) -> Bridge {
    bridge_with(fake, json!({}), with_secret()).await
}

pub async fn try_start(bridge: &Bridge, player: &Player) -> (StatusCode, Json) {
    sign_in::try_start(bridge, player, &Routes::site(SITE)).await
}

/// Starts a sign-in; the state Challonge would hand back.
pub async fn start(bridge: &Bridge, player: &Player) -> String {
    let url = sign_in::start(bridge, player, &Routes::site(SITE)).await;
    sign_in::query(&url)["state"].clone()
}

/// What the browser sees when Challonge sends it back.
pub async fn back(bridge: &Bridge, query: &str) -> String {
    sign_in::back(bridge, &Routes::site(SITE), query).await
}

/// The player's answer on the page that asked.
pub async fn decide(bridge: &Bridge, confirmation: &str, choice: &str) -> String {
    sign_in::decide(bridge, &Routes::site(SITE), confirmation, choice).await
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
    sign_in::cancel(bridge, player, &Routes::site(SITE)).await
}

pub async fn disconnect(bridge: &Bridge, player: &Player) -> (StatusCode, Json) {
    sign_in::disconnect(bridge, player, &Routes::site(SITE)).await
}

/// The Challonge user ID connected to the player's Ember ID, or None.
pub async fn account(bridge: &Bridge, player: &Player) -> Option<String> {
    sign_in::account(bridge, player, &Routes::site(SITE)).await["user_id"]
        .as_str()
        .map(str::to_owned)
}

/// A lookup on `connection` for these Challonge usernames and Discord IDs.
pub async fn lookup(
    bridge: &Bridge,
    connection: &str,
    names: &[&str],
    discord: &[&str],
) -> (StatusCode, Json) {
    let accounts: Vec<(&str, &str)> = names.iter().map(|name| (SITE, *name)).collect();
    sites::lookup(bridge, connection, &accounts, discord).await
}

/// The Challonge players a lookup on `connection` found: (username asked,
/// user ID, Ember ID).
pub async fn found(
    bridge: &Bridge,
    connection: &str,
    names: &[&str],
) -> Vec<(String, String, String)> {
    sites::found(bridge, connection, SITE, names).await
}
