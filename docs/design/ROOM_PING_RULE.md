# Max ping: a room rule

A room's creator can choose a Max ping: Off (the default), 80, 100, 120, 150
or 200 ms. A player whose measured ping to the players they would be paired
with is over it cannot take a seat, a queue place or a cabinet. Like Wired
players only, it blocks seats alone: they still join, watch and chat, and a
ping that cannot be measured never blocks. The host can change it in Room
settings; it binds the next seat taken and moves nobody.

## Which ping, measured where

A match is played player to player, so the figure that matters is the
requester's round trip to the players they would fight. Ember already measures
exactly that before anyone sits: the member latency each player's helper keeps
to every other member while the room board is open (`MEMBER_LATENCY.md`,
`rust/sf4-net/src/service/latency.rs`). It is the number the board shows
beside each member. No second measurement is added.

The other measurements do not fit a seat request:

- The connection check and Auto input delay (`rust/sf4-net/src/probe.rs`,
  `TickAutoDelay`) measure the match's own path, but only between two players
  already seated together, and only on one of their PCs.
- A finished game's ping (`TakeFinishedGame`) exists only after playing.
- The room's authority measures nothing between members: a private room's runs
  in a player's game, a public room's in the room host on the server, and the
  public room host binds no latency endpoint at all.

In a public room the member latency is measured through relays only, so
strangers never learn each other's addresses. A relay figure is roughly the
path to the other player's relay and on from it: higher than the direct path
the match usually takes, most of all between players near each other. The
rule compares it as measured, the figure the board shows the player. A pair
the rule passes is within the limit; a close call through a relay can be
refused, which errs on the side the creator asked for.

## Who it is compared against

The players the seat could pair the requester with:

- Classic: the fighters seated at that table and everyone queued there, since
  rotation can bring any of them across. An empty table has nobody to measure
  against: the first player sits, and the second is checked against them when
  they sit, so every pair that sits down together directly is checked.
- Arcade and King of the hill: every player at a cabinet. The room pairs
  whoever waited longest at whichever table is free, so any of them can be the
  next opponent. Moving between cabinets is not sitting down and is never
  checked.

A pair formed later by the queue's rotation is not checked again: each of its
players was checked against the other when the second of them arrived, unless
that figure could not be measured then (below).

## Measured, and enforced by the room

The requester's game attaches its measured figures to the seat request
(`Action::pings`: member and milliseconds, only measured values, only for the
players above, computed from the same room snapshot whose table revision the
request carries). The room's authority works out the opponents from its own
state and refuses the request with `RejectReason::PingOverLimit` when a figure
for one of them is over the limit. A request stamped against another table
revision is already refused as stale, so the authority and the requester
agree on who was measured.

The figure comes from the helper's measurement of the live connection, never
from anything the player types or chooses. The authority cannot verify it any
more than it can verify the reported network link: both rely on the same
build, which only same-build peers share a room with (`sidecarHash`).

Reporting every pair's ping to the authority all the time was considered and
rejected: it would add a stream of room commits to every room with the rule,
in the path whose backlog already limits the busiest rooms, to cover only the
pairs this design cannot measure either (players in games measure nothing).

## When the ping is not known

- Measuring (a member just joined or the board just opened): the board does
  not offer the seat yet and says the ping is being measured. It lasts
  seconds: the helper either measures a value or gives up on the member, and
  then the seat is offered.
- Not measurable: a player in a game, playing or watching (the latency
  endpoint stands down for both sides of a game), a member the helper cannot
  reach, or a board that has no figures yet. The pair counts as within the limit,
  as an Unknown link sits under Wired players only. Refusing it would keep
  everyone out of a busy Classic table, whose fighters and queue are all in
  the game, until it emptied.

So a newcomer queueing behind a game in progress is checked only against the
players there who are out of a game. This is the rule's known gap, and the
in-game check below looks at it.

## Rooms and the wire

- Private rooms get the rule from Create (`Charter::maxPingMs`); the authority
  starts with it. Public rooms are opened by the service with no rules, so the
  creator's game sends `SetMaxPing` once it is in the room as host, as it does
  `SetWiredOnly` (`netplay::CreatedRules`). The host changes it with
  `SetMaxPing`, a host room action like `SetWiredOnly`.
- Room state and checkpoints carry `max_ping_ms` only when on, and a seat
  request carries `pings` only when it has figures; both are read only by the
  same build, and a state without them reads as before. `SetMaxPing` and
  `PingOverLimit` are appended, so an older peer refuses them as unknown.
- The public room listing does not carry the rule in this change. The bridge
  would need a new rooms column and its listing a new optional field, while
  the list's filters and cards are being changed by another stream. Joining
  costs a player nothing under a seat-only rule, and the room's status line and
  each table's rules line say Max ping as soon as they are in. Nothing the
  bridge or a Stable 1.6.1 client reads changes.
- Create keeps the choice with the room defaults and in templates
  (`maxPingMs`, written only when on).

## Where it lives

- `src/session/RoomPingRule.hxx`: the choices, who a seat is compared against
  (`PingOpponents`) and the verdict over a set of figures (`PingOverLimit`),
  shared by the authority and the board so the two cannot disagree.
- `RoomAuthority::SeatRefusal`: Wired players only and Max ping, the one seat
  gate for a Queue and a cabinet.
- `session::SeatPings` (`MemberLatency.hxx`): the figures this PC's helper has
  for those opponents, and whether any is still being measured. The helper
  measures while the board or a table's options page is drawn, the two places
  a seat is taken.
- The board's `SeatFence` (`RoomControls`) says why a seat is not offered,
  for either rule.

## In-game checks (two PCs)

1. Private room, Max ping 80, two PCs whose board shows over 80 ms to each
   other: the second cannot take the open seat and is told the ping; under the
   limit it sits.
2. The same room with the limit raised by the host: the next seat is offered
   at once; lowered under a seated pair: nobody is unseated.
3. A public room created with Max ping: the room's status line shows it after
   creation (the creator's `SetMaxPing` reached the room host), and the seat
   gate refuses as above with relay figures.
4. Queueing behind a game in progress is allowed (the fighters' ping is not
   measurable), and the board right after joining waits for the measurement
   before offering a seat.
5. Arcade with Max ping: a player over the limit to anyone at a cabinet is not
   offered a free cabinet; one already at a cabinet still moves between them.
