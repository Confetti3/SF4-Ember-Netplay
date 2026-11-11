# Fighter points

A points total, a 3- to 5-digit number, for each fighter a player has used in
rated public room games, in the spirit of USF4's own PP and BP numbers. It is a
grind value to show off, not skill: it only goes up, has no cap, nothing in a
rating is read to make it, and nothing in a rating is moved by it (ranked play
is untouched). The one skill mark stays the account's class
(`SKILL_BADGE.md`); there are no per-fighter ranks and no rating points are
shown. This replaces `MASTERY.md` (pull request 183): its points stay, its ten
named tiers and 30 emblems become a number in the game's own digits on a frame
coloured by the five award tiers. An earlier draft of this document gave each
fighter a level from 1 to 60; the points replace it, and the file keeps its
name (the `fighter_levels` table and the C++ and Rust type names keep theirs; the
wire says "points" throughout).

Where points show and how the card is laid out is `PLAYER_CARD.md`; who sees
what is settled here (Privacy).

## Rules

- A game counts when a public room host reports it won (the games that move a
  rating today, `RoomAuthority::RecordWonGame`), both fighters' Discord
  accounts are known and differ, and the host reported both fighters.
- The winner's fighter gains a game and a win, the loser's fighter a game.
  Points are `games + wins`, so a loss is 1 and a win 2, as `MASTERY.md` had.
- Past eight games between the same two accounts in one sitting (a sitting
  ends after three hours without a game between them) a game counts for
  nothing, for both. This is the repeat cap of `SKILL_BADGE.md`, now counted
  for every pair of accounts, not only pairs that both take part in ranking
  (Bridge, `record_games`).
- Nothing else weighs a game: no opponent strength, no lopsided-game or
  unsure-rating weight.
- Taking part in ranking does not change what counts. A player who stopped
  taking part still earns points (USF4 keeps every player's per-character
  record the same way); whether others see them is Privacy.
- No history is backfilled: no stored game names its fighters, so every
  account starts with no points on every fighter when the bridge and the 1.6.0
  room host are both live.

### Points and tiers

A fighter's **points** for a Discord account are `games + wins` of its counted
games: a win adds 2, a loss 1. They never go down and have no cap. A fighter
with no counted game has no points and is not listed.

The **tier** is the frame's colour, from the five award tiers (`v2::Tier`,
`v2::TierColor` in `src/ui/V2.cxx`), by points:

| Tier | Frame | From points | Games at 1.5 points a game |
|---|---|---|---|
| 0 | Ash | 0 | the first |
| 1 | Copper | 200 | about 133 |
| 2 | Silver | 700 | about 467 |
| 3 | Gold | 1,500 | about 1,000 |
| 4 | Ember | 2,500 | about 1,667 |

The thresholds live only in the bridge, one constant table
(`skill/levels.rs`, `TIER_STARTS`): a client is sent each fighter's points, its
tier and the points where that tier and the next one begin, so the thresholds can be
retuned after real play without a client update or a data change (games and
wins are stored, never points or tiers). A client never computes a tier. The
number is drawn on dark ground; the tier colours only the frame. Points show
up to 999,999 (six digits).

## Room host: what it must report

Today a won game is `{table, generation, winner, loser}`
(`src/common/SkillBadge.hxx` `WonGame`, `SkillBadgeJson.hxx`), and the bridge
keys rated games on `(room, table, generation)` (`rated_games`,
`skill::record_games`).

Two additions:

1. **Fighters.** `RecordWonGame` takes `Member::fighter` of `table.p1` and
   `table.p2` (`src/session/RoomTypes.hxx`: the fighter shared through the
   pre-battle message, `RoomAuthority::SetMemberChara`; a Random pick has
   already resolved there, `RandomFighterPick.hxx`). Written as
   `"fighters":[winner, loser]` only when both are 0 to 43
   (`FighterCount`, `src/common/FighterCatalog.hxx`).
