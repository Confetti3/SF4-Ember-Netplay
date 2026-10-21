//! A tournament set's replays (docs/design/TOURNAMENT_DESK.md, 4b): the game
//! names an archived `.emberreplay` of a confirmed game and the helper sends
//! it to the set's bridge; the set's pack is fetched into a file the game
//! names. The game keeps track of what was sent and asks again after a
//! failure.
use std::sync::Arc;

use ember_protocol::replays::{self, MAX_REPLAY};
use serde_json::json;

use super::{Failure, Outcome, Shared, check_path, client, play::check_match_id};

/// A pack: its header and at most 32 replays (the bridge's limit).
const MAX_PACK: usize = 64 * 1024 + 32 * MAX_REPLAY;

/// Sends the replay at `path`, played under room revision `revision`, to
/// the set `match_id`. `kept` is false when the bridge already had it.
pub async fn send(
    shared: &Arc<Shared>,
    bridge_id: &str,
    match_id: &str,
    revision: u64,
    path: &str,
) -> Outcome {
    check_match_id(match_id)?;
    let replay = super::read_file(path, MAX_REPLAY)?;
    let crc = replays::crc_name(&replay)
        .filter(|crc| replays::saved_at(crc, &replay).is_some())
        .ok_or_else(|| Failure::new("invalid_replay"))?;
    client::with_session(shared, bridge_id, |bridge, token| {
        let url = format!(
            "{}/v1/matches/{match_id}/replays/{crc}?revision={revision}",
            bridge.origin
        );
        let body = replay.to_vec();
        async move {
            let (status, response) = shared
                .client
                .call_bounded(
                    reqwest::Method::PUT,
                    &url,
                    Some(&token),
                    Some(body),
                    "application/octet-stream",
                    1024,
                )
                .await?;
            // A bridge without the replay routes takes none, ever: the
            // game is told so once rather than sending again.
            if status == 200 {
                client::answer(status, &response, &[200])?;
            } else {
                client::feature_answer(status, &response, 201, &[], "replays_unsupported")?;
            }
            Ok(Some(json!({ "kept": status == 201 })))
        }
    })
    .await
}

/// Writes the set's pack to `path`.
pub async fn pack(shared: &Arc<Shared>, bridge_id: &str, match_id: &str, path: &str) -> Outcome {
    check_match_id(match_id)?;
    let target = check_path(path)?.to_owned();
    client::with_session(shared, bridge_id, |bridge, token| {
        let url = format!("{}/v1/matches/{match_id}/replays.pack", bridge.origin);
        let target = target.clone();
        async move {
            let (status, response) = shared
                .client
                .call_bounded(reqwest::Method::GET, &url, Some(&token), None, "", MAX_PACK)
                .await?;
            if status != 200 {
                return client::answer(status, &response, &[]).map(|_| None);
            }
            if !response.starts_with(replays::PACK_MAGIC) {
                return Err(Failure::new("bridge_invalid_response"));
            }
            // ponytail: a plain write; a torn file is refused by the game's import.
            tokio::task::spawn_blocking(move || std::fs::write(target, response))
                .await
                .map_err(|_| Failure::new("internal"))?
                .map_err(|_| Failure::new("io"))?;
            Ok(Some(json!({ "path": path })))
        }
    })
    .await
}
