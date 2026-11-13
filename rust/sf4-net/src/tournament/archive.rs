//! The public replay archive for the game's own browser
//! (docs/design/REPLAY_ARCHIVE.md): a page of the bridge's `GET /v1/archive`,
//! and one listed game written into a file the game names, which the game
//! then plays. The listing's types are the protocol's (`ember_protocol::archive`).
//!
//! Both are open routes, so no session is opened. The bridge says where its
//! archive lists its games in its capabilities (`Capabilities::archive`),
//! which is not always its own host. A bridge that says nothing is read at
//! `fallback_listing`. A game is fetched only from the host the listing is
//! on and only under the archive's own file names, so an answer cannot send
//! the helper anywhere else.
use std::{sync::Arc, time::Duration};

use ember_protocol::{
    archive::{MAX_URL, PATH, Page, Query},
    encoding::check_origin,
    replays::{self, MAX_REPLAY},
};
use serde_json::json;

use super::{
    Code, Failure, Outcome, Shared,
    bridges::Approved,
    check_path,
    client::{self, BridgeCall, Refusals, Verb},
    play::{self, Need},
};

/// The most a listing answer is read for: a full page of games with the sets,
/// events and countries that go with it.
const MAX_LISTING: usize = 320 * 1024;
const LISTING_TIMEOUT: Duration = Duration::from_secs(15);
const FETCH_TIMEOUT: Duration = Duration::from_secs(30);

/// A 404 is a place that does not serve the archive: a bridge without one.
const REFUSALS: Refusals<'static> = Refusals {
    reasons: &[],
    missing: Some(Code::ReplaysUnsupported),
};

/// Where a bridge that names no archive in its capabilities (the deployments
/// before they did) lists it: the `archive.` host beside a `bridge.` host
/// (production), else under `/archive/` on the bridge's own host (staging).
fn fallback_listing(origin: &str) -> String {
    match origin.strip_prefix("https://bridge.") {
        Some(rest) => format!("https://archive.{rest}{PATH}"),
        None => format!("{origin}/archive{PATH}"),
    }
}

/// The listing URL of `bridge`, split into its origin and path, and what the
/// bridge offers beside it.
struct Archive {
    origin: String,
    path: String,
    profiles: bool,
}

async fn archive_of(shared: &Arc<Shared>, bridge: &Approved) -> Result<Archive, Failure> {
    let offered = play::features(shared, bridge, Need::Profiles).await?;
    let listing = offered
        .archive
        .unwrap_or_else(|| fallback_listing(&bridge.origin));
    let invalid = || Failure::new(Code::BridgeInvalidResponse);
    let url = reqwest::Url::parse(&listing).map_err(|_| invalid())?;
    let origin = url.origin().ascii_serialization();
    if url.path() != PATH
        || url.query().is_some()
        || !url.username().is_empty()
        || check_origin(&origin, client::POLICY).is_err()
    {
        return Err(invalid());
    }
    Ok(Archive {
        origin,
        path: PATH.to_owned(),
        profiles: offered.profiles,
    })
}

/// `path` with `pairs` as its query.
fn target(path: &str, pairs: &[(&str, String)]) -> Result<String, Failure> {
    let mut url =
        reqwest::Url::parse("https://archive.invalid").map_err(|_| Failure::new(Code::Internal))?;
    url.set_path(path);
    if !pairs.is_empty() {
        url.query_pairs_mut().extend_pairs(pairs);
    }
    Ok(match url.query() {
        Some(query) => format!("{}?{query}", url.path()),
        None => url.path().to_owned(),
    })
}

/// What a bridge is asked: `name` only when it lists the profile routes, which
/// is where it reads it; the substring `q` the game sends beside it is all an
/// older bridge reads.
fn asked(query: &Query, profiles: bool) -> Query {
    Query {
        name: query.name.clone().filter(|_| profiles),
        ..query.clone()
    }
}

