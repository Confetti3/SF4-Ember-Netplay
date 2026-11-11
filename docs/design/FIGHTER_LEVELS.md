# Fighter levels

A level, 1 to 60, for each fighter a player has used in rated public room
games. It is a grind value to show off, not skill: it only goes up, nothing in
a rating is read to make it, and nothing in a rating is moved by it. The one
skill mark stays the account's class (`SKILL_BADGE.md`); there are no
per-fighter ranks and no rating points are shown. This replaces
`MASTERY.md` (pull request 183): its points stay, its ten named tiers and 30
emblems become a number in Ember's brush digits on a frame coloured by the
five award tiers.

Where a level shows and how the card is laid out is `PLAYER_CARD.md`; who sees
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
  taking part still earns levels (USF4 keeps every player's per-character
  record the same way); whether others see them is Privacy.
- No history is backfilled: no stored game names its fighters, so every
  account starts at no level on every fighter when the bridge and the 1.6.0
  room host are both live.

### The curve

Points to reach level `L` are `(L - 1)(L + 3)`. Level 1 is the first counted
game; each step costs `2L + 3` points, 5 for the first, 121 for the last.

| Level | Frame | Points | Games at 1.5 a game |
|---|---|---|---|
| 1 | Ash | 0 | the first |
| 13 | Copper | 192 | about 130 |
| 25 | Silver | 672 | about 450 |
| 37 | Gold | 1,440 | about 960 |
| 49 | Ember | 2,496 | about 1,660 |
| 60 | Ember | 3,717 | about 2,480 |

The level of `p` points is `1 + isqrt(p + 4) - 2`, at most 60 (integer square
root, no floats). The curve lives only in the bridge (`skill/levels.rs`): a
client is sent each fighter's level and the points of its floor and of the
next level, so the curve can be retuned after real play without a client
update or a data change (games and wins are stored, never points or levels).

The frame's tier is `Tier(1 + (level - 1) / 12)`: Ash 1 to 12, Copper 13 to
24, Silver 25 to 36, Gold 37 to 48, Ember 49 to 60 (`v2::Tier`,
`v2::TierColor` in `src/ui/V2.cxx`). The number is drawn on dark ground; the
tier colours only the frame.

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
   are not rated, and they would earn no level. `WonGame` gains `game`, the
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
before (game 0) and earn no level. Players are matched by build, so a 1.5.x
player only ever plays in 1.5.x rooms and never sees a level.

## Bridge

### Migration 058 (`058_fighter_levels.sql`)

057 is left to the migration-ledger change on `wip/160-bridge`; `db.rs`
allows a gap (55 is absent at this base). If 057 stays unused, renumber at
merge.

```sql
-- Each fighter's counted games for a Discord account (FIGHTER_LEVELS.md).
-- Points are games + wins; the level is worked out from them when served.
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

-- A pair's lifetime record beside its sitting. Rows are kept now: a new
-- sitting restarts `games` in place instead of deleting the row.
ALTER TABLE rated_pairs ADD COLUMN first_wins INTEGER NOT NULL DEFAULT 0;
ALTER TABLE rated_pairs ADD COLUMN second_wins INTEGER NOT NULL DEFAULT 0;
```

Indexes: `fighter_levels` is read only by account (its key). `rated_pairs` is
read by pair (its key); friends read it by pair too (`FRIENDS.md`).

Row lifetimes: `fighter_levels` and `rated_pairs` for good (at most 44 rows
per account; one per pair that ever played); `rated_games` with its room row,
as before. Table and column names are kept; `rated_pairs` now covers every
pair of Discord accounts, which its comment says.

### `record_games` (`server/ember/bridge/src/skill/mod.rs`)

One pass per new game, in this order, so the rating keeps its own rules and
levels never read them:

1. Key `(room, table, generation, game)` into `rated_games`; seen before:
   skip.
2. Both Discord accounts (`account`, any account, not `rated_account`); none
   or the same: skip.
3. The pair's sitting (`rated_pairs`): `games` restarts at 1 when
   `played_at` is over three hours old, else adds 1; the winner's side of
   `first_wins`/`second_wins` adds 1; `played_at` is now. The old
   `DELETE FROM rated_pairs WHERE played_at < ?` goes.
4. With `fighters` and a sitting count of at most `FULL_REPEATS`: upsert both
   rows of `fighter_levels` (`skill/levels.rs`).
