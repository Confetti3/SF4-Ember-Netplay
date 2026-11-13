//! The public replay archive for the game's own browser
//! (docs/design/REPLAY_ARCHIVE.md, "Listing"): a page of the bridge's
//! `GET /v1/archive`, and one listed game written into a file the game
//! names, which the game then plays.
//!
//! Both are open routes, so no session is opened. The listing is read from the
//! place the approved bridge's archive is served from: under `/archive/` on
//! the bridge's own host, on the bridge's own route, or on the `archive.`
//! sibling of a `bridge.` host. Games are fetched only from the origin of one
//! of those places and only under the archive's own file names, so an answer
//! cannot send the helper anywhere else.
//!
//! What the bridge answers is read tolerantly: members the game does not know
//! are ignored, missing ones take their defaults, and a game that is not
//! well formed is left out rather than failing the page.
use std::{
    collections::{BTreeMap, HashMap},
    sync::{Arc, Mutex, OnceLock},
    time::Duration,
};

use ember_protocol::replays::{self, MAX_REPLAY};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};

use super::{
    Code, Failure, Outcome, Shared, bridges::Approved, check_path,
    client::{self, BridgeCall, Refusals, Verb},
};

/// The most a listing answer is read for: a full page of 400 games with the
/// sets, events and countries that go with it.
const MAX_LISTING: usize = 320 * 1024;
/// How long a listing or a game may take.
const LISTING_TIMEOUT: Duration = Duration::from_secs(15);
const FETCH_TIMEOUT: Duration = Duration::from_secs(30);
/// What a page keeps of an answer: the bridge's own page size and its
/// choices' limits.
const MAX_GAMES: usize = 400;
const MAX_EVENTS: usize = 100;
const MAX_COUNTRIES: usize = 300;
/// The longest text kept of a name, an event or a round.
const MAX_TEXT: usize = 96;
const MAX_URL: usize = 512;
const MAX_QUERY: usize = 128;
const MAX_CURSOR: usize = 200;
/// The rounds the listing filters by.
const ROUNDS: [&str; 5] = ["top8", "pools", "winners", "losers", "grand"];

/// A 404 is a place that does not serve the archive; it is told apart from a
/// bridge that has none by trying the others.
const REFUSALS: Refusals<'static> = Refusals {
    reasons: &[],
    missing: Some(Code::ReplaysUnsupported),
};

/// What the game asks of the listing: a filter, the order (`class`, else
/// newest first) and where to go on from (a listing's `next`).
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Query {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub event: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub fighter: Option<u8>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub country: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub round: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub q: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub class: Option<u8>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sort: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub cursor: Option<String>,
}

/// Text that may go into a query: one line, bounded.
fn plain(text: &str, most: usize) -> bool {
    text.len() <= most && !text.chars().any(char::is_control)
}

impl Query {
    /// Refuses what the bridge would: it is never asked a query the game
    /// could not have made.
    fn check(&self) -> Result<(), Failure> {
        let text = |field: &Option<String>| field.as_deref().is_none_or(|text| plain(text, MAX_QUERY));
        let country = self
            .country
            .as_deref()
            .is_none_or(|code| code.len() == 2 && code.bytes().all(|b| b.is_ascii_uppercase()));
        let round = self.round.as_deref().is_none_or(|round| ROUNDS.contains(&round));
        let class = self.class.is_none_or(|band| (1..=10).contains(&band));
        let sort = self
            .sort
            .as_deref()
            .is_none_or(|sort| sort == "class" || sort == "newest");
        let cursor = self.cursor.as_deref().is_none_or(|cursor| {
            !cursor.is_empty()
                && cursor.len() <= MAX_CURSOR
                && cursor
                    .bytes()
                    .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b) || b == b'.')
        });
        if text(&self.event) && text(&self.q) && country && round && class && sort && cursor {
            Ok(())
        } else {
            Err(Failure::new(Code::InvalidRequest))
        }
    }

    /// The query pairs of the fields it has, in the one order, so two askings
    /// of the same compare equal (and the bridge's cache serves the second).
    fn pairs(&self) -> Vec<(&'static str, String)> {
        let mut pairs = Vec::new();
        if let Some(class) = self.class {
            pairs.push(("class", class.to_string()));
        }
        if let Some(country) = &self.country {
            pairs.push(("country", country.clone()));
        }
        if let Some(cursor) = &self.cursor {
            pairs.push(("cursor", cursor.clone()));
        }
        if let Some(event) = &self.event {
            pairs.push(("event", event.clone()));
        }
        if let Some(fighter) = self.fighter {
            pairs.push(("fighter", fighter.to_string()));
        }
        if let Some(q) = &self.q {
            pairs.push(("q", q.clone()));
        }
        if let Some(round) = &self.round {
            pairs.push(("round", round.clone()));
        }
        // Newest first is the bridge's own order, so it is never asked for.
        if self.sort.as_deref() == Some("class") {
            pairs.push(("sort", "class".to_owned()));
        }
        pairs
    }
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

