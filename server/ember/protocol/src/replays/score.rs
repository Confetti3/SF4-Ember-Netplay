//! A replay's final round score, read as the game reads it (`Parse` and
//! `Score` in `src/common/ReplayInputs.hxx`, whose layout comment says where
//! each value is). The body says how many rounds each side had won as each
//! round started, but not who won the last one: that follows from the rounds
//! when only one side was a round short of the match, and for a deciding
//! round it is the winner the slot record names, which only a record the game
//! wrote does (`RecordWinner` in `src/common/ReplaySlots.hxx`). A record that
//! names a winner the rounds disagree with leaves the score unknown.
use super::{BODY_PLAYED_FROM, ROUND_RECORD, u32_at};

/// Where the body says how it is laid out, and the layout read here: u16 1,
/// u16 8. The native online service's replays (3, 3) are laid out otherwise.
pub(super) const LAYOUT_AT: usize = 0x08;
pub(super) const LAYOUT: [u8; 4] = [1, 0, 8, 0];
/// The number of rounds played.
pub(super) const ROUNDS_PLAYED: usize = 0x18;
/// The rounds setting: 1, 3, 5, 7, 15 or 99, a match first to half of it,
/// rounded up.
pub(super) const ROUNDS_SETTING: usize = 0x2E0;
const SETTINGS: [u32; 6] = [1, 3, 5, 7, 15, 99];
/// In each round's record, from `BODY_PLAYED_FROM`: the rounds each side had
/// won as it started, P1 first, and the bytes of its input stream. The
/// streams follow the last record, back to back, to the end of the body.
pub(super) const WINS: [usize; 2] = [0x1C, 0x58];
pub(super) const STREAM_BYTES: usize = 0x7C;
/// A stream is 3-byte records, one a frame, or, with `REPEAT` set, how many
/// more frames the one before lasted.
pub(super) const FRAME_RECORD: usize = 3;
const REPEAT: u32 = 0x40_0000;
/// The most frames the game reads a match to.
const MOST_FRAMES: u64 = 0x7FFF_FFFF;
/// In the slot record: the length of its title, which only a record Ember
/// made up has, and the winner the game wrote, 0 for P1 or 1 for P2.
pub(super) const TITLE: usize = 22;
pub(super) const WINNER: usize = 49;

/// The rounds each side won, P1 first, when `body` is laid out as the game
/// reads it and its rounds say who won, with `record`'s winner for a
/// deciding round.
pub(super) fn rounds(record: &[u8], body: &[u8]) -> Option<[u8; 2]> {
    let starts = round_starts(body)?;
    let setting = u32_at(body, ROUNDS_SETTING);
    let first_to = SETTINGS.contains(&setting).then_some(setting.div_ceil(2))?;
    score(&starts, first_to, recorded_winner(record))
}

/// What each side had won as each round started, when the body is laid out
/// as the game reads it and its sizes add up: its round records lie inside
/// it, their streams are whole frame records that fill the rest of it, and
/// the frames they hold are no more than the game reads.
fn round_starts(body: &[u8]) -> Option<Vec<[u32; 2]>> {
    if body.len() < BODY_PLAYED_FROM || body[LAYOUT_AT..LAYOUT_AT + 4] != LAYOUT {
        return None;
    }
    let played = u32_at(body, ROUNDS_PLAYED) as usize;
    if played == 0 || played > (body.len() - BODY_PLAYED_FROM) / ROUND_RECORD {
        return None;
    }
    let (records, streams) = body[BODY_PLAYED_FROM..].split_at(played * ROUND_RECORD);
    let records: Vec<&[u8]> = records.chunks(ROUND_RECORD).collect();
    let stream_bytes: Vec<u64> = records
        .iter()
        .map(|round| u64::from(u32_at(round, STREAM_BYTES)))
        .collect();
    let whole = stream_bytes
        .iter()
        .all(|bytes| bytes % FRAME_RECORD as u64 == 0)
        && stream_bytes.iter().sum::<u64>() == streams.len() as u64;
    if !whole {
        return None;
    }
    let frames: u64 = streams
        .as_chunks::<FRAME_RECORD>()
        .0
        .iter()
        .map(|&[low, middle, high]| {
            let value = u32::from_le_bytes([low, middle, high, 0]);
            if value & REPEAT == 0 {
                1
            } else {
                u64::from(value & (REPEAT - 1))
            }
        })
        .sum();
    (frames <= MOST_FRAMES).then(|| {
        records
            .iter()
            .map(|round| WINS.map(|at| u32_at(round, at)))
            .collect()
    })
}

