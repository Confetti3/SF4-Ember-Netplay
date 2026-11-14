//! The cards a `Reader` assembles are, byte for byte, the ones assembled one
//! account at a time before it, for every kind of player and caller; and a
//! look-up of 16 cards or a search that lists 20 players runs as many
//! statements as one of 2.
use std::path::Path;

use ember_protocol::levels::{CardSearch, CardSelector, SearchAnswer};

use super::*;
use crate::{Db, db::counting::counted, routes};

const NOW: u64 = 1_800_000_000;
/// No account holds it.
const NOBODY: &str = "1999998";
/// An opponent of some of the players, never looked up.
const OPPONENT: &str = "1999999";

/// Player `n`'s Discord account; the caller is 0.
fn account(n: usize) -> String {
    (1_000_000 + n).to_string()
}

fn caller() -> String {
    account(0)
}

fn id(prefix: &str, n: usize) -> String {
    format!("{prefix}_{n:08x}-1111-4111-8111-111111111111")
}

/// Player `n`'s friend code, as the bridge writes one.
fn code(n: usize) -> String {
    format!("A{:03}-B{:03}", n % 1000, n % 1000)
}

fn classes(shown: bool) -> Classes {
    Classes {
        shown,
        developers: vec![account(4), account(10)],
    }
}

fn connect(tx: &Transaction<'_>, account: &str) -> rusqlite::Result<()> {
    let ember_id = format!("ember-{account}");
    tx.execute(
        "INSERT INTO identities (ember_id, public_key, created_at) VALUES (?1, ?1, 1)",
        [&ember_id],
    )?;
    tx.execute(
        "INSERT INTO discord_accounts (user_id, ember_id, username, connected_at)
         VALUES (?1, ?2, ?2, 1)",
        params![account, ember_id],
    )?;
    Ok(())
}