2. **The game in its battle.** A rematch in place keeps the generation
   (`RoomModelActions.cxx`, the `continued` branch: `RecordWonGame(id, *table,
   action.result)` with `table->matchGeneration` unchanged; `rematch::
   GoesOnInPlace` makes it the normal path when both pick Rematch with the
   same fighter). Every game after the first of such a battle reuses the key
   and is dropped by `INSERT OR IGNORE` in `record_games`: today those games
   are not rated, and they would earn no points. `WonGame` gains `game`, the
   games before it in the battle (`Action::game`; `EndMatch` gets the agreed
   result's game passed down), written only when not zero, so a battle of one
   game reads as before. The bridge keys on `(room, table, generation, game)`.
   Rating these games is a behaviour change to `SKILL_BADGE.md`; see Open
   questions.

The status line becomes, per game:
`{"table":n,"generation":n,"winner":s,"loser":s,"game":n,"fighters":[n,n]}`,
`game` and `fighters` optional. The checkpoint's `won_games` takes the same
optional members, so a checkpoint from before restores.

### The games cap

The largest game is now 213 bytes (table 7, generation `UINT64_MAX`, two
57-byte Ember IDs, game 1000, fighters 43 and 43): 32 of them are 6,849 bytes,
over the 6,144 enforced together by `SupervisorGamesBytes`
(`server/roomhost/RoomHostStatus.hxx`), `MAX_GAMES_BYTES`
(`server/ember-rooms/src/protocol.rs`) and `MAX_GAMES_REPORT`
(`server/ember/bridge/src/routes/rooms/supervisor.rs`). All three become
8,192; `the_limits_agree_with_the_room_host_and_the_bridge` (ember-rooms)
keeps them equal and `TestTheLargestLineFitsTheSupervisorsLimits`
(`src/tests/room_host_config_test.cxx`) builds the new worst case. The line
stays far under `MaximumStatusLineBytes` (about 44 KB of 60,000).

`MaximumWonGames` stays 32: lowering it would refuse checkpoints written
before (`LoadServerOwned` checks the size).

**Deploy order.** Supervisor and bridge first, then publish the room host. An
older supervisor drops a whole games array over 6,144 bytes, which a busy
1.6.0 room could reach, and its games would be neither rated nor counted.

### Older room hosts

The bridge reads `game` and `fighters` as optional (`Game` in
`skill/mod.rs` does not deny unknown fields, so an older bridge ignores them).
A room of an older build reports neither: its games are rated exactly as
before (game 0) and earn no points. Players are matched by build, so a 1.5.x
player only ever plays in 1.5.x rooms and never sees any points.

## Bridge

### Migration 057 (`057_fighter_levels.sql`)

The file is also named in the list in `src/db/migrations.rs`, which records
each migration by file name and checksum.

```sql
-- Each fighter's counted games for a Discord account (FIGHTER_LEVELS.md).
-- Points are games + wins; the points and tier are worked out when served.
CREATE TABLE fighter_levels (
    discord_user_id TEXT NOT NULL,
    fighter INTEGER NOT NULL CHECK (fighter BETWEEN 0 AND 43),
    games INTEGER NOT NULL,
    wins INTEGER NOT NULL CHECK (wins <= games),
    first_at INTEGER NOT NULL,
    played_at INTEGER NOT NULL,
    PRIMARY KEY (discord_user_id, fighter)
) WITHOUT ROWID;

-- A game's place in its battle joins the key (rematches in place).
CREATE TABLE rated_games_next (
    room_id TEXT NOT NULL REFERENCES rooms(room_id) ON DELETE CASCADE,
    table_id INTEGER NOT NULL,
    generation INTEGER NOT NULL,
    game INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (room_id, table_id, generation, game)
) WITHOUT ROWID;
INSERT INTO rated_games_next SELECT room_id, table_id, generation, 0 FROM rated_games;
DROP TABLE rated_games;
ALTER TABLE rated_games_next RENAME TO rated_games;

-- A pair's lifetime record, in a table of its own. The sitting stays in
-- `rated_pairs`, whose rows a bridge from before this migration deletes
-- three hours after a pair's last game: after a rollback that delete would
-- have wiped win columns kept there.
CREATE TABLE pair_records (
    first TEXT NOT NULL, second TEXT NOT NULL,
    first_wins INTEGER NOT NULL, second_wins INTEGER NOT NULL,
    played_at INTEGER NOT NULL,
    PRIMARY KEY (first, second)
) WITHOUT ROWID;
```

Indexes: `fighter_levels` is read only by account (its key). `pair_records`
is read by pair (its key); friends read it by pair too (`FRIENDS.md`).

Row lifetimes: `fighter_levels` and `pair_records` for good (at most 44 rows
per account; one per pair that ever played); `rated_games` with its room row,
and `rated_pairs` (the sitting) for three hours after a pair's last game, both
as before. The sitting now covers every pair of Discord accounts.

### `record_games` (`server/ember/bridge/src/skill/mod.rs`)

One pass per new game, in this order, so the rating keeps its own rules and
points never read them:

1. Key `(room, table, generation, game)` into `rated_games`; seen before:
   skip.
2. Both Discord accounts (`account`, any account, not `rated_account`); none
   or the same: skip.
3. The pair's sitting (`rated_pairs`, as before: a row older than three hours
   is deleted, then `games` adds 1) and its record (`pair_records`): the
   winner's side of `first_wins`/`second_wins` adds 1; `played_at` is now.
