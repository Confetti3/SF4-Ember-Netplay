# SF4 Ember Netplay v1.5.2

v1.5.2 fixes what players reported on 1.5.1. The frame meter no longer shows to a player during a live match. The Wi-Fi and cable marks are back on the room board. Links pasted from Steam and chat apps now work in Join.

Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **From 1.5.1, update in Ember.** Ember offers the update at launch. Your settings, recordings and replays are kept.
- **The zip and setup.exe are on the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.5.2)** with their `.sha256` files.
- **Everyone in a room needs 1.5.2,** spectators included.

## Changes

- **The match frame meter is for watching.** Settings > Match frame meter now shows the meter over online matches you watch, never over a match you are playing. Before, only the player who had turned it on saw it, which gave one side frame data the other did not have. A Practice room still shows it to both players. Replays and video exports keep it.

## Fixes

- **Wi-Fi and cable marks on the room board.** Each player's connection shows again after their ping on table seats and Arcade cabinets, where it went missing in 1.5.0, and now also on the results screen. In the Members list it moved beside the ping.
- **Leaving a public room returns to the public room list.** The 1.5.1 fix only worked when the room closed at once. A normal Leave still went Home.
- **Links pasted from Steam and chat apps join.** Join and Paste room link now ignore what a copy adds around a link: line ends, invisible characters, quotes and brackets. They also read links that Steam chat wraps in its link filter or writes with `%23` for `#`. A public room link pasted into Join opens that room in Public rooms.
- **News:** a release's notes in the Reader are printed once, not twice over each other.

## Updates

- **In-game updates work on Steam Deck and Linux outside `drive_c`.** Ember refused to update an installation in a folder below `Z:\home`, which covers `~/Ember` from the older Steam Deck installer, so those installations could never update in the game. This applies from the update to 1.5.2 itself. Windows installations under a linked folder, such as a moved Steam library, update too.
- **A failed update names its log where it is.** The message shows the log's full path, and on Steam Deck and Linux also the path to it inside the game's Proton prefix.
