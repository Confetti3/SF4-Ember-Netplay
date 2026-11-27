# Quick join: players near you, near your class

Quick join is one press on the Public rooms list. It picks the room whose players fit the
player best and asks for its ticket, as pressing the room's row does. It orders the rooms
it could join and never rules one out, so it still finds any open seat. This page is the
design as built; the study it came from is `notes/design/QUICK_JOIN_MATCHING_DESIGN_2026-11-16.md`
in the workspace.

## What it can know

Before joining, the client knows only what the bridge's detailed listing says. Two facts
were added for Quick join:

- `classes`: one class band per member (1 to 10, 0 for a member placing, a developer or
  one with none), ascending. It is not in member order, so in a room of several members it
  does not say whose class is whose, or which `fighters` entry goes with it. In a room of
  one the class is that member's, and `host_name` names them when they moderate.
- `waiting`: at most 8 players a newcomer would meet at once (`room::WaitingAlone`), each
  with their class and the area they showed when they joined; for a room of one that pairs
  the member's class with their area. In a Classic room that is each table in play where
  one fighter sits alone between games with nobody queued, in table order: the newcomer
  picks the table. In a room that seats by cabinet the room host lists only the lone
  cabinet that has waited longest, since that is the player a newcomer plays; the client
  reads no seating rule of its own.

Latency is not knowable before joining: a public room's member latency runs through
relays, only after joining. Area is the ping signal: the waiting player's own area, else
the room's (its creator's).

## The wire

| Step | Change |
| --- | --- |
| Join | `SessionJoinRequest` carries `area` beside `country` (`ShownArea`: none while the player hides it). The room keeps it as `Member::area` and in its snapshot. |
| Room host | `roomhost::DetailsOf` adds `classes` and `waiting` to the status line's `details`, each only when it has an entry. The largest details object stays under the supervisor's 2048 bytes (`room_host_config_test`). `room::WaitingPlayer` and `room::MaximumWaiting` (`RoomTypes.hxx`) are the one C++ definition of a waiting entry and its limit; `MAX_WAITING` in ember-protocol is guarded against the room host's assert by a test there. |
| Bridge | `QuickJoin::from_value` reads both leniently, each field alone. They are columns of the `rooms` table beside `fighters` (`classes`, one byte per member; `waiting`, a class byte, an area length byte and the area for each player; migration 068, which nightly first committed as 067), parsed by `Details::from_value` and written by the poll in the statement that writes the room's other details, so a change of either is a change of details and a restarted bridge lists them at once. A report with a `details` object replaces both (one it leaves out or that is not valid is cleared); one without leaves them as they were. |
| Listing | A player's `GET /v1/rooms?detail=1` carries both. While classes are not shown (`rooms.skill_badges` off) it leaves `classes` out and lists every waiting class as 0. The plain listing, a connection's listing and the web page's `/v1/rooms/public` never carry either: the page needs no account. A player's detailed listing does tie them in a room of one: its class sits beside `host_name` (when that member moderates), and its waiting entry adds the area. |
| Client | `DecodeDetails` reads each list whole or not at all. |

A class comes only from the room's signed ticket, which carries one only while classes
are shown and the player takes part. Players list only their own build's rooms, so in
public rooms a new client always meets a new room host.

| Client and room host | Bridge | Behaviour |
| --- | --- | --- |
| before | any | Unchanged: the host writes neither key. |
| new | before | The bridge ignores unknown details keys; Quick join orders rooms as before. |
| new | new | As described here. |

## The choice

`netplay/QuickJoinFit.hxx` is pure. Its candidates are the rooms the panel would let the
player press (joinable, not refused this visit, listed by the saved filter) in the list's
own order (`PublicRoomsPanel`'s `Before`: joinable, the player's area, its neighbours,
free seats, members, age, id). Each room's fit is compared key by key, and the first of
the best wins a tie:

1. Ping: Near (the opponent's area is the player's or next to it, `AreaNear`) before Other.
2. Rank: Close (classes 0 or 1 apart), Wide (2), Unknown (either side has none), Far (3 or
   more). Unknown sits between Wide and Far so players still placing get games.
3. Someone waits alone to play before nobody.

The opponent is the best fitting of the waiting players the room lists (one in a room that
seats by cabinet, so no seating rule is read here). With nobody waiting it is the room as a
whole: its area and the lower median of its members' non-zero classes.

The player's own band is their class while they are Ranked and classes are shown
(`SeekerBand`); without one every rank is Unknown. A player who asked for any class seeks
with band 0, which is the same: no further case is needed. With nothing new listed every room
ties on rank and waiting, and the pick is the list's own order with near rooms first, as
before.

## What the player sees

- The Filters page has a Quick join row: "Near my class first" (default) or "Any class".
  Under Any the seeker has no band. It hides no room, so it is not part of the filter
  (`RoomFilter`): it is the `quickJoinAnyClass` preference, saved beside the filter as a
  plain bool, and Clear filters leaves it as it is. (A filter saved by a build that kept it
  inside the filter's object, as `opponents`, still loads; the key is ignored.)
- When the player has a class, wants similar ones, some room listed classes and no room
  Quick join could pick ranked Close or Wide (not only the room it picked: ping comes
  first, so a nearer room with a far class can win over a farther room with a close one),
  the status line says that nobody near their class had a free seat while it waits
  (`public.quick_wider`). The panel's `QuickPick` only chooses; `Join` asks for the
  room and carries that flag into the pending request.
- A room's card shows its lowest and highest class emblems, and its detail line names them
  (`public.room_detail_classes`, `public.room_detail_class`).

## Not done here

- A measured ping before joining: rejected in the study (probes would put endpoint ids in
  a public listing, cost hundreds of dials per press and measure the relay path).
- A follow-up that ranks rooms whose host set a Max ping rule of 120 ms or less first
  within the Near band (the room's `maxPingMs` in the listing) is the next step.
