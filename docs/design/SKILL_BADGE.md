# Public room classes

A player's class shows beside their name in public rooms: one of USF4's own
online classes, D, D+, C, C+, B, B+, A, A+, Master and Grand Master, drawn with
the game's own class emblems. It answers "who am I sitting down against?"
without a number, a leaderboard or a mode to grind. The research behind it is
`PLAYER_SKILL_BADGE_RESEARCH.md` (October 2026, kept outside the repository).

## Rules

- Only public room games count. The room host is the only party that knows
  both fighters' verified Ember IDs and the agreed result; a private room has
  no authority that could vouch for either.
- Only won games count. Draws, cancels, aborts and disconnects never do,
  in keeping with `NATIVE_MATCH_RESULT.md`.
- The rating belongs to the Discord account connected to the Ember ID. An
  Ember ID costs nothing to replace, a Discord account does, so a new Ember ID
  does not start over and two throwaway IDs cannot farm each other. A game
  with a fighter who has no Discord account counts for neither.
- The rating is Glicko-2 with one game per rating period, and it never leaves
  the bridge. Only the class does.
- A new player has no class until the rating is sure enough, about ten games
  against settled players.
- A class changes only when the rating has crossed into another class by a
  margin (25 points) since the class last shown, so one good or bad evening
  rarely moves it. There is no promotion match and no announcement.
- The rating grows less sure with each day without a game. After about two
  months the class shows faint until a few games settle it again; it never
  drops on its own.
- Classes are fixed rating ranges, not percentiles, so a class cannot change
  because other people played. The 1500 every player starts at is the floor
  of B; the ranges are calibrated against real ratings during a trial with
  classes hidden (`rooms.skill_badges` off) before anyone sees one. Games
  only move ratings; a class is worked out when a ticket first shows it, so
  ranges changed during the trial apply to everyone's first class.

## Playing for fun

The rules are meant to make every game safe to play: nobody should avoid a
stronger opponent, a weaker friend, a long set or a break.

- A lopsided game hardly counts. Between two settled players a game counts in
  full within 150 rating points (about a class and a half) and less beyond,
  down to a tenth at 450 points (about four and a half classes). A Grand Master
  who loses to a D player gives up a few points, not a class.
- An upset still counts. When the lower-rated player wins, their game counts at
  least half, whatever the gap, so beating a much stronger player is worth
  about as much as beating a peer, while the stronger player's loss stays
  light.
- A long set with one friend does not swing either class. Past eight games
  against the same opponent in one sitting, each game counts a quarter; a
  sitting ends after three hours without a game between the two.
- Games while a rating is still placing count in full: it needs them to
  settle, and its gap to anyone says little yet.
- A class never drops on its own, needs a margin to move, and has no number,
  no leaderboard and no promotion match to play for.

A game's weight is a fraction of a Glicko-2 rating period: the information it
adds and the volatility change it brings both scale with it
(`Rating::after`). An upset's floor could be farmed by a strong player losing
on purpose to a second account; each account needs its own Discord account,
and the sitting rule caps it to eight such games every three hours.

## How a game becomes a class

1. The room model records each won game between two accounts in a server-owned
   room (`RoomAuthority::RecordWonGame`), keeping the newest 32 in its
   checkpoint.
2. The room host lists the committed ones in its status line as `games`
   (`SessionServer::CommittedServerOwned`, `server/roomhost/RoomHostStatus.hxx`).
   The supervisor passes the array on in `GET /rooms` without reading it, as
   it does `details` (`PUBLIC_ROOMS.md`, Supervisor API). The bridge's room
   poll reads each game by itself, so one it cannot read is skipped alone,
   and rates each new one once, keyed by room, table and generation
   (`server/ember/bridge/src/skill`).
3. When a player asks for a public room ticket, a build that shows classes
   sets `skill_badge` in the request and the bridge signs the player's class
   into the ticket (`RoomTicket::badge`, `ember_protocol::skill`), remembering
   it as the class last shown. A ticket without one is byte for byte what it
   was before.
4. The room host's helper verifies the ticket and reports the class with the
   account on `connected`. The session server hands that `Admission` to the
   room model at join: nothing a joiner writes in its own join request is
   read.
5. A client keeps classes only in a room it joined as public
   (`IrohRoom::ServerOwned`, known from its own join), dropping any other
   room's where it copies the snapshot for the overlay
   (`sf4e__NetplayRuntime__Status.cxx`), because a private room's host writes
   its own snapshot.

## Presentation

The emblem is the game's own (`class_*.dds` in
`resource/ui/network_mode/Ranking.emz`), read from the player's install at
runtime like the fighter portraits and never repackaged. It follows the
country flag and leads the name on table seats and in the Members list
(`src/ui/NameMarks.cxx`). The
tooltip names the class; a faint emblem's adds that the player has not played
lately. A player still placing, or without a Discord account, shows nothing.

## Deploying

Upgrade the bridge first: it accepts the `skill_badge` ticket request field
that new builds send, and an older bridge refuses it.

A ticket carries a class only for a build that asked for one, and the bridge
issues a ticket only for a room of the joiner's own build, which the
supervisor hosts with that build's room host and helper. So a class never
reaches a helper that would refuse it (`RoomTicket` refuses unknown fields),
and room hosts and builds can follow in any order. If a build ID is ever
mapped to an older helper, keep `rooms.skill_badges` off while it is.

Leave `rooms.skill_badges` off until the class ranges are calibrated.

## Left for later

- Reduced weight for games on a fighter the player rarely uses, so trying a
  new character is free too. It needs the fighters in each reported game.
- Classes in private rooms, organised (bridge-matched) games and the Discord
  room bot.
- A quiet marker for players who often leave unfinished games.
