//! The deployed configuration applies over the database of the deployment
//! before it, as `deploy/setup.sh` does when it updates the staging bridge.
mod common;

use common::Bridge;
use ember_bridge::{AppState, Keys, config::Tenant};

fn tenants(json: &str) -> Vec<Tenant> {
    let value: serde_json::Value = serde_json::from_str(json).unwrap();
    serde_json::from_value(value["tenants"].clone()).unwrap()
}

/// The tenants the staging bridge was first deployed with.
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

#[tokio::test]
async fn the_deployed_tenants_apply_over_the_first_deployment() {
    let bridge = Bridge::start_with(
        |config| config.tenants = tenants(DEPLOYED),
        Default::default(),
    )
    .await;
    assert!(sync(&bridge, tenants(include_str!("../deploy/tenants.json"))).await);

    // A connection keeps its kind; BluMint's partner API is its own connection.
    let mut changed = tenants(DEPLOYED);
    changed[0].connections[0].kind = "blumint".into();
    assert!(!sync(&bridge, changed).await);
}
