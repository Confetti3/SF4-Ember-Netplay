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

/// What the game says of a replay for the archive (docs/design/REPLAY_ARCHIVE.md),
/// as the bridge's query: `&a=&b=&side=&country=`, empty when it says none.
/// Names longer than the bridge keeps, and anything but a two-letter
/// country, are left out.
pub fn sides(names: &[Option<String>; 2], side: Option<u8>, country: Option<String>) -> String {
    let mut query = String::new();
    for (key, name) in ["a", "b"].into_iter().zip(names) {
        if let Some(name) = name.as_deref().filter(|name| !name.is_empty() && name.chars().count() <= 64) {
            query.push_str(&format!("&{key}="));
            for byte in name.bytes() {
                if byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'.' | b'_' | b'~') {
                    query.push(char::from(byte));
                } else {
                    query.push_str(&format!("%{byte:02X}"));
                }
            }
        }
    }
    if let Some(side) = side.filter(|side| *side < 2) {
        query.push_str(&format!("&side={side}"));
    }
    if let Some(country) = country.filter(|code| code.len() == 2 && code.bytes().all(|b| b.is_ascii_uppercase())) {
        query.push_str(&format!("&country={country}"));
    }
    query
}

/// Sends the replay at `path`, played under room revision `revision`, to
/// the set `match_id`, with `sides` (`sides`) for the bridge's archive.
/// `kept` is false when the bridge already had it.
pub async fn send(
    shared: &Arc<Shared>,
    bridge_id: &str,
    match_id: &str,
    revision: u64,
    path: &str,
    sides: &str,
) -> Outcome {
    check_match_id(match_id)?;
    let replay = super::read_file(path, MAX_REPLAY)?;
    let crc = replays::crc_name(&replay)
        .filter(|crc| replays::saved_at(crc, &replay).is_some())
        .ok_or_else(|| Failure::new("invalid_replay"))?;
    client::with_session(shared, bridge_id, |bridge, token| {
        let url = format!(
            "{}/v1/matches/{match_id}/replays/{crc}?revision={revision}{sides}",
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

/// Sends the replay at `path` to the archive of the first of the player's
/// bridges that keeps one: their own approvals, then the built-ins they
/// used. `bridge_id` names it; `kept` is false when it already had the game.
/// None keeps one: `replays_unsupported`, so the game stops sending; a bridge
/// that failed otherwise is the failure, so the game tries again later.
pub async fn archive(shared: &Arc<Shared>, path: &str, sides: &str) -> Outcome {
    let replay = super::read_file(path, MAX_REPLAY)?;
    let crc = replays::crc_name(&replay)
        .filter(|crc| replays::saved_at(crc, &replay).is_some())
        .ok_or_else(|| Failure::new("invalid_replay"))?;
    // ponytail: the store is in memory once opened, so a plain lock.
    let bridges: Vec<String> = shared
        .bridges
        .lock()
        .map_err(|_| Failure::new("internal"))?
        .list()
        .into_iter()
        .filter(|listed| !listed.builtin || listed.active)
        .map(|listed| listed.bridge.bridge_id)
        .collect();
    let query = sides.strip_prefix('&').unwrap_or(sides).to_owned();
    let mut failed = Failure::new("replays_unsupported");
    for bridge_id in bridges {
        let sent = client::with_session(shared, &bridge_id, |bridge, token| {
            let url = format!("{}/v1/replays/{crc}?{query}", bridge.origin);
            let body = replay.to_vec();
            let bridge_id = bridge_id.clone();
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
                if status == 200 {
                    client::answer(status, &response, &[200])?;
                } else {
                    client::feature_answer(status, &response, 201, &[], "replays_unsupported")?;
                }
                Ok(Some(json!({ "kept": status == 201, "bridge_id": bridge_id })))
            }
        })
        .await;
        match sent {
            Err(Failure(code)) if code == "replays_unsupported" => {}
            Err(failure) => failed = failure,
            sent => return sent,
        }
    }
    Err(failed)
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

#[cfg(test)]
mod tests {
    #[test]
    fn sides_are_a_query_the_bridge_reads() {
        let names = [Some("Ann B".to_owned()), Some("Ü&x".to_owned())];
        assert_eq!(super::sides(&names, Some(1), Some("BE".into())), "&a=Ann%20B&b=%C3%9C%26x&side=1&country=BE");
        assert_eq!(super::sides(&[None, Some(String::new())], Some(2), Some("be".into())), "");
    }
}

