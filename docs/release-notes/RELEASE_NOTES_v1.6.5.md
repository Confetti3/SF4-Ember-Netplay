# SF4 Ember Netplay v1.6.5

v1.6.5 is a patch for 1.6.4. One relay check that misses your relay no longer moves you off it, which is what kept changing relays every few minutes on 1.6.4. A room table no longer stays stuck after a game, and public Classic rooms can have up to 8 tables.

Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **From 1.6.4, update in Ember.** Ember offers the update at launch. Your settings, recordings and replays are kept.
- **The zip and setup.exe are on the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.6.5)** with their `.sha256` files.
- **Everyone in a room needs 1.6.5,** spectators included.

## Changes

### Connection

- **One missed check no longer moves your relay.** Ember measures the relays about every 20 seconds and keeps only the first few answers. When your own relay was not among them, 1.6.4 moved you to another relay, often a farther one, and moved you back about half a minute later. Players' logs showed this happening about every five minutes. Ember now keeps your relay through one check that did not measure it. It moves when the next check, about 20 seconds later, misses it too, or when another relay is clearly faster, so a change of relay is now rare rather than impossible.
- **The Europe relay was holding packets, and that is fixed on the server.** Since mid-November Ember's Europe relay held game packets for about a tenth of a second every few seconds, which shows in a match as rollback jumping to 7 frames. It was fixed on the relay on 25 November and needs no update. If you still see these spikes in relayed matches, send a report with **Report a problem**.

### Rooms

- **Public Classic rooms choose their tables, up to 8.** A public Classic room always opened with four tables and its host could not change that. Create room now offers the number of tables for it, as for a private room, and the host can add tables or take empty ones away in Room settings. When everyone has left, the room goes back to four. A tournament room keeps the tables it was given.
- **A table frees itself after a game.** When one player's game was slow to close after a match, or their connection to the room had restarted, the table stayed on "Waiting for all players and spectators to finish returning from the previous match" until that player left the room. The room now stops waiting after 15 seconds.
- **The unresolved result messages say where Abandon is.** They now point to **Table options**.
- **Player 2 faces player 1.** On table cards and Arcade cabinets the second player's picture is mirrored, so the pair face each other. A cabinet's FP moved to the top-left corner, clear of the Ultra mark.

## Known issues

- **A busy room can drop you if your connection to it stalls.** In a full public room, a player whose connection to the room hangs for several seconds can get "You lost your connection to the room". Join the room again from Public rooms.
- **A fight can fail to load after a long session,** in Training or online: the game keeps drawing but the fight never starts. The cause is still being looked into. Playing borderless or restarting the game after a long session may avoid it. If it happens to you, send your `sf4e.log` from that session.
- **Players in South-East Asia may be connected through a farther relay** when n0's Asia relay does not answer them. There is no Ember relay in Asia yet.
- **The Reply moves list has no EX version of Rolento's Mekong Delta Attack.**
