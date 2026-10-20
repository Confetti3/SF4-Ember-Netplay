//! The deployed configurations in `deploy/`: each tenants file makes a valid
//! bridge configuration as `deploy/setup.sh` writes it (`ember-bridge
//! apply-tenants`, `Config::deployed`), and applies over the database of the
//! deployment before it.
mod common;

use common::Bridge;
use ember_bridge::{AppState, Keys, config::Config, config::Tenant};
use serde_json::{Value, json};

const PRODUCTION: &str = include_str!("../deploy/tenants.json");
const STAGING: &str = include_str!("../deploy/tenants.staging.json");

fn tenants(json: &str) -> Vec<Tenant> {
    let value: Value = serde_json::from_str(json).unwrap();
    serde_json::from_value(value["tenants"].clone()).unwrap()
}

/// The tenants the production bridge was first deployed with.
const DEPLOYED: &str = r#"{ "tenants": [
    { "id": "blumint", "name": "BluMint", "connections": [
        { "id": "blumint-staging", "kind": "direct", "environment": "staging", "display_name": "BluMint (staging)" } ] },
    { "id": "ember", "name": "Ember", "connections": [
        { "id": "ember-test", "kind": "direct", "environment": "staging", "display_name": "Ember test" } ] }
] }"#;

/// Syncs `tenants` into the bridge's database, as a restart with them would.
async fn sync(bridge: &Bridge, tenants: Vec<Tenant>) -> bool {
    let mut config = (*bridge.state().config).clone();
    config.tenants = tenants;
    config.validate().unwrap();
    let state = AppState::new(
        config,
        Keys::generate(),
        Default::default(),
        bridge.state().db.clone(),
        bridge.clock.clone(),
    );
    ember_bridge::sync_config(&state).await.is_ok()
}

/// A bridge.json as `ember-bridge init` writes it.
fn initial() -> Value {
    json!({
        "bridge_id": "brg_0dbc0598-2312-4ce3-9df8-e160330565e6",
        "display_name": "Local Ember bridge",
        "origin": "http://127.0.0.1:8787",
        "listen": "127.0.0.1:8787",
        "database": "bridge.sqlite3",
        "secrets": "bridge-secrets.json",
        "allow_loopback_http": true,
        "allow_private_webhooks": true,
        "mock_browser": true,
        "tenants": [],
    })
}

fn deploy(installed: &Value, file: &str) -> Result<(Value, Config), String> {
    let (text, config) =
        Config::deployed(&serde_json::to_vec(installed).unwrap(), file.as_bytes())?;
    Ok((serde_json::from_slice(&text).unwrap(), config))
}

/// What setup.sh writes on a first install.
fn deployed_config(file: &str) -> Config {
    deploy(&initial(), file).unwrap().1
}

#[tokio::test]
async fn the_deployed_tenants_apply_over_the_first_deployment() {
    let bridge = Bridge::start_with(
        |config| config.tenants = tenants(DEPLOYED),
        Default::default(),
    )
    .await;
    assert!(sync(&bridge, tenants(PRODUCTION)).await);

    // A connection keeps its kind; BluMint's partner API is its own connection.
    let mut changed = tenants(DEPLOYED);
    changed[0].connections[0].kind = "blumint".into();
    assert!(!sync(&bridge, changed).await);
}

#[tokio::test]
async fn the_staging_tenants_apply_to_a_new_bridge() {
    // The staging file also names the sign-in sites its connections look up,
    // which setup.sh writes into the same configuration.
    let sites = deployed_config(STAGING).sign_in_sites;
    let bridge = Bridge::start_with(
        |config| {
            config.sign_in_sites = sites;
            config.integration_secrets = Some("unused-in-tests.json".into());
        },
        Default::default(),
    )
    .await;
    assert!(sync(&bridge, tenants(STAGING)).await);
}

#[test]
fn production_and_staging_are_separate_bridges() {
    let production = deployed_config(PRODUCTION);
    let staging = deployed_config(STAGING);

    // Production keeps its address and port; staging has its own.
    assert_eq!(production.origin, "https://bridge.embernetplay.link");
    assert_eq!(production.listen, "127.0.0.1:47820");
    assert_eq!(production.display_name, "Ember bridge");
    assert_eq!(staging.origin, "https://bridge-staging.embernetplay.link");
    assert_eq!(staging.listen, "127.0.0.1:47821");
    assert_eq!(staging.display_name, "Ember bridge (staging)");

    // One Discord application, each bridge with its own redirect; public
    // rooms only on production, where the room supervisor connects.
    let client_id = |config: &Config| config.discord.as_ref().unwrap().client_id.clone();
    assert_eq!(client_id(&production), client_id(&staging));
    assert!(staging.rooms.is_none());

    // Staging serves only staging connections, BluMint's through its partner API.
    let connections: Vec<_> = staging
        .tenants
        .iter()
        .flat_map(|tenant| &tenant.connections)
        .collect();
    assert!(connections.iter().all(|c| c.environment == "staging"));
    let (tenant, partner) = staging.connection("blumint-partner-staging").unwrap();
    assert_eq!(tenant.id, "blumint");
    assert_eq!(partner.kind, "blumint");
}

#[test]
fn a_deployment_keeps_the_bridge_and_turns_development_off() {
    let (first, _) = deploy(&initial(), STAGING).unwrap();
    assert_eq!(first["bridge_id"], initial()["bridge_id"]);
    for switch in [
        "allow_loopback_http",
        "allow_private_webhooks",
        "mock_browser",
    ] {
        assert_eq!(first[switch], json!(false), "{switch}");
    }
    // Applying the same file again changes nothing.
    let (again, _) = deploy(&first, STAGING).unwrap();
    assert_eq!(again, first);
}

#[test]
fn a_tenants_file_the_bridge_would_not_start_with_is_refused() {
    let refused = |change: Value| {
        let mut file: Value = serde_json::from_str(STAGING).unwrap();
        file.as_object_mut()
            .unwrap()
            .extend(change.as_object().unwrap().clone());
        deploy(&initial(), &file.to_string()).unwrap_err()
    };
    assert_eq!(
        refused(json!({ "database": "elsewhere.sqlite3" })),
        r#"bridge.json has database "elsewhere.sqlite3"; this deployment expects "bridge.sqlite3""#
    );
    assert_eq!(
        refused(json!({ "origin": "http://bridge-staging.embernetplay.link" })),
        "origin must be a bare https origin"
    );
    assert!(refused(json!({ "unknown_setting": 1 })).starts_with("invalid configuration"));
    assert!(deploy(&initial(), "[]").is_err());
}
