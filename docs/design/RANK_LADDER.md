# Rank ladder, character ranks and mastery

Extends `SKILL_BADGE.md`, which it changes in the places named below. Nothing here
is built. The rating stays hidden and rated public room games stay the only thing
that moves it; the rest is display that grows beside it.

## The ladder: 29 ranks

USF4's arcade-mode ranking movie (`resource/ui/advertise/ranking/REL/ac_ranking.emz`,
`class.gfx`) has a `class` sprite of 29 frames, which the board selects with
`class_package.SelectClass(player_class)`. Public room classes would use the same
29 ranks in place of the ten of `SKILL_BADGE.md`.

| Band | Rank |
|---|---|
| 1, 2 | Beginner, Rookie |
| 3 to 5 | D, DD, DDD |
| 6 to 11 | C, C+, CC, CC+, CCC, CCC+ |
| 12 to 17 | B, B+, BB, BB+, BBB, BBB+ |
| 18 to 23 | A, A+, AA, AA+, AAA, AAA+ |
| 24 | Master |
| 25 | Grand Master |
| 26 to 29 | Ultimate Master, Warrior, Grand Warrior, Ultimate Warrior |

The letter ranks are the letter image once, twice or three times with a plus after
it for the plus steps; D has no plus. The named ranks are one image each, and
Grand Master is the movie's `cls_gm` sprite.

### Rating floors

Steps are 40 points wide, anchored so that B (band 12) starts at 1500, the rating
every player starts at and today's B floor. To be calibrated against real ratings
before ranks show, as the ten classes were.

| Band | Floor | Band | Floor | Band | Floor |
|---|---|---|---|---|---|
| 1 Beginner | below 1100 | 11 CCC+ | 1460 | 21 AA+ | 1860 |
| 2 Rookie | 1100 | 12 B | 1500 | 22 AAA | 1900 |
| 3 D | 1140 | 13 B+ | 1540 | 23 AAA+ | 1940 |
| 4 DD | 1180 | 14 BB | 1580 | 24 Master | 1980 |
| 5 DDD | 1220 | 15 BB+ | 1620 | 25 Grand Master | 2020 |
| 6 C | 1260 | 16 BBB | 1660 | 26 Ultimate Master | 2060 |
| 7 C+ | 1300 | 17 BBB+ | 1700 | 27 Warrior | 2100 |
| 8 CC | 1340 | 18 A | 1740 | 28 Grand Warrior | 2140 |
| 9 CC+ | 1380 | 19 A+ | 1780 | 29 Ultimate Warrior | 2180 |
| 10 CCC | 1420 | 20 AA | 1820 | | |

`BAND_MARGIN` shrinks from 25 to about 10, since steps are narrower. The weights
for lopsided games (`FULL_WEIGHT_GAP`, `MIN_WEIGHT_GAP`) stay in rating points,
so "a class and a half" and "four and a half classes" in `SKILL_BADGE.md` become
about four and eleven ranks. That is intended: a rank is a finer step, so the
same rating gap now spans more of them. Whether `SETTLED_DEVIATION` (200, now
five ranks wide) should fall is for the calibration.

### What shows before a rank

- **Never played:** no badge. The account has no rating row, so `load` and `badge_of`,
  which answer band 0 today for an account with no row, would answer no badge. A
  never-played account that takes part is still `Ranked` in
  `GET /v1/ranking/me`, with no badge; `NoAccount` stays for a player without a
  Discord account.
- **Played, not settled:** band 0, the "?" pill, as in `SKILL_BADGE.md`.

This changes `SKILL_BADGE.md`, where band 0 and "?" also covered an account that
has not played: the first rated game creates the row and starts the "?".

### Builds that know only ten classes

A band above ten fails `SkillBadge::check` and `Board::check`
(`server/ember/protocol/src/skill.rs`), so it breaks an old build in three places:
the room ticket and the helper's parse of it, the badge inside
`GET /v1/ranking/me`, and the board. `TicketRequest::skill_badge` is a plain
`bool` and the two GETs have no body, so none of them can say how many bands the
build reads. A new capability beside `ranking` does: a build sends `skill_ranks`
on the ticket request, and `ranks=29` on the two GETs, only to a bridge that
offers it, because an older bridge refuses unknown request fields
(`SKILL_BADGE.md`, "Deploying"). So the bridge goes first, then the builds.

Without them the bridge answers the old ten-class band, mapped from the stored
29-band by that band's floor against the old floors (band 4, floor 1180, is a D,
as 1200 is the D+ edge), with no second margin, so an old build's class only moves
when the rank does.

The client changes are the table `Classes[]` in `src/ui/NameMarks.cxx`, the
Rankings page chart and the web board. The 14 locale catalogs need keys for the 8
named ranks (Beginner, Rookie, Master, Grand Master, Ultimate Master, Warrior,
Grand Warrior, Ultimate Warrior), which replace the existing `class.master` and
`class.grand_master`; the letters are the game's own tokens and the 12 mastery
tiers reuse these names.

### Art

