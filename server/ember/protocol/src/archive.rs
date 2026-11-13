//! The public replay archive's listing (docs/design/REPLAY_ARCHIVE.md,
//! "Listing"): the query `GET /v1/archive` takes and the page it answers. The
//! bridge serializes a `Page`, the archive site reads it as JSON, and the
//! game's helper reads it back with these types. A request is what the bridge
//! checks; an answer is read tolerantly: members this build does not know are
//! ignored, a missing optional member takes its default, and a game that is
//! not whole is dropped rather than failing the page (`Page::deserialize`).
use std::collections::BTreeMap;

use serde::{Deserialize, Deserializer, Serialize};

/// The listing's route on the host that serves the archive.
pub const PATH: &str = "/v1/archive";
/// The most games a page holds.
pub const MAX_GAMES: usize = 400;
/// The most events and countries a page offers to filter by.
pub const MAX_EVENTS: usize = 100;
pub const MAX_COUNTRIES: usize = 300;
/// The longest address a game is downloaded from, and the longest cursor.
pub const MAX_URL: usize = 512;
pub const MAX_CURSOR: usize = 200;
/// The longest text a listing's query holds (a search, an event's title).
pub const MAX_QUERY: usize = 128;
/// The rounds a listing filters by.
pub const ROUNDS: [&str; 5] = ["top8", "pools", "winners", "losers", "grand"];

/// Whether `text` is a cursor as the listing hands them out: `.`-joined
/// numbers and a digest in lower-case hex.
pub fn is_cursor(text: &str) -> bool {
    !text.is_empty()
        && text.len() <= MAX_CURSOR
        && text
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b) || b == b'.')
}

fn is_digest(text: &str) -> bool {
    text.len() == 64 && text.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

/// What `GET /v1/archive` is asked: a filter, the order (`sort=class`, else
/// newest first) and where to go on from (a page's `next`).
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Query {
    /// An event's title, or `online` for games of no tournament set.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub event: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub fighter: Option<u8>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub country: Option<String>,
    /// One of `ROUNDS`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub round: Option<String>,
    /// Part of a player's name or an event's title.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub q: Option<String>,
    /// A side's exact name, which a player's profile lists their sets by.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
    /// A class (band 1 to 10) either side has.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub class: Option<u8>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sort: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub cursor: Option<String>,
}

fn plain(text: &str) -> bool {
    text.len() <= MAX_QUERY && !text.chars().any(char::is_control)
}

impl Query {
    /// Whether the bridge would take this query: the helper asks nothing the
    /// game could not have made.
    pub fn check(&self) -> bool {
        let text = |field: &Option<String>| field.as_deref().is_none_or(plain);
        text(&self.event)
            && text(&self.q)
            && self.name.as_deref().is_none_or(|name| !name.is_empty() && plain(name))
            && self
                .country
                .as_deref()
                .is_none_or(|code| code.len() == 2 && code.bytes().all(|b| b.is_ascii_uppercase()))
            && self.round.as_deref().is_none_or(|round| ROUNDS.contains(&round))
            && self.class.is_none_or(|band| (1..=10).contains(&band))
            && self.sort.as_deref().is_none_or(|sort| sort == "class" || sort == "newest")
            && self.cursor.as_deref().is_none_or(is_cursor)
    }

    /// The query pairs of the fields it has, in one order (so two askings of
    /// the same are one request to the bridge's cache). Newest first is the
    /// bridge's own order and is never asked for.
    pub fn pairs(&self) -> Vec<(&'static str, String)> {
        let sort = (self.sort.as_deref() == Some("class")).then(|| "class".to_owned());
        [
            ("class", self.class.map(|band| band.to_string())),
            ("country", self.country.clone()),
            ("cursor", self.cursor.clone()),
            ("event", self.event.clone()),
            ("fighter", self.fighter.map(|fighter| fighter.to_string())),
            ("name", self.name.clone()),
            ("q", self.q.clone()),
            ("round", self.round.clone()),
            ("sort", sort),
        ]
        .into_iter()
        .filter_map(|(key, value)| Some((key, value?)))
        .collect()
    }
}

/// One listed game.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Game {
    /// The SHA-256 of the replay's body, which the file is kept under.
    pub digest: String,
    #[serde(default)]
    pub crc: String,
    /// Where the file is downloaded from.
    pub url: String,
    #[serde(default)]
    pub saved_at: i64,
    #[serde(default)]
    pub size: u64,
    pub fighters: [u8; 2],
    #[serde(default)]
    pub names: [Option<String>; 2],
    #[serde(default)]
    pub countries: [Option<String>; 2],
    /// The tournament set it was a game of, and that set's event, round and phase.
    #[serde(default)]
    pub set: Option<String>,
    #[serde(default)]
    pub event: Option<String>,
    #[serde(default)]
    pub round: Option<String>,
    #[serde(default)]
    pub phase: Option<String>,
    /// Each side's class (band 1 to 10), none while the bridge hides classes.
    #[serde(default)]
    pub classes: [Option<u8>; 2],
}

