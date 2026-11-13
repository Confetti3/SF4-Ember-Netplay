use std::collections::{HashMap, HashSet};

use rusqlite::{Transaction, params};

use super::*;
use crate::{Db, archive::classes};

async fn archive() -> Db {
    Db::open(std::path::Path::new(":memory:")).unwrap()
}

/// That everything migration 055 derives agrees with the games: each stored
/// game's keys are the view's, the counts are the keys', the kept total is
/// every row's, and the sides' players are the view's.
pub(crate) fn assert_index(tx: &Transaction<'_>) {
    let none = |query: &str| {
        let found: i64 = tx
            .query_row(&format!("SELECT COUNT(*) FROM ({query})"), [], |row| {
                row.get(0)
            })
            .unwrap();
        assert_eq!(found, 0, "{query}");
    };
    none(
        "SELECT key, saved_at, digest FROM archive_keys EXCEPT SELECT key, saved_at, unhex(digest) FROM archive_game_keys",
    );
    none(
        "SELECT key, saved_at, unhex(digest) FROM archive_game_keys EXCEPT SELECT key, saved_at, digest FROM archive_keys",
    );
    none(
        "SELECT key, games FROM archive_key_counts EXCEPT SELECT key, COUNT(*) FROM archive_keys GROUP BY key",
    );
    none(
        "SELECT key, COUNT(*) FROM archive_keys GROUP BY key EXCEPT SELECT key, games FROM archive_key_counts",
    );
    none(
        "SELECT g.digest FROM archive_games g JOIN archive_side_players s ON s.digest = g.digest
         WHERE g.player_a IS NOT s.player_a OR g.player_b IS NOT s.player_b",
    );
    none(
        "SELECT 1 FROM archive_kept WHERE games != (SELECT COUNT(*) FROM archive_games)
            OR bytes != (SELECT COALESCE(SUM(size), 0) FROM archive_games)",
    );
}

/// A small deterministic generator, so a failure replays.
struct Dice(u64);

impl Dice {
    fn roll(&mut self, sides: u64) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0 % sides
    }

    fn pick<'a, T>(&mut self, of: &'a [T]) -> &'a T {
        &of[self.roll(of.len() as u64) as usize]
    }

    fn maybe<T: Clone>(&mut self, chance: u64, of: &[T]) -> Option<T> {
        (self.roll(100) < chance).then(|| self.pick(of).clone())
    }
}

const NAMES: &[&str] = &[
    "Ann", "anna", "Bob", "Bobby", "Cid", "Dee_J", "Eve", "evelyn", "Fox", "Gil",
];
const COUNTRIES: &[&str] = &["JP", "US", "FR", "BR"];
const EVENTS: &[&str] = &["Spring Cup", "Annual Bash", "Fox Open"];
const ROUNDS: &[&str] = &[
    "Winners Round 1",
    "Losers Final",
    "Grand Final",
    "Winners Final",
];
const PHASES: &[&str] = &["Top 8", "Pools A", "top 16"];
const PLAYERS: &[&str] = &[
    "emb_p1", "emb_p2", "emb_p3", "emb_p4", "emb_p5", "emb_p6", "emb_p7", "emb_p8",
];

/// A board: some players with a class, some without.
fn any_board(dice: &mut Dice) -> HashMap<String, u8> {
    PLAYERS
        .iter()
        .filter_map(|player| {
            dice.maybe(70, &[1u8, 3, 5, 7, 7, 8, 9, 10])
                .map(|band| (player.to_string(), band))
        })
        .collect()
}

/// Gives every game its players' classes on `board`, a few passes at a time.
fn settle(tx: &Transaction<'_>, board: &HashMap<String, u8>) {
    classes::record(tx, board).unwrap();
    for _ in 0..1000 {
        classes::apply(tx, 97).unwrap();
        let unsettled: i64 = tx
            .query_row(
                "SELECT COUNT(*) FROM archive_player_classes WHERE settled = 0",
                [],
                |row| row.get(0),
            )
            .unwrap();
        if unsettled == 0 {
            break;
        }
    }
    // Every side carries its player's class.
    let wrong: i64 = tx
        .query_row(
            "SELECT COUNT(*) FROM archive_games g WHERE
                g.class_a IS NOT (SELECT band FROM archive_player_classes WHERE ember_id = g.player_a)
             OR g.class_b IS NOT (SELECT band FROM archive_player_classes WHERE ember_id = g.player_b)",
            [],
            |row| row.get(0),
        )
        .unwrap();
    assert_eq!(wrong, 0);
}

