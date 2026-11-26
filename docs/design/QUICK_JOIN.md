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
  one with none), ascending, so it never pairs a class with `host_name` or `fighters`.
- `waiting`: at most 8 players a newcomer would play at once, in the order they would meet
  them (`room::WaitingAlone`), each with their class and the area they showed when they
  joined. In a Classic room that is each table in play where one fighter sits alone
  between games with nobody queued, in table order: the newcomer picks the table. In a
  room that seats by cabinet it is each lone cabinet, longest waiting first: the newcomer
  plays that one.

Latency is not knowable before joining: a public room's member latency runs through
relays, only after joining. Area is the ping signal: the waiting player's own area, else
the room's (its creator's).

## The wire

| Step | Change |
| --- | --- |
| Join | `SessionJoinRequest` carries `area` beside `country` (`ShownArea`: none while the player hides it). The room keeps it as `Member::area` and in its snapshot. |
| Room host | `roomhost::DetailsOf` adds `classes` and `waiting` to the status line's `details`, each only when it has an entry. The largest details object stays under the supervisor's 2048 bytes (`room_host_config_test`). |
| Bridge | `QuickJoin::from_value` reads both leniently, each field alone. They are kept in memory only (`Shared::quick_joins`, no migration): each poll rebuilds the map from every room the supervisor reports and swaps it in after its write commits, so a restarted bridge lists none until its first poll (5 s). |
| Listing | A player's `GET /v1/rooms?detail=1` carries both. While classes are not shown (`rooms.skill_badges` off) it leaves `classes` out and lists every waiting class as 0. The plain listing, a connection's listing and the web page's `/v1/rooms/public` never carry either: the page needs no account, and a one-member room would tie a class to its host's name. |
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

The opponent is a waiting player: the first listed in a room that seats by cabinet, the
best fitting in any other room. With nobody waiting it is the room as a whole: its area
and the lower median of its members' non-zero classes.

The player's own band is their class while they are Ranked and classes are shown
(`SeekerBand`); without one every rank is Unknown. With nothing new listed every room
ties on rank and waiting, and the pick is the list's own order with near rooms first, as
before.

## What the player sees

- The Filters page has a Quick join row: "Near my class first" (default) or "Any class".
  Under Any every rank is Unknown. It hides no room, so it is not an active filter and
  Clear filters keeps it (`RoomFilter::Cleared`). It is saved as `opponents` in the
  filter's object.
- When the player has a class, wants similar ones, some room listed classes and the pick
  is not Close or Wide, the status line says once that nobody near their class had a free
  seat (`public.quick_wider`).
- A room's card shows its lowest and highest class emblems, and its detail line names them
  (`public.room_detail_classes`, `public.room_detail_class`).

## Not done here

- A measured ping before joining: rejected in the study (probes would put endpoint ids in
  a public listing, cost hundreds of dials per press and measure the relay path).
- A follow-up that ranks rooms whose host set a Max ping rule of 120 ms or less first
  within the Near band (the room's `maxPingMs` in the listing) is the next step.