/// A page of the archive on `bridge_id`. A bridge that does not list the
/// profile routes does not know `name`, so only the substring `q` beside it
/// is asked.
pub async fn list(shared: &Arc<Shared>, bridge_id: &str, query: &Query) -> Outcome {
    if !query.check() {
        return Err(Failure::new(Code::InvalidRequest));
    }
    let bridge = client::approved(shared, bridge_id)?;
    let archive = archive_of(shared, &bridge).await?;
    let path = target(&archive.path, &asked(query, archive.profiles).pairs())?;
    let call = BridgeCall::new(Verb::Get, &path, &[200])
        .refusals(REFUSALS)
        .limit(MAX_LISTING)
        .timeout(LISTING_TIMEOUT);
    let host = Approved {
        origin: archive.origin,
        ..bridge
    };
    let (status, body) = client::send(shared, &host, None, &call).await?;
    if status != 200 {
        return Err(client::refused(status, &body, &REFUSALS));
    }
    let page: Page = serde_json::from_slice(&body)
        .map_err(|_| Failure::new(Code::BridgeInvalidResponse))?;
    serde_json::to_value(page.bounded())
        .map(Some)
        .map_err(|_| Failure::new(Code::Internal))
}

/// The digest `url` names, when it is a game of the archive on `origin`: a
/// plain address with no credentials, query or fragment, whose file is
/// `games/<digest>.emberreplay`.
fn game_digest(url: &str, origin: &str) -> Result<String, Failure> {
    let invalid = || Failure::new(Code::InvalidRequest);
    if url.len() > MAX_URL {
        return Err(invalid());
    }
    let parsed = reqwest::Url::parse(url).map_err(|_| invalid())?;
    if !parsed.username().is_empty()
        || parsed.password().is_some()
        || parsed.query().is_some()
        || parsed.fragment().is_some()
        || parsed.origin().ascii_serialization() != origin
    {
        return Err(invalid());
    }
    parsed
        .path()
        .rsplit_once("/games/")
        .and_then(|(_, file)| file.strip_suffix(".emberreplay"))
        .filter(|digest| {
            digest.len() == 64
                && digest
                    .bytes()
                    .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
        })
        .map(str::to_owned)
        .ok_or_else(invalid)
}

/// Writes the game at `url`, listed by the archive of `bridge_id`, to `path`
/// (a `.emberreplay`), once it is whole and is the game its address names.
pub async fn fetch(shared: &Arc<Shared>, bridge_id: &str, url: &str, path: &str) -> Outcome {
    let target = check_path(path)?.to_owned();
    if !path.ends_with(".emberreplay") {
        return Err(Failure::new(Code::InvalidPath));
    }
    let bridge = client::approved(shared, bridge_id)?;
    let digest = game_digest(url, &archive_of(shared, &bridge).await?.origin)?;
    let (status, body) = shared
        .client
        .exchange(
            reqwest::Method::GET,
            url,
            None,
            None,
            "application/octet-stream",
            MAX_REPLAY,
            Some(FETCH_TIMEOUT),
        )
        .await?;
    if status != 200 {
        return Err(client::refused(status, &body, &Refusals::NONE));
    }
    let named = replays::Replay::parse(&body)
        .map(|replay| replay.digest_hex())
        .ok_or_else(|| Failure::new(Code::InvalidReplay))?;
    if named != digest {
        return Err(Failure::new(Code::InvalidReplay));
    }
    // Written whole beside its name and then moved there, so the game never
    // reads half a file.
    tokio::task::spawn_blocking(move || {
        let partial = target.with_extension("emberreplay.part");
        std::fs::write(&partial, &body)?;
        std::fs::rename(&partial, &target)
    })
    .await
    .map_err(|_| Failure::new(Code::Internal))?
    .map_err(|_| Failure::new(Code::Io))?;
    Ok(Some(json!({ "path": path, "digest": digest })))
}

#[cfg(test)]
mod tests {
    use serde_json::{Value, json};

    use super::*;
    use crate::tournament::{Lane, Request};

    const DIGEST: &str = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    #[test]
    fn requests_have_the_shapes_the_game_sends() {
        let list = json!({ "op": "archive_list", "bridge_id": "brg_x",
            "query": { "q": "Player 1", "name": "Player 1", "fighter": 12, "class": 7, "sort": "class", "cursor": "10.ab" } });
        let first = json!({ "op": "archive_list", "bridge_id": "brg_x", "query": {} });
        let fetch = json!({ "op": "archive_fetch", "bridge_id": "brg_x",
            "url": "https://x.example/games/a.emberreplay", "path": "C:/w/a.emberreplay" });
        for (value, op) in [(list, "archive_list"), (first, "archive_list"), (fetch, "archive_fetch")] {
            let parsed: Request = serde_json::from_value(value.clone()).expect("a request");
            assert_eq!(parsed.op(), op);
            assert_eq!(parsed.lane(), Lane::Bridge);
            assert_eq!(serde_json::to_value(&parsed).unwrap(), value);
        }
        let bare: Request =
            serde_json::from_value(json!({ "op": "archive_list", "bridge_id": "brg_x" })).unwrap();
        assert_eq!(bare.op(), "archive_list");
        for bad in [
            json!({ "op": "archive_list" }),
            json!({ "op": "archive_list", "bridge_id": "brg_x", "extra": 1 }),
            json!({ "op": "archive_list", "bridge_id": "brg_x", "query": { "fighter": "Ryu" } }),
            json!({ "op": "archive_fetch", "bridge_id": "brg_x", "url": "u" }),
        ] {
            assert!(serde_json::from_value::<Request>(bad.clone()).is_err(), "{bad}");
        }
    }