/// A game as the brute force sees it.
#[derive(Clone, Debug)]
struct Game {
    digest: String,
    saved_at: i64,
    stored: bool,
    fighters: [u8; 2],
    names: [Option<String>; 2],
    countries: [Option<String>; 2],
    set: Option<String>,
    event: Option<String>,
    round: Option<String>,
    phase: Option<String>,
    classes: [Option<u8>; 2],
}

fn ascii_has(text: &str, part: &str) -> bool {
    text.to_ascii_lowercase()
        .contains(&part.to_ascii_lowercase())
}

fn ascii_starts(text: Option<&str>, part: &str) -> bool {
    text.is_some_and(|text| {
        text.to_ascii_lowercase()
            .starts_with(&part.to_ascii_lowercase())
    })
}

/// Whether `filter` keeps `game`, as the listing's documentation says.
fn keeps(filter: &Filter, game: &Game) -> bool {
    let either = |of: &[Option<String>; 2], value: &str| {
        of.iter().any(|side| side.as_deref() == Some(value))
    };
    game.stored
        && filter.event.as_deref().is_none_or(|event| {
            if event == "online" {
                game.set.is_none()
            } else {
                game.event.as_deref() == Some(event)
            }
        })
        && filter
            .fighter
            .is_none_or(|fighter| game.fighters.contains(&fighter))
        && filter
            .country
            .as_deref()
            .is_none_or(|country| either(&game.countries, country))
        && filter.round.as_deref().is_none_or(|round| match round {
            "top8" => ascii_starts(game.phase.as_deref(), "top"),
            "pools" => game.set.is_some() && !ascii_starts(game.phase.as_deref(), "top"),
            "winners" => ascii_starts(game.round.as_deref(), "winners"),
            "losers" => ascii_starts(game.round.as_deref(), "losers"),
            "grand" => ascii_starts(game.round.as_deref(), "grand"),
            _ => false,
        })
        && filter
            .q
            .as_deref()
            .map(str::trim)
            .filter(|q| !q.is_empty())
            .is_none_or(|q| {
                game.names
                    .iter()
                    .chain([&game.event])
                    .flatten()
                    .any(|text| ascii_has(text, q))
            })
        && filter.name.as_deref().is_none_or(|name| either(&game.names, name))
        && filter
            .class
            .is_none_or(|class| game.classes.contains(&Some(class)))
}

fn top(game: &Game) -> u8 {
    game.classes
        .iter()
        .map(|band| band.unwrap_or(0))
        .max()
        .unwrap()
}