5. Both accounts take part in ranking (`ranking_choices`): rate as today,
   with the sitting weight from step 3.

This removes `sitting_weight`'s own delete-and-count; a game where one side
does not take part now counts toward the pair's sitting, which only matters
for a pair whose choice changes mid-sitting.

## Wire contracts

Capability `levels` (`ember_protocol::levels::LEVELS_FEATURE`), offered
wherever public rooms are on (`routes/discovery.rs`, beside `ranking`).
Every type below denies unknown fields; a build asks only a bridge that lists
`levels`, and an older bridge answers 404, which the helper words as
`levels_unavailable` (as `ranking_unavailable`, `tournament/ranking.rs`).

### `GET /v1/levels/me`

Player session (`routes::play::playing`), bucket `levels:{ember_id}` 60 per
10 minutes shared with card look-ups.

```json
{"levels":"no_account"}
{"levels":"counted","shown":true,"fighters":[
  {"fighter":3,"level":42,"games":1284,"wins":702,"floor":1845,"next":1932,"since":1729468800}]}
```

| Field | Rule |
|---|---|
| `levels` | `no_account` (no Discord account on the Ember ID) or `counted` |
| `shown` | others see this account's levels (Privacy); written only when true |
| `fighters` | played fighters only, ascending by `fighter`, unique, at most 44 |
| `fighter` | 0 to 43 |
| `level` | 1 to 60 |
| `games`, `wins` | counted games; `wins <= games` |
| `floor` | points of this level; `floor <= games + wins` |
| `next` | points of the next level, absent at 60; `games + wins < next` |
| `since` | `first_at`, unix seconds |

Read only: nothing is remembered as shown.

### The card token in the room ticket

A ticket is signed when a player asks to join, before any fighter is picked
(`routes/rooms/ticket.rs`), and lives 60 seconds. It cannot carry the level of
the fighter they will play, nor anything that changes while they stay. What it
carries instead is a **card token**: an opaque, bridge-sealed name for the
player's Discord account that the room shows its members, so any member can
ask the bridge for that player's card later, as often as it needs, fresh.