    #[test]
    fn a_bridge_that_names_no_archive_is_read_where_the_deployments_list_it() {
        assert_eq!(
            fallback_listing("https://bridge-staging.embernetplay.link"),
            "https://bridge-staging.embernetplay.link/archive/v1/archive"
        );
        assert_eq!(
            fallback_listing("https://bridge.embernetplay.link"),
            "https://archive.embernetplay.link/v1/archive"
        );
        assert_eq!(
            fallback_listing("http://127.0.0.1:47821"),
            "http://127.0.0.1:47821/archive/v1/archive"
        );
    }

    #[test]
    fn a_game_is_fetched_only_from_the_host_its_listing_is_on() {
        let origin = "https://archive.example";
        let at = |address: &str| game_digest(address, origin);
        for good in [
            format!("https://archive.example/replays-live/games/{DIGEST}.emberreplay"),
            format!("https://archive.example/games/{DIGEST}.emberreplay"),
        ] {
            assert_eq!(at(&good).as_deref().ok(), Some(DIGEST), "{good}");
        }
        for bad in [
            format!("https://elsewhere.example/games/{DIGEST}.emberreplay"),
            format!("http://archive.example/games/{DIGEST}.emberreplay"),
            format!("https://archive.example:8443/games/{DIGEST}.emberreplay"),
            format!("https://user@archive.example/games/{DIGEST}.emberreplay"),
            format!("https://archive.example/games/{DIGEST}.emberreplay?x=1"),
            format!("https://archive.example/games/{DIGEST}.emberreplay#x"),
            format!("https://archive.example/sets/{DIGEST}.emberreplay"),
            "https://archive.example/games/short.emberreplay".to_owned(),
            format!("https://archive.example/games/{DIGEST}.usf4replay"),
            format!("https://archive.example.evil.example/games/{DIGEST}.emberreplay"),
            "not an address".to_owned(),
            format!("https://archive.example/{}/games/{DIGEST}.emberreplay", "p".repeat(MAX_URL)),
        ] {
            assert!(at(&bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn an_exact_name_goes_only_to_a_bridge_that_lists_profiles_with_q_beside_it() {
        let query: Query = serde_json::from_value(json!({ "name": "Ann", "q": "Ann" })).unwrap();
        assert!(query.check());
        assert_eq!(
            asked(&query, true).pairs(),
            vec![("name", "Ann".to_owned()), ("q", "Ann".to_owned())]
        );
        assert_eq!(asked(&query, false).pairs(), vec![("q", "Ann".to_owned())]);
        let long: Query = serde_json::from_value(json!({ "name": "n".repeat(33) })).unwrap();
        assert!(!long.check());
    }

    #[test]
    fn a_search_is_escaped_in_the_path() {
        let query: Query = serde_json::from_value(json!({ "q": "A&B =c+d/é #1", "name": "Ann" })).unwrap();
        assert_eq!(
            target(PATH, &query.pairs()).unwrap(),
            "/v1/archive?name=Ann&q=A%26B+%3Dc%2Bd%2F%C3%A9+%231"
        );
        assert_eq!(target(PATH, &[]).unwrap(), "/v1/archive");
    }

    #[test]
    fn a_bridge_without_an_archive_is_told_apart() {
        let missing = |status, body: &[u8]| client::refused(status, body, &REFUSALS);
        assert!(matches!(missing(404, b""), Failure::Local(Code::ReplaysUnsupported)));
        let not_found: Value = json!({ "error": {
            "code": "not_found", "message": "m", "retryable": false, "request_id": "req_1", "details": {},
        }});
        let body = serde_json::to_vec(&not_found).unwrap();
        assert!(matches!(missing(404, &body), Failure::Local(Code::ReplaysUnsupported)));
        assert_eq!(missing(429, b"").as_str(), "http_429");
    }
}
