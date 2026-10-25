//! The helper's replay channel requests against a real bridge on loopback:
//! two players share a private room game, which lists once both have
//! uploaded it; the file downloads byte for byte; turning sharing off hides it
//! from the list and the file route; and the refusals the game words apart.
#![cfg(windows)]

mod public_rooms_support;

use ember_protocol::replays::{EXPORT_MAGIC, RECORD_BYTES, crc32};
use public_rooms_support::*;
use serde_json::{Value, json};

/// An export of a replay of fighters 39 and 23 played at `time`, as the
/// game's archive writes one.
fn export(time: u64) -> Vec<u8> {
    let mut replay = vec![0u8; 0x400];
    replay[..4].copy_from_slice(b"#BRP");
    let filetime = time * 10_000_000 + 116_444_736_000_000_000;
    replay[0x10..0x18].copy_from_slice(&filetime.to_le_bytes());
    replay[0x20..0x24].copy_from_slice(&39u32.to_le_bytes());
    replay[0x170..0x174].copy_from_slice(&23u32.to_le_bytes());
    let mut record = vec![0u8; RECORD_BYTES];
    record[4] = 1;
    record[5..9].copy_from_slice(&crc32(&replay).to_le_bytes());
    record[9..13].copy_from_slice(&(replay.len() as u32).to_le_bytes());
    record[13..17].copy_from_slice(&(time as u32).to_le_bytes());
    record[71] = 39;
    record[105] = 23;
    let mut out = EXPORT_MAGIC.to_vec();
    out.extend_from_slice(&record);
    out.extend_from_slice(&[0x0E, 0x0E]);
    out.extend_from_slice(&replay);
    out
}

async fn connect_discord(running: &ember_bridge::Running, user_id: &str, ember_id: &str) {
    let (user_id, ember_id) = (user_id.to_owned(), ember_id.to_owned());
    running
        .state
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO discord_accounts (user_id, ember_id, username, connected_at) VALUES (?1, ?2, 'player', 1)",
                [&user_id, &ember_id],
            )?;
            Ok(())
        })
        .await
        .unwrap();
}

fn upload(bridge_id: &str, file: &std::path::Path, room_id: &str, side: u8, name: &str) -> Value {
    json!({ "op": "replay_upload", "bridge_id": bridge_id, "file": file.to_string_lossy(),
        "room_id": room_id, "table": 0, "generation": 1, "side": side, "name": name, "country": "GB" })
}

