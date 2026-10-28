//! A tournament set's replays (docs/design/TOURNAMENT_DESK.md, 4b): the game
//! names an archived `.emberreplay` of a confirmed game and the helper sends
//! it to the set's bridge; the set's pack is fetched into a file the game
//! names. The game keeps track of what was sent and asks again after a
//! failure.
use std::sync::Arc;

use ember_protocol::replays::{self, MAX_REPLAY, Sides};
use serde_json::json;

use super::{
    Code, Failure, Outcome, Shared, check_path,
    client::{self, Refusals},
    play::check_match_id,
};

/// A bridge without the replay routes takes none, ever: the game is told
/// so once rather than sending again.
const UNSUPPORTED: Refusals<'static> = Refusals {
    reasons: &[],
    missing: Some(Code::ReplaysUnsupported),
};

/// A pack: its header and at most 32 replays (the bridge's limit).
const MAX_PACK: usize = 64 * 1024 + 32 * MAX_REPLAY;

/// `path` on `origin` with `pairs` as its query.
fn url(origin: &str, path: &str, pairs: &[(&str, String)]) -> Result<String, Failure> {
    let mut url = reqwest::Url::parse(&format!("{origin}{path}")).map_err(|_| Failure::new(Code::Internal))?;
    if !pairs.is_empty() {
        url.query_pairs_mut().extend_pairs(pairs);
    }
    Ok(url.into())
}

/// Sends the replay at `path`, played under room revision `revision`, to
/// the set `match_id`, with what the game says of its `sides` for the
/// bridge's archive.
/// `kept` is false when the bridge already had it.
pub async fn send(
    shared: &Arc<Shared>,
    bridge_id: &str,
    match_id: &str,
    revision: u64,
    path: &str,
    sides: &Sides,
) -> Outcome {
    check_match_id(match_id)?;
    let replay = super::read_file(path, MAX_REPLAY)?;
    let crc = replays::crc_name(&replay)
        .filter(|crc| replays::saved_at(crc, &replay).is_some())
        .ok_or_else(|| Failure::new(Code::InvalidReplay))?;
    let mut pairs = sides.pairs();
    pairs.push(("revision", revision.to_string()));
    client::with_session(shared, bridge_id, |bridge, token| {
        let url = url(&bridge.origin, &format!("/v1/matches/{match_id}/replays/{crc}"), &pairs);
        let body = replay.to_vec();
        async move {
            let url = url?;
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
            client::answer(status, &response, &[200, 201], &UNSUPPORTED)?;
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
pub async fn archive(shared: &Arc<Shared>, path: &str, sides: &Sides) -> Outcome {
    let replay = super::read_file(path, MAX_REPLAY)?;
    let crc = replays::crc_name(&replay)
        .filter(|crc| replays::saved_at(crc, &replay).is_some())
        .ok_or_else(|| Failure::new(Code::InvalidReplay))?;
    let bridges: Vec<String> = shared
        .bridges
        .lock()
        .map_err(|_| Failure::new(Code::Internal))?
        .list()
        .into_iter()
        .filter(|listed| !listed.builtin || listed.active)
        .map(|listed| listed.bridge.bridge_id)
        .collect();
    let pairs = sides.pairs();
    let mut failed = Failure::new(Code::ReplaysUnsupported);
    for bridge_id in bridges {
        let sent = client::with_session(shared, &bridge_id, |bridge, token| {
            let url = url(&bridge.origin, &format!("/v1/replays/{crc}"), &pairs);
            let body = replay.to_vec();
            let bridge_id = bridge_id.clone();
            async move {
                let url = url?;
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
                client::answer(status, &response, &[200, 201], &UNSUPPORTED)?;
                Ok(Some(json!({ "kept": status == 201, "bridge_id": bridge_id })))
            }
        })
        .await;
        match sent {
            Err(Failure::Local(Code::ReplaysUnsupported)) => {}
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
                return client::answer(status, &response, &[], &Refusals::NONE).map(|_| None);
            }
            if !response.starts_with(replays::PACK_MAGIC) {
                return Err(Failure::new(Code::BridgeInvalidResponse));
            }
            // ponytail: a plain write; a torn file is refused by the game's import.
            tokio::task::spawn_blocking(move || std::fs::write(target, response))
                .await
                .map_err(|_| Failure::new(Code::Internal))?
                .map_err(|_| Failure::new(Code::Io))?;
            Ok(Some(json!({ "path": path })))
        }
    })
    .await
}

#[cfg(test)]
mod tests {
    use super::{super::Request, Sides};
    use serde_json::json;

    /// The requests as the game sends them (SendSetReplays).
    #[test]
    fn the_games_replay_requests_read() {
        let archive = json!({ "op": "replay_archive", "path": "C:/r.emberreplay",
            "sides": { "a": "Ann", "b": "", "side": 1, "country": "BE" } });
        let Ok(Request::ReplayArchive { sides, .. }) = serde_json::from_value(archive) else {
            panic!("replay_archive was not read");
        };
        assert_eq!((sides.a.as_deref(), sides.side, sides.country.as_deref()), (Some("Ann"), Some(1), Some("BE")));
        let set = json!({ "op": "replay_send", "bridge_id": "brg_x", "match_id": "emt_x", "revision": "4",
            "path": "C:/r.emberreplay", "sides": { "a": "Ann", "b": "Bob" } });
        assert!(matches!(serde_json::from_value(set), Ok(Request::ReplaySend { .. })));
    }

    #[test]
    fn sides_are_a_query_the_bridge_reads() {
        let sides = Sides {
            a: Some("Ann B".into()),
            b: Some("Ü&x".into()),
            side: Some(1),
            country: Some("BE".into()),
        };
        assert_eq!(
            super::url("http://bridge", "/v1/replays/0badf00d", &sides.pairs()).ok().as_deref(),
            Some("http://bridge/v1/replays/0badf00d?a=Ann+B&b=%C3%9C%26x&side=1&country=BE")
        );
        assert_eq!(
            super::url("http://bridge", "/v1/replays/0badf00d", &Sides::default().pairs()).ok().as_deref(),
            Some("http://bridge/v1/replays/0badf00d")
        );
    }
}