- `TicketRequest::card: bool` (written only when true). A build sends it only
  to a bridge that lists `levels` (`tournament/play.rs` `Features`), since an
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
{"cards":[{"fighters":[{"fighter":3,"level":31,"games":612}],"relation":"none"},null]}
```

| Field | Rule |
|---|---|
| request `cards` | 1 to 16 selectors: `card` (a token) or `friend` (a friend id, `FRIENDS.md`) |
| answer `cards` | one entry per selector, in order; `null` for a token that does not open, is over 7 days old, or names an account without a Discord account now, and for a friend id that is not the caller's friend |
| `fighters` | as in `levels/me` but `fighter`, `level`, `games` only; absent when the player does not show levels |
| `relation` | the caller's relation to that player: `none`, `friend`, `sent`, `received`, `blocked` (the caller blocked them); `none` for a player who blocked the caller |

The bridge serves `relation` once `friends` is offered; before that it is
always `none`.

## Privacy

- Your own levels: always, to you.
- Others' levels: shown to a room member (through the card token) and to a
  friend while that player takes part in ranking (`ranking_choices.ranked`,
  default true). A player who stopped taking part keeps earning levels,
  shows none to others, and sees on their Fighters tab that others do not see
  them. This answers `MASTERY.md`'s open question; see Open questions.
- Others see a fighter's level and games, never wins, win rate, points or
  floor. Points minus games would be wins.
- No private room shows levels: its host writes its own snapshot, so a token
  there is dropped like a class.
- A card token says nothing by itself; it is unreadable without the bridge's
  key and unlinkable between joins. It is no more than a room member can see.

## Client

- `netplay::levels::Own` (`src/netplay/Levels.hxx`): `unavailable`,
  `no_account` or counted, with an array of 44 `{level, games, wins, floor,
  next, since}` (level 0 for unplayed) and `shown`. Read on Profile's open, at
  start, and twice after each set this PC fought (8 and 20 seconds after the
  set's end, since the bridge polls rooms every 5 seconds,
  `routes/rooms/poll.rs` `POLL_SECS`). Kept per bridge and Ember ID, as
  rankings are (`RankingRequests.hxx`).
- `netplay::levels::Cards`: token to card, refreshed when a member with a
  token joins and 8 seconds after any set in the room ends, at most one
  look-up of up to 16 tokens every 10 seconds.
- A level-up is found by comparing the new `Own` with the one before the set;
  `PLAYER_CARD.md` draws it.

### The digits atlas

Ember's level digits come from the brush strokes in
`mockups/level-digits/make_font.py` (outlines 0 to 9 and the Dev mark's flame
renderer). The game draws them from an atlas, never from a font.

- **Source and output.** The generator moves into the repository as
  `scripts/level-digits/make_level_digits.py` (Pillow only; the TTF stays a
  mockup tool) and writes `assets/level/digits.png` and
  `src/common/LevelDigits.hxx`, both checked in. The build never runs Python;
  `CMakeLists.txt` installs `assets/level` beside `assets/flags`.
  `scripts/level-digits/check_level_digits.py` renders again and fails when
  either file differs, as `check-flag-atlas.py` does for flags.
  `assets/level/README.md` says the art is Ember's own, like
  `mark_developer.png`.
- **Layers.** Each digit is two sprites: *under* (the shadow, red rim, orange
  and yellow spreads of `flame`) and *core* (the white stroke). A number draws
  every under sprite left to right, then every core sprite, so a slanted
  digit's glow never covers its neighbour's core where they tuck (`TUCK`).
- **Sizes.** Rendered at cap heights 20, 40 and 80 px (four times
  supersampled, as `render_mask` does). A number drawn `h` tall uses the
  smallest size at least `h`, so it scales down by at most half; above 80 it
  scales up (a card at 175 % interface scale). Each cell carries the glow pad
  `0.3h + 4`, so cells are about `1.56h` by `1.69h`; ten digits by two
  layers by three sizes is about 1,280 by 480 px, decoded at its own size like
  the flag atlas (`SelectionArt::Impl::Policy` `exactWidth`, `exactHeight`).
- **The header.** For each size: the cap height, and for each digit its two
  cells (x, y, w, h in the atlas), the ink's left bearing and the advance,
  both in that size's pixels, and the baseline in the cell.
- **Laying out a number.** The pen starts at 0; each digit's cells are placed
  at `pen + bearing - pad`, `baseline - capHeight - pad`; the pen moves by
  the advance. Positions are rounded to whole pixels at the drawn scale. The
  width is from the first digit's ink to the last digit's ink plus its lean,
  so a number centres on what is painted. One- and two-digit numbers are all
  a level needs; the same code lays out any count.
- **Fallback.** While the atlas has not loaded, or is missing, the number is
  drawn in the heading font in ivory, on the same dark ground and frame (as
  the Dev mark falls back to a pill, `NameMarks.cxx` `MarkOf`).
- **Kit.** `SelectionArt::LevelDigits()` loads the atlas (as
  `DeveloperMark`); `src/ui/LevelMark.cxx` lays numbers out; `V2.hxx` declares
  `v2::LevelTier(level)`, `v2::Level(d, at, height, level)` (the number on
  dark ground in its tier's frame, returning its width) and
  `v2::LevelWidth(level, height)`, with the two parts built on them in
  `PLAYER_CARD.md` (`LevelBand`, `Meter`). Speech and the text probe read the
  row's words ("Level 42"), never the art.

## Work packages

Shared by the three documents; each owns the files listed, no two at once.
The order: P0; then P1, P2 and P5 in parallel; P4 once P1 is in, P3 once P2
is in; then the interface P6, P7, P8 in that order (they share
`locales/*.po`, the `sf4e_ui` source line and `ApplicationShell` files).
Servers deploy before the Nightly that carries P1 is published (The games
cap).

| Package | Owns | Depends | Tests | Only in the game |
|---|---|---|---|---|
| P0 Contracts | `server/ember/protocol/src/{levels.rs,friends.rs,lib.rs}`, `rooms/mod.rs` (`TicketRequest::card`), `rooms/ticket.rs` (`RoomTicket::card`) | none | shapes, `check`, unknown fields refused, tickets without `card` byte-identical | none |
| P1 Room host | `src/common/{SkillBadge.hxx,SkillBadgeJson.hxx}`, `src/session/{RoomAuthority.hxx,RoomModelServerOwned.cxx,RoomModelActions.cxx,RoomModel.cxx,RoomTypes.hxx,RoomModelJson.cxx,Admission.hxx,IrohRoomPeers.cxx}`, `server/roomhost/{RoomHostStatus.hxx,README.md}`, `server/ember-rooms/src/protocol.rs`, `server/ember/bridge/src/routes/rooms/supervisor.rs` (the limit only), `rust/sf4-net/src/{control.rs,service/controls.rs,service/protocol.rs}` | P0 | `room_server_owned_test` (fighters, game index across a rematch in place, old checkpoint restores), `room_host_config_test` (largest line), `session_server_moderation_test` (card admitted, forged card ignored), ember-rooms limits test, helper `connected` with `card` | two 1.6.0 PCs in a public room: a set of three games in place reaches the bridge as three games |
| P2 Bridge levels | `migrations/058_fighter_levels.sql`, `src/db.rs` (one line), `src/skill/{mod.rs,levels.rs}`, `src/cards.rs`, `src/routes/{levels.rs,mod.rs,discovery.rs}`, `src/routes/rooms/ticket.rs`, `tests/levels.rs` | P0 | migration from 056 keeps rated games and pairs; three games of one generation count three times; cap at nine; a game without fighters rates but levels nothing; opted-out player levels, shows none; token round trip, 7-day expiry, wrong purpose; 404 shape | none |
| P3 Bridge friends | `FRIENDS.md` | P2 | there | there |
| P4 Helper and runtime | `rust/sf4-net/src/tournament/{levels.rs,friends.rs,mod.rs,play.rs,rooms.rs}`, `src/netplay/{Levels.hxx,LevelRequests.hxx,Friends.hxx,FriendRequests.hxx,TournamentStatus.hxx}`, `src/session/SocialAnswers.{hxx,cxx}`, `src/sf4e/{sf4e__NetplayRuntime__Social.cxx,sf4e__NetplayRuntime.hxx,sf4e__NetplayRuntime__Status.cxx}` (publishing levels and friends; dropping `card` outside public rooms), the answer hook beside `TakeRankingsAnswer`, the netplay and session library lines of `CMakeLists.txt`, `src/tests/{social_answers_test.cxx,tournament_fuzz_test.cxx}` | P0; P1 for `Member::card` | ops round-trip, `card` asked only with `levels` listed, strict decoders refuse unknown members, request cadence | none |
| P5 Digits and level parts | `scripts/level-digits/*`, `assets/level/*`, `src/common/LevelDigits.hxx`, `src/ui/LevelMark.{hxx,cxx}`, `src/ui/V2.hxx` (declarations of `Level`, `LevelWidth`, `LevelTier`, `LevelBand`, `Meter`, `PLAYER_CARD.md`), `src/ui/SelectionArt.{hxx,cxx}`, the install line and the `sf4e_ui` source line of `CMakeLists.txt`, the lab case | none | checker passes; layout widths for 1 to 60 at each size; lab renders a strip of 1 to 60 | the flame against the Dev mark at three sizes |
| P6 to P8 Interface | `PLAYER_CARD.md`, `FRIENDS.md` | P4, P5 (P8 also P3) | there | there |

**Smallest slice that ships something visible.** P0 (levels types and
`TicketRequest`/`RoomTicket` untouched), P1 without the card token, P2 without
`cards.rs` and the look-up, P4's `levels_me` alone, P5, and P6's Card and
Fighters tabs: levels are recorded and the player sees their own on their
card and Fighters tab. The card token, levels in rooms and friends follow
without changing anything that slice stored.

## Open questions

| Question | Recommendation | Cost to change later |
|---|---|---|
| Do a player's games count while they do not take part in ranking? Do others see their levels? | Count always; others see levels only while they take part (the Room board's footer line). | Showing is a read filter: one line. Not counting could never be undone, which is why counting is the default. |
| Rate rematch-in-place games too (the `game` key)? | Yes: today games 2 and 3 of a set played in place are silently not rated. | Ignoring `game` again is one line; ratings moved meanwhile stay moved. |
| Is the curve right? | Ship `(L-1)(L+3)`; read real `games` after four weeks and retune. | None: only the bridge knows it, stored data is games and wins. |
| A level in the match HUD? | No for 1.6.0: the HUD has no room, and the class is the skill mark there. | A HUD layout change and a settings row. |
| Levels in private rooms? | No: no authority vouches for anything there. | Would need a private host attestation, a new trust path. |
| Levels on embernetplay.link? | Not in 1.6.0. | A public read route and page; the data is there. |
| Count games of a tournament's bound room? | The same games as the rating, whatever `RecordWonGame` counts. | None. |
