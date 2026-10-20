# SF4 Ember Netplay v1.3.1

v1.3.1 fixes matches that desynced in their first second for players whose game runs its Frame rate option on Smooth. It also brings the game to the front when you open a link, quiets the screen a little, and makes crash reports easier to act on.

Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **From 1.3.0, update in Ember.** Ember offers the update at launch. Settings, recordings and replays stay.
- **From 1.2.x, run setup.exe once.** Close SF4 and run `sf4-ember-netplay-1.3.1-setup.exe` over your current install. The zip, setup.exe and their `.sha256` files are on the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.3.1).
- **Everyone in a room needs 1.3.1.** This includes spectators. 1.3.0 cannot join these rooms.

## Changes since v1.3.0

### Fixes

- **Matches no longer desync at the start for players on Smooth frame rate.** With the game's Frame rate set to Smooth (`FrameRate=SMOOTH` in the game's `config.ini`), or with no FrameRate line in that file, the game moves a fight by the measured length of each frame instead of one frame at a time. Rollback replays frames back to back and each PC measures its own, so every match ended within its first second and looked like a disconnect. Ember now runs every match and watched game one frame at a time, as the game does for its own online battles, and puts your setting back when the fight ends. Your `config.ini` is not changed.
- **Open in Ember brings the game to the front.** With Ember already running, a match, room, public room, Discord or replay link opened from a website now restores the game if it is minimized and brings it forward. If Windows keeps the browser in front, the game's taskbar button flashes instead.
- **Spectators no longer see the unread chat count over the fight.** The line over a watched match shows only your place in the queue. The Chat row in the room still counts unread messages.

### Interface

- **The F10 hint fades after a few seconds.** "SF4 Ember Netplay / F10 or Start" shows for 4 seconds when Ember's menu closes, then fades. Settings > Interface > Menu shortcut hint turns it off.

### Crash reports

- **Crash reports name the calls that led to a crash.** The crash record now lists the game and Ember code on the crashing thread's stack, so reports sent without a crash dump can still be traced.
- **A frozen game is named as one.** When Windows closes the game as Not Responding, the launcher log now says the game froze instead of calling the exit code unknown. If your game freezes, please send `sf4e.log` and `launcher.log` and say what you were doing, especially if you had switched to another window.

## Known issues

- **The game can freeze after switching to another window.** A few players have reported this on 1.3.0. The cause is not known yet; logs from a freeze help.
- **Two rare mid-match crashes are still under investigation:** one during heavy rollback and one shortly after a spectator is dropped for falling too far behind. The new crash record should show where they come from.
- **Offline Training and the game's own replays still follow the Smooth frame rate setting.** The frame meter and replay playback are most accurate with the game's Frame rate on Fixed.
- **Shared position save and reset are unavailable in rollback matches,** including Practice rooms. Offline Training keeps them.
- **Video export can stay unfinished** if the game stops rendering during export. Later replay requests can stay blocked until the game restarts.
- **Picking your fighter, costume, color or Ultra does not count as activity for the 90 second Ready timeout.** A chat message does count.
- **A two-player private room cannot survive its host's game crashing.** The remaining player needs a new room. Leaving normally is fine.
- **Browser links need a fallback on Linux and Steam Deck.** Copy the link and paste it into Ember instead of using Open in Ember.

Report problems in [Discord](https://discord.gg/uPNqF5A5uq) with `sf4e.log` and `launcher.log` from `%APPDATA%\sf4e\logs`, the time it happened and what you were doing. For installer or update problems, include `%TEMP%\sf4-netplay-update.log` too.

## Testing

The full build passed all 190 automated tests.

In game, the same changes were played on top of the current Nightly: a two-PC match played to the end with one PC on Smooth frame rate and the other on Fixed, with no desync, plus the Training and replay self-tests. The frame rate fix was found from three 1.3.0 reports and confirmed in that match.