impl Game {
    /// Whether the game can be fetched: a digest and an address.
    pub fn is_whole(&self) -> bool {
        is_digest(&self.digest) && !self.url.is_empty() && self.url.len() <= MAX_URL
    }
}

/// A tournament set the page's games name: its score, and its pack's address
/// once the bucket holds one.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Set {
    #[serde(default)]
    pub score: [u32; 2],
    #[serde(default)]
    pub pack: Option<String>,
}

/// One page of the listing.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Page {
    #[serde(default, deserialize_with = "whole_games")]
    pub games: Vec<Game>,
    #[serde(default)]
    pub sets: BTreeMap<String, Set>,
    /// What the filters can choose from.
    #[serde(default)]
    pub events: Vec<String>,
    #[serde(default)]
    pub countries: Vec<String>,
    /// Every game the archive holds, and the games the filter keeps (none
    /// when counting them would read too many).
    #[serde(default)]
    pub total: Option<i64>,
    #[serde(default)]
    pub matching: Option<i64>,
    /// The order used: `class`, else `newest`.
    #[serde(default)]
    pub sort: String,
    #[serde(default)]
    pub more: bool,
    #[serde(default)]
    pub next: Option<String>,
}

/// The games that are whole, at most a page's worth; one that is not is
/// dropped and the page goes on.
fn whole_games<'de, D: Deserializer<'de>>(deserializer: D) -> Result<Vec<Game>, D::Error> {
    let games = Vec::<serde_json::Value>::deserialize(deserializer)?;
    Ok(games
        .into_iter()
        .filter_map(|game| serde_json::from_value::<Game>(game).ok())
        .filter(Game::is_whole)
        .take(MAX_GAMES)
        .collect())
}

impl Page {
    /// The page held to the listing's limits: a cursor the bridge would not
    /// hand out is no way to go on, and the choices are bounded.
    pub fn bounded(mut self) -> Self {
        self.events.truncate(MAX_EVENTS);
        self.countries.retain(|code| code.len() == 2);
        self.countries.truncate(MAX_COUNTRIES);
        self.next = self.next.filter(|next| is_cursor(next));
        self.more = self.next.is_some();
        if self.sort != "class" {
            self.sort = "newest".to_owned();
        }
        self
    }
}

#[cfg(test)]
mod tests {
    use serde_json::{Value, json};

    use super::*;

    const DIGEST: &str = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    fn game() -> Game {
        Game {
            digest: DIGEST.into(),
            crc: "0badf00d".into(),
            url: format!("https://archive.example/replays/games/{DIGEST}.emberreplay"),
            saved_at: 1_700_000_000,
            size: 2048,
            fighters: [0, 17],
            names: [Some("Player 1".into()), None],
            countries: [Some("GB".into()), None],
            set: Some("emt_a".into()),
            event: Some("Player 1's Cup".into()),
            round: Some("Top 8".into()),
            phase: None,
            classes: [Some(7), None],
        }
    }

    /// The listing's JSON as the bridge wrote it before it had these types
    /// (the archive site reads this shape): the page serializes to it, member
    /// for member, nulls included.
    #[test]
    fn a_page_serializes_to_the_shape_the_site_reads() {
        let page = Page {
            games: vec![game()],
            sets: BTreeMap::from([(
                "emt_a".to_owned(),
                Set { score: [2, 1], pack: None },
            )]),
            events: vec!["Player 1's Cup".into()],
            countries: vec!["BE".into(), "GB".into()],
            total: Some(120),
            matching: None,
            sort: "class".into(),
            more: true,
            next: Some("9.1700000000.ab".into()),
        };
        assert_eq!(
            serde_json::to_value(&page).unwrap(),
            json!({
                "games": [{
                    "digest": DIGEST, "crc": "0badf00d",
                    "url": format!("https://archive.example/replays/games/{DIGEST}.emberreplay"),
                    "saved_at": 1_700_000_000i64, "size": 2048, "fighters": [0, 17],
                    "names": ["Player 1", null], "countries": ["GB", null],
                    "set": "emt_a", "event": "Player 1's Cup", "round": "Top 8", "phase": null,
                    "classes": [7, null],
                }],
                "sets": { "emt_a": { "score": [2, 1], "pack": null } },
                "events": ["Player 1's Cup"], "countries": ["BE", "GB"],
                "total": 120, "matching": null,
                "sort": "class", "more": true, "next": "9.1700000000.ab",
            })
        );
        let empty = serde_json::to_value(Page { sort: "newest".into(), ..Page::default() }).unwrap();
        assert_eq!(
            empty,
            json!({ "games": [], "sets": {}, "events": [], "countries": [], "total": null,
                "matching": null, "sort": "newest", "more": false, "next": null })
        );
    }