/// `games`, made through the write paths' kinds of statement: kept pending,
/// then stored, given classes and changed, some stored and taken back.
fn fill(tx: &Transaction<'_>, dice: &mut Dice, count: usize) -> Vec<Game> {
    let mut games = Vec::new();
    for n in 0..count {
        let set = dice.maybe(15, &["emt_1", "emt_2"]).map(str::to_owned);
        let mut game = Game {
            digest: format!("{n:064x}"),
            // Few seconds, so many games share one.
            saved_at: dice.roll(count as u64 / 4 + 1) as i64,
            stored: false,
            fighters: [dice.roll(6) as u8, dice.roll(6) as u8],
            names: [0, 1].map(|_| dice.maybe(60, NAMES).map(str::to_owned)),
            countries: [0, 1].map(|_| dice.maybe(50, COUNTRIES).map(str::to_owned)),
            event: set
                .as_ref()
                .and_then(|_| dice.maybe(90, EVENTS).map(str::to_owned)),
            round: set
                .as_ref()
                .and_then(|_| dice.maybe(80, ROUNDS).map(str::to_owned)),
            phase: set
                .as_ref()
                .and_then(|_| dice.maybe(70, PHASES).map(str::to_owned)),
            set,
            classes: [None, None],
        };
        tx.execute(
            "INSERT INTO archive_games (digest, crc, saved_at, size, fighter_a, fighter_b, name_a, name_b,
                country_a, country_b, match_id, event_title, round_label, phase, sent_by, priority,
                next_attempt_at, received_at)
             VALUES (?1, '00000000', ?2, 100, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, 1, 0, 0)",
            params![
                game.digest, game.saved_at, game.fighters[0], game.fighters[1], game.names[0], game.names[1],
                game.countries[0], game.countries[1], game.set, game.event, game.round, game.phase,
                dice.pick(PLAYERS)
            ],
        )
        .unwrap();
        // Most are stored, as the bucket takes them.
        if dice.roll(100) < 90 {
            tx.execute(
                "UPDATE archive_games SET state = 'stored', replay = NULL, stored_at = 1 WHERE digest = ?1",
                [&game.digest],
            )
            .unwrap();
            game.stored = true;
        }
        // The side its sender played, if its copy said.
        if let Some(side) = dice.maybe(80, &[0u8, 1]) {
            tx.execute(
                "UPDATE archive_games SET sent_side = ?2 WHERE digest = ?1",
                params![game.digest, side],
            )
            .unwrap();
        }
        games.push(game);
        if n == count / 2 {
            settle(tx, &any_board(dice));
        }
    }
    // Later changes: a side named, a country filled, a game folded away.
    for _ in 0..count / 5 {
        let at = dice.roll(count as u64) as usize;
        let game = &mut games[at];
        match dice.roll(4) {
            0 => {
                game.names[1] = Some(dice.pick(NAMES).to_string());
                tx.execute(
                    "UPDATE archive_games SET name_b = ?2, named_by_b = ?3 WHERE digest = ?1",
                    params![game.digest, game.names[1], dice.pick(PLAYERS)],
                )
                .unwrap();
            }
            1 => {
                game.countries[0] = game.countries[0].clone().or(Some("JP".into()));
                tx.execute(
                    "UPDATE archive_games SET country_a = COALESCE(country_a, 'JP') WHERE digest = ?1",
                    [&game.digest],
                )
                .unwrap();
            }
            2 => {
                game.stored = false;
                tx.execute(
                    "UPDATE archive_games SET state = 'duplicate', replay = NULL WHERE digest = ?1",
                    [&game.digest],
                )
                .unwrap();
            }
            _ => {
                // A retry's bookkeeping changes nothing listed.
                tx.execute(
                    "UPDATE archive_games SET state = state, attempts = attempts + 1 WHERE digest = ?1",
                    [&game.digest],
                )
                .unwrap();
            }
        }
    }
    // The board moves, and the games follow it.
    settle(tx, &any_board(dice));
    for game in &mut games {
        game.classes = tx
            .query_row(
                "SELECT class_a, class_b FROM archive_games WHERE digest = ?1",
                [&game.digest],
                |row| Ok([row.get(0)?, row.get(1)?]),
            )
            .unwrap();
    }
    games
}

/// A random filter over what `fill` makes, sometimes one no game matches.
fn any_filter(dice: &mut Dice) -> Filter {
    let event = dice
        .maybe(15, &["online", "Spring Cup", "Annual Bash", "Nope"])
        .map(str::to_owned);
    let q = dice
        .maybe(
            30,
            &["an", "bob", "_", "e", "Fox", "zz", "%", " Ann ", "VE"],
        )
        .map(str::to_owned);
    Filter {
        event,
        fighter: dice.maybe(30, &[0u8, 1, 2, 5, 40]),
        country: dice.maybe(25, COUNTRIES).map(str::to_owned),
        round: dice
            .maybe(10, &["top8", "pools", "winners", "losers", "grand", "nope"])
            .map(str::to_owned),
        q,
        // A name exactly: some in another case or no game's at all.
        name: dice
            .maybe(20, &["Ann", "ann", "Bob", "Eve", "Dee_J", "Nobody"])
            .map(str::to_owned),
        class: dice.maybe(25, &[1u8, 5, 7, 10, 2]),
    }
}

