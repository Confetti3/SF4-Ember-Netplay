# Fighter mastery

Extends `SKILL_BADGE.md`, which it changes in the one place named below. Nothing
here is built. The ten public room classes and the hidden rating stay exactly as
they are; mastery is a separate grind value that sits beside them.

## What it is

Mastery is progression, not skill. It never moves a rating, is never moved by one,
and is kept for each fighter separately. Everybody starts every fighter at 0,
whatever else they have won. Ranks already say how good a player is; mastery says
how much a player has played a fighter, for showing off.

## Points

Points come from rated public room games with that fighter, the same games that
move the rating. They never go down: a loss adds 1 and a win adds 2. There is no
bonus for the opponent's strength, and nothing in a rating or a class is read.

A game that counts at the repeat weight of `SKILL_BADGE.md` (past eight in one
sitting) gives no points, so playing one friend over and over earns nothing. The
flat point for a loss can be farmed up to that cap, which is accepted. The weights
for lopsided games and for unsure ratings do not apply.

Table `character_mastery(discord_user_id, fighter, points)`. The level and tier are
derived from `points`, so the thresholds can change without touching stored data.
Like the rating, mastery needs a Discord account; a game against a fighter without
one gives no points to either.

## Tiers and levels

There are 60 levels per fighter in 10 tiers of 6, named for the fire in their
emblem, from Ash to Solar. A tier shows by name with its emblem; no level number is shown. The levels
of a tier split its points evenly.

| Tier | Levels | Points to reach |
|---|---|---|
| Ash | 1 to 6 | 0 |
| Cinder | 7 to 12 | 40 |
| Spark | 13 to 18 | 120 |
| Kindle | 19 to 24 | 250 |
| Ember | 25 to 30 | 450 |
| Flame | 31 to 36 | 750 |
| Blaze | 37 to 42 | 1200 |
| Inferno | 43 to 48 | 1900 |
| Phoenix | 49 to 54 | 2900 |
| Solar | 55 to 60 | 4200 |

The thresholds are a proposal. A player who wins half their games earns 1.5 points
a game, so Solar is about 2800 games with one fighter; to be checked against
real play.

The tier names are Ember's own, not class letters or USF4 rank names, so a tier
cannot be mistaken for a class. A tier is still shown with its fighter's portrait and name beside it.

## Art

Ember draws its own emblems, in the palette of `src/ui/Theme.cxx`. None of USF4's arcade textures is used. There are 30
badges, 3 for each tier: the plain shield, the same with a laurel, and the same with
wings and a crest on top. A level shows badge `ceil(level / 2)`, so each tier's six
levels step through its three badges two levels at a time.

## Where it shows

When the player takes part in ranking, mastery shows on the player's card and badge
in every public room, in character select, in Rankings, and on an online page that
lists every fighter's mastery for a player beside the ranking board. It does not
show in private rooms, which have no bridge ticket, nor in a match.

A ticket is signed when a player joins, before they pick a fighter, so it carries
no mastery. A fighter's tier comes from a new bridge route, a capability beside
`ranking`, keyed on the Ember IDs of the room's seats as the room host's status
reports them, so the bridge needs no room membership of its own. It answers only
for accounts that take part in ranking, and the client asks again when a fighter is
picked. A player's own points show only as a tier. The route's exact shape is to
be settled with the upstream owner.

## Reporting the fighters

`Game` in `server/ember/bridge/src/skill/mod.rs` has only the table, generation,
winner and loser, so the room host must also report the winner's and the loser's
fighter: `RoomAuthority::RecordWonGame` and the host's status line
(`server/roomhost/RoomHostStatus.hxx`) change, and the bridge reads the new fields
as optional. The games array may be 6144 bytes for 32 games, 192 a game, which
three places enforce together (`MaximumWonGames` and `SupervisorGamesBytes` in the
host, `MAX_GAMES_BYTES` in `ember-rooms` and the bridge). Two small numbers a game
must be measured against it, and if they do not fit the limits rise together.

## Not taking part

This changes "Not taking part" in `SKILL_BADGE.md`, where an opted-out player's
games count for neither fighter. Mastery always counts, as the game's own
per-character record does: USF4 keeps every player's wins, battles and points for
each fighter and has no setting to turn it off. Such a game gives both players
mastery points, and the rating of neither moves unless both take part. The
account's own play counts stay hidden until it takes part again. Whether an
opted-out player's mastery should count is an open question for the upstream owner.