The emblems are USF4's own arcade-mode textures, shipped with Ember's assets.
This reverses the rule in `SKILL_BADGE.md` that emblems are read from the player's
install and never repackaged. The reason is that these textures are not in a Steam
install: its `resource/ui/advertise/ranking/ac_ranking.emz` has 7 entries and none
is a class or rank image, while the arcade install's `ac_ranking.emz` has the
`class` movie and its textures. Whether Ember may ship them is an open question
for the upstream owner, and none is added before it is answered.

## Character ranks

Each rated game would also rate the two fighters used, in a table
`character_ratings(discord_user_id, fighter, rating, deviation, volatility, band,
played_at)`, with its own `band` for the margin as `skill_ratings` has. The game
is rated with the same `Rating::after`, against the opponent's rating for the
fighter they played, or their overall rating if they have none for it, and its
weight is computed from those two ratings and the sitting exactly as `gap_weight`
and `sitting_weight` do for the overall rating. A new fighter starts at the
account's overall rating with a deviation of 150 (a proposal): above the 110 at
which a rating's games count in full, so its first games settle it quickly.

`Game` in `server/ember/bridge/src/skill/mod.rs` has only the table, generation,
winner and loser, so the room host must report the winner's and the loser's
fighter as well: `RoomAuthority::RecordWonGame` and the host's status line
(`server/roomhost/RoomHostStatus.hxx`) change, and the bridge reads the new fields
as optional. The games array may be 6144 bytes for 32 games, 192 a game, which
three places enforce together (`MaximumWonGames` and `SupervisorGamesBytes` in the
host, `MAX_GAMES_BYTES` in `ember-rooms` and the bridge). Two small numbers a game
must be measured against it, and if they do not fit the limits rise together.
`SKILL_BADGE.md` already notes under "Left for later" that weighting by fighter
needs the fighters in each reported game.

### How it reaches the screen

A ticket is signed when a player joins, before they pick a fighter, so it carries
only the overall rank. A fighter's rank and mastery come from a new bridge route,
a capability beside `ranking`, keyed on the Ember IDs of the room's seats as the
room host's status reports them, so the bridge needs no room membership of its
own. It answers only for accounts that take part in ranking, and the client asks
again when a fighter is picked. It shows in character select and on the card.
A match still shows only the overall rank, taken from the room snapshot as
`SKILL_BADGE.md` describes, so a private host cannot put a fighter's rank there.
The route's exact shape is to be settled with the upstream owner.

## Mastery

Mastery is progression, not rating, and is kept for each fighter separately. It
never moves a rating and is not a prior for one: it starts at nothing and grows
with the games that rate the fighter. Everybody starts every fighter at 0,
whatever else they have won.

Points come from rated public games with that fighter. They never go down: a loss
adds 1 and a win adds 2. A win against a stronger opponent adds more, on top of
the 2: +1 if the opponent's tier on the fighter they played is above the winner's
tier on theirs, +2 if it is two or more above. So a win is worth 2 to 4. A player
with no mastery row for a fighter is at Beginner. A game that counts at the repeat
weight of `SKILL_BADGE.md` (past eight in one sitting) gives no points, so playing
one friend over and over earns nothing. The weights for lopsided games and for
unsure ratings do not apply to mastery, and a loss's flat point can be farmed up
to the repeat cap, which is accepted. Table
`character_mastery(discord_user_id, fighter, points)`; the tier is derived, so
the thresholds can change without touching stored data.

Mastery always counts, as the game's own per-character record does: USF4 keeps
every player's wins, battles, streaks and points for each fighter and has no
setting to turn it off. This changes "Not taking part" in `SKILL_BADGE.md`, where
an opted-out player's games count for neither fighter: such a game still gives
both players mastery points, and the rating of neither moves unless both take
part. The account's own play counts stay hidden until it takes part again. The
bridge uses an opponent's tier only to size a bonus and never returns it, and a
player's own points show only as a tier. Like the rating, mastery needs a Discord
account; a game against a fighter without one gives no points to either.

There are 60 levels per fighter in 12 tiers of 5. The tiers take USF4 arcade
mode's names in its order, with the letter ranks as plain D, C, B and A. A tier
shows by name with its level as up to four pips; no level number is shown. The
levels of a tier split its points evenly.

| Tier | Levels | Points to reach |
|---|---|---|
| Beginner | 1 to 5 | 0 |
| Rookie | 6 to 10 | 30 |
| D | 11 to 15 | 90 |
| C | 16 to 20 | 180 |
| B | 21 to 25 | 300 |
| A | 26 to 30 | 480 |
| Master | 31 to 35 | 720 |
| Grand Master | 36 to 40 | 1050 |
| Ultimate Master | 41 to 45 | 1500 |
| Warrior | 46 to 50 | 2200 |
| Grand Warrior | 51 to 55 | 3100 |
| Ultimate Warrior | 56 to 60 | 4200 |

A tier and a rank share names, so a tier is never shown alone: it always names its
fighter, with the fighter's portrait and name beside it, wherever it appears, and
a rank never does. It uses the same emblem art as a rank, in a different frame, so
the fighter is what tells the two apart.

It shows, when the player takes part in ranking, on the player's card and badge in
every public room, in character select, in Rankings, and on an online page that
lists every fighter's mastery for a player beside the ranking board. It does not
show in private rooms, which have no bridge ticket, nor in a match, where only the
fighter's rank shows.
