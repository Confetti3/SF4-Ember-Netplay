//! A tournament set's replays (docs/design/TOURNAMENT_DESK.md, 4b): the game
//! names an archived `.emberreplay` of a confirmed game and the helper sends
//! it to the set's bridge; the set's pack is fetched into a file the game
//! names. The game keeps track of what was sent and asks again after a
//! failure.
use std::sync::Arc;

use ember_protocol::replays::{self, MAX_REPLAY, Sides};
use serde_json::json;

use super::{
    Code, Failure, Locked, Outcome, Shared, check_path,
    client::{self, Body, BridgeCall, Refusals, Verb},
    play::check_match_id,
};

/// A pack: its header and at most 32 replays (the bridge's limit).
const MAX_PACK: usize = 64 * 1024 + 32 * MAX_REPLAY;
/// The most a bridge answers to a replay it takes.
const MAX_KEPT_ANSWER: usize = 1024;
/// The statuses of a replay the bridge took: kept now, or already had.
const KEPT: &[u16] = &[201, 200];
/// What a replay is sent as.
const REPLAY_TYPE: &str = "application/octet-stream";

/// A bridge without the replay routes takes none, ever: the game is told so
/// once rather than sending again.
const REFUSALS: Refusals<'static> = Refusals {
    reasons: &[],
    missing: Some(Code::ReplaysUnsupported),
};

/// `path` with `pairs` as its query.
fn target(path: &str, pairs: &[(&str, String)]) -> Result<String, Failure> {
    let mut url =
        reqwest::Url::parse("https://bridge.invalid").map_err(|_| Failure::new(Code::Internal))?;
    url.set_path(path);
    if !pairs.is_empty() {
        url.query_pairs_mut().extend_pairs(pairs);
    }
    Ok(match url.query() {
        Some(query) => format!("{}?{query}", url.path()),
        None => url.path().to_owned(),
    })
}

/// The replay at `path`, and the CRC its archive name is made of.
fn read_replay(path: &str) -> Result<(zeroize::Zeroizing<Vec<u8>>, String), Failure> {
    let replay = super::read_file(path, MAX_REPLAY)?;
    let crc = replays::crc_name(&replay)
        .filter(|crc| replays::saved_at(crc, &replay).is_some())
        .ok_or_else(|| Failure::new(Code::InvalidReplay))?;
    Ok((replay, crc))
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
    let (replay, crc) = read_replay(path)?;
    let mut pairs = sides.pairs();
    pairs.push(("revision", revision.to_string()));
    let target = target(&format!("/v1/matches/{match_id}/replays/{crc}"), &pairs)?;
    let call = replay_put(&target, &replay);
    let status = client::with_session(shared, bridge_id, |bridge, token| {
        let call = &call;
        async move {
            let (status, response) = client::send(shared, &bridge, Some(&token), call).await?;
            client::answer(status, &response, KEPT, &REFUSALS)?;
            Ok(status)
        }
    })
    .await?;
    Ok(Some(json!({ "kept": status == 201 })))
}

/// The call that stores `replay` at `target`: 201 when the bridge keeps it
/// now, 200 when it already had it.
fn replay_put<'a>(target: &'a str, replay: &'a [u8]) -> BridgeCall<'a> {
    BridgeCall::new(Verb::Put, target, KEPT)
        .body(Body::Raw {
            content_type: REPLAY_TYPE,
            bytes: replay,
        })
        .refusals(REFUSALS)
        .limit(MAX_KEPT_ANSWER)
}

/// Sends the replay at `path` to the archive of the first of the player's
/// bridges that keeps one: their own approvals, then the built-ins they
/// used. `bridge_id` names it; `kept` is false when it already had the game.
/// None keeps one: `replays_unsupported`, so the game stops sending; a bridge
/// that failed otherwise is the failure, so the game tries again later.
pub async fn archive(shared: &Arc<Shared>, path: &str, sides: &Sides) -> Outcome {
    let (replay, crc) = read_replay(path)?;
    let bridges: Vec<String> = shared
        .bridges
        .locked()?
        .list()
        .into_iter()
        .filter(|listed| !listed.builtin || listed.active)
        .map(|listed| listed.bridge.bridge_id)
        .collect();
    let target = target(&format!("/v1/replays/{crc}"), &sides.pairs())?;
    let call = replay_put(&target, &replay);
    let mut failed = Failure::new(Code::ReplaysUnsupported);
    for bridge_id in bridges {
        let sent = client::with_session(shared, &bridge_id, |bridge, token| {
            let call = &call;
            async move {
                let (status, response) = client::send(shared, &bridge, Some(&token), call).await?;
                client::answer(status, &response, KEPT, &REFUSALS)?;
                Ok(status)
            }
        })
        .await;
        match sent {
            Ok(status) => {
                return Ok(Some(
                    json!({ "kept": status == 201, "bridge_id": bridge_id }),
                ));
            }
            Err(Failure::Local(Code::ReplaysUnsupported)) => {}
            Err(failure) => failed = failure,
        }
    }
    Err(failed)
}

/// Writes the set's pack to `path`.
pub async fn pack(shared: &Arc<Shared>, bridge_id: &str, match_id: &str, path: &str) -> Outcome {
    check_match_id(match_id)?;
    let target = check_path(path)?.to_owned();
    let route = format!("/v1/matches/{match_id}/replays.pack");
    let call = BridgeCall::new(Verb::Get, &route, &[200]).limit(MAX_PACK);
    let response = client::with_session(shared, bridge_id, |bridge, token| {
        let call = &call;
        async move {
            let (status, response) = client::send(shared, &bridge, Some(&token), call).await?;
            if status != 200 {
                return Err(client::refused(status, &response, &Refusals::NONE));
            }
            Ok(response)
        }
    })
    .await?;
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
        assert_eq!(
            (sides.a.as_deref(), sides.side, sides.country.as_deref()),
            (Some("Ann"), Some(1), Some("BE"))
        );
        let set = json!({ "op": "replay_send", "bridge_id": "brg_x", "match_id": "emt_x", "revision": "4",
            "path": "C:/r.emberreplay", "sides": { "a": "Ann", "b": "Bob" } });
        assert!(matches!(
            serde_json::from_value(set),
            Ok(Request::ReplaySend { .. })
        ));
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
            super::target("/v1/replays/0badf00d", &sides.pairs())
                .ok()
                .as_deref(),
            Some("/v1/replays/0badf00d?a=Ann+B&b=%C3%9C%26x&side=1&country=BE")
        );
        assert_eq!(
            super::target("/v1/replays/0badf00d", &Sides::default().pairs())
                .ok()
                .as_deref(),
            Some("/v1/replays/0badf00d")
        );
    }
}
