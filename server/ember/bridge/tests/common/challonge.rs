//! The stand-in Challonge, the bridge configured with the shipped Challonge
//! definition pointed at it, and the requests the Challonge sign-in tests
//! make, on the shared sign-in helpers (`common::sign_in`, `common::sites`).
//!
//! Every test binary that has `common` builds this, so what a binary does
//! not use is not a mistake.
#![allow(unused_imports)]
use std::sync::Arc;

use ember_bridge::{
    config::{SignInSite, sites::Limits},
    integrations::Secrets,
};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};

use super::{
    Bridge, Player,
    sign_in::{Code, FakeSite, definition},
    sites::{self, bridge_with_sites},
};
pub use super::{
    sign_in::{Gate, User, confirmation, player},
    sites::{
        LIMITS, TestSite, connect_discord, database_holds, direct, insert_account, link_of,
        remove_link, secrets,
    },
};

/// Challonge, by the site ID its definition gives it.
pub const CHALLONGE: TestSite = TestSite("challonge");
pub const SITE: &str = CHALLONGE.0;
pub const PLAYER1: User = ("1001", "Player_1");
pub const PLAYER2: User = ("1002", "player2");
/// Another Challonge account, which took Player 1's name in another case after
/// it was given up.
pub const PLAYER3: User = ("1004", "PLAYER_1");
pub const CLIENT_ID: &str = "ember-test-client";
pub const CLIENT_SECRET: &str = "challonge-test-secret";
/// Finds players by Challonge username only.
pub const BRACKETS: &str = "bracket-bot";
/// Finds players by Discord account only.
pub const DISCORD_ONLY: &str = "discord-bot";
/// Finds players either way.
pub const BOTH: &str = "both-bot";

pub static HELD: Gate = Gate::new();

/// A stand-in for Challonge's token and `/me` endpoints. Codes `good-player1`,
/// `good-player2` and `good-player3` sign those users in, `held-player1` waits for
/// `HELD`, `busy` answers 429, `odd-name` signs in a user whose username
/// lookups cannot match, `no-name` one with an empty username, and anything
/// else is refused. `/me` answers only
/// with Challonge's `Authorization-Type: v2`, giving Player 1's ID as a string
/// and the others' as numbers, both of which Challonge's API uses.
pub fn fake_challonge() -> Arc<FakeSite> {
    FakeSite::new(
        "/oauth/token",
        "/v2.1/me.json",
        (CLIENT_ID, CLIENT_SECRET),
        "/v1/sites/challonge/callback",
        vec![
            ("good-player1", Code::SignsIn(PLAYER1)),
            ("good-player2", Code::SignsIn(PLAYER2)),
            ("good-player3", Code::SignsIn(PLAYER3)),
            ("odd-name", Code::SignsIn(("1003", "has space"))),
            ("no-name", Code::SignsIn(("1005", ""))),
            ("held-player1", Code::Held(&HELD, PLAYER1)),
            ("busy", Code::Fails(429)),
        ],
        &[("authorization-type", "v2")],
        |(id, username)| {
            let id = if id == PLAYER1.0 {
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

/// A bridge with Challonge sign-in against `fake`, with these `limits` and
/// these integration secrets, and connections that find players
/// by Challonge username (whose tenant is named Cups), by Discord account,
/// or both. The clock is frozen.
pub async fn bridge_with(fake: &Arc<FakeSite>, limits: Limits, secrets: Secrets) -> Bridge {
    let mut site = challonge_site(&fake.serve().await);
    site.limits = Some(limits);
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
    bridge_with(fake, LIMITS, with_secret()).await
}

/// Signs `player` in as `who` (`player1`, `player2` or `player3`), saying yes when
/// the page asks before connecting an account no other Ember ID has.
pub async fn connect(bridge: &Bridge, player: &Player, who: &str) {
    let state = CHALLONGE.start(bridge, player).await;
    connect_with(bridge, &state, who).await;
}

/// Finishes the sign-in `state` as `who`, as `connect` does.
pub async fn connect_with(bridge: &Bridge, state: &str, who: &str) {
    let mut page = CHALLONGE
        .back(bridge, &format!("code=good-{who}&state={state}"))
        .await;
    if page.contains("Connect this Challonge account?") {
        page = CHALLONGE
            .decide(bridge, &confirmation(&page), "connect")
            .await;
    }
    assert!(page.contains("Challonge connected"), "{page}");
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