/// Every game the listing lists for `filter` in `order`, page after page,
/// and the counts it gives.
fn every_page(
    tx: &Transaction<'_>,
    filter: &Filter,
    order: Order,
    limits: Limits,
) -> (Vec<String>, Option<i64>) {
    let mut seen = Vec::new();
    let mut cursor = Cursor::first(order);
    for _ in 0..1_000_000 {
        let Page { games, next } = page(tx, filter, true, order, &cursor, limits).unwrap();
        assert!(games.len() <= limits.listed);
        seen.extend(games.into_iter().map(|game| game.digest));
        let Some(next) = next else {
            let (_, matching) = counts(tx, filter, true, limits).unwrap();
            return (seen, matching);
        };
        // Each cursor reads back as itself and goes on from somewhere new.
        assert_eq!(Cursor::parse(order, &next.text(order)), Some(next.clone()));
        assert_ne!(next, cursor, "{filter:?} {order:?}");
        cursor = next;
    }
    panic!("{filter:?} never ends");
}

/// Whatever the filter, order and limits, the pages together list just the
/// games a look at every game would, each once and in order, and a count
/// it gives is that many.
#[tokio::test]
async fn pages_list_what_looking_at_every_game_would() {
    let db = archive().await;
    db.write(|tx| {
        let mut dice = Dice(0x5eed_cafe);
        let games = fill(tx, &mut dice, 1500);
        assert_index(tx);
        for round in 0..160 {
            let filter = if round == 0 {
                Filter::default()
            } else {
                any_filter(&mut dice)
            };
            let order = if dice.roll(2) == 0 {
                Order::Newest
            } else {
                Order::Class
            };
            let limits = Limits {
                listed: *dice.pick(&[1, 7, 400]),
                looked: *dice.pick(&[1, 4, 50, 10_000]),
                gathered: *dice.pick(&[0, 5, 100_000]),
                counted: *dice.pick(&[3, 100_000]),
            };
            let mut wanted: Vec<&Game> = games.iter().filter(|game| keeps(&filter, game)).collect();
            wanted.sort_by(|one, other| {
                let by_class = if order == Order::Class {
                    top(other).cmp(&top(one))
                } else {
                    std::cmp::Ordering::Equal
                };
                by_class
                    .then(other.saved_at.cmp(&one.saved_at))
                    .then(other.digest.cmp(&one.digest))
            });
            let wanted: Vec<String> = wanted.into_iter().map(|game| game.digest.clone()).collect();
            let (seen, matching) = every_page(tx, &filter, order, limits);
            assert_eq!(seen, wanted, "{filter:?} {order:?} {limits:?}");
            if let Some(matching) = matching {
                assert_eq!(matching as usize, wanted.len(), "{filter:?} {limits:?}");
            }
            if limits.counted >= 100_000 {
                assert!(matching.is_some(), "{filter:?}");
            }
        }
        Ok(())
    })
    .await
    .unwrap();
}

/// A page reads the driver's postings off the index in order, and each
/// game's row by its digest: never the games first, never a sort.
#[tokio::test]
async fn a_page_walks_the_index_in_order() {
    let db = archive().await;
    db.read(|tx| {
        let mut explain = tx.prepare(&format!("EXPLAIN QUERY PLAN {}", by_one_key()))?;
        let nulls = vec![rusqlite::types::Null; explain.parameter_count()];
        let plan: Vec<String> = explain
            .query_map(rusqlite::params_from_iter(nulls), |row| row.get(3))?
            .collect::<rusqlite::Result<_>>()?;
        assert_eq!(plan.len(), 2, "{plan:?}");
        assert!(
            plan[0].starts_with("SEARCH k USING PRIMARY KEY (key=?"),
            "{plan:?}"
        );
        assert!(
            plan[1].starts_with("SEARCH g USING PRIMARY KEY (digest=?)"),
            "{plan:?}"
        );
        Ok(())
    })
    .await
    .unwrap();
}

