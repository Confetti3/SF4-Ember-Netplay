# Public room classes

A player's class shows beside their name in public rooms: one of USF4's own
online classes, D, D+, C, C+, B, B+, A, A+, Master and Grand Master, drawn with
the game's own class emblems. It answers "who am I sitting down against?"
without a number or a mode to grind. A board lists players by class, with no
positions and no numbers (Rankings). The research behind it is
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
  no position on the board and no promotion match to play for.

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
lately. A player without a Discord account or not taking part shows nothing.

The game has no art for the two marks that are not classes:

- **"?"**: the player's rating is still placing, shown as a word in a small
  pill in Ember's own colours. A ticket has always carried this as band 0;
  builds before this one show nothing for it, so it needed no new field.
- **Dev**: one of Ember's developers, shown as Ember's own flame "DEV" art
  (`assets/input/mark_developer.png`, drawn at the emblems' height and its own
  width), or as "Dev" in a pill while that file has not loaded. The bridge's
  operator lists their Discord user IDs in `rooms.developers`; no client can
  claim it. It stands in
  place of the class (`SkillBadge::developer`, band 0), on tickets and on the
  player's own standing. A developer is not on the board, since the board
  lists classes. Their games are rated as anyone's, so their opponents'
  ratings stay fair, and their class comes back if they leave the list.

A player can hide emblems on their own screen (Interface, `classEmblems`).
That is local: their games still count and others still see their class.

## Rankings

Online play has a Rankings page with the player's own class and the board.
The bridge serves both to players (`ember_protocol::skill`,
`server/ember/bridge/src/skill/ranking.rs`, offered as the `ranking`
capability wherever public rooms are on):

- `GET /v1/ranking/me` is the player's standing: no Discord account, not
  taking part, or taking part with the class their next room ticket carries
  (placing, faint or settled; none while `rooms.skill_badges` is off). Like a
  ticket, it remembers the class as shown.
- `PUT /v1/ranking/me` takes part or not, and sets how the board lists them.
- `GET /v1/ranking/board` lists up to 500 players: everyone who takes part,
  is listed and has a settled class. The highest class comes first; within a
  class, whoever played most recently, so the board is about who plays and
  its cap drops whoever played longest ago. It carries the name, the flag and
  the class, never a rating, a position or a number. Faint and placing players
  are not on it.

The board shows the name and flag the player's game sends, the same ones
rooms show, never the Discord account's name. The game sends them from the
Rankings page, so nobody is listed before seeing the page that says how they
appear and how to stop. A changed name or flag follows the next time the page
opens.

### Not taking part

Taking part is the default and belongs to the Discord account. A player who
stops has no class on any ticket, is not on the board, and their games count
for neither fighter, like a game with a fighter who has no Discord account.
Their rating is kept untouched and their listing forgotten. Coming back
resumes the rating where it was, grown less sure by the time away like any
break, so after a long one the class shows faint until a few games settle it.
Starting over is not offered: it would turn a bad run into a free reset.

The choice changes at most once a day, so nobody can stop after a loss and
come back after a win. The few seconds between a game's end and the bridge's
next room poll are not covered; a player who stops in them skips that game.

## Deploying

Upgrade the bridge first: it accepts the `skill_badge` ticket request field
that new builds send, and an older bridge refuses it. The ranking routes come
with migration 030; a build that finds no `ranking` routes says the service
does not offer rankings yet, and older builds never ask for them.

A ticket carries a class only for a build that asked for one, and the bridge
issues a ticket only for a room of the joiner's own build, which the
supervisor hosts with that build's room host and helper. So a class never
reaches a helper that would refuse it (`RoomTicket` refuses unknown fields),
and room hosts and builds can follow in any order. If a build ID is ever
mapped to an older helper, keep `rooms.skill_badges` off while it is.

The developer mark is a field older helpers refuse, so a ticket carries it
only for a build that asks (`TicketRequest::developer_badge`). A build asks
only a bridge whose capabilities list `ranking`, because an older bridge
refuses the request field. Every other ticket is byte for byte what it was.

Leave `rooms.skill_badges` off until the class ranges are calibrated.

## Left for later

- Reduced weight for games on a fighter the player rarely uses, so trying a
  new character is free too. It needs the fighters in each reported game.
- Classes in private rooms, organised (bridge-matched) games and the Discord
  room bot.
- A quiet marker for players who often leave unfinished games.
- Sending `skill_badge` only to a bridge that offers `ranking`, so a build
  also gets tickets from a bridge that predates classes.
