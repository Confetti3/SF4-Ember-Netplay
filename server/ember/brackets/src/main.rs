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
    // A worker that stops (a panic) stops the connector, so that its
    // service manager starts it again, rather than leave a tournament
    // unread while results are still taken.
    let aborts: Vec<_> = tasks.iter().map(|task| task.abort_handle()).collect();
    let (stopped, mut first_stopped) = tokio::sync::mpsc::channel(1);
    for task in tasks {
        let stopped = stopped.clone();
        tokio::spawn(async move {
            let _ = stopped.send(task.await).await;
        });
    }
    let outcome = tokio::select! {
        signal = tokio::signal::ctrl_c() => signal.map_err(|error| error.to_string()),
        Some(ended) = first_stopped.recv() => Err(match ended {
            Err(error) if error.is_panic() => "a worker panicked; stopping".to_owned(),
            _ => "a worker stopped; stopping".to_owned(),
        }),
    };
    for abort in aborts {
        abort.abort();
    }
    outcome
}