/// The final score of a match first to `first_to` whose rounds started with
/// `starts`, P1 first, and whose record names `recorded` as its winner (if
/// any): none when the rounds cannot have been played (the first starts at
/// 0-0, each round gives each side one win or none, a draw giving both, and
/// nobody has the match before the last round), when nobody can win the last
/// round, when it was a deciding round and no record names its winner, and
/// when a record names another winner than the rounds do.
fn score(starts: &[[u32; 2]], first_to: u32, recorded: Option<usize>) -> Option<[u8; 2]> {
    let last = first_to - 1;
    if starts.first()? != &[0, 0] {
        return None;
    }
    let mut before = [0, 0];
    for start in starts {
        for (&wins, &had) in start.iter().zip(&before) {
            if wins > last || wins.checked_sub(had)? > 1 {
                return None;
            }
        }
        before = *start;
    }
    let winner = match before.map(|wins| wins == last) {
        [true, false] => 0,
        [false, true] => 1,
        [true, true] => recorded?,
        [false, false] => return None,
    };
    if recorded.is_some_and(|named| named != winner) {
        return None;
    }
    before[winner] += 1;
    // At most 50, the most rounds the game's settings play to.
    Some(before.map(|wins| wins as u8))
}

/// The winner a record the game wrote names; none for a record Ember made up
/// (it has a title) or any other value.
fn recorded_winner(record: &[u8]) -> Option<usize> {
    (u32_at(record, TITLE) == 0 && record[WINNER] <= 1).then(|| usize::from(record[WINNER]))
}

#[cfg(test)]
mod tests {
    use super::super::{CRC, HEAD, MadeUpMatch, Replay, SIZE, crc32, made_up};
    use super::*;

    const SAVED_AT: u32 = 1_800_000_000;

    fn file(starts: &[[u32; 2]], setting: u32, recorded: Option<u8>) -> Vec<u8> {
        MadeUpMatch {
            starts: starts.to_vec(),
            setting,
            recorded,
            inputs: 0x10,
        }
        .file(SAVED_AT, [20, 34])
    }

    /// The rounds of `file`, which reads as a replay whatever they are.
    fn rounds_of(file: &[u8]) -> Option<[u8; 2]> {
        Replay::parse(file).expect("a whole replay").rounds
    }

    fn played(starts: &[[u32; 2]], setting: u32, recorded: Option<u8>) -> Option<[u8; 2]> {
        rounds_of(&file(starts, setting, recorded))
    }

    /// `file` with the four bytes of its body at `at` set to `value`.
    fn with_u32(mut file: Vec<u8>, at: usize, value: u32) -> Vec<u8> {
        file[HEAD + at..HEAD + at + 4].copy_from_slice(&value.to_le_bytes());
        file
    }

    /// `file` with its record's size and CRC made right for its body again.
    fn resealed(mut file: Vec<u8>) -> Vec<u8> {
        let (size, crc) = ((file.len() - HEAD) as u32, crc32(&file[HEAD..]));
        file[8 + SIZE..8 + SIZE + 4].copy_from_slice(&size.to_le_bytes());
        file[8 + CRC..8 + CRC + 4].copy_from_slice(&crc.to_le_bytes());
        file
    }

    /// `file`, a match of two rounds, with `extra` after its last round's
    /// stream, which counts it when `counted`.
    fn with_stream(file: Vec<u8>, extra: &[u8], counted: bool) -> Vec<u8> {
        let at = BODY_PLAYED_FROM + ROUND_RECORD + STREAM_BYTES;
        let bytes = u32_at(&file[HEAD..], at) + if counted { extra.len() as u32 } else { 0 };
        let mut file = with_u32(file, at, bytes);
        file.extend_from_slice(extra);
        resealed(file)
    }

    /// From the rounds when one side was a round short, from the game's
    /// record for a deciding round, and unknown without that record.
    #[test]
    fn the_score_is_known_only_when_the_last_rounds_winner_is() {
        // Player 2 took both.
        assert_eq!(played(&[[0, 0], [0, 1]], 3, None), Some([0, 2]));
        // A deciding round: Ember's made-up record names nobody.
        let deciding = [[0, 0], [1, 0], [1, 1]];
        assert_eq!(played(&deciding, 3, None), None);
        assert_eq!(played(&deciding, 3, Some(1)), Some([1, 2]));
        assert_eq!(played(&deciding, 3, Some(0)), Some([2, 1]));
        // First to three.
        assert_eq!(played(&[[0, 0], [1, 0], [2, 0]], 5, None), Some([3, 0]));
        // A draw gives both a round; the round after it decides.
        assert_eq!(played(&[[0, 0], [1, 1]], 3, Some(1)), Some([1, 2]));
        // Nobody a round short of the match: nobody can win this round.
        assert_eq!(played(&[[0, 0], [1, 0]], 5, None), None);
    }