/// A place the archive may be served from: an origin and the path in front
/// of the archive's routes on it.
#[derive(Clone, Debug, PartialEq, Eq)]
struct Base {
    origin: String,
    prefix: &'static str,
}

/// The places the archive of the bridge at `origin` may be served from, the
/// most likely first: under `/archive/` on the bridge's own host (a bridge
/// that serves its archive beside itself), on the bridge's own route, and,
/// for a host named `bridge.`, the `archive.` host beside it (an archive with
/// a site of its own).
fn bases(origin: &str) -> Vec<Base> {
    let mut places = vec![
        Base {
            origin: origin.to_owned(),
            prefix: "/archive",
        },
        Base {
            origin: origin.to_owned(),
            prefix: "",
        },
    ];
    if let Some(rest) = origin.strip_prefix("https://bridge.") {
        places.push(Base {
            origin: format!("https://archive.{rest}"),
            prefix: "",
        });
    }
    places
}

/// Where each bridge's archive was last heard from, so the next page asks
/// there first.
fn known() -> &'static Mutex<HashMap<String, Base>> {
    static KNOWN: OnceLock<Mutex<HashMap<String, Base>>> = OnceLock::new();
    KNOWN.get_or_init(|| Mutex::new(HashMap::new()))
}

/// `places` with the one `bridge_id` was last heard from first.
fn ordered(bridge_id: &str, places: Vec<Base>) -> Vec<Base> {
    let heard = known()
        .lock()
        .ok()
        .and_then(|known| known.get(bridge_id).cloned());
    let Some(heard) = heard else {
        return places;
    };
    let mut ordered = vec![heard.clone()];
    ordered.extend(places.into_iter().filter(|place| *place != heard));
    ordered
}

/// One game of a listing, as the game reads it. A side with no name, country
/// or class says so with an empty text or 0, and an unknown fighter is -1.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Game {
    pub digest: String,
    pub crc: String,
    pub url: String,
    pub saved_at: u64,
    pub size: u64,
    pub fighters: [i16; 2],
    pub names: [String; 2],
    pub countries: [String; 2],
    pub classes: [u8; 2],
    #[serde(skip_serializing_if = "String::is_empty")]
    pub set: String,
    #[serde(skip_serializing_if = "String::is_empty")]
    pub event: String,
    #[serde(skip_serializing_if = "String::is_empty")]
    pub round: String,
    #[serde(skip_serializing_if = "String::is_empty")]
    pub phase: String,
}

/// A tournament set's score for the listing's games, and its pack's address
/// once the bridge holds one.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize)]
pub struct SetInfo {
    pub score: [u32; 2],
    #[serde(skip_serializing_if = "String::is_empty")]
    pub pack: String,
}

/// One page of the archive's listing.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize)]
pub struct Page {
    pub games: Vec<Game>,
    pub sets: BTreeMap<String, SetInfo>,
    pub events: Vec<String>,
    pub countries: Vec<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub total: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub matching: Option<u64>,
    /// The order the bridge used: `class`, else `newest`.
    pub order: String,
    pub more: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub next: Option<String>,
}

/// `text`, cut to `most` bytes at a character boundary.
fn clip(text: &str, most: usize) -> String {
    if text.len() <= most {
        return text.to_owned();
    }
    let mut end = most;
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    text[..end].to_owned()
}

/// A text member, or empty for one that is missing or not text. One line.
fn text_of(value: &Value, key: &str, most: usize) -> String {
    let text = value.get(key).and_then(Value::as_str).unwrap_or_default();
    clip(&text.replace(|c: char| c.is_control(), " "), most)
}

fn count_of(value: &Value, key: &str) -> Option<u64> {
    value.get(key).and_then(Value::as_u64)
}

