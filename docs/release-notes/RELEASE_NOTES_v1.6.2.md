# SF4 Ember Netplay v1.6.2

v1.6.2 is a patch for 1.6.1. The public relays in the US and Europe are back beside Ember's own, Ember's menus show the game's own art behind them, and a set of fixes from player reports and from a review of the whole code: replays under Proton, the cursor while watching, menu timers after Alt+Tab, and updating over read-only files.

Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **From 1.6.1, update in Ember.** Ember offers the update at launch. Your settings, recordings and replays are kept.
- **The zip and setup.exe are on the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.6.2)** with their `.sha256` files.
- **Everyone in a room needs 1.6.2,** spectators included.

## Changes

### Connections

- **n0's public relays in the US East and Europe are back,** beside Ember's relays in Kansas and Europe. From 1.5.5 they were left out because some internet providers' router security features (Xfinity, Spectrum, Cox) blocked them; those providers' home lines reach them again. Ember connects through whichever relay answers fastest, so more players get a nearby relay.
- **The Network panel names the relay you are on:** Ember US (Kansas), Ember Europe, n0 US East, n0 US West, n0 Europe or n0 Asia.

### Menus

- **The game's own art behind Ember's menus.** Scenes from the game's arcade prologues and openings cycle behind every full-window page, with the game's painted jungle still when no scene can be shown. Endings are never used. The pictures are read from your own game files, never shipped with Ember. Turn it off in **Settings > Interface > Look > Background artwork**.
- **Menu timers survive Alt+Tab in exclusive fullscreen.** After a return, Ready and table offers could be ignored, requests could wait for good and the typing indicator could stay on. Training notices, the replay strip and the replay camera's pause had the same problem.
- **Interface size lists every size from 0.70x.** Below 1.00x the row had no value to choose from.

### Rooms

- **Fighters on a cabinet's screen.** A table shows each player's fighter on an Arcade cabinet screen, and Join queue and Watch are back at the foot of the card, centred, as in 1.6.0.
- **Invite in public rooms too.** A public room's bar has Invite with the friends you can invite and Copy room link.
- **The line under the tables** shows one line of what the lit place says, instead of three lines drawn over the Ready button.
- **A new room forgets the last table offer,** so Ready or a seat at the same table number is no longer ignored in the next room.
- **Room recovery is sturdier** after a member drops while the room is full, and a few rarer recovery cases no longer fail.

### Watching and replays

- **The mouse cursor stays visible and free while watching a room game,** on every monitor.
- **Replays work under Proton/Linux.** Ember finds the game's replay saves through the Steam client that launched the game, so "no save folder for the Steam account" no longer stops watching or importing.

### Cosmetic

- **The Arcade pair lines meet the cabinets.**
- **The selected member's frame no longer runs under the scroll bar.**

### Updates and settings

- **Updating works when a product file is read-only.** Since 1.5.7 such a file stopped the update with "Cannot flush update file". The update now checks every file it will replace before it starts.
- **One damaged value in settings.json no longer resets the rest.** A value of the wrong type keeps that one setting's default; your other settings, fighter picks and stage choices stay.

## Known issues

- **A fight can fail to load after a long session,** in Training or online: the game keeps drawing but the fight never starts. The likely cause is the game running short of memory space after many returns from Alt+Tab in exclusive fullscreen; playing borderless or restarting the game after a long session avoids it. If it happens to you, send your `sf4e.log` from that session.
- **Players in South-East Asia may be connected through a farther relay** when n0's Asia relay does not answer them. There is no Ember relay in Asia yet.
- **The Reply moves list has no EX version of Rolento's Mekong Delta Attack.**