/// Games saved in the same second, across a page's end, are each listed
/// once, in one order.
#[tokio::test]
async fn pages_part_at_a_game_not_at_a_second() {
    let db = archive().await;
    db.write(|tx| {
        for (saved_at, letter) in [(5, 'a'), (5, 'b'), (5, 'c'), (5, 'd'), (4, 'e'), (6, 'f')] {
            tx.execute(
                "INSERT INTO archive_games (digest, crc, saved_at, size, fighter_a, fighter_b, sent_by, priority,
                    state, next_attempt_at, received_at)
                 VALUES (?1, '00000000', ?2, 1, 1, 1, 'emb_p1', 1, 'stored', 0, 0)",
                params![letter.to_string().repeat(64), saved_at],
            )?;
        }
        let limits = Limits { listed: 2, ..LIMITS };
        let (seen, matching) = every_page(tx, &Filter::default(), Order::Newest, limits);
        let letters: Vec<char> = seen.iter().map(|digest| digest.chars().next().unwrap()).collect();
        assert_eq!(letters, ['f', 'd', 'c', 'b', 'a', 'e']);
        assert_eq!(matching, Some(6));
        Ok(())
    })
    .await
    .unwrap();
}

#[test]
fn only_a_listings_cursor_reads() {
    let digest = "0".repeat(64);
    assert_eq!(
        Cursor::parse(Order::Newest, &format!("12.{digest}")),
        Some(Cursor {
            segment: 0,
            saved_at: 12,
            digest: digest.clone()
        })
    );
    assert_eq!(
        Cursor::parse(Order::Class, &format!("7.12.{digest}")),
        Some(Cursor {
            segment: 7,
            saved_at: 12,
            digest: digest.clone()
        })
    );
    for (order, text) in [
        (Order::Newest, "12".to_owned()),
        (Order::Newest, format!("x.{digest}")),
        (Order::Newest, format!("12.{}", &digest[1..])),
        (Order::Newest, format!("12.{}", "A".repeat(64))),
        (Order::Newest, format!("7.12.{digest}")),
        (Order::Class, format!("12.{digest}")),
        (Order::Class, format!("11.12.{digest}")),
        (Order::Class, format!("-1.12.{digest}")),
    ] {
        assert_eq!(Cursor::parse(order, &text), None, "{text}");
    }
    for order in [Order::Newest, Order::Class] {
        let first = Cursor::first(order);
        assert_eq!(Cursor::parse(order, &first.text(order)), Some(first));
    }
}

fn classes_of(tx: &Transaction<'_>) -> HashMap<String, [Option<u8>; 2]> {
    tx.prepare("SELECT digest, class_a, class_b FROM archive_games")
        .unwrap()
        .query_map([], |row| Ok((row.get(0)?, [row.get(1)?, row.get(2)?])))
        .unwrap()
        .collect::<rusqlite::Result<_>>()
        .unwrap()
}