/// The two members of an array member, each read by `one`; a missing or
/// shorter array leaves the rest as `none`.
fn pair_of<T: Clone>(value: &Value, key: &str, none: T, one: impl Fn(&Value) -> Option<T>) -> [T; 2] {
    let mut pair = [none.clone(), none];
    if let Some(items) = value.get(key).and_then(Value::as_array) {
        for (slot, item) in pair.iter_mut().zip(items) {
            if let Some(read) = one(item) {
                *slot = read;
            }
        }
    }
    pair
}

fn is_digest(text: &str) -> bool {
    text.len() == 64 && text.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

/// One game, or none when it has no digest or address to fetch it by.
fn decode_game(value: &Value) -> Option<Game> {
    let digest = text_of(value, "digest", 64);
    let url = text_of(value, "url", MAX_URL);
    if !is_digest(&digest) || url.is_empty() {
        return None;
    }
    let name = |item: &Value| item.as_str().map(|name| clip(name, MAX_TEXT).replace(|c: char| c.is_control(), " "));
    Some(Game {
        digest,
        crc: text_of(value, "crc", 8),
        url,
        saved_at: count_of(value, "saved_at").unwrap_or(0),
        size: count_of(value, "size").unwrap_or(0),
        fighters: pair_of(value, "fighters", -1, |item| {
            item.as_u64().and_then(|id| i16::try_from(id).ok())
        }),
        names: pair_of(value, "names", String::new(), name),
        countries: pair_of(value, "countries", String::new(), |item| {
            item.as_str()
                .filter(|code| code.len() == 2)
                .map(str::to_owned)
        }),
        classes: pair_of(value, "classes", 0, |item| {
            item.as_u64().and_then(|band| u8::try_from(band).ok())
        }),
        set: text_of(value, "set", MAX_TEXT),
        event: text_of(value, "event", MAX_TEXT),
        round: text_of(value, "round", MAX_TEXT),
        phase: text_of(value, "phase", MAX_TEXT),
    })
}

/// The list of texts under `key`, at most `most` of them, each bounded.
fn texts_of(value: &Value, key: &str, most: usize) -> Vec<String> {
    value
        .get(key)
        .and_then(Value::as_array)
        .map(|items| {
            items
                .iter()
                .filter_map(Value::as_str)
                .take(most)
                .map(|text| clip(text, MAX_TEXT))
                .collect()
        })
        .unwrap_or_default()
}

/// The page in the bridge's answer, or none for an answer with no games
/// array at all (not a listing).
fn decode_page(value: &Value) -> Option<Page> {
    let games = value.get("games")?.as_array()?;
    let games: Vec<Game> = games.iter().filter_map(decode_game).take(MAX_GAMES).collect();
    let mut sets = BTreeMap::new();
    if let Some(listed) = value.get("sets").and_then(Value::as_object) {
        for (id, set) in listed.iter().take(MAX_GAMES) {
            let score = pair_of(set, "score", 0, |item| {
                item.as_u64().and_then(|wins| u32::try_from(wins).ok())
            });
            sets.insert(
                clip(id, MAX_TEXT),
                SetInfo {
                    score,
                    pack: text_of(set, "pack", MAX_URL),
                },
            );
        }
    }
    // A cursor is kept only in the shape the bridge hands out.
    let next = value
        .get("next")
        .and_then(Value::as_str)
        .filter(|next| {
            !next.is_empty()
                && next.len() <= MAX_CURSOR
                && next
                    .bytes()
                    .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b) || b == b'.')
        })
        .map(str::to_owned);
    let order = if value.get("sort").and_then(Value::as_str) == Some("class") {
        "class"
    } else {
        "newest"
    };
    Some(Page {
        games,
        sets,
        events: texts_of(value, "events", MAX_EVENTS),
        countries: texts_of(value, "countries", MAX_COUNTRIES)
            .into_iter()
            .filter(|code| code.len() == 2)
            .collect(),
        total: count_of(value, "total"),
        matching: count_of(value, "matching"),
        order: order.to_owned(),
        more: next.is_some(),
        next,
    })
}