/// The caller and `targets` players, and a few of what each can be: not
/// connected to Discord, never played, played, opted out of ranking, rated
/// and settled or still placing, a developer, with or without a card row and
/// what it shows, and the caller's friend, blocked, blocking, asked or
/// asking, with a record against the caller or not.
fn seed(tx: &Transaction<'_>, targets: usize) -> rusqlite::Result<()> {
    let caller = caller();
    connect(tx, &caller)?;
    tx.execute(
        "INSERT INTO account_points (discord_user_id, games, wins, lower_wins, first_at, played_at)
         VALUES (?1, 9, 4, 1, 1, 1)",
        [&caller],
    )?;
    for n in 0..=targets {
        tx.execute(
            "INSERT INTO social_accounts (discord_user_id, name, code, code_at, seen_at, off)
             VALUES (?1, ?2, ?3, 0, 0, ?4)",
            params![
                account(n),
                format!("Player {n}"),
                code(n),
                n > 0 && n % 13 == 0
            ],
        )?;
    }
    for n in 1..=targets {
        let account = account(n);
        let number = n as u64;
        if n % 9 != 0 {
            connect(tx, &account)?;
        }
        if n % 3 != 0 {
            tx.execute(
                "INSERT INTO account_points (discord_user_id, games, wins, lower_wins, first_at, played_at)
                 VALUES (?1, ?2, ?3, ?4, 1, 1)",
                params![account, number + 1, number / 2, number / 4],
            )?;
            for fighter in [number % 40, (number + 1) % 40] {
                tx.execute(
                    "INSERT INTO fighter_levels
                         (discord_user_id, fighter, games, wins, lower_wins, first_at, played_at)
                     VALUES (?1, ?2, ?3, ?4, ?5, 1, 1)",
                    params![account, fighter, number * 7, number * 3, number],
                )?;
            }
        }
        if n % 4 != 1 {
            tx.execute(
                "INSERT INTO award_counters (discord_user_id, streak, best_streak, peak_band)
                 VALUES (?1, 0, ?2, ?3)",
                params![account, number % 5, number % 11],
            )?;
        }
        if n % 7 == 0 {
            tx.execute(
                "INSERT INTO ranking_choices (game, discord_user_id, ranked) VALUES ('usf4', ?1, 0)",
                [&account],
            )?;
        }
        if n % 2 == 0 {
            tx.execute(
                "INSERT INTO skill_ratings
                     (game, discord_user_id, rating, deviation, volatility, band, played_at)
                 VALUES ('usf4', ?1, ?2, ?3, 0.06, ?4, ?5)",
                params![
                    account,
                    1_300.0 + number as f64 * 4.0,
                    if n % 4 == 0 { 80.0 } else { 320.0 },
                    number % 10 + 1,
                    NOW - number * 3_600,
                ],
            )?;
        }
        if n % 5 != 2 {
            tx.execute(
                "INSERT INTO player_cards (discord_user_id, name, country, main_fighter, since,
                     worn_title, worn_icon, pins, device, link, region, looking, updated_at)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)",
                params![
                    account,
                    format!("Player {n}"),
                    (n % 2 == 0).then_some("JP"),
                    (n % 5 != 0).then_some(number % 40),
                    1_748_736_000 + number * 1_000,
                    (n % 3 == 0).then_some("matches"),
                    (n % 4 == 0).then_some("streak"),
                    if n % 2 == 0 { "rank,wins" } else { "" },
                    number % 5,
                    number % 3,
                    (n % 3 == 0).then_some("euc1"),
                    number % 64,
                    number,
                ],
            )?;
        }
        // A counted game against the caller (the pair's first), and against
        // someone else.
        if n % 3 != 1 {
            tx.execute(
                "INSERT INTO pair_records (first, second, first_wins, second_wins, played_at)
                 VALUES (?1, ?2, ?3, ?4, ?5)",
                params![caller, account, number % 5, number % 3, number],
            )?;
        }
        if n % 2 == 1 {
            tx.execute(
                "INSERT INTO pair_records (first, second, first_wins, second_wins, played_at)
                 VALUES (?1, ?2, ?3, 1, ?4)",
                params![account, OPPONENT, number % 4, number],
            )?;
        }
        match n % 6 {
            1 => {
                tx.execute(
                    "INSERT INTO friendships (first, second, id, since) VALUES (?1, ?2, ?3, 1)",
                    params![caller, account, id("fr", n)],
                )?;
            }
            2 => {
                tx.execute(
                    "INSERT INTO friend_blocks (blocker, blocked, id, name, at)
                     VALUES (?1, ?2, ?3, 'Blocked', 1)",
                    params![caller, account, id("bl", n)],
                )?;
            }
            3 => {
                tx.execute(
                    "INSERT INTO friend_blocks (blocker, blocked, id, name, at)
                     VALUES (?1, ?2, ?3, 'Blocker', 1)",
                    params![account, caller, id("bl", n)],
                )?;
            }
            4 => {
                // Every other one withdrawn: it no longer makes them `sent`.
                tx.execute(
                    "INSERT INTO friend_requests (id, sender, receiver, label, sent_at, withdrawn_at)
                     VALUES (?1, ?2, ?3, 'label', 1, ?4)",
                    params![id("rq", n), caller, account, (n % 12 == 10).then_some(2)],
                )?;
            }
            5 => {
                // Every other one hidden: it does not make them `received`.
                tx.execute(
                    "INSERT INTO friend_requests (id, sender, receiver, label, sent_at, hidden_at)
                     VALUES (?1, ?2, ?3, 'label', 1, ?4)",
                    params![id("rq", n), account, caller, (n % 12 == 11).then_some(2)],
                )?;
            }
            _ => {}
        }
    }
    Ok(())
}

async fn seeded(targets: usize) -> Db {
    let db = Db::open(Path::new(":memory:")).unwrap();
    db.write(move |tx| Ok(seed(tx, targets)?)).await.unwrap();
    db
}

