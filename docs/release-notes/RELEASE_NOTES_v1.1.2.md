# SF4 Ember Netplay v1.1.2

v1.1.2 recommends input delay from your usual ping, takes an idle fighter out of the seat after 90 seconds, lets you set your fighter before you sit down, and shows the right region for public rooms. The Russian translation now uses the terms a native speaker suggested.

Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **Everyone in a room needs 1.1.2.** 1.1.1 and 1.1.2 cannot join each other's rooms.
- **From 1.1.1, use the updater.** Ember offers 1.1.2 in game. You can also download the zip from the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.1.2) and extract it over your Ember folder. Your settings carry over.
- **New installs can use setup.exe.** The installer is also available on the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.1.2).

## Changes since v1.1.1

- **Input delay follows your usual ping.** Check connection used to go by the slowest 5% of replies, so an 80 ms connection with a few spikes was recommended 2 frames. It now goes by the ping it shows, in steps players suggested: up to 80 ms is 1 frame, 81 to 150 ms is 2, 151 to 220 ms is 3, and 221 ms and up is 4, with one more frame for each further 70 ms. Large spikes can still raise it by one step. Auto can now choose up to 4 frames.
- **An idle fighter leaves the seat after 90 seconds.** As in the base game, when one fighter at a table is ready, the other has 90 seconds to ready. Any room action, such as sending a chat message, starts the 90 seconds again. When time runs out they leave the seat, stay in the room, and the next player in the queue sits down. The last 30 seconds show a countdown, and chat says who left and why. Tournament tables are not affected.
- **Set your fighter before you sit down.** Members who are in a room but not in a seat now have Fighter, Ultra combo, Appearance and Fighter options on the table page, so their pick is ready when they sit. Stage stays with Player 1.
- **Public rooms show their creator's region.** Every public room listed US East, the region of the server running it. A room now shows the region of the player who created it, both in Ember and on the website. Rooms created by older versions keep showing the server's region.
- **Russian translation.** Rooms are now лобби, fighters бойцы and stages арены, following a Russian player's suggestions for the Home screen, and the rest of the translation uses the same terms.
- **Less memory for Japanese, Korean and Chinese text on high-DPI displays.** At display scaling above 200%, the small match text no longer draws these characters at extra density.

## Known issues

- **Picking your fighter, costume, colour or Ultra does not count as activity for the 90 seconds.** Those choices stay on your PC until you ready. A chat message does count.
- **Spectators cannot stop watching until the game ends** ([#23](https://github.com/Confetti3/SF4-Ember-Netplay/issues/23)).
- **A message sent just as its sender leaves can be missed.** Messages already received stay in Chat.
- **A two-player private room cannot survive its host's game crashing.** The remaining player still has to open a new room. Leaving normally is fine.
- **Browser links need a fallback on Linux and Steam Deck.** Copy the link and paste it into Ember instead of using Open in Ember.

Report problems in [Discord](https://discord.gg/uPNqF5A5uq) with `sf4e.log` and `launcher.log` from `%APPDATA%\sf4e\logs`, the time it happened and what you were doing. For installer or update problems, include `%TEMP%\sf4-netplay-update.log` too.

## Testing

The full build passed all 108 automated tests. The networking helper and all server (bridge) tests passed. Every change went through code review until it was approved.

In game on one PC, this build opened a public room on the 1.1.2 server, and the room listed the creator's region.

A match between two PCs has not yet been tested on this exact build.
