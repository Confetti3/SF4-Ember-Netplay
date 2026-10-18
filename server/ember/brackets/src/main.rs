//! `ember-brackets serve <brackets.json>`.
use std::{path::PathBuf, process::ExitCode};

use ember_brackets::{Clock, Config, Connector};

const USAGE: &str = "usage: ember-brackets serve <brackets.json>";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let [command, path] = args.as_slice() else {
        eprintln!("{USAGE}");
        return ExitCode::FAILURE;
    };
    if command != "serve" {
        eprintln!("{USAGE}");
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
            eprintln!("ember-brackets: {error}");
            ExitCode::FAILURE
        }
    }
}

async fn serve(path: PathBuf) -> Result<(), String> {
    let config = Config::load(&path)?;
    let listen = config.results.listen.clone();
    let tournaments = config.tournaments.len();
    let connector = Connector::new(config, Clock::default())?;
    let listener = tokio::net::TcpListener::bind(&listen)
        .await
        .map_err(|error| format!("cannot listen on {listen}: {error}"))?;
    let (address, tasks) = connector
        .start(listener)
        .map_err(|error| error.to_string())?;
    println!(
        "Ember brackets running {tournaments} tournament(s), receiving results on http://{address}{}",
        connector.results_path()
    );
    tokio::signal::ctrl_c()
        .await
        .map_err(|error| error.to_string())?;
    for task in tasks {
        task.abort();
    }
    Ok(())
}
