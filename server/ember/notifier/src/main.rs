//! `ember-notifier serve <notifier.json>` and
//! `ember-notifier discord-register <notifier.json>`.
use std::{path::PathBuf, process::ExitCode};

use ember_notifier::{Clock, Config, Notifier, register_commands};

const USAGE: &str = "usage: ember-notifier serve|discord-register <notifier.json>";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let [command, path] = args.as_slice() else {
        eprintln!("{USAGE}");
        return ExitCode::FAILURE;
    };
    let path = PathBuf::from(path);
    let result = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .map_err(|error| error.to_string())
        .and_then(|runtime| match command.as_str() {
            "serve" => runtime.block_on(serve(path)),
            "discord-register" => runtime.block_on(register(path)),
            _ => Err(USAGE.into()),
        });
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

async fn register(path: PathBuf) -> Result<(), String> {
    register_commands(&Config::load(&path)?).await?;
    println!("Registered the /room command.");
    Ok(())
}
