//! A tournament set's replays (docs/design/TOURNAMENT_DESK.md, 4b): the game
//! names an archived `.emberreplay` of a confirmed game and the helper sends
//! it to the set's bridge; the set's pack is fetched into a file the game
//! names. The game keeps track of what was sent and asks again after a
//! failure.
use std::{future::Future, sync::Arc, time::Duration};

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
/// The most a replay request has from when the helper receives it to its
/// answer: its wait for the queue and an HTTP slot, then every bridge it
/// tries (`Request::budget`, `within`). A job past it starts no work. The
/// game sends no other replay while one is out and gives up on an answer
/// only after three minutes (ReplaySender::kAnswerMs), so by then this one
/// has answered or been cut off and cannot still reach a bridge.
pub(super) const REPLAY_BUDGET: Duration = Duration::from_secs(45);

/// A bridge without the replay routes takes none, ever: the game is told so
/// once rather than sending again.
const REFUSALS: Refusals<'static> = Refusals {
    reasons: &[],
    missing: Some(Code::ReplaysUnsupported),
};
/// A set's route words two refusals apart: `set_closed`, final for the set
/// though the online archive may still take the game, and
/// `replay_unconfirmed`, the game not confirmed yet, which is tried again.
const SET_REFUSALS: Refusals<'static> = Refusals {
    reasons: &["set_closed", "replay_unconfirmed"],
    ..REFUSALS
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
    let call = replay_put(&target, &replay).refusals(SET_REFUSALS);
    let status = put_to(shared, &call, bridge_id).await?;
    Ok(Some(json!({ "kept": status == 201 })))
}

/// Puts `call` to the bridge `bridge_id`: the status it kept the replay with.
async fn put_to(
    shared: &Arc<Shared>,
    call: &BridgeCall<'_>,
    bridge_id: &str,
) -> Result<u16, Failure> {
    client::with_session(shared, bridge_id, |bridge, token| async move {
        let (status, response) = client::send(shared, &bridge, Some(&token), call).await?;
        client::answer(status, &response, KEPT, &call.refusals)?;
        Ok(status)
    })
    .await
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
/// that failed otherwise is the failure, so the game tries again later. The
/// worker cuts the whole of it off at the request's deadline (REPLAY_BUDGET),
/// so no bridge is tried once that has passed.
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
    let put = |bridge_id: String| {
        let call = &call;
        async move { put_to(shared, call, &bridge_id).await }
    };
    first_keeping(bridges, put).await
}

/// `put` to each of `bridges` in turn until one keeps the replay.
async fn first_keeping<F, Fut>(bridges: Vec<String>, mut put: F) -> Outcome
where
    F: FnMut(String) -> Fut,
    Fut: Future<Output = Result<u16, Failure>>,
{
    let mut failed = Failure::new(Code::ReplaysUnsupported);
    for bridge_id in bridges {
        match put(bridge_id.clone()).await {
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
    use super::{
        super::{Code, Failure, Request, client, within},
        REFUSALS, REPLAY_BUDGET, SET_REFUSALS, Sides, first_keeping,
    };
    use serde_json::json;
    use std::{sync::Mutex, time::Duration};
    use tokio::time::Instant;

    /// A set's route tells the game `set_closed`, final for the set alone,
    /// and `replay_unconfirmed`, tried again later; any other refusal is the
    /// bridge's code, which the game also tries again unless it is final
    /// (ReplaySender.hxx, SendSettled). The online archive's route words
    /// neither apart.
    #[test]
    fn a_sets_refusals_are_worded_apart() {
        let body = |code: &str, reason: &str| {
            serde_json::to_vec(&json!({ "error": {
                "code": code, "message": "m", "retryable": false, "request_id": "req_1",
                "details": { "reason": reason },
            }}))
            .unwrap()
        };
        let set = |status, body: &[u8]| client::refused(status, body, &SET_REFUSALS).into_reason();
        let closed = body("invalid_request", "set_closed");
        assert_eq!(set(400, &closed), "set_closed");
        let unconfirmed = body("service_unavailable", "replay_unconfirmed");
        assert_eq!(set(503, &unconfirmed), "replay_unconfirmed");
        let busy = body("service_unavailable", "busy");
        assert_eq!(set(503, &busy), "service_unavailable");
        assert_eq!(set(503, b""), "http_503");
        let other = body("invalid_request", "other");
        assert_eq!(set(400, &other), "invalid_request");
        let archive = client::refused(400, &closed, &REFUSALS).into_reason();
        assert_eq!(archive, "invalid_request");
    }

    /// A replay request answers by its deadline, whatever its bridges do, and
    /// tries no bridge once the deadline has passed.
    #[tokio::test(start_paused = true)]
    async fn a_replay_request_answers_by_its_deadline() {
        let bridges = || vec!["brg_a".to_owned(), "brg_b".to_owned()];
        let start = Instant::now();
        let hung = within(
            start + REPLAY_BUDGET,
            first_keeping(bridges(), |_| {
                std::future::pending::<Result<u16, Failure>>()
            }),
        );
        assert!(matches!(
            hung.await,
            Err(Failure::Local(Code::BridgeUnreachable))
        ));
        assert_eq!(start.elapsed(), REPLAY_BUDGET);
        let tried = Mutex::new(Vec::new());
        let start = Instant::now();
        let slow = within(
            start + REPLAY_BUDGET,
            first_keeping(bridges(), |bridge_id| {
                tried.lock().unwrap().push(bridge_id);
                async {
                    tokio::time::sleep(REPLAY_BUDGET + Duration::from_secs(1)).await;
                    Err(Failure::new(Code::Io))
                }
            }),
        );
        assert!(matches!(
            slow.await,
            Err(Failure::Local(Code::BridgeUnreachable))
        ));
        assert_eq!(*tried.lock().unwrap(), ["brg_a"]);
        // In time, a bridge without an archive passes it on to the next.
        let kept = within(
            Instant::now() + REPLAY_BUDGET,
            first_keeping(bridges(), |bridge_id| async move {
                if bridge_id == "brg_a" {
                    Err(Failure::new(Code::ReplaysUnsupported))
                } else {
                    Ok(201)
                }
            }),
        );
        assert_eq!(
            kept.await.ok().flatten(),
            Some(json!({ "kept": true, "bridge_id": "brg_b" }))
        );
    }

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
