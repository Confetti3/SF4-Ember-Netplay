//! Games a bridge kept before migration 054, when both players' copies of
//! one game were two games: each is read back from the bucket, and once
//! every copy is known they are folded into one.
use super::*;

/// A copy of a game as a bridge before migration 054 kept it: stored, its
/// file in the bucket as `bucket` (the file itself, unless that says
/// otherwise), sent by `by` for side `side` with that side's name and
/// country, and not known by `game`.
struct KeptBefore {
    file: Vec<u8>,
    bucket: Option<Vec<u8>>,
    by: String,
    side: u8,
    name: Option<&'static str>,
    country: Option<&'static str>,
}

impl KeptBefore {
    fn new(
        file: Vec<u8>,
        by: &Player,
        side: u8,
        name: Option<&'static str>,
        country: Option<&'static str>,
    ) -> Self {
        let by = by.id().to_string();
        Self {
            file,
            bucket: None,
            by,
            side,
            name,
            country,
        }
    }
}

/// Keeps `copies` as a bridge before migration 054 did, each received a
/// second after the one before it.
async fn keep_before(f: &Fixture, copies: Vec<KeptBefore>) {
    for copy in &copies {
        let path = format!("/replays/games/{}.emberreplay", digest(&copy.file));
        let held = copy.bucket.clone().unwrap_or_else(|| copy.file.clone());
        f.stored.lock().unwrap().push((path, held));
    }
    f.bridge
        .state()
        .db
        .write(move |tx| {
            // Such a bridge had not folded anything yet.
            tx.execute("INSERT OR IGNORE INTO archive_fold_owed VALUES (1)", [])?;
            for (at, copy) in copies.iter().enumerate() {
                let replay = Replay::parse(&copy.file).unwrap();
                let own = |value: Option<&'static str>, side: u8| value.filter(|_| copy.side == side);
                tx.execute(
                    "INSERT INTO archive_games (digest, crc, saved_at, size, fighter_a, fighter_b, name_a, name_b,
                        country_a, country_b, sent_side, sent_by, priority, state, next_attempt_at, received_at, stored_at)
                     VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, 1, 'stored', 0, ?13, ?13)",
                    rusqlite::params![
                        replay.digest_hex(),
                        replay.crc_name(),
                        replay.saved_at,
                        copy.file.len(),
                        replay.fighters[0],
                        replay.fighters[1],
                        own(copy.name, 0),
                        own(copy.name, 1),
                        own(copy.country, 0),
                        own(copy.country, 1),
                        copy.side,
                        copy.by,
                        1_800_000_000 + at as i64,
                    ],
                )?;
            }
            Ok(())
        })
        .await
        .unwrap();
}

/// Copies of one game kept before games were known by what was played are
/// folded into the copy kept first, whatever order they are read back in,
/// with each other copy naming the side its sender played by the rules a
/// late copy names one by; the other copies leave the bucket, then the
/// bridge.
#[tokio::test]
async fn copies_kept_before_are_folded_into_the_first() {
    let f = fixture().await;
    let first = played_game(32);
    let (unnamed, named) = (copy_from(&first, 1), copy_from(&first, 2));
    // Bob's first copy names nobody, so it alone could not name the side his
    // second copy names; the copy kept is Ann's, which came first.
    keep_before(
        &f,
        vec![
            KeptBefore::new(first.clone(), &f.a, 1, Some("Ann"), Some("NL")),
            KeptBefore::new(unnamed.clone(), &f.b, 0, None, None),
            KeptBefore::new(named.clone(), &f.b, 0, Some("Bob"), Some("BE")),
        ],
    )
    .await;
    removed(&f, &unnamed).await;
    removed(&f, &named).await;
    let list = listed(&f, "").await;
    assert_eq!(games(&list).len(), 1);
    let kept = listed_game(&list, &first);
    assert_eq!(kept["names"], json!(["Bob", "Ann"]));
    assert_eq!(kept["countries"], json!(["BE", "NL"]));
    assert_eq!(
        games_held(&f),
        vec![format!("/replays/games/{}.emberreplay", digest(&first))]
    );
    let known = "SELECT COUNT(*) FROM archive_games WHERE game = ?1";
    let game = Replay::parse(&first).unwrap().game_hex();
    assert_eq!(count(&f, known, &game).await, 1);
}

/// A copy is known only by the replay it was stored as: one the bucket
/// gives back wrong stays unknown, and no game kept twice is folded until it
/// is known, since it may be the copy that came first.
#[tokio::test]
async fn folding_waits_for_every_copy_to_be_read() {
    let f = fixture().await;
    let ann = played_game(33);
    let bob = copy_from(&ann, 1);
    let other = played_game(34);
    keep_before(
        &f,
        vec![
            KeptBefore {
                bucket: Some(b"not a replay".to_vec()),
                ..KeptBefore::new(other.clone(), &f.a, 0, Some("Ann"), None)
            },
            KeptBefore::new(ann.clone(), &f.a, 0, Some("Ann"), None),
            KeptBefore::new(bob.clone(), &f.b, 1, Some("Bob"), None),
        ],
    )
    .await;
    let known = "SELECT COUNT(*) FROM archive_games WHERE game IS NOT NULL AND digest != ?1";
    for _ in 0..200 {
        if count(&f, known, &digest(&other)).await == 2 {
            break;
        }
        tokio::time::sleep(std::time::Duration::from_millis(50)).await;
    }
    assert_eq!(count(&f, known, &digest(&other)).await, 2);
    ember_bridge::maintain(f.bridge.state()).await;
    f.bridge.clock.advance(3600);
    tokio::time::sleep(std::time::Duration::from_millis(300)).await;
    ember_bridge::maintain(f.bridge.state()).await;
    let unknown = "SELECT COUNT(*) FROM archive_games WHERE game IS NULL AND digest = ?1";
    assert_eq!(count(&f, unknown, &digest(&other)).await, 1);
    assert_eq!(games(&listed(&f, "").await).len(), 3);
    // Given back right, it is known, and the game kept twice is folded.
    let path = format!("/replays/games/{}.emberreplay", digest(&other));
    f.stored.lock().unwrap().push((path, other.clone()));
    removed(&f, &bob).await;
    let list = listed(&f, "").await;
    assert_eq!(games(&list).len(), 2);
    assert_eq!(listed_game(&list, &ann)["names"], json!(["Ann", "Bob"]));
}
