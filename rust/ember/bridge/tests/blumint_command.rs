//! `ember-bridge blumint-register` runs without the server: nothing else in
//! this process sets up TLS, so registration must do it itself. Its own test
//! binary, because the setup is process-wide and any started bridge would do
//! it first.
use std::sync::{Arc, Mutex};

use axum::{Router, http::StatusCode, routing::post};
use ember_bridge::{AppState, Clock, Config, Db, Keys, config, integrations::Secrets};
use zeroize::Zeroizing;

#[tokio::test]
async fn registration_works_without_a_running_bridge() {
    let calls = Arc::new(Mutex::new(0));
    let counted = calls.clone();
    let record = move || {
        let counted = counted.clone();
        async move {
            *counted.lock().unwrap() += 1;
            StatusCode::OK
        }
    };
    let app = Router::new()
        .route("/api/tournaments/setLookupPlayer", post(record.clone()))
        .route("/api/tournaments/match/setCreate", post(record.clone()))
        .route("/api/tournaments/match/setRetrieveStatus", post(record));
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let api_base = format!("http://{}/api", listener.local_addr().unwrap());
    tokio::spawn(async move { axum::serve(listener, app).await });

    let dir = std::env::temp_dir().join(format!(
        "ember-bridge-command-{}",
        ember_protocol::encoding::b64u(&rand16())
    ));
    std::fs::create_dir_all(&dir).unwrap();
    let config = Config {
        bridge_id: ember_protocol::encoding::prefixed_id("brg", rand16()),
        display_name: "Test bridge".into(),
        origin: "https://bridge.example".into(),
        listen: "127.0.0.1:0".into(),
        database: dir.join("bridge.sqlite3"),
        secrets: dir.join("secrets.json"),
        allow_loopback_http: true,
        allow_private_webhooks: true,
        mock_browser: false,
        discord: None,
        integration_secrets: None,
        rooms: None,
        tenants: vec![config::Tenant {
            id: "bm".into(),
            name: "BluMint".into(),
            connections: vec![config::Connection {
                id: "blumint-test".into(),
                kind: "blumint".into(),
                environment: "staging".into(),
                display_name: "BluMint (test)".into(),
                enabled: true,
                api_base: Some(api_base),
                discord_lookup: false,
                disputes: None,
                results_url: None,
                rooms: None,
            }],
        }],
    };
    config.validate().unwrap();
    let db = Db::open(&config.database).unwrap();
    let secrets = Secrets {
        api_keys: [(
            "blumint-test".to_owned(),
            Zeroizing::new("bm-key".to_owned()),
        )]
        .into(),
        ..Default::default()
    };
    let state = AppState::new(config, Keys::generate(), secrets, db, Clock::default());
    ember_bridge::sync_config(&state).await.unwrap();
    ember_bridge::register_blumint(&state, "blumint-test")
        .await
        .unwrap();
    assert_eq!(*calls.lock().unwrap(), 3);
    let _ = std::fs::remove_dir_all(&dir);
}

fn rand16() -> [u8; 16] {
    let mut bytes = [0; 16];
    getrandom::fill(&mut bytes).unwrap();
    bytes
}