/// A page of the archive on `bridge_id`.
pub async fn list(shared: &Arc<Shared>, bridge_id: &str, query: &Query) -> Outcome {
    query.check()?;
    let bridge = client::approved(shared, bridge_id)?;
    let pairs = query.pairs();
    let mut failed = None;
    for place in ordered(bridge_id, bases(&bridge.origin)) {
        let path = target(&format!("{}/v1/archive", place.prefix), &pairs)?;
        let call = BridgeCall::new(Verb::Get, &path, &[200])
            .refusals(REFUSALS)
            .limit(MAX_LISTING)
            .timeout(LISTING_TIMEOUT);
        let host = Approved {
            origin: place.origin.clone(),
            ..bridge.clone()
        };
        match client::send(shared, &host, None, &call).await {
            Ok((200, body)) => {
                let value: Value = serde_json::from_slice(&body)
                    .map_err(|_| Failure::new(Code::BridgeInvalidResponse))?;
                let page = decode_page(&value)
                    .ok_or_else(|| Failure::new(Code::BridgeInvalidResponse))?;
                if let Ok(mut known) = known().lock() {
                    known.insert(bridge_id.to_owned(), place);
                }
                return serde_json::to_value(&page)
                    .map(Some)
                    .map_err(|_| Failure::new(Code::Internal));
            }
            Ok((status, body)) => {
                let failure = client::refused(status, &body, &REFUSALS);
                // Not served here: the next place may serve it. Any other
                // refusal is about the request, whoever served it.
                if !matches!(failure, Failure::Local(Code::ReplaysUnsupported)) {
                    return Err(failure);
                }
            }
            Err(failure) => failed = Some(failure),
        }
    }
    Err(failed.unwrap_or_else(|| Failure::new(Code::ReplaysUnsupported)))
}

/// The digest `url` names, when it is a game of the archive served from one
/// of the places of the bridge at `origin`: a plain address with no
/// credentials, query or fragment, on one of their origins, whose file is
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
    {
        return Err(invalid());
    }
    let served_from = parsed.origin().ascii_serialization();
    let allowed = bases(origin).iter().any(|place| {
        reqwest::Url::parse(&place.origin)
            .is_ok_and(|base| base.origin().ascii_serialization() == served_from)
    });
    if !allowed || ember_protocol::encoding::check_origin(&served_from, client::POLICY).is_err() {
        return Err(invalid());
    }
    let file = parsed
        .path()
        .rsplit_once("/games/")
        .and_then(|(_, file)| file.strip_suffix(".emberreplay"))
        .ok_or_else(invalid)?;
    if is_digest(file) {
        Ok(file.to_owned())
    } else {
        Err(invalid())
    }
}