4. With `fighters` and a sitting count of at most `FULL_REPEATS`: upsert both
   rows of `fighter_levels` (`skill/levels.rs`).
5. Both accounts take part in ranking (`ranking_choices`): rate as today,
   with the sitting weight from step 3.

A game where one side does not take part now counts toward the pair's
sitting, which only matters for a pair whose choice changes mid-sitting. A
game whose `fighters` is present but is not two fighters is skipped whole, as
any game the bridge cannot read. Whether games after a battle's first move
ratings is one constant, `RATE_GAMES_AFTER_THE_FIRST`.

## Wire contracts

Capability `points` (`ember_protocol::levels::POINTS_FEATURE`), offered
wherever public rooms are on (`routes/discovery.rs`, beside `ranking`).
Requests refuse unknown members and answers ignore them, so a field a later
bridge adds does not break a build already out (`levels.rs` header). A build
asks only a bridge that lists `points`, and an older bridge answers 404, which the helper words as
`points_unavailable` (as `ranking_unavailable`, `tournament/ranking.rs`).

### `GET /v1/points/me`

Player session (`routes::play::playing`), bucket `points:{ember_id}` 60 per
10 minutes shared with card look-ups.

```json
{"points":"no_account"}
{"points":"counted","shown":true,"fighters":[
  {"fighter":3,"points":1986,"games":1284,"wins":702,"tier":3,"floor":1500,"next":2500,"since":1729468800}]}
```

| Field | Rule |
|---|---|
| `points` | `no_account` (no Discord account on the Ember ID) or `counted` |
| `shown` | others see this account's points (Privacy); written only when true |
| `fighters` | played fighters only, ascending by `fighter`, unique, at most 44 |
| `fighter` | 0 to 43 |
| `points` | `games + wins`, at least 1 |
| `games`, `wins` | counted games; `wins <= games` |
| `tier` | 0 to 4, the frame's colour (Points and tiers) |
| `floor` | the points where `tier` begins, at most `points`; 0 at tier 0; always present (the meter runs from `floor` to `next`) |
| `next` | the points where the next tier begins; absent exactly when `tier` is 4; otherwise `points < next` |
| `since` | `first_at`, unix seconds |

Read only: nothing is remembered as shown.

### The card token in the room ticket

A ticket is signed when a player asks to join, before any fighter is picked
(`routes/rooms/ticket.rs`), and lives 60 seconds. It cannot carry the points of
the fighter they will play, nor anything that changes while they stay. What it
carries instead is a **card token**: an opaque, bridge-sealed name for the
player's Discord account that the room shows its members, so any member can
ask the bridge for that player's card later, as often as it needs, fresh.

- `TicketRequest::card: bool` (written only when true). A build sends it only
  to a bridge that lists `points` (`tournament/play.rs` `Features`), since an
  older one refuses the field.
- `RoomTicket::card: Option<String>` (written only when present), for a
  joiner who asked and has a Discord account. A ticket without one is byte
  for byte what it was. The bridge issues tickets only for rooms of the
  joiner's own build, so a ticket with `card` never reaches a helper that
  refuses it (the argument of `SKILL_BADGE.md`, Deploying).