    /// A record whose winner the rounds disagree with leaves the score
    /// unknown, as does one that names a winner before anyone can finish.
    #[test]
    fn a_record_that_disagrees_with_the_rounds_names_no_winner() {
        assert_eq!(played(&[[0, 0], [1, 0]], 3, Some(1)), None);
        assert_eq!(played(&[[0, 0], [1, 0]], 3, Some(0)), Some([2, 0]));
        assert_eq!(played(&[[0, 0], [0, 1]], 3, Some(0)), None);
        assert_eq!(played(&[[0, 0]], 5, Some(1)), None);
        // A record value that is no side names nobody.
        assert_eq!(played(&[[0, 0], [1, 1]], 3, Some(2)), None);
    }

    /// Rounds that cannot have been played: more won than the match has, a
    /// match that starts a round up, a count that goes down, two rounds won in
    /// one, and a count past what a signed one holds.
    #[test]
    fn rounds_that_cannot_have_been_played_have_no_score() {
        let impossible: [(&[[u32; 2]], u32); 6] = [
            (&[[0, 0], [3, 0]], 3),
            (&[[1, 0]], 3),
            (&[[0, 0], [1, 0], [0, 1]], 3),
            (&[[0, 0], [2, 0]], 5),
            (&[[0, 0], [0x8000_0000, 0]], 5),
            (&[[0, 0], [1, 0], [u32::MAX, 1]], 5),
        ];
        for (starts, setting) in impossible {
            for recorded in [None, Some(0), Some(1)] {
                assert_eq!(played(starts, setting, recorded), None, "{starts:?}");
            }
        }
    }

    /// Only the game's own settings say what a match is played to.
    #[test]
    fn only_the_games_settings_score() {
        for setting in [0, 2, 4, 9, 100, u32::MAX] {
            assert_eq!(played(&[[0, 0], [1, 0]], setting, None), None, "{setting}");
        }
        for setting in SETTINGS {
            let first_to = setting.div_ceil(2);
            let starts: Vec<[u32; 2]> = (0..first_to).map(|won| [won, 0]).collect();
            let expected = Some([first_to as u8, 0]);
            assert_eq!(played(&starts, setting, Some(0)), expected, "{setting}");
        }
        // A match of one round is all deciding round.
        assert_eq!(played(&[[0, 0]], 1, None), None);
    }

    /// The body's sizes must add up as the game's reader adds them; a replay
    /// whose rounds cannot be read still reads, with no score.
    #[test]
    fn a_body_whose_sizes_do_not_add_up_has_no_score() {
        let good = file(&[[0, 0], [0, 1]], 3, None);
        assert_eq!(rounds_of(&good), Some([0, 2]));
        let first_stream = BODY_PLAYED_FROM + STREAM_BYTES;
        let broken = [
            (
                "another layout",
                with_u32(good.clone(), LAYOUT_AT, 0x0003_0003),
            ),
            ("no rounds", with_u32(good.clone(), ROUNDS_PLAYED, 0)),
            (
                "more rounds than records",
                with_u32(good.clone(), ROUNDS_PLAYED, 3),
            ),
            (
                "streams one byte short",
                with_u32(good.clone(), first_stream, 2),
            ),
            (
                "streams one frame long",
                with_u32(good.clone(), first_stream, 6),
            ),
            (
                "a byte past the streams",
                with_stream(good.clone(), &[0], false),
            ),
            (
                "a stream of a part frame",
                with_stream(good.clone(), &[0], true),
            ),
        ];
        for (what, file) in broken {
            assert_eq!(rounds_of(&resealed(file)), None, "{what}");
        }
        // The most frames the game reads, and one repeat past them.
        let most = (MOST_FRAMES / u64::from(REPEAT - 1)) as usize;
        let repeat = (REPEAT | (REPEAT - 1)).to_le_bytes();
        let long = |repeats: usize| repeat[..FRAME_RECORD].repeat(repeats);
        assert_eq!(
            rounds_of(&with_stream(good.clone(), &long(most), true)),
            Some([0, 2])
        );
        assert_eq!(rounds_of(&with_stream(good, &long(most + 1), true)), None);
        // A replay too short for a round's record has no score either.
        assert_eq!(rounds_of(&made_up(b"a game", SAVED_AT, [1, 2])), None);
    }

    /// Both players' copies of one game: their headers differ where each PC
    /// writes its own values, their scores do not.
    #[test]
    fn both_players_copies_have_one_score() {
        let mine = file(&[[0, 0], [1, 0], [1, 1]], 3, Some(0));
        let mut theirs = mine.clone();
        theirs[HEAD + 0x10] ^= 1;
        let theirs = resealed(theirs);
        let [mine, theirs] = [&mine, &theirs].map(|file| Replay::parse(file).unwrap());
        assert_ne!(mine.digest, theirs.digest);
        assert_eq!(mine.game, theirs.game);
        assert_eq!((mine.rounds, theirs.rounds), (Some([2, 1]), Some([2, 1])));
    }
}