/// Writes the game at `url`, listed by the archive of `bridge_id`, to `path`
/// (a `.emberreplay`), once it is whole and is the game its address names.
pub async fn fetch(shared: &Arc<Shared>, bridge_id: &str, url: &str, path: &str) -> Outcome {
    let target = check_path(path)?.to_owned();
    if !path.ends_with(".emberreplay") {
        return Err(Failure::new(Code::InvalidPath));
    }
    let bridge = client::approved(shared, bridge_id)?;
    let digest = game_digest(url, &bridge.origin)?;
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

    fn game(extra: Value) -> Value {
        let mut game = json!({
            "digest": DIGEST, "crc": "0badf00d",
            "url": format!("https://bridge.example/archive/replays/games/{DIGEST}.emberreplay"),
            "saved_at": 1_700_000_000u64, "size": 2048, "fighters": [0, 1],
            "names": ["Player 1", "Player 2"], "countries": ["GB", null], "classes": [7, null],
            "set": null, "event": null, "round": null, "phase": null,
        });
        for (key, value) in extra.as_object().cloned().unwrap_or_default() {
            game[key] = value;
        }
        game
    }

    fn query(value: Value) -> Query {
        serde_json::from_value(value).expect("a query")
    }

    #[test]
    fn requests_have_the_shapes_the_game_sends() {
        let list = json!({ "op": "archive_list", "bridge_id": "brg_x",
            "query": { "q": "Player 1", "fighter": 12, "class": 7, "sort": "class", "cursor": "10.ab" } });
        let first = json!({ "op": "archive_list", "bridge_id": "brg_x", "query": {} });
        let fetch = json!({ "op": "archive_fetch", "bridge_id": "brg_x",
            "url": "https://x.example/games/a.emberreplay", "path": "C:/w/a.emberreplay" });
        for (value, op) in [(list, "archive_list"), (first, "archive_list"), (fetch, "archive_fetch")] {
            let parsed: Request = serde_json::from_value(value.clone()).expect("a request");
            assert_eq!(parsed.op(), op);
            assert_eq!(parsed.lane(), Lane::Bridge);
            assert_eq!(serde_json::to_value(&parsed).unwrap(), value);
        }
        // The query may be left out; it then asks for the first page of everything.
        let bare: Request =
            serde_json::from_value(json!({ "op": "archive_list", "bridge_id": "brg_x" })).unwrap();
        assert_eq!(bare.op(), "archive_list");
        for bad in [
            json!({ "op": "archive_list" }),
            json!({ "op": "archive_list", "bridge_id": "brg_x", "extra": 1 }),
            json!({ "op": "archive_list", "bridge_id": "brg_x", "query": { "limit": 5 } }),
            json!({ "op": "archive_list", "bridge_id": "brg_x", "query": { "fighter": "Ryu" } }),
            json!({ "op": "archive_fetch", "bridge_id": "brg_x", "url": "u" }),
        ] {
            assert!(
                serde_json::from_value::<Request>(bad.clone()).is_err(),
                "{bad}"
            );
        }
    }

    #[test]
    fn each_filter_is_a_query_pair_in_one_order() {
        assert!(Query::default().pairs().is_empty());
        let everything = query(json!({ "event": "Player 1's Cup", "fighter": 3, "country": "NL",
            "round": "top8", "q": "Ann", "class": 9, "sort": "class", "cursor": "2.1700000000.ab" }));
        let pairs = everything.pairs();
        let keys: Vec<_> = pairs.iter().map(|(key, _)| *key).collect();
        assert_eq!(
            keys,
            ["class", "country", "cursor", "event", "fighter", "q", "round", "sort"]
        );
        for (one, key, value) in [
            (json!({ "event": "online" }), "event", "online"),
            (json!({ "fighter": 12 }), "fighter", "12"),
            (json!({ "country": "JP" }), "country", "JP"),
            (json!({ "round": "grand" }), "round", "grand"),
            (json!({ "q": "Ann" }), "q", "Ann"),
            (json!({ "class": 10 }), "class", "10"),
            (json!({ "sort": "class" }), "sort", "class"),
            (json!({ "cursor": "1.ab" }), "cursor", "1.ab"),
        ] {
            assert_eq!(query(one).pairs(), vec![(key, value.to_owned())]);
        }
        // Newest first is the bridge's own order and is never asked for.
        assert!(query(json!({ "sort": "newest" })).pairs().is_empty());
    }

    #[test]
    fn a_search_is_escaped_in_the_path() {
        let pairs = query(json!({ "q": "A&B =c+d/é #1", "event": "Cup 100%" })).pairs();
        assert_eq!(
            target("/archive/v1/archive", &pairs).unwrap(),
            "/archive/v1/archive?event=Cup+100%25&q=A%26B+%3Dc%2Bd%2F%C3%A9+%231"
        );
        assert_eq!(target("/v1/archive", &[]).unwrap(), "/v1/archive");
    }

    #[test]
    fn a_query_the_bridge_would_refuse_is_refused_first() {
        for bad in [
            json!({ "class": 0 }),
            json!({ "class": 11 }),
            json!({ "round": "semis" }),
            json!({ "country": "nl" }),
            json!({ "country": "NLD" }),
            json!({ "sort": "oldest" }),
            json!({ "cursor": "" }),
            json!({ "cursor": "zz.1" }),
            json!({ "q": "a\nb" }),
            json!({ "q": "x".repeat(MAX_QUERY + 1) }),
        ] {
            assert!(query(bad.clone()).check().is_err(), "{bad}");
        }
        for good in [
            json!({}),
            json!({ "class": 1, "round": "pools", "country": "BE", "sort": "newest" }),
            json!({ "cursor": "1700000000.0123abcd", "q": "Player 2" }),
        ] {
            assert!(query(good.clone()).check().is_ok(), "{good}");
        }
    }

    #[test]
    fn a_full_page_is_read_whole() {
        let page = decode_page(&json!({
            "games": [
                game(json!({ "set": "emt_a", "event": "Player 1's Cup", "round": "Top 8", "phase": "Winners" })),
                game(json!({ "digest": "f".repeat(64), "names": [null, "Player 3"], "fighters": [5, 9],
                    "classes": [null, 10], "countries": [null, "BE"] })),
            ],
            "sets": { "emt_a": { "score": [2, 1], "pack": "https://bridge.example/archive/replays/sets/emt_a.emberpack" } },
            "events": ["Player 1's Cup"], "countries": ["BE", "GB"],
            "total": 120, "matching": 2, "sort": "class", "more": true, "next": "9.1700000000.ab",
        }))
        .expect("a page");
        assert_eq!(page.games.len(), 2);
        let first = &page.games[0];
        assert_eq!(first.names, ["Player 1", "Player 2"]);
        assert_eq!((first.fighters, first.classes), ([0, 1], [7, 0]));
        assert_eq!(first.countries, ["GB", ""]);
        assert_eq!((first.set.as_str(), first.round.as_str(), first.phase.as_str()), ("emt_a", "Top 8", "Winners"));
        let second = &page.games[1];
        assert_eq!((second.names[0].as_str(), second.countries[1].as_str()), ("", "BE"));
        assert_eq!((second.fighters, second.classes), ([5, 9], [0, 10]));
        assert_eq!(page.sets["emt_a"].score, [2, 1]);
        assert!(page.sets["emt_a"].pack.ends_with("emt_a.emberpack"));
        assert_eq!(page.events, ["Player 1's Cup"]);
        assert_eq!(page.countries, ["BE", "GB"]);
        assert_eq!((page.total, page.matching), (Some(120), Some(2)));
        assert_eq!(page.order, "class");
        assert!(page.more);
        assert_eq!(page.next.as_deref(), Some("9.1700000000.ab"));
    }

    #[test]
    fn a_minimal_page_takes_its_defaults() {
        let page = decode_page(&json!({ "games": [] })).expect("a page");
        assert_eq!(page, Page { order: "newest".into(), ..Page::default() });
        // Only a digest and an address are needed of a game.
        let page = decode_page(&json!({ "games": [{ "digest": DIGEST, "url": "https://x.example/g" }] }))
            .expect("a page");
        let only = &page.games[0];
        assert_eq!((only.fighters, only.classes, only.saved_at, only.size), ([-1, -1], [0, 0], 0, 0));
        assert_eq!(only.names, ["", ""]);
        assert!(!page.more && page.next.is_none() && page.total.is_none() && page.matching.is_none());
        // Not a listing at all.
        assert!(decode_page(&json!({ "rooms": [] })).is_none());
        assert!(decode_page(&json!({ "games": "none" })).is_none());
        assert!(decode_page(&json!([])).is_none());
    }

    #[test]
    fn members_the_game_does_not_know_are_ignored() {
        let mut odd = game(json!({ "replay_of_the_year": true, "tags": ["a"] }));
        odd["classes"] = json!([3, 4, 5]);
        let page = decode_page(&json!({ "games": [odd], "banner": "hello", "total": null,
            "sets": { "emt_a": { "score": [1, 0], "badge": 1 } }, "sort": "sideways", "next": "not a cursor" }))
        .expect("a page");
        assert_eq!(page.games[0].classes, [3, 4]);
        assert_eq!(page.order, "newest");
        assert!(page.total.is_none());
        assert_eq!(page.sets["emt_a"].pack, "");
        // A cursor the bridge would not hand out is no way to go on.
        assert!(!page.more && page.next.is_none());
    }

    #[test]
    fn a_game_that_is_not_well_formed_is_left_out_not_fatal() {
        let page = decode_page(&json!({ "games": [
            { "digest": "short", "url": "https://x.example/g" },
            { "url": "https://x.example/g" },
            { "digest": DIGEST },
            "game",
            game(json!({ "fighters": "none", "names": 5, "saved_at": -4, "size": "big" })),
        ] }))
        .expect("a page");
        assert_eq!(page.games.len(), 1);
        let kept = &page.games[0];
        assert_eq!((kept.fighters, kept.names.clone(), kept.saved_at, kept.size), ([-1, -1], ["".to_owned(), "".to_owned()], 0, 0));
    }

    #[test]
    fn a_page_is_bounded_and_its_text_is_one_line() {
        let long = "n".repeat(500);
        let many: Vec<Value> = (0..MAX_GAMES + 20)
            .map(|n| game(json!({ "digest": format!("{n:064x}"), "names": [long, "A\nB"] })))
            .collect();
        let page = decode_page(&json!({ "games": many })).expect("a page");
        assert_eq!(page.games.len(), MAX_GAMES);
        assert_eq!(page.games[0].names[0].len(), MAX_TEXT);
        assert_eq!(page.games[0].names[1], "A B");
        // A cut never splits a character.
        assert_eq!(clip("ééé", 3), "é");
    }

    #[test]
    fn the_archive_is_looked_for_where_a_bridge_may_serve_it() {
        let place = |origin: &str, prefix| Base { origin: origin.to_owned(), prefix };
        assert_eq!(
            bases("https://bridge-staging.example"),
            vec![
                place("https://bridge-staging.example", "/archive"),
                place("https://bridge-staging.example", ""),
            ]
        );
        assert_eq!(
            bases("https://bridge.example"),
            vec![
                place("https://bridge.example", "/archive"),
                place("https://bridge.example", ""),
                place("https://archive.example", ""),
            ]
        );
        assert_eq!(bases("http://127.0.0.1:47821").len(), 2);
        // The place last heard from goes first, once.
        known().lock().unwrap().insert("brg_heard".into(), place("https://archive.example", ""));
        let order = ordered("brg_heard", bases("https://bridge.example"));
        assert_eq!(order.len(), 3);
        assert_eq!(order[0], place("https://archive.example", ""));
        assert_eq!(ordered("brg_new", bases("https://bridge.example")), bases("https://bridge.example"));
    }

    #[test]
    fn a_game_is_fetched_only_from_the_archive_of_its_bridge() {
        let origin = "https://bridge.example";
        let at = |address: &str| game_digest(address, origin);
        for good in [
            format!("https://bridge.example/archive/replays/games/{DIGEST}.emberreplay"),
            format!("https://archive.example/replays-live/games/{DIGEST}.emberreplay"),
            format!("https://bridge.example/replays/games/{DIGEST}.emberreplay"),
        ] {
            assert_eq!(at(&good).as_deref().ok(), Some(DIGEST), "{good}");
        }
        for bad in [
            format!("https://elsewhere.example/archive/replays/games/{DIGEST}.emberreplay"),
            format!("http://bridge.example/archive/replays/games/{DIGEST}.emberreplay"),
            format!("https://bridge.example:8443/archive/replays/games/{DIGEST}.emberreplay"),
            format!("https://user@bridge.example/archive/replays/games/{DIGEST}.emberreplay"),
            format!("https://bridge.example/archive/replays/games/{DIGEST}.emberreplay?x=1"),
            format!("https://bridge.example/archive/replays/games/{DIGEST}.emberreplay#x"),
            format!("https://bridge.example/archive/replays/sets/{DIGEST}.emberreplay"),
            "https://bridge.example/archive/replays/games/short.emberreplay".to_owned(),
            format!("https://bridge.example/archive/replays/games/{DIGEST}.usf4replay"),
            format!("https://bridge.example.evil.example/replays/games/{DIGEST}.emberreplay"),
            "not an address".to_owned(),
            format!("https://bridge.example/{}/games/{DIGEST}.emberreplay", "p".repeat(MAX_URL)),
        ] {
            assert!(at(&bad).is_err(), "{bad}");
        }
        // A bridge on loopback may serve over http, and nothing else may.
        let local = "http://127.0.0.1:47821";
        assert!(game_digest(&format!("http://127.0.0.1:47821/archive/replays/games/{DIGEST}.emberreplay"), local).is_ok());
        assert!(game_digest(&format!("http://bridge.example/archive/replays/games/{DIGEST}.emberreplay"), "http://bridge.example").is_err());
    }

    #[test]
    fn a_bridge_without_an_archive_is_told_apart() {
        let missing = |status, body: &[u8]| client::refused(status, body, &REFUSALS);
        assert!(matches!(missing(404, b""), Failure::Local(Code::ReplaysUnsupported)));
        let not_found = serde_json::to_vec(&json!({ "error": {
            "code": "not_found", "message": "m", "retryable": false, "request_id": "req_1", "details": {},
        }}))
        .unwrap();
        assert!(matches!(missing(404, &not_found), Failure::Local(Code::ReplaysUnsupported)));
        assert_eq!(missing(429, b"").as_str(), "http_429");
        assert_eq!(missing(503, b"").as_str(), "http_503");
        let bad_cursor = serde_json::to_vec(&json!({ "error": {
            "code": "invalid_request", "message": "m", "retryable": false, "request_id": "req_1", "details": {},
        }}))
        .unwrap();
        assert_eq!(missing(400, &bad_cursor).as_str(), "invalid_request");
    }
}
