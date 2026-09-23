# Private custom rooms

A room supports 16 members and four independent versus tables. A member can
play or watch one table at a time, or remain idle after joining. Each table can
hold two fighters and up to 14 spectators. Everyone must use a compatible
launcher and sidecar build.

## Open a room

1. Start the game through the launcher and wait for networking to become ready.
2. At the main menu, open **Host room**, choose the room name, capacity, and
   default battle rules, then create the room.
3. Use **Copy invitation** to share the private invitation with your players.
4. Guests open **Join room** and paste the invitation. Joining a room leaves
   them idle until they explicitly choose a table and queue or watch.

The host manages the room independently of the P1 seat. The host may stay idle,
play at any table, or watch.

## Play and rematch

Choose a table and select **Join queue**. Open seats fill in queue order. Seated
fighters choose their characters and both select **Ready** before each game.
Before preparation begins, a fighter can withdraw readiness with **Unready**.

Battle setup shows **Recommended delay** and **Selected delay**. Your saved
selection is retained; a new profile starts at two frames. **Check connection**
measures the connection to the other fighter for five seconds. **Apply
recommendation** copies a valid recommendation into your selection. You can
also adjust Selected delay from zero to ten and Ready without a usable probe.
Ready locks your own delay for that game; Unready unlocks it. A route change or
a new recommendation never changes delay during a fight.

Advice requires at least 80 valid replies out of 100 probes from the current
connection and path. It estimates one-way latency as half of the measured 95th
percentile round-trip time and allows two frames of prediction. These are
tuning assumptions, not a guarantee about frame timing or network quality.

Rematches are unlimited. Results count wins for the current fighter pair; they
never force a seat rotation. The running count, such as 2 - 1, replaces "VS" on
the table card and "vs" on the match HUD. Both fighters must ready again after returning to
the room. When a fighter leaves their seat, the next queued member takes that
seat and the new pair starts with fresh win counts. An unresolved game does not
award a win or erase earlier wins for the same pair.

Queued members automatically watch from the next game while retaining their
queue positions. Leaving the queue and stopping spectating are separate actions.

## Watch

**Watch next game** reserves a spectator place without joining the queue. A
game already in progress cannot be joined halfway through. **Stop watching**
returns the spectator to the room; the fighters continue playing.

The fighters never wait for a spectator. A spectator whose connection is still
being set up when the fighters are ready (after a short grace period) misses
that game and watches the next one. A spectator that falls about half a second
behind the live game is dropped from it. A spectator still closing out the
previous game does not stop the fighters starting the next one; it joins the
game after that.

## Manage the room

The host can rename the room, lock admission, change its capacity, edit a waiting
table's battle rules, and kick members. Capacity cannot be reduced below current
membership. A kicked peer cannot rejoin the same room under another name.

Room chat retains the latest 100 messages, with a limit of 256 UTF-8 bytes per
message. A member's messages are removed when they leave the room. **Mute chat** hides a member's messages locally.

Both fighters report the game's native outcome. A win is counted only when the
reports agree. Conflicting reports, or missing reports after the result deadline,
leave the game unresolved. The host can **Cancel unresolved game** to allow the
fighters to ready again without awarding a win or removing previously counted
wins. This keeps a disagreement from becoming an automatic result.

The member menu offers **Transfer host** to the current host. Normal host
departure transfers moderation to the oldest eligible remaining member. The
room's technical coordination leader is separate from that moderation role.

## Recover room control

Room mutations pause immediately when coordination becomes unavailable. A
healthy game can continue on its existing gameplay connection while room
control reconnects. Native results wait for committed matching fighter reports;
the result deadline pauses while room control is recovering.

After 15 seconds, **Replace room** is offered. It remains unavailable while
local gameplay owns its sockets. If preparation is incomplete, selecting it
explicitly cancels preparation and waits for its local connections to close.
It then opens a fresh room with new invitations and authority. Saved preferences and
local records remain. The confirmation defaults to Cancel, and recovery of the
old room continues while you decide.

A two-member room needs both voters. Unexpected loss of either member cannot
recover the old quorum; use the replacement action after gameplay has retired.
Larger rooms can recover when their existing voter majority is available.
The minority never recreates the old room.

New room invitations use `sf4e3:` and Discord invitations use compact `emd2:`
secrets. Copy a fresh invitation after an authority change; old authority
references and expired invitations are rejected.

## Validation status

See [CUSTOM_ROOMS_STATUS.md](../validation/CUSTOM_ROOMS_STATUS.md) for the exact build and test
evidence. Recovery is being validated in the designated integration candidate;
the preserved September 8 package does not contain these changes. Local
component and synthetic helper tests do not establish native gameplay,
multi-machine acceptance, or installation acceptance.
