//! `ember-rooms <config.json>`
//!
//! Runs the public room supervisor on the loopback address named in the
//! configuration file. It writes no request log.
use std::{path::Path, process::ExitCode};

use ember_rooms::{Settings, Supervisor, Tuning, serve_until};

const USAGE: &str = "usage: ember-rooms <config.json>";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let [path] = args.as_slice() else {
        return fail(USAGE);
    };
    let settings = match Settings::load(Path::new(path)) {
        Ok(settings) => settings,
        Err(error) => return fail(&error),
    };
    let runtime = match tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()
    {
        Ok(runtime) => runtime,
        Err(error) => return fail(&error.to_string()),
    };
    match runtime.block_on(serve(settings)) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => fail(&error),
    }
}

fn fail(message: &str) -> ExitCode {
    eprintln!("ember-rooms: {message}");
    ExitCode::FAILURE
}

async fn serve(settings: Settings) -> Result<(), String> {
    let address = settings.config.bind_address()?;
    for (id, build) in &settings.config.builds {
        for path in [&build.room_host, &build.helper] {
            if !Path::new(path).exists() {
                eprintln!("ember-rooms: build {id}: {path} does not exist");
            }
        }
    }
    let listener = tokio::net::TcpListener::bind(address)
        .await
        .map_err(|error| format!("cannot listen on {address}: {error}"))?;
    let tuning = Tuning::from_config(&settings.config);
    let supervisor = Supervisor::new(settings, tuning);
    eprintln!("ember-rooms: listening on {address}");
    serve_until(listener, supervisor, shutdown())
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
