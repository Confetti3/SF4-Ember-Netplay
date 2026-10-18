//! A bridge with configured sign-in sites, the connections that look players
//! up on them, and the lookups those connections make.
use ember_bridge::{
    config::{self, SignInSite},
    integrations::Secrets,
};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};
use zeroize::Zeroizing;

use super::{
    Bridge, Player,
    sign_in::{self, Routes},
};

/// A `direct` connection that finds players by Discord account if
/// `discord_lookup`, and on `lookup_sites`.
pub fn direct(id: &str, discord_lookup: bool, lookup_sites: &[&str]) -> config::Connection {
    config::Connection {
        id: id.into(),
        kind: "direct".into(),
        environment: "staging".into(),
        display_name: format!("Direct {id}"),
        enabled: true,
        api_base: None,
        discord_lookup,
        lookup_sites: lookup_sites.iter().map(|site| (*site).to_owned()).collect(),
        disputes: None,
        results_url: None,
        rooms: None,
    }
}

/// The integration secrets holding these sites' client secrets.
pub fn secrets(sites: &[(&str, &str)]) -> Secrets {
    Secrets {
        site_client_secrets: sites
            .iter()
            .map(|(site, secret)| ((*site).to_owned(), Zeroizing::new((*secret).to_owned())))
            .collect(),
        ..Default::default()
    }
}

/// A bridge with these sign-in sites and secrets, and a tenant named Cups
/// with these connections. The clock is frozen.
pub async fn bridge_with_sites(
    sites: Vec<SignInSite>,
    secrets: Secrets,
    connections: Vec<config::Connection>,
) -> Bridge {
    let bridge = Bridge::start_with(
        |config| {
            config.sign_in_sites = sites;
            config.integration_secrets = Some("unused-in-tests.json".into());
            config.tenants.push(config::Tenant {
                id: "cups".into(),
                name: "Cups".into(),
                connections,
            });
        },
        secrets,
    )
    .await;
    bridge.clock.freeze();
    bridge
}

/// A site definition with these `limits` fields changed.
pub fn with_limits(site: SignInSite, limits: Json) -> SignInSite {
    let mut site = serde_json::to_value(site).unwrap();
    let defaults = json!({
        "monthly_request_budget": 400, "daily_request_budget": 40, "starts_per_hour": 30,
    });
    let current = site
        .get("limits")
        .filter(|limits| !limits.is_null())
        .cloned()
        .unwrap_or(defaults);
    let mut merged = current.as_object().unwrap().clone();
    merged.extend(limits.as_object().unwrap().clone());
    site["limits"] = Json::Object(merged);
    serde_json::from_value(site).unwrap()
}

/// A lookup on `connection` for these site accounts and Discord IDs.
pub async fn lookup(
    bridge: &Bridge,
    connection: &str,
    accounts: &[(&str, &str)],
    discord: &[&str],
) -> (StatusCode, Json) {
    let provider = bridge.provider(connection).await;
    let accounts: Vec<Json> = accounts
        .iter()
        .map(|(site, id)| json!({ "site": site, "id": id }))
        .collect();
    bridge
        .post(
            &provider,
            "/v1/players/lookup",
            json!({ "accounts": accounts, "discord": discord }),
        )
        .await
}

/// The accounts a lookup on `connection` found on `site`: (ID asked, user
/// ID, Ember ID).
pub async fn found(
    bridge: &Bridge,
    connection: &str,
    site: &str,
    ids: &[&str],
) -> Vec<(String, String, String)> {
    let accounts: Vec<(&str, &str)> = ids.iter().map(|id| (site, *id)).collect();
    let (status, answer) = lookup(bridge, connection, &accounts, &[]).await;
    assert_eq!(status, StatusCode::OK, "{answer}");
    answer["accounts"]
        .as_array()
        .map(|accounts| {
            accounts
                .iter()
                .map(|account| {
                    assert_eq!(account["site"], site);
                    (
                        account["id"].as_str().unwrap().to_owned(),
                        account["user_id"].as_str().unwrap().to_owned(),
                        account["ember_id"].as_str().unwrap().to_owned(),
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

/// Writes the Discord account a Discord sign-in would, as tests/partner.rs
/// does.
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

/// Writes an account on `site` straight into the bridge's database.
pub async fn insert_account(
    bridge: &Bridge,
    site: &str,
    user_id: &str,
    player: &Player,
    key: Option<&str>,
) -> rusqlite::Result<()> {
    let (site, user_id, ember_id, key) = (
        site.to_owned(),
        user_id.to_owned(),
        player.id().as_str().to_owned(),
        key.map(str::to_owned),
    );
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO site_accounts (site, user_id, ember_id, username, username_key, connected_at)
                 VALUES (?1, ?2, ?3, 'Renamed', ?4, 1)",
                rusqlite::params![site, user_id, ember_id, key],
            )?;
            Ok(())
        })
        .await
        .map_err(|_| rusqlite::Error::QueryReturnedNoRows)
}

/// Starts a sign-in on `site`; the state the site would hand back.
pub async fn start_on(bridge: &Bridge, site: &str, player: &Player) -> String {
    let url = sign_in::start(bridge, player, &Routes::site(site)).await;
    sign_in::query(&url)["state"].clone()
}

/// Signs `player` in on `site` with `code`, saying yes when the page asks
/// before connecting; the last page the browser shows.
pub async fn connect_on(bridge: &Bridge, site: &str, player: &Player, code: &str) -> String {
    let routes = Routes::site(site);
    let state = start_on(bridge, site, player).await;
    let page = sign_in::back(bridge, &routes, &format!("code={code}&state={state}")).await;
    if page.contains("name=\"confirmation\"") {
        let confirmation = sign_in::confirmation(&page);
        return sign_in::decide(bridge, &routes, &confirmation, "connect").await;
    }
    page
}

/// The user ID of the player's account on `site`, or None.
pub async fn account_on(bridge: &Bridge, site: &str, player: &Player) -> Option<String> {
    sign_in::account(bridge, player, &Routes::site(site)).await["user_id"]
        .as_str()
        .map(str::to_owned)
}

/// This month's requests to `site`.
pub async fn requests_spent(bridge: &Bridge, site: &str) -> u64 {
    let (now, site) = (bridge.clock.now(), site.to_owned());
    bridge
        .state()
        .db
        .read(move |tx| {
            Ok(tx.query_row(
                "SELECT COALESCE(SUM(spent), 0) FROM site_requests
                 WHERE site = ?1 AND day LIKE strftime('%Y-%m', ?2, 'unixepoch') || '-%'",
                rusqlite::params![site, now],
                |row| row.get(0),
            )?)
        })
        .await
        .unwrap()
}

/// Whether `text` is anywhere in the bridge's database files.
pub async fn database_holds(bridge: &Bridge, text: &str) -> bool {
    let database = bridge.state().config.database.clone();
    ["", "-wal"].iter().any(|suffix| {
        let mut path = database.clone().into_os_string();
        path.push(suffix);
        std::fs::read(path).is_ok_and(|bytes| {
            bytes
                .windows(text.len())
                .any(|window| window == text.as_bytes())
        })
    })
}
