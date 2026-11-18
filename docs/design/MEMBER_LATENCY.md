# Member latency: what the member list shows

The room board shows a ping beside each member. It comes from a QUIC
connection each player's helper keeps to every other member on a separate
latency endpoint (`rust/sf4-net/src/service/latency.rs`) while the board is
open. The number is the smoothed round trip of that connection's selected
path, in whole milliseconds, with the route it was measured over (`direct` or
`relay`).

The board counts as open on a table's options page too, so a seat taken there
carries the same figures to the room's Max ping rule (`ROOM_PING_RULE.md`).

## What the list shows, and why

The latency endpoint is not the match's endpoint. In a public room it uses
relays only, so members who do not know each other never learn each other's IP
addresses from the member list. The match itself runs on the main endpoint,
which usually goes direct. A relay figure is therefore roughly the path to the
other player's home relay plus the path from theirs, not what the match will
run at.

The row says which it is:

| Row | Meaning | Colour |
| --- | --- | --- |
| `62 ms` | Measured on a direct path (a private room). It predicts the match. | Input delay bands |
| `Relay ~80 ms` (`~80 ms` where the row is narrow) | Measured through a relay. A direct match is usually faster. | Neutral |
| `...` | No value yet. | Neutral |
| `Direct 9 ms` / `Relay 84 ms` | The connection check's median on the match's own path. | Input delay bands |

The bands are the input delay bands (under 80 ms, under 150 ms, above), so only
figures that predict the match use them. The tip on the chip explains the
figure; a checked figure also gives its jitter.

## The checked figure

"Check connection" and Auto input delay run a five second probe on the game
path between two players. While that result counts, the row of that opponent
shows its median and route instead of the relay figure, and its tip reads
"Checked on the match's path: {route}, {median} ms, {jitter} ms jitter."

The result counts for the player seated across from the local player, while the
check is finished with a recommendation and was made at the table's current
revision. A new check, a change of opponent or a change of the table's revision
ends it, and the row returns to the member's own figure. The decision is one
pure function (`DecidePing` in `src/ui/LatencyMark.hxx`) shared by every
surface that draws a member's tag: the room board's list and cards, the table
and Arcade cabinets, and the post-match rows.
