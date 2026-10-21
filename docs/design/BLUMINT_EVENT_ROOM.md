# BluMint event room

Status: design. Branch `fraccie/blumint-mode`. Not reviewed. From the meeting
with BluMint (2026-10-21).

BluMint creates and runs the whole event. Once it starts, the organizer opens
one link that makes a private Ember room for the event. The streamer stays in
that room and plays match after match: every match BluMint creates for players
in the room lands in the room's queue and starts as soon as a table is free.
The next players wait in the room already, so a set starts the moment the
previous one ends, while BluMint's bracket updates from Ember's reports.

## What BluMint's API allows (checked 2026-10-21)

Game partner API 2.0.0 (`https://staging.blumint.io/docs`, guide at
`/docs/game-guide`):

| Route | Direction | Ember uses it |
|---|---|---|
| `POST /tournaments/match/setCreate`, `setLookupPlayer`, `match/setClose` | Ember registers callbacks | yes, once (`ember-bridge blumint-register`) |
| create, lookup, close callbacks | BluMint calls the bridge | yes, already built |
| `POST /tournaments/match/submit` | Ember reports | yes, already built (status, progress, result, abort) |
| `GET /tournaments/search?gameId=…` | Ember reads | not yet: the only read there is |
| `POST /tournaments/match/setRetrieveStatus` | | no: BluMint never calls it |

There is no read for a bracket, a match, a queue or a participant list, and
no webhook. Everything about matches reaches Ember as a push, through the
create and close callbacks the bridge already serves. `/tournaments/search`
returns a tournament's title, dates (start, end, check-in, ready), region,
rules, blurb and sponsors, paged, filtered by `gameId`, `title` and `region`.

A create carries the two players (their Ember IDs, from lookup), the set
length and optional starting wins, plus BluMint's own `matchSettings` and
`spectators`. It does not name the tournament.

## Read-only on Ember's side

Ember writes nothing to BluMint beyond the reports BluMint needs to update its
bracket, which already exist. It creates no tournaments, changes no brackets,
seeds nothing and reports no forfeits. The room only reads:

- the matches BluMint pushes (create, close), as now;
- the event's details from `/tournaments/search`, for the room's header
  (title, start and end, rules), refreshed rarely and cached by the bridge.

The queue is Ember's own and lives in the room: it orders BluMint's matches,
it never makes one.

## Flow

1. **BluMint** runs registration and the bracket as now; players connect
   Discord to their Ember ID once (lookup).
2. **Organizer** gets an event-room link when the event starts, for example
   `https://embernetplay.link/e#<bridge id>/<tournament id>` (made by the
   bridge, from BluMint's tournament id). Opening it in Ember creates a
   private room bound to that event: the bridge records room ↔ event and
   gives the room a token.
3. **Players** join the room through its invitation, or are sent there when
   BluMint gives them a match (their match link opens the event room when one
   exists for it).
4. **BluMint creates a match** for two players. The bridge makes the match as
   now and, when either player is in an event room, also tells that room
   (over the room host's bridge connection). The room puts the set in its
   queue: on the streamer's table when the streamer plays it, else on the
   next free table.
5. **The set plays** as a normal Ember tournament match at that table;
   reports go to BluMint as now. When it ends, the next set in the queue is
   seated at once; players already in the room have no link to open.
6. **BluMint closes a match** (host took over, score entered, removed): the
   bridge ends it as now and the room drops it from the queue.

## What to build

| Part | Work |
|---|---|
| Bridge | Event room binding: `POST /v1/blumint/event-rooms` (organizer's Ember ID, tournament id) → room token; table room ↔ event; push each created and closed match whose player is in the room to the room host; cached `/tournaments/search` read for the room header. Migration. |
| Bridge | Event-room link page `/e#…` (like `/m#…`), opening `ember://blumint/room?...`. |
| Game | Open the link: create the private room, bound to the event; BluMint theme (BLUMINT_MODE.md). |
| Game | Room queue of BluMint sets: ordered by creation, the streamer's table first, seat the next set when a table frees, show "Up next" with the player tags. |
| Game | Players already in the room are seated by the queue, no link needed; a player outside gets the usual tournament-match notice. |

Rooms, tables, queues and tournament matches exist already
(`TRAINING_IN_ROOMS.md`, the Arcade room work, Katie's bridge); this adds the
binding and the routing, not a second system.

## Open questions for BluMint

1. **Which tournament is a create for?** The create does not name it. Can
   BluMint add the tournament id (in `matchSettings` or a field of its own)?
   Without it the room routes by player: a match whose player is in the room
   goes to the room, which fails only for a player in two events at once.
2. **How does the organizer get the tournament id** for the link? From the
   event page's URL, or can BluMint show Ember's event-room link on the host
   page when the game is Ember's?
3. **Matches ahead of time.** For a king of the hill, does BluMint create the
   streamer's next match only after the result, or can it create the next one
   or two early so they wait in Ember's queue? Ember can queue them either
   way.
4. **API key scope.** `/tournaments/search` needs our partner key
   (`X-Api-Key`); is that the same key reports use?

## Not in scope

Ember brackets, seeding, check-in, payouts and forfeits stay BluMint's.