- Token: `c1.` and base64url (no padding) of `Keys::seal` over 17 bytes:
  purpose byte `0x01`, the Discord user ID as a little-endian u64, the issue
  time as a little-endian u64. 76 characters after the prefix; checked as at
  most 96 characters of the base64url alphabet. A fresh nonce each ticket, so
  tokens from two joins cannot be matched by anyone but the bridge. Valid for
  7 days from issue (`cards::open` refuses older).
- The room host's helper reports it on `connected` (`card`, beside `account`
  and `badge`, `rust/sf4-net/src/service/protocol.rs`); the session server
  hands it to the room model in the `Admission` (`src/session/Admission.hxx`),
  never from the joiner's own words; the snapshot carries it as
  `Member::card` (written only when set). The client drops it in any room it
  did not join as public, as it drops classes
  (`sf4e__NetplayRuntime__Status.cxx`, `ShownRoomOf`).

### `POST /v1/cards/lookup`

```json
{"cards":[{"card":"c1.…"},{"friend":"fr_…"}]}
{"cards":[{"fighters":[{"fighter":3,"points":1986,"tier":3}],"relation":"none"},null]}
```

| Field | Rule |
|---|---|
| request `cards` | 1 to 16 selectors: `card` (a token) or `friend` (a friend id, `FRIENDS.md`) |
| answer `cards` | one entry per selector, in order; `null` for a token that does not open, is over 7 days old, or names an account without a Discord account now, and for a friend id that is not the caller's friend |
| `fighters` | as in `points/me` but `fighter`, `points`, `tier` only; absent when the player does not show points |
| `relation` | the caller's relation to that player: `none`, `friend`, `sent`, `received`, `blocked` (the caller blocked them); `none` for a player who blocked the caller |

The bridge serves `relation` once `friends` is offered; before that it is
always `none`.

## Privacy

- Your own points: always, to you.
- Others' points: shown to a room member (through the card token) and to a
  friend while that player takes part in ranking (`ranking_choices.ranked`,
  default true). A player who stopped taking part keeps earning points,
  shows none to others, and sees on their Fighters tab that others do not see
  them. This answers `MASTERY.md`'s open question; see Open questions.
- Others see a fighter's points and tier, never wins, games or win rate.
  Points minus games would be wins, which is why the card carries the points
  only.
- No private room shows points: its host writes its own snapshot, so a token
  there is dropped like a class.
- A card token says nothing by itself; it is unreadable without the bridge's
  key and unlinkable between joins. It is no more than a room member can see.

## Client

- `netplay::levels::Own` (`src/netplay/Levels.hxx`): `unavailable`,
  `no_account` or counted, with an array of 44 `{points, games, wins, tier,
  floor, next, since}` (points 0 for unplayed) and `shown`. Read on Profile's open, at
  start, and twice after each set this PC fought (8 and 20 seconds after the
  set's end, since the bridge polls rooms every 5 seconds,
  `routes/rooms/poll.rs` `POLL_SECS`). Kept per bridge and Ember ID, as
  rankings are (`RankingRequests.hxx`).
- `netplay::levels::Cards`: token to card, refreshed when a member with a
  token joins and 8 seconds after any set in the room ends, at most one
  look-up of up to 16 tokens every 10 seconds.
- A tier change is found by comparing the new `Own` with the one before the set;
  `PLAYER_CARD.md` draws it.

### Drawing points

Points are drawn as the game draws its own PP and BP: in the game's digits, read
from the player's installed game at runtime and never shipped, as the class
emblems are. They replace the flame digits Ember once drew (the packaged atlas
and its generator are gone).

- **The sheet.** `resource/ui/net_result/CMN/net_result.tex.emz` in the game
  folder holds the ranked result screen's number art: the EMB entry
  `net_vs_number.dds`, 256 by 256, uncompressed 32-bit BGRA with alpha, the same
  in every language. It has three sets of 0 to 9: white, gold (the "rank up"
  numbers) and blue ("rank down"), in rows of glyphs. `SelectionArt::PointDigits()`
  opens it the way `RankingEmblem` opens `Ranking.emz` (`src/ui/SelectionArt.cxx`)
  and returns the texture with a `point_digits::Sheet`.