/// A class that moves reaches its player's games a bounded number of games
/// a pass, then the player is settled; one who leaves the board leaves no
/// class behind, nor a row.
#[tokio::test]
async fn classes_reach_the_games_a_bounded_stretch_a_pass() {
    let db = archive().await;
    db.write(|tx| {
        for n in 0..5 {
            tx.execute(
                "INSERT INTO archive_games (digest, crc, saved_at, size, fighter_a, fighter_b, sent_by, sent_side,
                    priority, state, next_attempt_at, received_at)
                 VALUES (?1, '00000000', ?2, 1, 1, 2, 'emb_p1', ?3, 1, 'stored', 0, 0)",
                params![format!("{n:064x}"), n, n % 2],
            )?;
        }
        let carrying = |tx: &Transaction<'_>, band: Option<u8>| {
            classes_of(tx).values().flatten().filter(|class| **class == band).count()
        };
        let unsettled = |tx: &Transaction<'_>| -> i64 {
            tx.query_row("SELECT COUNT(*) FROM archive_player_classes WHERE settled = 0", [], |row| row.get(0))
                .unwrap()
        };
        classes::record(tx, &HashMap::from([("emb_p1".to_owned(), 7)]))?;
        classes::apply(tx, 2)?;
        assert_eq!(carrying(tx, Some(7)), 2);
        assert_eq!(unsettled(tx), 1);
        classes::apply(tx, 2)?;
        classes::apply(tx, 2)?;
        assert_eq!(carrying(tx, Some(7)), 5);
        // The last pass found fewer than it could give: settled.
        assert_eq!(unsettled(tx), 0);
        assert_index(tx);
        let (seen, _) = every_page(tx, &Filter { class: Some(7), ..Filter::default() }, Order::Newest, LIMITS);
        assert_eq!(seen.len(), 5);
        // Nothing moved: nothing to do.
        classes::record(tx, &HashMap::from([("emb_p1".to_owned(), 7)]))?;
        assert_eq!(unsettled(tx), 0);
        classes::record(tx, &HashMap::new())?;
        classes::apply(tx, 100)?;
        assert_eq!(carrying(tx, Some(7)), 0);
        let rows: i64 = tx.query_row("SELECT COUNT(*) FROM archive_player_classes", [], |row| row.get(0))?;
        assert_eq!(rows, 0);
        assert_index(tx);
        let (seen, matching) = every_page(tx, &Filter { class: Some(7), ..Filter::default() }, Order::Newest, LIMITS);
        assert!(seen.is_empty());
        assert_eq!(matching, Some(0));
        Ok(())
    })
    .await
    .unwrap();
}

/// A game sent later by a player already on the board carries their class
/// at once, as does a side named later.
#[tokio::test]
async fn a_new_game_carries_its_players_class() {
    let db = archive().await;
    db.write(|tx| {
        classes::record(tx, &HashMap::from([("emb_p1".to_owned(), 9), ("emb_p2".to_owned(), 4)]))?;
        classes::apply(tx, 100)?;
        tx.execute(
            "INSERT INTO archive_games (digest, crc, saved_at, size, fighter_a, fighter_b, sent_by, sent_side,
                priority, next_attempt_at, received_at)
             VALUES (?1, '00000000', 1, 1, 1, 2, 'emb_p1', 0, 1, 0, 0)",
            [&"a".repeat(64)],
        )?;
        assert_eq!(classes_of(tx)[&"a".repeat(64)], [Some(9), None]);
        tx.execute(
            "UPDATE archive_games SET name_b = 'Bob', named_by_b = 'emb_p2' WHERE digest = ?1",
            [&"a".repeat(64)],
        )?;
        assert_eq!(classes_of(tx)[&"a".repeat(64)], [Some(9), Some(4)]);
        // Not listed until the bucket stores it, then by both classes.
        assert!(every_page(tx, &Filter::default(), Order::Class, LIMITS).0.is_empty());
        tx.execute("UPDATE archive_games SET state = 'stored'", [])?;
        let keys: HashSet<String> = tx
            .prepare("SELECT key FROM archive_keys")?
            .query_map([], |row| row.get(0))?
            .collect::<rusqlite::Result<_>>()?;
        for key in ["*", "f:1", "f:2", "n:Bob", "o", "k:9", "k:4", "t:9"] {
            assert!(keys.contains(key), "{key} not in {keys:?}");
        }
        assert_eq!(keys.len(), 8, "{keys:?}");
        assert_index(tx);
        Ok(())
    })
    .await
    .unwrap();
}

