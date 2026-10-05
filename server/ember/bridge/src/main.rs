//! `ember-bridge init <dir>`, `serve <config>`,
//! `credential <config> provider <connection> <label>` or
//! `credential <config> organizer <tenant> <label>`, `credentials <config>`
//! `revoke <config> <credential-id>` and `blumint-register <config> <connection>`.
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
        ["credentials", path] => runtime.block_on(credentials(PathBuf::from(path))),
        ["revoke", path, id] => runtime.block_on(revoke(PathBuf::from(path), id)),
        ["blumint-register", path, connection] => runtime.block_on(async {
            let state = open(Path::new(path))?;
            ember_bridge::sync_config(&state)
                .await
                .map_err(|error| error.message.to_string())?;
            ember_bridge::register_blumint(&state, connection).await?;
            println!("BluMint now calls this bridge for {connection}.");
            Ok(())
        }),
        ["result-secret"] => {
            // A platform verifies the results sent to its results_url with
            // this; the operator stores it with set-integration-secret.sh.
            let secret = ember_protocol::webhook::Secret::from_bytes(random32());
            println!("{}", secret.reveal().as_str());
            eprintln!(
                "Give this to the platform and store it with set-integration-secret.sh result <connection-id>."
            );
            Ok(())
        }
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
  ember-bridge credential <bridge.json> organizer <tenant-id> <label>
  ember-bridge credentials <bridge.json>
  ember-bridge revoke <bridge.json> <credential-id>
  ember-bridge blumint-register <bridge.json> <connection-id>
  ember-bridge result-secret";

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
        discord: None,
        integration_secrets: None,
        rooms: None,
        match_expiry_hours: config::DEFAULT_MATCH_EXPIRY_HOURS,
        tenants: vec![config::Tenant {
            id: "local".into(),
            name: "Local tournaments".into(),
            connections: vec![config::Connection {
                id: "mock-local".into(),
                kind: "mock".into(),
                environment: "local".into(),
                display_name: "Mock provider".into(),
                enabled: true,
                api_base: None,
                discord_lookup: false,
                disputes: None,
                results_url: None,
                rooms: None,
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

fn random32() -> [u8; 32] {
    let mut bytes = [0; 32];
    getrandom::fill(&mut bytes).expect("operating system RNG");
    bytes
}

fn random() -> [u8; 16] {
    let mut bytes = [0; 16];
    getrandom::fill(&mut bytes).expect("operating system RNG");
    bytes
}

fn open(path: &Path) -> Result<AppState, String> {
    let config = Config::load(path)?;
    let keys = Keys::load(&config.secrets)?;
    let integrations = ember_bridge::integrations::Secrets::load(&config)?;
    if config.discord.is_some() && integrations.discord_client_secret.is_none() {
        eprintln!("ember-bridge: Discord sign-in is off until its client secret is stored");
    }
    let db =
        Db::open(&config.database).map_err(|error| format!("cannot open database: {error}"))?;
    let state = AppState::new(config, keys, integrations, db, Clock::default());
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

/// Lists issued credentials (never their tokens) so one can be revoked.
async fn credentials(path: PathBuf) -> Result<(), String> {
    let state = open(&path)?;
    let list = ember_bridge::list_credentials(&state)
        .await
        .map_err(|error| format!("cannot list credentials: {}", error.message))?;
    for credential in list {
        let scope = credential
            .connection_id
            .as_deref()
            .unwrap_or(&credential.tenant_id);
        let revoked = credential
            .revoked_at
            .map(|at| format!("  revoked {}", ember_protocol::event::rfc3339(at)))
            .unwrap_or_default();
        println!(
            "{}  {:<9}  {}  {}  {:?}{revoked}",
            credential.id,
            credential.role,
            scope,
            ember_protocol::event::rfc3339(credential.created_at),
            credential.label,
        );
    }
    Ok(())
}

async fn revoke(path: PathBuf, id: &str) -> Result<(), String> {
    let state = open(&path)?;
    let changed = ember_bridge::revoke_credential(&state, id)
        .await
        .map_err(|error| format!("cannot revoke {id}: {}", error.message))?;
    println!(
        "{id} {}",
        if changed {
            "revoked"
        } else {
            "was already revoked"
        }
    );
    Ok(())
}