#[tokio::test]
async fn two_players_share_a_private_game_and_one_stops_sharing() {
    let (first_dirs, second_dirs) = (dirs("replays-first"), dirs("replays-second"));
    let (_supervisor, url) = fake_supervisor().await;
    let (running, origin) = bridge(&first_dirs.0, Some(url)).await;
    let mut first = Worker::start(&first_dirs, &"a".repeat(64));
    let mut second = Worker::start(&second_dirs, &"b".repeat(64));
    let (first_id, bridge_id) = first.join(&origin).await;
    let (second_id, second_bridge) = second.join(&origin).await;
    assert_eq!(bridge_id, second_bridge);

    // Without a Discord account nothing is shared, and no choice can be kept.
    let sharing = json!({ "op": "replay_sharing", "bridge_id": bridge_id });
    assert_eq!(ok(first.ask(sharing.clone()).await), json!({ "sharing": "no_account" }));
    let off = json!({ "op": "replay_share", "bridge_id": bridge_id, "choice": { "sharing": false } });
    assert_eq!(refused(first.ask(off.clone()).await), "no_discord_account");

    connect_discord(&running, "100", &first_id).await;
    connect_discord(&running, "200", &second_id).await;
    assert_eq!(ok(first.ask(sharing.clone()).await), json!({ "sharing": "on" }));

    // The game each archived: the same export, under the archive's own name.
    let file = first_dirs.0.join("20251025-120000-00000000.emberreplay");
    let bytes = export(now() - 600);
    std::fs::write(&file, &bytes).unwrap();
    let room_id = "5".repeat(32);
    let held = ok(first.ask(upload(&bridge_id, &file, &room_id, 0, "Player 1")).await);
    assert_eq!(held, json!({ "state": "held" }));
    let list = json!({ "op": "replay_list", "bridge_id": bridge_id, "query": {} });
    assert_eq!(ok(first.ask(list.clone()).await), json!({ "replays": [] }));
    let published = ok(second.ask(upload(&bridge_id, &file, &room_id, 1, "Player 2")).await);
    assert_eq!(published["state"], "published");
    let id = published["id"].as_str().unwrap().to_owned();

    // Each side's name is its own upload's; the caller's side is marked.
    let page = ok(first.ask(list.clone()).await);
    let entry = &page["replays"][0];
    assert_eq!(entry["id"], id.as_str());
    assert_eq!(entry["room"], "private");
    assert_eq!(entry["sides"][0]["name"], "Player 1");
    assert_eq!(entry["sides"][0]["fighter"], 39);
    assert_eq!(entry["sides"][0]["own"], true);
    assert_eq!(entry["sides"][1]["name"], "Player 2");
    assert_eq!(entry["sides"][1]["fighter"], 23);
    // A fighter filter lists it for either side's fighter, and not for another.
    let filtered = |fighter: u8| json!({ "op": "replay_list", "bridge_id": bridge_id, "query": { "fighter": fighter } });
    assert_eq!(ok(first.ask(filtered(23)).await)["replays"].as_array().unwrap().len(), 1);
    assert_eq!(ok(first.ask(filtered(1)).await)["replays"].as_array().unwrap().len(), 0);

    // The download is the export byte for byte, never over another file.
    let folder = second_dirs.0.join("channel");
    let download = json!({ "op": "replay_download", "bridge_id": bridge_id, "id": id, "folder": folder.to_string_lossy() });
    let written = ok(second.ask(download.clone()).await);
    let path = std::path::PathBuf::from(written["file"].as_str().unwrap());
    assert_eq!(path, folder.join(format!("{id}.emberreplay")));
    assert_eq!(std::fs::read(&path).unwrap(), bytes);
    assert_eq!(ok(second.ask(download.clone()).await), written);

    // The second player stops sharing: the game is gone from the list and
    // the file route at once, and back when they share again.
    let second_off = json!({ "op": "replay_share", "bridge_id": bridge_id, "choice": { "sharing": false } });
    assert_eq!(ok(second.ask(second_off).await), json!({ "sharing": "off" }));
    assert_eq!(ok(first.ask(list.clone()).await), json!({ "replays": [] }));
    std::fs::remove_file(&path).unwrap();
    assert_eq!(refused(second.ask(download.clone()).await), "replay_not_found");
    let second_on = json!({ "op": "replay_share", "bridge_id": bridge_id, "choice": { "sharing": true } });
    assert_eq!(ok(second.ask(second_on).await), json!({ "sharing": "on" }));
    assert_eq!(ok(first.ask(list).await)["replays"].as_array().unwrap().len(), 1);

    // A file that is no replay never leaves the PC.
    let junk = first_dirs.0.join("junk.emberreplay");
    std::fs::write(&junk, b"EMBRPLY1 not a replay").unwrap();
    assert_eq!(refused(first.ask(upload(&bridge_id, &junk, &room_id, 0, "Player 1")).await), "replay_unreadable");

    first.task.abort();
    second.task.abort();
    running.abort();
}

#[tokio::test]
async fn a_bridge_without_the_channel_is_told_apart() {
    let dirs = dirs("no-replays");
    let (running, origin) = bridge(&dirs.0, None).await;
    let mut worker = Worker::start(&dirs, &"a".repeat(64));
    let (_, bridge_id) = worker.join(&origin).await;
    for request in [
        json!({ "op": "replay_sharing", "bridge_id": bridge_id }),
        json!({ "op": "replay_list", "bridge_id": bridge_id }),
    ] {
        assert_eq!(refused(worker.ask(request).await), "replay_channel_unavailable");
    }
    worker.task.abort();
    running.abort();
}