/// `target`'s card as it was assembled before `Reader`: one account at a
/// time, a dozen statements each.
fn one_card(tx: &Transaction<'_>, viewer: &Viewer<'_>, target: &str) -> Result<Option<PlayerCard>> {
    let relation = match viewer.caller {
        Some(caller) => store::relation_one(tx, caller, target)?,
        None => Some(Relation::None),
    };
    let Some(relation) = relation else {
        return Ok(None);
    };
    let Some(card) = levels::awarded_card(tx, target)? else {
        return Ok(None);
    };
    let relation = if viewer.friends_offered {
        relation
    } else {
        Relation::None
    };
    let badge = if viewer.classes.shown {
        skill::last_shown(tx, target, &viewer.classes.developers, viewer.now)?
    } else {
        None
    };
    let mut card = PlayerCard {
        relation,
        badge,
        ..card
    };
    if let Some(stored) = stored(tx, target)? {
        card = describe(card, &stored);
    }
    if let Some(caller) = viewer.caller.filter(|caller| *caller != target) {
        card.versus = store::record(tx, caller, target)?;
        if relation == Relation::Friend {
            card.friend = store::friendship_id(tx, caller, target)?;
        }
    }
    Ok(Some(card))
}

#[tokio::test]
async fn a_card_read_with_many_is_the_card_read_alone() {
    let db = seeded(60).await;
    db.read(|tx| {
        let caller = caller();
        let mut accounts: Vec<String> = (0..=60).map(account).collect();
        accounts.extend([NOBODY.to_owned(), OPPONENT.to_owned(), account(5)]);
        let targets: Vec<&str> = accounts.iter().map(String::as_str).collect();
        for (caller, friends_offered, shown) in [
            (Some(caller.as_str()), true, true),
            (Some(caller.as_str()), false, true),
            (Some(caller.as_str()), true, false),
            (None, true, true),
        ] {
            let classes = classes(shown);
            let viewer = Viewer {
                caller,
                friends_offered,
                classes: &classes,
                now: NOW,
            };
            let read = Reader::read(tx, &viewer, &targets, Parts::CARD)?;
            let mut cards = Vec::new();
            for target in &targets {
                let card = read.card(target);
                let expected = one_card(tx, &viewer, target)?;
                assert_eq!(
                    serde_json::to_string(&card).unwrap(),
                    serde_json::to_string(&expected).unwrap(),
                    "{target}"
                );
                assert_eq!(card, expected, "{target}");
                cards.push(card);
            }
            // Each kind of card is among them, so the equality means something.
            assert!(cards.iter().any(Option::is_none));
            let shown: Vec<&PlayerCard> = cards.iter().flatten().collect();
            assert!(
                shown
                    .iter()
                    .any(|card| card.name.is_some() && !card.pins.is_empty())
            );
            assert!(shown.iter().any(|card| card.name.is_none()));
            assert!(shown.iter().any(|card| card.awards.best_streak > 0));
            assert!(shown.iter().any(|card| card.awards.peak_class > 0));
            if caller.is_some() {
                assert!(shown.iter().any(|card| card.versus.is_some()));
                assert!(shown.iter().any(|card| card.versus.is_none()));
            }
            if caller.is_some() && friends_offered {
                for relation in [
                    Relation::Friend,
                    Relation::Blocked,
                    Relation::Sent,
                    Relation::Received,
                    Relation::None,
                ] {
                    assert!(
                        shown.iter().any(|card| card.relation == relation),
                        "{relation:?}"
                    );
                }
                assert!(shown.iter().any(|card| card.friend.is_some()));
            }
            if shown_classes(&viewer) {
                assert!(
                    shown
                        .iter()
                        .any(|card| card.badge == Some(SkillBadge::DEVELOPER))
                );
                assert!(shown.iter().any(|card| {
                    card.badge
                        .is_some_and(|badge| badge != SkillBadge::DEVELOPER)
                }));
                assert!(shown.iter().any(|card| card.badge.is_none()));
            }
        }
        Ok(())
    })
    .await
    .unwrap();
}

