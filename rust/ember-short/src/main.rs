//! `ember-short [--listen 127.0.0.1:47810]`
//!
//! Serves the short invitation store on a loopback address for nginx to
//! proxy. It writes no request log.
use std::{net::SocketAddr, process::ExitCode, time::Duration};

use ember_short::{AppState, Limits, SWEEP_INTERVAL_SECS, serve_until, system_clock};

const DEFAULT_LISTEN: &str = "127.0.0.1:47810";
const USAGE: &str = "usage: ember-short [--listen 127.0.0.1:47810]";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let listen = match args
        .iter()
        .map(String::as_str)
        .collect::<Vec<_>>()
        .as_slice()
    {
        [] => DEFAULT_LISTEN.to_owned(),
        ["--listen", address] => (*address).to_owned(),
        _ => return fail(USAGE),
    };
    let Ok(address) = listen.parse::<SocketAddr>() else {
        return fail("--listen needs an address and port, such as 127.0.0.1:47810");
    };
    // nginx terminates TLS and sets X-Real-IP; a public bind would let any
    // client choose the address it is counted under.
    if !address.ip().is_loopback() {
        return fail("--listen must be a loopback address");
    }
    let runtime = match tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()
    {
        Ok(runtime) => runtime,
        Err(error) => return fail(&error.to_string()),
    };
    match runtime.block_on(serve(address)) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => fail(&error),
    }
}

fn fail(message: &str) -> ExitCode {
    eprintln!("ember-short: {message}");
    ExitCode::FAILURE
}

async fn serve(address: SocketAddr) -> Result<(), String> {
    let state = AppState::new(Limits::default(), system_clock());
    let listener = tokio::net::TcpListener::bind(address)
        .await
        .map_err(|error| format!("cannot listen on {address}: {error}"))?;
    let sweeper = state.clone();
    tokio::spawn(async move {
        let mut tick = tokio::time::interval(Duration::from_secs(SWEEP_INTERVAL_SECS));
        loop {
            tick.tick().await;
            sweeper.sweep();
        }
    });
    eprintln!("ember-short: listening on {address}");
    serve_until(listener, state, shutdown())
        .await
        .map_err(|error| error.to_string())
}

async fn shutdown() {
    #[cfg(unix)]
    {
        let mut terminate =
            match tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate()) {
                Ok(signal) => signal,
                Err(_) => {
                    let _ = tokio::signal::ctrl_c().await;
                    return;
                }
            };
        tokio::select! {
            _ = terminate.recv() => {}
            _ = tokio::signal::ctrl_c() => {}
        }
    }
    #[cfg(not(unix))]
    {
        let _ = tokio::signal::ctrl_c().await;
    }
}
