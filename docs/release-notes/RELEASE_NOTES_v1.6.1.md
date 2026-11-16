# SF4 Ember Netplay v1.6.1

v1.6.1 is a hotfix for 1.6.0. Replays play to the end again, sitting down at a table takes one press, a private room has one Invite button, and Training has a way back to the room.

Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **From 1.6.0, update in Ember.** Ember offers the update at launch. Your settings, recordings and replays are kept.
- **The zip and setup.exe are on the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.6.1)** with their `.sha256` files.
- **Everyone in a room needs 1.6.1,** spectators included.

## Changes

### Replays

- **A replay no longer stops about 50 seconds into a fight.** In 1.6.0 the snapshots that let you jump back filled a table the game's save states share, and the replay ended with a netplay message that had nothing to do with it. The snapshots now stay within that table, and a snapshot that cannot be taken is skipped without ending anything.
- **Jumping back stays inside the current round** and always reaches the round's start.
- **"Watch now" watches.** The large button beside a replay, and beside a match in the archive, starts it in one press. In 1.6.0 it opened the list of choices instead.

### Rooms

- **Sit down in one press.** An open seat is its own button: "Play as P1" or "Play as P2". Click it, or on a pad choose the seat with Left and Right and press Select. The extra box of the same choices is gone.
- **Tables fill their column.** The tables of a room share the height they have: large fighter pictures and names in a big window, compact cards in a small one, so four tables fit at 720p without a scroll bar. The round rules are shown once at the TABLES heading, and a table shows its own only when they differ.
- **Join queue and Watch are on the table.** A table with players shows both at its top right.
- **A press cannot send twice.** A double click or a second press before the room answers is ignored.
- **Watching a game? Ember asks first** before a press that would end what you are watching.
- **"Leave seat" is on your own table; Ready up is on the bar.**

### Private rooms

- **One Invite button.** It lists the friends you can invite right now, then "Copy short link" and "Copy invitation". Choosing a friend sends the invite.
- **The room says who can join.** The chip at the top right reads "Friends can join" or "Invitation only", following the room's Privacy.
- **When friends cannot be invited, Ember says why:** Admission is Locked, or the room is full.
- **A friend's invite is shown as a friend's invite,** not as a Discord invitation, and joining a friend's room while you are in another asks the question once.

### Public rooms

- **Clearer messages when you lose a room.** A lost connection says so and tells you to join again from Public rooms; a room that turns you away just after a disconnect says your old seat may still be held and to wait a minute.

### Training

- **Back to room.** In Training entered from a room, F10 returns to the room, and the Training controls have a "Back to room" row. Your place in the room is kept.
- **Empty all slots.** Reply has a row that clears every reply slot after asking.
- **Move strengths are listed weakest to strongest,** EX last, in the Reply moves list (Elena's Mallet Smash, Dudley's Thunder Bolt, El Fuerte's Guacamole Leg Throw, Hugo's Hammer Mountain, Decapre's Scramble, Guy's Bushin Senpukyaku).

### Help & about, and Home

- **Links to Ember's Discord, Ko-fi, website and its developer's Twitter** in Settings > About > Help & about, and as four small icons at the top right of Home. Each asks before it opens your browser.

### Crash reports

- **A crash in code that is no longer loaded now names the module it was in,** and the report says whether a dump was asked for and what became of it.

## Known issues

- **A fight can fail to load after a long session,** in Training or online: the game keeps drawing but the fight never starts. It was reported on 1.6.0 as a Training freeze. The likely cause is the game running short of memory space after many returns from Alt+Tab in exclusive fullscreen; until it is fixed, playing borderless or restarting the game after a long session avoids it. 1.6.1 records what was loading and the memory state when it happens: if it happens to you, send your `sf4e.log` from that session.
- **Players in South-East Asia may be connected through the Europe relay** when the public Asia relay does not answer them. There is no Ember relay in Asia yet.
- **The Reply moves list has no EX version of Rolento's Mekong Delta Attack.**
- **There is no US West relay yet.**
- **Translations of the new text in 13 languages need a native read.** Better wording is welcome.

## Thanks

Sir LarpsALot for the replay, Training freeze, reply slot and move order reports; Bunkei for the way back from Training; Exis for the lobby drop reports and logs; HouseWD, G and Bogart Willey for their reports.