    #[test]
    fn a_page_reads_back_what_it_wrote_and_what_older_pages_lacked() {
        let page = Page { games: vec![game()], sort: "newest".into(), ..Page::default() };
        let back: Page = serde_json::from_value(serde_json::to_value(&page).unwrap()).unwrap();
        assert_eq!(back, page);
        let minimal: Page = serde_json::from_value(json!({})).unwrap();
        assert_eq!(minimal, Page::default());
        let least: Page = serde_json::from_value(json!({ "games": [
            { "digest": DIGEST, "url": "https://x.example/g", "fighters": [1, 2] }
        ] }))
        .unwrap();
        assert_eq!(least.games[0].names, [None, None]);
        assert_eq!((least.games[0].saved_at, least.games[0].size), (0, 0));
    }

    #[test]
    fn unknown_members_are_ignored_and_a_game_that_is_not_whole_is_dropped() {
        let mut odd = serde_json::to_value(Page { games: vec![game()], ..Page::default() }).unwrap();
        odd["banner"] = json!("hello");
        odd["games"][0]["tags"] = json!(["a"]);
        let broken = [
            json!({ "digest": "short", "url": "u", "fighters": [0, 1] }),
            json!({ "url": "u", "fighters": [0, 1] }),
            json!({ "digest": DIGEST, "fighters": [0, 1] }),
            json!({ "digest": DIGEST, "url": "u", "fighters": "none" }),
            json!({ "digest": DIGEST, "url": "u", "fighters": [0, 300] }),
            json!("game"),
        ];
        odd["games"].as_array_mut().unwrap().extend(broken);
        let page: Page = serde_json::from_value(odd).unwrap();
        assert_eq!(page.games, vec![game()]);
        let many: Vec<Value> = (0..MAX_GAMES + 20)
            .map(|n| json!({ "digest": format!("{n:064x}"), "url": "u", "fighters": [0, 1] }))
            .collect();
        let page: Page = serde_json::from_value(json!({ "games": many })).unwrap();
        assert_eq!(page.games.len(), MAX_GAMES);
    }

    #[test]
    fn a_page_is_held_to_the_limits() {
        let page: Page = serde_json::from_value(json!({
            "countries": ["BE", "nld", "GB"], "sort": "sideways", "more": true, "next": "not a cursor",
        }))
        .unwrap();
        let page = page.bounded();
        assert_eq!(page.countries, ["BE", "GB"]);
        assert_eq!((page.sort.as_str(), page.more, page.next), ("newest", false, None));
        let page = Page { next: Some("9.ab".into()), sort: "class".into(), ..Page::default() }.bounded();
        assert_eq!((page.sort.as_str(), page.more), ("class", true));
    }

    fn query(value: Value) -> Query {
        serde_json::from_value(value).expect("a query")
    }

    #[test]
    fn each_filter_is_a_query_pair_in_one_order() {
        assert!(Query::default().pairs().is_empty());
        let all = query(json!({ "event": "Cup", "fighter": 3, "country": "NL", "round": "top8",
            "q": "Ann", "name": "Ann", "class": 9, "sort": "class", "cursor": "2.ab" }));
        let keys: Vec<_> = all.pairs().into_iter().map(|(key, _)| key).collect();
        assert_eq!(keys, ["class", "country", "cursor", "event", "fighter", "name", "q", "round", "sort"]);
        assert!(query(json!({ "sort": "newest" })).pairs().is_empty());
        assert_eq!(query(json!({ "name": "Ann" })).pairs(), vec![("name", "Ann".to_owned())]);
    }

    #[test]
    fn a_query_the_bridge_would_refuse_is_refused_first() {
        for bad in [
            json!({ "class": 0 }),
            json!({ "class": 11 }),
            json!({ "round": "semis" }),
            json!({ "country": "nl" }),
            json!({ "sort": "oldest" }),
            json!({ "cursor": "" }),
            json!({ "cursor": "zz.1" }),
            json!({ "q": "a\nb" }),
            json!({ "name": "" }),
            json!({ "q": "x".repeat(MAX_QUERY + 1) }),
        ] {
            assert!(!query(bad.clone()).check(), "{bad}");
        }
        assert!(query(json!({ "class": 1, "round": "pools", "country": "BE", "name": "Player 2" })).check());
        assert!(query(json!({ "cursor": "1700000000.0123abcd" })).check());
    }
}