/// How long listings take over an archive of `SF4E_ARCHIVE_SCALE` games
/// (a million by default), each a few names and classes among thousands of
/// players: run with `--ignored --nocapture`, in release.
#[tokio::test]
#[ignore]
async fn scale() {
    let games: u64 = std::env::var("SF4E_ARCHIVE_SCALE")
        .ok()
        .and_then(|n| n.parse().ok())
        .unwrap_or(1_000_000);
    // On disk, as the bridge keeps it.
    let path = std::env::temp_dir().join("ember-archive-scale.sqlite3");
    for end in ["", "-wal", "-shm"] {
        let _ = std::fs::remove_file(format!("{}{end}", path.display()));
    }
    let db = Db::open(&path).unwrap();
    db.write(move |tx| {
        let started = std::time::Instant::now();
        let mut dice = Dice(0x5ca1e);
        let mut insert = tx.prepare(
            "INSERT INTO archive_games (digest, crc, saved_at, size, fighter_a, fighter_b, name_a, name_b,
                country_a, country_b, sent_by, sent_side, named_by_b, priority, state, next_attempt_at, received_at)
             VALUES (?1, '00000000', ?2, 9000, ?3, ?4, ?5, ?6, ?7, ?8, ?9, 0, ?10, 1, 'stored', 0, 0)",
        )?;
        for n in 0..games {
            let (a, b) = (dice.roll(20_000), dice.roll(20_000));
            let named_b = dice.roll(2) == 0;
            insert.execute(params![
                format!("{n:064x}"),
                1_700_000_000 + n as i64 * 30,
                dice.roll(44),
                dice.roll(44),
                format!("player{a}"),
                named_b.then(|| format!("player{b}")),
                format!("C{}", dice.roll(60)),
                named_b.then(|| format!("C{}", dice.roll(60))),
                format!("emb_{a}"),
                named_b.then(|| format!("emb_{b}")),
            ])?;
        }
        drop(insert);
        eprintln!("{games} games in {:?}", started.elapsed());
        let board: HashMap<String, u8> =
            (0..5000u64).map(|n| (format!("emb_{}", n * 4), (n % 10) as u8 + 1)).collect();
        let started = std::time::Instant::now();
        classes::record(tx, &board)?;
        classes::apply(tx, i64::MAX)?;
        eprintln!("classes given in {:?}", started.elapsed());
        let size: i64 = tx.query_row("SELECT page_count * page_size FROM pragma_page_count(), pragma_page_size()", [], |row| row.get(0))?;
        eprintln!("database {} MB", size / 1_000_000);
        Ok(())
    })
    .await
    .unwrap();
    let time = |what: &'static str, filter: Filter, order: Order| {
        let db = &db;
        async move {
            let started = std::time::Instant::now();
            let (listed, next, matching) = db
                .read(move |tx| {
                    let page = page(tx, &filter, true, order, &Cursor::first(order), LIMITS)?;
                    let (_, matching) = counts(tx, &filter, true, LIMITS)?;
                    Ok((page.games.len(), page.next.is_some(), matching))
                })
                .await
                .unwrap();
            eprintln!(
                "{what:<34} {listed:>4} listed, more {next:<5}, matching {matching:?}, {:?}",
                started.elapsed()
            );
        }
    };
    time("newest", Filter::default(), Order::Newest).await;
    time("highest class first", Filter::default(), Order::Class).await;
    time(
        "fighter",
        Filter {
            fighter: Some(7),
            ..Filter::default()
        },
        Order::Newest,
    )
    .await;
    time(
        "country",
        Filter {
            country: Some("C3".into()),
            ..Filter::default()
        },
        Order::Newest,
    )
    .await;
    time(
        "class",
        Filter {
            class: Some(10),
            ..Filter::default()
        },
        Order::Newest,
    )
    .await;
    time(
        "class, by class",
        Filter {
            class: Some(7),
            ..Filter::default()
        },
        Order::Class,
    )
    .await;
    time(
        "fighter + country",
        Filter {
            fighter: Some(7),
            country: Some("C3".into()),
            ..Filter::default()
        },
        Order::Newest,
    )
    .await;
    time(
        "fighter + class",
        Filter {
            fighter: Some(7),
            class: Some(10),
            ..Filter::default()
        },
        Order::Newest,
    )
    .await;
    time(
        "fighter + country + class",
        Filter {
            fighter: Some(7),
            country: Some("C3".into()),
            class: Some(10),
            ..Filter::default()
        },
        Order::Newest,
    )
    .await;
    time(
        "search one name",
        Filter {
            q: Some("player12345".into()),
            ..Filter::default()
        },
        Order::Newest,
    )
    .await;
    time(
        "search a few names",
        Filter {
            q: Some("player1234".into()),
            ..Filter::default()
        },
        Order::Newest,
    )
    .await;
    time(
        "search many names",
        Filter {
            q: Some("player1".into()),
            ..Filter::default()
        },
        Order::Newest,
    )
    .await;
    time(
        "search everyone",
        Filter {
            q: Some("play".into()),
            ..Filter::default()
        },
        Order::Newest,
    )
    .await;
    time(
        "search nothing",
        Filter {
            q: Some("nobody".into()),
            ..Filter::default()
        },
        Order::Newest,
    )
    .await;
    time(
        "search + fighter",
        Filter {
            q: Some("player1".into()),
            fighter: Some(7),
            ..Filter::default()
        },
        Order::Newest,
    )
    .await;
    db.write(move |tx| {
        let started = std::time::Instant::now();
        tx.execute(
            "INSERT INTO archive_games (digest, crc, saved_at, size, fighter_a, fighter_b, name_a, sent_by, sent_side,
                priority, next_attempt_at, received_at)
             VALUES (?1, '00000000', 1, 9000, 1, 2, 'player4', 'emb_4', 0, 1, 0, 0)",
            [&"f".repeat(64)],
        )?;
        tx.execute("UPDATE archive_games SET state = 'stored' WHERE digest = ?1", [&"f".repeat(64)])?;
        eprintln!("a game taken and stored            {:?}", started.elapsed());
        let started = std::time::Instant::now();
        let mut board: HashMap<String, u8> =
            (0..5000u64).map(|n| (format!("emb_{}", n * 4), (n % 10) as u8 + 1)).collect();
        board.insert("emb_0".into(), 3);
        classes::refresh_with(tx, &board)?;
        eprintln!("a pass, one player's class moved   {:?}", started.elapsed());
        let started = std::time::Instant::now();
        classes::refresh_with(tx, &HashMap::new())?;
        eprintln!("a pass, classes hidden (capped)    {:?}", started.elapsed());
        let started = std::time::Instant::now();
        let kept: i64 = tx.query_row("SELECT bytes FROM archive_kept", [], |row| row.get(0))?;
        eprintln!("kept bytes {kept}                  {:?}", started.elapsed());
        Ok(())
    })
    .await
    .unwrap();
}

