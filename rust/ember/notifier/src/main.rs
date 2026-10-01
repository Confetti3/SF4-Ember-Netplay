//! `ember-notifier serve <notifier.json>`.
use std::{path::PathBuf, process::ExitCode};

use ember_notifier::{Clock, Config, Notifier};

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let [command, path] = args.as_slice() else {
        eprintln!("usage: ember-notifier serve <notifier.json>");
        return ExitCode::FAILURE;
    };
    if command != "serve" {
        eprintln!("usage: ember-notifier serve <notifier.json>");
        return ExitCode::FAILURE;
    }
    let result = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .map_err(|error| error.to_string())
        .and_then(|runtime| runtime.block_on(serve(PathBuf::from(path))));
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("ember-notifier: {error}");
            ExitCode::FAILURE
        }
    }
}

async fn serve(path: PathBuf) -> Result<(), String> {
    let config = Config::load(&path)?;
    let listen = config.listen.clone();
    let notifier = Notifier::new(config, Clock::default())?;
    let listener = tokio::net::TcpListener::bind(&listen)
        .await
        .map_err(|error| format!("cannot listen on {listen}: {error}"))?;
    let (address, tasks) = notifier
        .start(listener)
        .map_err(|error| error.to_string())?;
    println!("Ember notifier receiving webhooks on http://{address}/webhook");
    tokio::signal::ctrl_c()
        .await
        .map_err(|error| error.to_string())?;
    for task in tasks {
        task.abort();
    }
    Ok(())
}
