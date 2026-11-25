//! Each archived game's round score (`Replay::rounds`): kept with a new
//! game, online or a set's, listed with it, none when two copies of the game
//! disagree, and read back from the bucket for the games kept before
//! migration 067, each once.
use super::*;

/// The stored games whose rounds were never read (`?1` names none).
const UNREAD: &str =
    "SELECT COUNT(*) FROM archive_games WHERE rounds_read_at IS NULL AND digest != ?1";

/// Match `n` of Player 1 and Player 2, first to two, saved at `saved_at`:
/// its rounds started as `starts`, and its record names `recorded` as the
/// winner (none: the record Ember makes up, which names nobody).
fn scored(n: u8, starts: &[[u32; 2]], recorded: Option<u8>, saved_at: u32) -> Vec<u8> {
    replays::MadeUpMatch {
        starts: starts.to_vec(),
        setting: 3,
        recorded,
        inputs: u32::from(n),
    }
    .file(saved_at, [1, 2])
}

/// A new game's rounds are listed with it, none when its replay does not
/// say who won; the other player's copy of a game is the same game, with the
/// same rounds. Each was read as it came.
#[tokio::test]
async fn a_games_rounds_are_listed_with_it() {
    let f = fixture().await;
    let swept = scored(1, &[[0, 0], [0, 1]], None, 1_800_000_000);
    let deciding = scored(2, &[[0, 0], [1, 0], [1, 1]], Some(0), 1_800_000_100);
    let undecided = scored(3, &[[0, 0], [1, 0], [1, 1]], None, 1_800_000_200);
    let too_short = game(4, [20, 34]);
    for file in [&swept, &deciding, &undecided, &too_short] {
        let sent = send(&f, &f.a, file, "a=Player1&b=&side=0").await;
        assert_eq!(sent, StatusCode::CREATED);
    }
    let theirs = copy_from(&deciding, 1);
    assert_ne!(digest(&theirs), digest(&deciding));
    assert_eq!(Replay::parse(&theirs).unwrap().rounds, Some([2, 1]));
    assert_eq!(
        send(&f, &f.b, &theirs, "a=&b=Player2&side=1").await,
        StatusCode::OK
    );
    let list = listed_when(&f, "", |list| games(list).len() == 4).await;
    assert_eq!(listed_game(&list, &swept)["rounds"], json!([0, 2]));
    assert_eq!(listed_game(&list, &deciding)["rounds"], json!([2, 1]));
    assert_eq!(
        listed_game(&list, &deciding)["names"],
        json!(["Player1", "Player2"])
    );
    assert_eq!(listed_game(&list, &undecided)["rounds"], Json::Null);
    assert_eq!(listed_game(&list, &too_short)["rounds"], Json::Null);
    assert_eq!(count(&f, UNREAD, "").await, 0);
}

/// A set's game carries its rounds as an online game does.
#[tokio::test]
async fn a_sets_game_carries_its_rounds() {
    let f = fixture().await;
    let id = set(&f, "rounds", 1).await;
    let set_copy = scored(5, &[[0, 0], [1, 0]], None, f.bridge.started_at + 1);
    assert_eq!(
        send_to_set(&f, &f.a, &id, &set_copy, "side=0").await,
        StatusCode::CREATED
    );
    let list = listed_when(&f, "", |list| games(list).len() == 1).await;
    let listed = listed_game(&list, &set_copy);
    assert_eq!(listed["set"], id.as_str());
    assert_eq!(listed["rounds"], json!([2, 0]));
}

/// Keeps `file` as a bridge before migration 067 kept a stored game, its
/// rounds never read, with the bucket holding `held` under its name.
async fn kept_before(f: &Fixture, file: &[u8], held: Option<&[u8]>) {
    if let Some(held) = held {
        let path = format!("/replays/games/{}.emberreplay", digest(file));
        f.stored.lock().unwrap().push((path, held.to_vec()));
    }
    let replay = Replay::parse(file).unwrap();
    let size = file.len();
    f.bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO archive_games (digest, crc, saved_at, size, fighter_a, fighter_b, sent_by, priority,
                    state, next_attempt_at, received_at, stored_at, game)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, 'emb_player1', 1, 'stored', 0, ?3, ?3, ?7)",
                rusqlite::params![
                    replay.digest_hex(),
                    replay.crc_name(),
                    replay.saved_at,
                    size,
                    replay.fighters[0],
                    replay.fighters[1],
                    replay.game_hex(),
                ],
            )?;
            Ok(())
        })
        .await
        .unwrap();
}

