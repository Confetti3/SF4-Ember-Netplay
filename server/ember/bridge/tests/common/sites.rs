//! A bridge with configured sign-in sites, the connections that look players
//! up on them, and the lookups those connections make.
use ember_bridge::{
    config::{self, SignInSite, sites::Limits},
    integrations::Secrets,
};
use ember_protocol::challenge::{Action, Method};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};
use zeroize::Zeroizing;

use super::{
    Bridge, Player, seed,
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

/// The limits Challonge's shipped definition sets, for tests to change one
/// of: `Limits { daily_request_budget: 4, ..LIMITS }`.
pub const LIMITS: Limits = Limits {
    monthly_request_budget: 400,
    daily_request_budget: 40,
    starts_per_hour: 30,
};

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
    seed::link_discord(bridge.state(), user_id, player.id().as_str(), "player1").await;
}

/// Writes an account on `site` straight into the bridge's database.
pub async fn insert_account(
    bridge: &Bridge,
    site: &str,
    user_id: &str,
    player: &Player,
    key: Option<&str>,
) -> rusqlite::Result<()> {
    seed::link_site(
        bridge.state(),
        site,
        user_id,
        player.id().as_str(),
        "Renamed",
        key,
    )
    .await
}

/// A configured sign-in site on a test bridge, by its ID, and the requests
/// a player's Ember and browser make on its routes.
#[derive(Clone, Copy)]
pub struct TestSite(pub &'static str);

impl TestSite {
    fn routes(self) -> Routes {
        Routes::site(self.0)
    }

    pub async fn try_start(self, bridge: &Bridge, player: &Player) -> (StatusCode, Json) {
        sign_in::try_start(bridge, player, &self.routes()).await
    }

    /// Starts a sign-in; the state the site would hand back.
    pub async fn start(self, bridge: &Bridge, player: &Player) -> String {
        let url = sign_in::start(bridge, player, &self.routes()).await;
        sign_in::query(&url)["state"].clone()
    }

    /// What the browser sees when the site sends it back with `query`.
    pub async fn back(self, bridge: &Bridge, query: &str) -> String {
        sign_in::back(bridge, &self.routes(), query).await
    }

    /// The player's answer on the page that asked.
    pub async fn decide(self, bridge: &Bridge, confirmation: &str, choice: &str) -> String {
        sign_in::decide(bridge, &self.routes(), confirmation, choice).await
    }

    /// Signs `player` in with `code`; the first page the browser shows.
    pub async fn sign_in(self, bridge: &Bridge, player: &Player, code: &str) -> String {
        let state = self.start(bridge, player).await;
        self.back(bridge, &format!("code={code}&state={state}"))
            .await
    }

    /// Signs `player` in with `code`, saying yes when the page asks before
    /// connecting; the last page the browser shows.
    pub async fn connect(self, bridge: &Bridge, player: &Player, code: &str) -> String {
        let page = self.sign_in(bridge, player, code).await;
        if !page.contains("name=\"confirmation\"") {
            return page;
        }
        self.decide(bridge, &sign_in::confirmation(&page), "connect")
            .await
    }

    pub async fn cancel(self, bridge: &Bridge, player: &Player) -> (StatusCode, Json) {
        sign_in::cancel(bridge, player, &self.routes()).await
    }

    pub async fn disconnect(self, bridge: &Bridge, player: &Player) -> (StatusCode, Json) {
        sign_in::disconnect(bridge, player, &self.routes()).await
    }

    /// The user ID of the player's account on the site, or None.
    pub async fn account(self, bridge: &Bridge, player: &Player) -> Option<String> {
        sign_in::account(bridge, player, &self.routes()).await["user_id"]
            .as_str()
            .map(str::to_owned)
    }

    /// The accounts a lookup on `connection` found on the site: (ID asked,
    /// user ID, Ember ID).
    pub async fn found(
        self,
        bridge: &Bridge,
        connection: &str,
        ids: &[&str],
    ) -> Vec<(String, String, String)> {
        let accounts: Vec<(&str, &str)> = ids.iter().map(|id| (self.0, *id)).collect();
        let (status, answer) = lookup(bridge, connection, &accounts, &[]).await;
        assert_eq!(status, StatusCode::OK, "{answer}");
        answer["accounts"]
            .as_array()
            .map(|accounts| {
                accounts
                    .iter()
                    .map(|account| {
                        assert_eq!(account["site"], self.0);
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

    /// This month's requests to the site.
    pub async fn requests_spent(self, bridge: &Bridge) -> u64 {
        let (now, site) = (bridge.clock.now(), self.0);
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

/// Removes the player's active link on `connection`, as they do from
/// Ember's list of links.
pub async fn remove_link(bridge: &Bridge, player: &Player, connection: &str) {
    let (_, links) = bridge.get(player.token(), "/v1/links").await;
    let link = links["links"]
        .as_array()
        .unwrap()
        .iter()
        .find(|link| link["connection_id"] == connection)
        .expect("a link on the connection")["link_id"]
        .as_str()
        .unwrap()
        .to_owned();
    let path = format!("/v1/links/{link}");
    let (status, removed) = sign_in::send(
        bridge,
        player,
        Action::LinkRemove,
        Method::Delete,
        &path,
        json!({ "link_id": link }),
    )
    .await;
    assert_eq!(status, StatusCode::OK, "{removed}");
}
