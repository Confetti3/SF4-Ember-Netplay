# SF4 Ember Netplay v1.6.5

v1.6.5 is a patch for 1.6.4. Your relay no longer changes every few minutes, which 1.6.4 was meant to stop and did not for many players, and a room table no longer stays stuck after a game.

Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **From 1.6.4, update in Ember.** Ember offers the update at launch. Your settings, recordings and replays are kept.
- **The zip and setup.exe are on the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.6.5)** with their `.sha256` files.
- **Everyone in a room needs 1.6.5,** spectators included.

## Changes

### Connection

- **Your relay stays where it is.** Every five minutes Ember measures the relays again and keeps only the first few answers. When your own relay was not among them, 1.6.4 moved you to another relay, often a farther one, and moved you back about half a minute later. Players' logs showed this happening on most of those checks. Ember now keeps your relay through one check that did not measure it, and only moves when a second check in a row misses it or another relay is clearly faster.
- **The Europe relay was holding packets, and that is fixed on the server.** Since mid-November Ember's Europe relay held game packets for about a tenth of a second every few seconds, which shows in a match as rollback jumping to 7 frames. It was fixed on the relay on 25 November and needs no update. If you still see these spikes in relayed matches, send a report with **Report a problem**.

### Rooms

- **A table frees itself after a game.** When one player's game was slow to close after a match, or their connection to the room had restarted, the table stayed on "Waiting for all players and spectators to finish returning from the previous match" until that player left the room. The room now stops waiting after 15 seconds.
- **The unresolved result messages say where Abandon is.** They now point to **Table options**.
- **Player 2 faces player 1.** On table cards and Arcade cabinets the second player's picture is mirrored, so the pair face each other. A cabinet's FP moved to the top-left corner, clear of the Ultra mark.

## Known issues

- **A busy room can drop you if your connection to it stalls.** In a full public room, a player whose connection to the room hangs for several seconds can get "You lost your connection to the room". Join the room again from Public rooms.
- **A fight can fail to load after a long session,** in Training or online: the game keeps drawing but the fight never starts. The cause is still being looked into. Playing borderless or restarting the game after a long session may avoid it. If it happens to you, send your `sf4e.log` from that session.
- **Players in South-East Asia may be connected through a farther relay** when n0's Asia relay does not answer them. There is no Ember relay in Asia yet.
- **The Reply moves list has no EX version of Rolento's Mekong Delta Attack.**