/// The games kept before rounds were are read back from the bucket: a
/// bucket that does not answer leaves them for a later pass; one that does
/// gives each its rounds, or none when its replay does not say, the bucket no
/// longer holds it or holds another replay under its name. A game is read
/// once.
#[tokio::test]
async fn games_kept_before_rounds_are_read_back_once() {
    let f = fixture().await;
    // From before they are kept, so no pass the bridge runs itself reads
    // them before this test does.
    f.refusing.store(true, Ordering::SeqCst);
    let won = scored(6, &[[0, 0], [1, 0]], None, 1_800_000_000);
    let unscored = played_game(7);
    let missing = scored(8, &[[0, 0], [0, 1]], None, 1_800_000_100);
    let replaced = scored(9, &[[0, 0], [0, 1]], None, 1_800_000_200);
    kept_before(&f, &won, Some(&won)).await;
    kept_before(&f, &unscored, Some(&unscored)).await;
    kept_before(&f, &missing, None).await;
    kept_before(&f, &replaced, Some(&won)).await;
    ember_bridge::read_archive_rounds(f.bridge.state()).await;
    assert_eq!(count(&f, UNREAD, "").await, 4);
    f.refusing.store(false, Ordering::SeqCst);
    ember_bridge::read_archive_rounds(f.bridge.state()).await;
    assert_eq!(count(&f, UNREAD, "").await, 0);
    let list = listed(&f, "").await;
    assert_eq!(listed_game(&list, &won)["rounds"], json!([2, 0]));
    for file in [&unscored, &missing, &replaced] {
        assert_eq!(listed_game(&list, file)["rounds"], Json::Null);
    }
    // The bucket given the right file later changes nothing.
    let path = format!("/replays/games/{}.emberreplay", digest(&replaced));
    f.stored.lock().unwrap().push((path, replaced.clone()));
    ember_bridge::read_archive_rounds(f.bridge.state()).await;
    let list = listed(&f, "").await;
    assert_eq!(listed_game(&list, &replaced)["rounds"], Json::Null);
}

/// Games the bucket keeps failing to give back, more than a pass takes and
/// newer than the rest, do not hold up the older games: each pass goes on
/// below the last, and the failing ones are tried again next sweep.
#[tokio::test]
async fn games_that_keep_failing_do_not_hold_up_the_others() {
    let f = fixture().await;
    let failing = ember_bridge::ARCHIVE_ROUNDS_PER_PASS as u32 + 1;
    let too_long = vec![0; replays::MAX_REPLAY + 1];
    for n in 0..failing {
        let file = scored(n as u8, &[[0, 0], [0, 1]], None, 1_800_001_000 + n);
        kept_before(&f, &file, Some(&too_long)).await;
    }
    let won = scored(200, &[[0, 0], [1, 0]], None, 1_800_000_000);
    kept_before(&f, &won, Some(&won)).await;
    ember_bridge::read_archive_rounds(f.bridge.state()).await;
    assert_eq!(count(&f, UNREAD, "").await, i64::from(failing));
    let list = listed(&f, "").await;
    assert_eq!(listed_game(&list, &won)["rounds"], json!([2, 0]));
}

/// The winner of a deciding round is the slot record's, which no hash of the
/// replay covers: two copies of one game that name different winners leave
/// it no rounds, whichever came first, and no later copy or read gives them
/// back. A copy that says nothing contradicts nothing.
#[tokio::test]
async fn copies_that_disagree_leave_a_game_no_rounds() {
    let f = fixture().await;
    let deciding = [[0, 0], [1, 0], [1, 1]];
    let claimed = scored(10, &deciding, Some(1), 1_800_000_000);
    let honest = copy_from(&scored(10, &deciding, Some(0), 1_800_000_000), 1);
    assert_eq!(Replay::parse(&claimed).unwrap().rounds, Some([1, 2]));
    assert_eq!(Replay::parse(&honest).unwrap().rounds, Some([2, 1]));
    assert_eq!(
        Replay::parse(&claimed).unwrap().game,
        Replay::parse(&honest).unwrap().game
    );
    let swept = scored(11, &[[0, 0], [0, 1]], None, 1_800_000_100);
    // Its record names the other winner than its rounds: it says nothing.
    let says_nothing = copy_from(&scored(11, &[[0, 0], [0, 1]], Some(0), 1_800_000_100), 1);
    assert_eq!(Replay::parse(&says_nothing).unwrap().rounds, None);
    for (by, file, sent) in [
        (&f.b, &claimed, StatusCode::CREATED),
        (&f.a, &honest, StatusCode::OK),
        (&f.a, &copy_from(&claimed, 2), StatusCode::OK),
        (&f.a, &swept, StatusCode::CREATED),
        (&f.b, &says_nothing, StatusCode::OK),
    ] {
        assert_eq!(send(&f, by, file, "").await, sent);
    }
    let list = listed_when(&f, "", |list| games(list).len() == 2).await;
    assert_eq!(listed_game(&list, &claimed)["rounds"], Json::Null);
    assert_eq!(listed_game(&list, &swept)["rounds"], json!([0, 2]));
    // Stored, they are read already: the backfill reads neither back.
    assert_eq!(count(&f, UNREAD, "").await, 0);
    ember_bridge::read_archive_rounds(f.bridge.state()).await;
    let list = listed(&f, "").await;
    assert_eq!(listed_game(&list, &claimed)["rounds"], Json::Null);
}

/// A set's copy that names another winner than its own players' online copy
/// of the game, folded into it, leaves the set's game no rounds.
#[tokio::test]
async fn a_sets_copy_that_disagrees_with_a_folded_copy_has_no_rounds() {
    let f = fixture().await;
    let id = set(&f, "disputed", 1).await;
    let deciding = [[0, 0], [1, 0], [1, 1]];
    let saved_at = f.bridge.started_at + 1;
    let online = copy_from(&scored(12, &deciding, Some(0), saved_at), 1);
    let set_copy = scored(12, &deciding, Some(1), saved_at);
    assert_eq!(
        send(&f, &f.b, &online, "a=&b=Player2&side=1").await,
        StatusCode::CREATED
    );
    assert_eq!(
        send_to_set(&f, &f.a, &id, &set_copy, "side=0").await,
        StatusCode::CREATED
    );
    let list = listed_when(&f, "", |list| {
        games(list).len() == 1 && games(list)[0]["set"] == id.as_str()
    })
    .await;
    assert_eq!(listed_game(&list, &set_copy)["rounds"], Json::Null);
}
