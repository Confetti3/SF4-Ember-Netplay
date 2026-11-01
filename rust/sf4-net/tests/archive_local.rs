//! Local end-to-end check of the replay archive (not part of the suite):
//! a loopback bridge with an archive in a real S3 store, a helper that has
//! approved it, and every `.emberreplay` in SF4E_ARCHIVE_REPLAYS sent through
//! `replay_archive`, then listed. SF4E_ARCHIVE_S3 is the store's origin
//! (bucket `replays`, keys SF4E_ARCHIVE_KEY / SF4E_ARCHIVE_SECRET).
//! SF4E_ARCHIVE_HOLD keeps the bridge up that many seconds afterwards, on
//! SF4E_ARCHIVE_PORT, for the archive page. The listing it saw is written to
//! archive-local-listing.json in the system temporary folder.
#![cfg(windows)]

mod tournament_support;

use std::time::Duration;

use ember_bridge::{
    Clock, config,
    integrations::Secrets,
    testing::{loopback_config, serve},
};
use serde_json::{Value, json};
use tokio::net::TcpListener;
use tournament_support::*;
use zeroize::Zeroizing;

#[tokio::test]
#[ignore]
async fn a_folder_of_replays_through_the_archive() {
    let var = |name: &str| std::env::var(name).unwrap_or_else(|_| panic!("{name}"));
    let folder = var("SF4E_ARCHIVE_REPLAYS");
    let dirs = dirs("archive-local");
    let port = std::env::var("SF4E_ARCHIVE_PORT").unwrap_or_else(|_| "0".into());
    let listener = TcpListener::bind(format!("0.0.0.0:{port}")).await.unwrap();
    let origin = format!("http://127.0.0.1:{}", listener.local_addr().unwrap().port());
    let mut config = loopback_config(&dirs.0, &origin);
    config.archive = Some(config::Archive {
        endpoint: var("SF4E_ARCHIVE_S3"),
        bucket: "replays".into(),
        region: "us-east-1".into(),
        access_key: var("SF4E_ARCHIVE_KEY"),
        public_base: "https://archive.invalid/replays".into(),
    });
    let secrets = Secrets {
        archive_secret_key: Some(Zeroizing::new(var("SF4E_ARCHIVE_SECRET"))),
        ..Default::default()
    };
    let running = serve(config, secrets, Clock::default(), listener).await;
    let provider =
        ember_bridge::issue_credential(&running.state, Some("mock-local"), None, "archive")
            .await
            .unwrap();

    let player = Dirs(dirs.0.join("player"));
    let mut worker = Worker::start(&player);
    worker.next_answer(0).await;
    worker.ask(json!({ "op": "identity_enable" })).await;
    let (_, _, _, inspected) = worker
        .ask(json!({ "op": "bridge_inspect", "origin": origin }))
        .await;
    let bridge_id = inspected["profile"]["bridge_id"]
        .as_str()
        .unwrap()
        .to_owned();
    let (ok, reason, _, _) = worker
        .ask(json!({ "op": "bridge_approve", "origin": origin, "bridge_id": bridge_id }))
        .await;
    assert!(ok, "{reason:?}");

    let mut files: Vec<_> = std::fs::read_dir(&folder)
        .unwrap()
        .filter_map(|entry| entry.ok().map(|entry| entry.path()))
        .filter(|path| path.extension().is_some_and(|ext| ext == "emberreplay"))
        .collect();
    files.sort();
    let (mut kept, mut known, mut refused) = (0, 0, Vec::new());
    for (n, path) in files.iter().enumerate() {
        let side = (n % 2) as u8;
        let (ok, reason, _, sent) = worker
            .ask(json!({ "op": "replay_archive", "path": path,
                "sides": { "a": format!("Player {}", 2 * n + 1), "b": format!("Player {}", 2 * n + 2), "side": side, "country": "US" } }))
            .await;
        match (ok, sent["kept"].as_bool()) {
            (true, Some(true)) => kept += 1,
            (true, _) => known += 1,
            _ => refused.push(format!("{}: {reason:?}", path.display())),
        }
    }
    println!(
        "sent {}: kept {kept}, already held {known}, refused {}",
        files.len(),
        refused.len()
    );
    for line in &refused {
        println!("  refused {line}");
    }

    // The Archive queue stores them; the listing shows only stored games.
    let client = reqwest::Client::new();
    let mut listed = Value::Null;
    for _ in 0..120 {
        listed = client
            .get(format!("{origin}/v1/archive"))
            .send()
            .await
            .unwrap()
            .json()
            .await
            .unwrap();
        if listed["games"].as_array().map_or(0, Vec::len) >= kept {
            break;
        }
        tokio::time::sleep(Duration::from_millis(500)).await;
    }
    let games = listed["games"].as_array().cloned().unwrap_or_default();
    println!(
        "listed {} stored games; first: {}",
        games.len(),
        games.first().unwrap_or(&Value::Null)
    );
    std::fs::write(
        dirs.0.parent().unwrap().join("archive-local-listing.json"),
        serde_json::to_vec_pretty(&listed).unwrap(),
    )
    .unwrap();
    assert!(refused.is_empty(), "refused: {refused:?}");
    assert_eq!(games.len(), kept, "every kept game is stored and listed");

    if let Ok(hold) = std::env::var("SF4E_ARCHIVE_HOLD") {
        println!("bridge up at {origin} for {hold} s");
        tokio::time::sleep(Duration::from_secs(hold.parse().unwrap())).await;
    }
    running.abort();
}
