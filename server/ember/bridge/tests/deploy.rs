//! The deployed configurations in `deploy/`: each tenants file makes a valid
//! bridge configuration as `deploy/setup.sh` writes it, and applies over the
//! database of the deployment before it.
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

/// What setup.sh writes: `ember-bridge init`'s configuration with the
/// tenants file over it and the development switches off.
fn deployed_config(file: &str) -> Config {
    let mut config = json!({
        "bridge_id": "brg_0dbc0598-2312-4ce3-9df8-e160330565e6",
        "display_name": "Local Ember bridge",
        "origin": "http://127.0.0.1:8787",
        "listen": "127.0.0.1:8787",
        "database": "bridge.sqlite3",
        "secrets": "bridge-secrets.json",
        "tenants": [],
    });
    let Value::Object(deployed) = serde_json::from_str(file).unwrap() else {
        panic!("a tenants file is a JSON object");
    };
    config.as_object_mut().unwrap().extend(deployed);
    for switch in [
        "allow_loopback_http",
        "allow_private_webhooks",
        "mock_browser",
    ] {
        config[switch] = json!(false);
    }
    let config: Config = serde_json::from_value(config).unwrap();
    config.validate().unwrap();
    config
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
    let bridge = Bridge::start().await;
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