- **Measuring.** `src/common/PointDigits.hxx` finds each glyph's ink box from the
  sheet's alpha: it scans the columns inside each row's known band (`kBands`, a
  couple of rows of slack) and takes every run of columns with ink (alpha 16 or
  more) as one glyph. A sheet that is not 256 by 256, or a row that does not hold
  the glyphs expected, is refused as an unusable file (retried, then logged
  once), so a changed game file never draws wrong digits. The result is a table
  of boxes, never numbers written in code.
- **Laying out a number.** `src/ui/LevelMark.cxx` places the digits of one set on
  one baseline (the bottom of their ink), left to right, with a 2 px gap
  between ink boxes at the sheet's scale. The tallest digit stands the height
  asked for; positions are rounded to whole pixels at the drawn scale; the width
  runs from the first digit's ink to the last's, so a number centres on what is
  painted. Points need up to six digits (the largest shown is 999,999); the same
  code lays out any count. A band over a small picture shrinks the digits until
  the number fits the picture's width.
- **The label.** `v2::Level` draws a short label before the plate, left of it, in
  the kit's bold heading font in white, like the game's "PP" and "BP": the string
  `points.label`, English "FP" (fighter points), in every catalog. The tier
  stays the colour of the frame round the number.
- **A gain.** `v2::LevelGain(d, at, height, gain)` draws "+16" with the gold set
  on no ground. The sheet has no plus, so the plus is the heading font's, in the
  same gold.
- **Fallback.** While the sheet has not loaded, or the game has none or it does
  not match, the number is drawn in the heading font's digits, white (gold for a
  gain), on the same dark ground and frame (as the Dev mark falls back to a pill,
  `NameMarks.cxx` `MarkOf`).