fn shown_classes(viewer: &Viewer<'_>) -> bool {
    viewer.classes.shown
}

/// A token for player `n`, as a room shows it.
fn token(keys: &Keys, n: usize) -> CardSelector {
    CardSelector::Card(issue(keys, &account(n), NOW).expect("a Discord user ID"))
}

/// The answer to `selectors` (made with the bridge's keys) on a bridge seeded
/// with 60 players, and the statements it ran.
async fn look_up(
    selectors: impl FnOnce(&Keys) -> Vec<CardSelector> + Send + 'static,
) -> (Vec<Option<PlayerCard>>, usize) {
    let db = seeded(60).await;
    db.read(move |tx| {
        let keys = Keys::generate();
        let selectors = selectors(&keys);
        let (caller, classes) = (caller(), classes(true));
        let viewer = Viewer {
            caller: Some(caller.as_str()),
            friends_offered: true,
            classes: &classes,
            now: NOW,
        };
        let (answers, statements) = counted(tx, || {
            routes::levels::answer(tx, &keys, &viewer, &selectors)
        });
        Ok((answers?.cards, statements))
    })
    .await
    .unwrap()
}

#[tokio::test]
async fn a_look_up_of_sixteen_cards_costs_what_one_of_two_does() {
    let (few, few_statements) = look_up(|keys| vec![token(keys, 1), token(keys, 2)]).await;
    let (many, many_statements) = look_up(|keys| (1..=16).map(|n| token(keys, n)).collect()).await;
    assert_eq!((few.len(), many.len()), (2, 16));
    assert!(many.iter().filter(|card| card.is_some()).count() >= 12);
    assert_eq!(few_statements, many_statements);
    assert!(many_statements <= 10, "{many_statements} statements");

    // Each kind of selector is one read more for all of its kind.
    let mixed = |count: usize| {
        move |keys: &Keys| -> Vec<CardSelector> {
            (0..count)
                .flat_map(|at| {
                    let n = 12 * at;
                    [
                        token(keys, n + 2),
                        CardSelector::Friend(id("fr", n + 1)),
                        CardSelector::Request(id("rq", n + 5)),
                        CardSelector::Code(code(n + 4).to_lowercase()),
                    ]
                })
                .collect()
        }
    };
    let (few, few_statements) = look_up(mixed(1)).await;
    let (many, many_statements) = look_up(mixed(4)).await;
    assert_eq!((few.len(), many.len()), (4, 16));
    assert!(few.iter().all(Option::is_some), "{few:?}");
    assert!(many.iter().filter(|card| card.is_some()).count() >= 12);
    assert_eq!(few_statements, many_statements);
    assert!(many_statements <= 13, "{many_statements} statements");
}

/// A search for "Player" on a bridge seeded with `targets` players, and the
/// statements it ran.
async fn search(targets: usize) -> (SearchAnswer, usize) {
    let db = seeded(targets).await;
    db.read(move |tx| {
        let keys = Keys::generate();
        let (caller, classes) = (caller(), classes(true));
        let viewer = Viewer {
            caller: Some(caller.as_str()),
            friends_offered: true,
            classes: &classes,
            now: NOW,
        };
        let search = CardSearch {
            name: Some("Player".into()),
            ..CardSearch::default()
        };
        let (answer, statements) =
            counted(tx, || routes::profiles::find(tx, &keys, &viewer, &search));
        Ok((answer?, statements))
    })
    .await
    .unwrap()
}

#[tokio::test]
async fn a_search_that_lists_twenty_costs_what_one_that_lists_two_does() {
    let (few, few_statements) = search(4).await;
    let (many, many_statements) = search(60).await;
    assert_eq!(few.players.len(), 2);
    assert!(!few.more);
    assert_eq!(many.players.len(), 20);
    assert!(many.more);
    assert!(many.players.iter().any(|hit| hit.badge.is_some()));
    assert_eq!(few_statements, many_statements);
    assert!(many_statements <= 4, "{many_statements} statements");
}
