//! `ember-bridge init <dir>`, `serve <config>`, and
//! `credential <config> provider <connection> <label>` or
//! `credential <config> organizer <tenant> <label>`.
use std::{
    path::{Path, PathBuf},
    process::ExitCode,
};

use ember_bridge::{AppState, Clock, Config, Db, Keys, config};

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let runtime = match tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
    {
        Ok(runtime) => runtime,
        Err(error) => return fail(&error.to_string()),
    };
    let result = match args
        .iter()
        .map(String::as_str)
        .collect::<Vec<_>>()
        .as_slice()
    {
        ["init", dir] => init(PathBuf::from(dir)),
        ["serve", path] => runtime.block_on(serve(PathBuf::from(path))),
        ["credential", path, "provider", connection, label] => runtime.block_on(credential(
            PathBuf::from(path),
            Some(connection.to_string()),
            None,
            label,
        )),
        ["credential", path, "organizer", tenant, label] => runtime.block_on(credential(
            PathBuf::from(path),
            None,
            Some(tenant.to_string()),
            label,
        )),
        _ => Err(USAGE.into()),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => fail(&error),
    }
}

const USAGE: &str = "usage:
  ember-bridge init <directory>
  ember-bridge serve <bridge.json>
  ember-bridge credential <bridge.json> provider <connection-id> <label>
  ember-bridge credential <bridge.json> organizer <tenant-id> <label>";

fn fail(message: &str) -> ExitCode {
    eprintln!("ember-bridge: {message}");
    ExitCode::FAILURE
}

/// Writes a local development configuration with a mock provider, a fresh
/// secrets file and an empty database location.
fn init(dir: PathBuf) -> Result<(), String> {
    std::fs::create_dir_all(&dir).map_err(|error| error.to_string())?;
    let config_path = dir.join("bridge.json");
    if config_path.exists() {
        return Err(format!("{} already exists", config_path.display()));
    }
    let config = Config {
        bridge_id: ember_protocol::encoding::prefixed_id("brg", random()),
        display_name: "Local Ember bridge".into(),
        origin: "http://127.0.0.1:8787".into(),
        listen: "127.0.0.1:8787".into(),
        database: "bridge.sqlite3".into(),
        secrets: "bridge-secrets.json".into(),
        allow_loopback_http: true,
        allow_private_webhooks: true,
        mock_browser: true,
        tenants: vec![config::Tenant {
            id: "local".into(),
            name: "Local tournaments".into(),
            connections: vec![config::Connection {
                id: "mock-local".into(),
                kind: "mock".into(),
                environment: "local".into(),
                display_name: "Mock provider".into(),
                enabled: true,
            }],
        }],
    };
    Keys::generate().save_new(&dir.join("bridge-secrets.json"))?;
    std::fs::write(
        &config_path,
        serde_json::to_vec_pretty(&config).map_err(|e| e.to_string())?,
    )
    .map_err(|error| error.to_string())?;
    println!(
        "Wrote {}. Keep bridge-secrets.json private; it is not needed by players.",
        config_path.display()
    );
    Ok(())
}

fn random() -> [u8; 16] {
    let mut bytes = [0; 16];
    getrandom::fill(&mut bytes).expect("operating system RNG");
    bytes
}

fn open(path: &Path) -> Result<AppState, String> {
    let config = Config::load(path)?;
    let keys = Keys::load(&config.secrets)?;
    let db =
        Db::open(&config.database).map_err(|error| format!("cannot open database: {error}"))?;
    let state = AppState::new(config, keys, db, Clock::default());
    Ok(state)
}

async fn serve(path: PathBuf) -> Result<(), String> {
    let state = open(&path)?;
    sync_tenants(&state).await?;
    let listener = tokio::net::TcpListener::bind(&state.config.listen)
        .await
        .map_err(|error| format!("cannot listen on {}: {error}", state.config.listen))?;
    let running = ember_bridge::start(state, listener).map_err(|error| error.to_string())?;
    println!("Ember bridge listening on http://{}", running.address);
    shutdown_signal().await?;
    // Every write is one transaction, so stopping between requests loses
    // nothing; a webhook cut off mid-send is retried on the next start.
    running.abort();
    Ok(())
}

/// Ctrl+C, or SIGTERM from a service manager.
async fn shutdown_signal() -> Result<(), String> {
    #[cfg(unix)]
    {
        let mut terminate =
            tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())
                .map_err(|error| error.to_string())?;
        tokio::select! {
            result = tokio::signal::ctrl_c() => result.map_err(|error| error.to_string()),
            _ = terminate.recv() => Ok(()),
        }
    }
    #[cfg(not(unix))]
    tokio::signal::ctrl_c()
        .await
        .map_err(|error| error.to_string())
}

async fn sync_tenants(state: &AppState) -> Result<(), String> {
    ember_bridge::sync_config(state)
        .await
        .map_err(|_| "cannot record tenants".to_owned())
}

async fn credential(
    path: PathBuf,
    connection: Option<String>,
    tenant: Option<String>,
    label: &str,
) -> Result<(), String> {
    let state = open(&path)?;
    sync_tenants(&state).await?;
    let token =
        ember_bridge::issue_credential(&state, connection.as_deref(), tenant.as_deref(), label)
            .await
            .map_err(|error| format!("cannot issue credential: {}", error.message))?;
    println!("{token}");
    eprintln!("Store this credential now. The bridge keeps only a hash of it.");
    Ok(())
}