- **Kit.** `V2.hxx` declares `v2::Level(d, at, height, points, tier)` (the label
  and the number on dark ground in the tier's frame, returning the width of both),
  `v2::LevelWidth(points, height)`, `v2::LevelGain` and `v2::FrameTier(tier)` (the
  award tier a bridge tier names), with the two parts built on them in
  `PLAYER_CARD.md` (`LevelBand`, `Meter`). The tier is always passed in, never
  worked out from the points. Speech and the text probe read the row's words
  ("1986 points"), never the art or the label.
- **Lab.** `UiLab --levels` shows 7, 86, 412, 1986 and 12840 at three heights with
  a gain, and again without art; the digits are the game's when `SF4E_GAME_DIR`
  names a game folder.

## Work packages

Shared by the three documents; each owns the files listed, no two at once.
The order: P0; then P1, P2 and P5 in parallel; P4 once P1 is in, P3 once P2
is in; then the interface P6, P7, P8 in that order (they share
`locales/*.po`, the `sf4e_ui` source line and `ApplicationShell` files).
Servers deploy before the Nightly that carries P1 is published (The games
cap).

| Package | Owns | Depends | Tests | Only in the game |
|---|---|---|---|---|
| P0 Contracts | `server/ember/protocol/src/{levels.rs,friends.rs,lib.rs}`, `rooms/mod.rs` (`TicketRequest::card`), `rooms/ticket.rs` (`RoomTicket::card`) | none | shapes, `check`, requests refuse unknown fields and answers ignore them, tickets without `card` byte-identical | none |
| P1 Room host | `src/common/{SkillBadge.hxx,SkillBadgeJson.hxx}`, `src/session/{RoomAuthority.hxx,RoomModelServerOwned.cxx,RoomModelActions.cxx,RoomModel.cxx,RoomTypes.hxx,RoomModelJson.cxx,Admission.hxx,IrohRoomPeers.cxx}`, `server/roomhost/{RoomHostStatus.hxx,README.md}`, `server/ember-rooms/src/protocol.rs`, `server/ember/bridge/src/routes/rooms/supervisor.rs` (the limit only), `rust/sf4-net/src/{control.rs,service/controls.rs,service/protocol.rs}` | P0 | `room_server_owned_test` (fighters, game index across a rematch in place, old checkpoint restores), `room_host_config_test` (largest line), `session_server_moderation_test` (card admitted, forged card ignored), ember-rooms limits test, helper `connected` with `card` | two 1.6.0 PCs in a public room: a set of three games in place reaches the bridge as three games |
| P2 Bridge levels | `migrations/057_fighter_levels.sql`, `src/db/migrations.rs` (one line), `src/skill/{mod.rs,levels.rs}`, `src/cards.rs`, `src/routes/{levels.rs,mod.rs,discovery.rs}`, `src/routes/rooms/ticket.rs`, `tests/levels.rs` | P0 | migration from 056 keeps rated games and pairs; three games of one generation count three times; cap at nine; a game without fighters rates but counts no points; opted-out player earns points, shows none; token round trip, 7-day expiry, wrong purpose; 404 shape | none |
| P3 Bridge friends | `FRIENDS.md` | P2 | there | there |
| P4 Helper and runtime | `rust/sf4-net/src/tournament/{levels.rs,friends.rs,mod.rs,play.rs,rooms.rs}`, `src/netplay/{Levels.hxx,LevelRequests.hxx,Friends.hxx,FriendRequests.hxx,TournamentStatus.hxx}`, `src/session/SocialAnswers.{hxx,cxx}`, `src/sf4e/{sf4e__NetplayRuntime__Social.cxx,sf4e__NetplayRuntime.hxx,sf4e__NetplayRuntime__Status.cxx}` (publishing levels and friends; dropping `card` outside public rooms), the answer hook beside `TakeRankingsAnswer`, the netplay and session library lines of `CMakeLists.txt`, `src/tests/{social_answers_test.cxx,tournament_fuzz_test.cxx}` | P0; P1 for `Member::card` | ops round-trip, `card` asked only with `levels` listed, decoders ignore unknown members and keep known keys exact, request cadence | none |
| P5 Digits and points parts | `src/common/PointDigits.hxx`, `src/ui/LevelMark.{hxx,cxx}`, `src/ui/V2.hxx` (declarations of `Level`, `LevelWidth`, `FrameTier`, `LevelBand`, `Meter`, `PLAYER_CARD.md`), `src/ui/SelectionArt.{hxx,cxx}`, the `sf4e_ui` source line of `CMakeLists.txt`, the lab case | none | glyph measuring on a synthetic sheet; layout from a synthetic glyph set | the digits against the game's own PP and BP at three sizes |
| P6 to P8 Interface | `PLAYER_CARD.md`, `FRIENDS.md` | P4, P5 (P8 also P3) | there | there |

**Smallest slice that ships something visible.** P0 (points types and
`TicketRequest`/`RoomTicket` untouched), P1 without the card token, P2 without
`cards.rs` and the look-up, P4's `points_me` alone, P5, and P6's Card and
Fighters tabs: points are recorded and the player sees their own on their
card and Fighters tab. The card token, points in rooms and friends follow
without changing anything that slice stored.

## Open questions

| Question | Recommendation | Cost to change later |
|---|---|---|
| Do a player's games count while they do not take part in ranking? Do others see their points? | Count always; others see points only while they take part (the Room board's footer line). | Showing is a read filter: one line. Not counting could never be undone, which is why counting is the default. |
| Rate rematch-in-place games too (the `game` key)? | Yes: today games 2 and 3 of a set played in place are silently not rated. | Ignoring `game` again is one line; ratings moved meanwhile stay moved. |
| Are the tier thresholds right? | Ship 0, 200, 700, 1,500 and 2,500 points; read real `games` and `wins` after four weeks and retune. | None: only the bridge knows them, stored data is games and wins. |
| Points in the match HUD? | No for 1.6.0: the HUD has no room, and the class is the skill mark there. | A HUD layout change and a settings row. |
| Points in private rooms? | No: no authority vouches for anything there. | Would need a private host attestation, a new trust path. |
| Points on embernetplay.link? | Not in 1.6.0. | A public read route and page; the data is there. |
| Count games of a tournament's bound room? | The same games as the rating, whatever `RecordWonGame` counts. | None. |
