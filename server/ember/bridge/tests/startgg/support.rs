//! The stand-in start.gg (its OAuth token endpoint and its GraphQL API), the
//! bridge configured with the shipped start.gg definition pointed at it, and
//! a bridge with both start.gg and Challonge, on the shared sign-in helpers
//! (`common::sign_in`, `common::sites`).
use std::sync::Arc;

use ember_bridge::{config::SignInSite, integrations::Secrets};
use serde_json::{Value as Json, json};

use crate::common::Bridge;
pub use crate::common::{
    sign_in::{Code, FakeSite, User, definition, player, send},
    sites::{
        account_on, bridge_with_sites, connect_on, database_holds, direct, found, link_of, lookup,
        requests_spent, secrets, start_on, with_limits,
    },
};

/// The site ID the definition gives start.gg.
pub const SITE: &str = "startgg";
/// start.gg user IDs are numbers, and gamer tags are no one's own: Kate's
/// twin has her tag.
pub const KATE: User = ("1234567", "Kate");
pub const TWIN: User = ("7654321", "Kate");
/// A user with no player profile, so no gamer tag.
pub const NAMELESS: User = ("2000", "");
/// Signs in, but the GraphQL API answers with errors and no user.
pub const REFUSED: User = ("0", "");
/// Signs in, but the GraphQL API answers more than the bridge reads (a valid
/// user padded past 64 KiB).
pub const HUGE: User = ("3000", "huge");
pub const CLIENT_ID: &str = "ember-startgg-client";
pub const CLIENT_SECRET: &str = "startgg-test-secret";
/// Finds players by start.gg user ID only.
pub const GG: &str = "gg-bot";

/// The callback start.gg must send the browser back to.
pub const CALLBACK: &str = "/v1/sites/startgg/callback";

/// A stand-in for start.gg's token endpoint and GraphQL API. Codes
/// `good-kate`, `good-twin`, `good-nameless`, `good-refused` and `good-huge`
/// sign those
/// users in; anything else is refused. The API gives the user ID as a
/// number, as start.gg's does.
pub fn fake_startgg() -> Arc<FakeSite> {
    FakeSite::new(
        "/oauth/access_token",
        "/gql/alpha",
        (CLIENT_ID, CLIENT_SECRET),
        CALLBACK,
        vec![
            ("good-kate", Code::SignsIn(KATE)),
            ("good-twin", Code::SignsIn(TWIN)),
            ("good-nameless", Code::SignsIn(NAMELESS)),
            ("good-refused", Code::SignsIn(REFUSED)),
            ("good-huge", Code::SignsIn(HUGE)),
        ],
        &[],
        |(id, tag)| match (id, tag) {
            ("0", _) => json!({ "data": { "currentUser": null },
                                "errors": [{ "message": "Invalid authentication token" }] }),
            ("3000", tag) => json!({ "data": { "currentUser": {
                                 "id": 3000, "player": { "gamerTag": tag } } },
                                 "extensions": { "padding": "x".repeat(70 * 1024) } }),
            (id, "") => json!({ "data": { "currentUser": {
                                 "id": id.parse::<u64>().unwrap(), "player": null } } }),
            (id, tag) => json!({ "data": { "currentUser": {
                                 "id": id.parse::<u64>().unwrap(), "player": { "gamerTag": tag } } } }),
        },
    )
}

/// start.gg as `deploy/sites/startgg.json` defines it, pointed at a
/// stand-in served at `origin`.
pub fn startgg_site(origin: &str) -> SignInSite {
    definition(SITE, origin, CLIENT_ID)
}

/// The integration secrets with start.gg's client secret.
pub fn with_secret() -> Secrets {
    secrets(&[(SITE, CLIENT_SECRET)])
}

/// A bridge with start.gg sign-in against `fake` and a connection that
/// finds players by start.gg user ID. The clock is frozen.
pub async fn bridge(fake: &Arc<FakeSite>) -> Bridge {
    let site = startgg_site(&fake.serve().await);
    bridge_with_sites(vec![site], with_secret(), vec![direct(GG, false, &[SITE])]).await
}

/// The user IDs a lookup on `connection` found on start.gg, with the
/// Ember IDs they are connected to.
pub async fn found_ids(bridge: &Bridge, connection: &str, ids: &[&str]) -> Vec<(String, String)> {
    found(bridge, connection, SITE, ids)
        .await
        .into_iter()
        .map(|(asked, user_id, ember_id)| {
            assert_eq!(asked, user_id);
            (user_id, ember_id)
        })
        .collect()
}

/// This site's accounts as stored: (user ID, username, username key).
pub async fn stored(bridge: &Bridge, site: &str) -> Vec<(String, String, Option<String>)> {
    let site = site.to_owned();
    bridge
        .state()
        .db
        .read(move |tx| {
            let mut statement = tx.prepare(
                "SELECT user_id, username, username_key FROM site_accounts WHERE site = ?1 ORDER BY user_id",
            )?;
            let rows = statement
                .query_map([&site], |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)))?
                .collect::<rusqlite::Result<_>>()?;
            Ok(rows)
        })
        .await
        .unwrap()
}

/// A definition the bridge has no code for: a made-up OAuth site, written
/// the way an operator writes one, served by a stand-in at `origin`.
pub fn bracketeer(origin: &str) -> Json {
    json!({
        "id": "bracketeer",
        "display_name": "Bracketeer",
        "client_id": "bracketeer-client",
        "authorize_url": format!("{origin}/login/oauth/authorize"),
        "authorize_params": { "prompt": "login" },
        "token_url": format!("{origin}/login/oauth/access_token"),
        "token_encoding": "form",
        "token_params": { "audience": "ember" },
        "user": {
            "method": "GET",
            "url": format!("{origin}/api/v3/whoami"),
            "headers": { "Accept": "application/vnd.bracketeer+json" },
            "id_pointer": "/account/uid",
            "username_pointer": "/account/handle"
        },
        "username_rule": { "characters": "letters_digits", "also": "_", "max_length": 20 },
        "lookup_by": "username",
        "hosts": ["bracketeer.example"],
        "limits": { "monthly_request_budget": 10, "daily_request_budget": 4, "starts_per_hour": 5 }
    })
}

/// A stand-in for Bracketeer: `good-kate` signs in user `b-17`, whose
/// handle is `KateB`, and its whoami answers only with its own `Accept`.
pub fn fake_bracketeer() -> Arc<FakeSite> {
    FakeSite::new(
        "/login/oauth/access_token",
        "/api/v3/whoami",
        ("bracketeer-client", "bracketeer-secret"),
        "/v1/sites/bracketeer/callback",
        vec![("good-kate", Code::SignsIn(("b-17", "KateB")))],
        &[("accept", "application/vnd.bracketeer+json")],
        |(id, handle)| json!({ "account": { "uid": id, "handle": handle, "plan": "free" } }),
    )
}
