//! `ember-bridge blumint-register` runs without the server: nothing else in
//! this process sets up TLS, so registration must do it itself. Its own test
//! binary, because the setup is process-wide and any started bridge would do
//! it first.
use std::sync::{Arc, Mutex};

use axum::{Router, http::StatusCode, routing::post};
use ember_bridge::{
    AppState, Clock, Db, Keys, config,
    integrations::Secrets,
    testing::{TempDir, connection, loopback_config},
};
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
        .route("/api/tournaments/match/setClose", post(record));
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let api_base = format!("http://{}/api", listener.local_addr().unwrap());
    tokio::spawn(async move { axum::serve(listener, app).await });

    let dir = TempDir::new("ember-bridge-command");
    let mut config = loopback_config(&dir, "https://bridge.example");
    let mut blumint = connection("blumint-test", "BluMint (test)");
    blumint.kind = "blumint".into();
    blumint.environment = "staging".into();
    blumint.api_base = Some(api_base);
    config.tenants = vec![config::Tenant {
        id: "bm".into(),
        name: "BluMint".into(),
        connections: vec![blumint],
    }];
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
}