/// Picked by class in the class order, the pages stop once the class's
/// segment is done: no class below can hold a game with a side of it. While
/// the bridge hides classes, a refresh leaves the games' classes as they are.
#[tokio::test]
async fn a_class_in_the_class_order_ends_with_its_segment() {
    let db = archive().await;
    db.write(|tx| {
        for n in 0..900u32 {
            tx.execute(
                "INSERT INTO archive_games (digest, crc, saved_at, size, fighter_a, fighter_b, sent_by, sent_side,
                    priority, state, next_attempt_at, received_at)
                 VALUES (?1, '00000000', ?2, 1, 1, 2, ?3, 0, 1, 'stored', 0, 0)",
                params![format!("{n:064x}"), n, if n % 3 == 0 { "emb_p7" } else { "emb_p3" }],
            )?;
        }
        settle(tx, &HashMap::from([("emb_p7".to_owned(), 7), ("emb_p3".to_owned(), 3)]));
        let filter = Filter { class: Some(7), ..Filter::default() };
        let limits = Limits { listed: 400, looked: 20, ..LIMITS };
        let (mut cursor, mut pages, mut seen) = (Cursor::first(Order::Class), Vec::new(), 0);
        loop {
            let Page { games, next } = page(tx, &filter, true, Order::Class, &cursor, limits)?;
            seen += games.len();
            pages.push(games.len());
            let Some(next) = next else { break };
            cursor = next;
        }
        assert_eq!(seen, 300);
        // Segments 10 to 8 hold no game and are passed over; none below 7 is
        // looked at. At most the last page is empty: the one that finds the
        // segment's last look ended right at its end.
        let empty = pages.iter().filter(|size| **size == 0).count();
        assert!(empty <= 1, "{pages:?}");

        classes::refresh(tx, false, &[], 0)?;
        let classed = classes_of(tx).values().filter(|sides| sides[0].is_some()).count();
        assert_eq!(classed, 900);
        Ok(())
    })
    .await
    .unwrap();
}
