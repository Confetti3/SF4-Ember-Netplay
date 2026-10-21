# SF4 Ember Netplay v1.4.1

v1.4.1 lets you record and play in Training from your pad with a button you choose, such as a stick click. Tournament match links now join their match by themselves, and a match whose host closed the game can be played again. It also makes a PlayStation touchpad, or any extra button, usable when you set up your buttons, and lets those buttons stand in for Select or Start.

Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **From 1.4.0, update in Ember.** Ember offers the update at launch. Settings, recordings and replays stay.
- **From 1.3.x on Windows, update in Ember too.** On Linux and Steam Deck, update from 1.3.x with the zip, as for 1.4.0.
- **The zip and setup.exe are on the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.4.1)** with their `.sha256` files.
- **Everyone in a room needs 1.4.1.** This includes spectators. 1.4.0 cannot join these rooms.
- **Going back to 1.4.0 keeps your button layouts.** 1.4.0 ignores the Training buttons and drops them the next time it saves your settings. If an extra button copies Select or Start, 1.4.0 turns that pad's extra buttons off.

## Changes since v1.4.0

### Training from the pad

- **Record and play with a pad button.** The button layout page has Training record and Training play rows. Select one and press the button you want, such as L3 or R3. In Training one press of it does what F7 or F8 does: Record goes to standby, then records, then stops, and Play plays the slot or stops it.
- **The button only records or plays in Training.** Any layout button can be chosen, including one that does an attack. In offline Training that button does nothing in the fight, so a press never attacks too. In matches and everywhere else it keeps its attack. Record and play cannot share a button, and Clear Training buttons turns both off.
- **The shortcut strip names your buttons.** With a pad, the strip over the frame meter shows the button you chose in place of F7 or F8. Back and Start on standby still starts a recording.
- **Quick presses act on the lab as it is.** Two Record or Play presses in a row are each decided when they arrive, so a quick double press no longer plays twice or records over a recorded slot without asking. A press made just before a room calls you back does nothing once the call comes.

### Tournaments

- **A match link joins its match.** Clicking a tournament match link while you are free now opens the match and joins its room by itself as soon as the match can be played, for two minutes after the click. A link clicked during a fight, in a room or during another tournament match waits for you to press Play.
- **A match you can play is a Play row.** Tournament matches shows each playable match as Play, with its round when the site names one and the score beside it. Before, the screen said to press Play but the match was a row named after its round.
- **A match can be played again after its host's game closed.** When the player hosting a tournament match's room closed or lost the game, neither player could get back in: Ember kept trying to join the closed room. Now, after two failed tries, Ember opens a new room for the match in its place, about 40 seconds after the first try. It waits while a game of the match is still being decided.

### Pad buttons

- **The touchpad counts as the button it copies.** A touchpad, PS button, Share button or paddle set to copy a button now acts as that button on the button layout page. Pressing it binds attacks, menu buttons and Training buttons, and it lights that button in the test. Pressing one that copies nothing explains how to give it a button.
- **Extra buttons can copy Select or Start.** The choices for an extra button now include View and Start, named as your pad names them (Share or Create and Options on a PlayStation pad). A touchpad that copies View resets and saves the position in Training as Back does.

## Known issues

- **If the game still stops responding after switching to another window,** send `sf4e.log` and `launcher.log` and say which input you were using.
- **Shared position save and reset are unavailable in rollback matches,** including Practice rooms. Offline Training keeps them.
- **Video export can stay unfinished** if the game stops rendering during export. Later replay requests can stay blocked until the game restarts.
- **Two rare mid-match crashes are still under investigation:** one during heavy rollback and one shortly after a spectator is dropped for falling too far behind.
- **Picking your fighter, costume, color or Ultra does not count as activity for the 90 second Ready timeout.** A chat message does count.
- **A two-player private room cannot survive its host's game crashing.** The remaining player needs a new room. Leaving normally is fine.
- **Browser links need a fallback on Linux and Steam Deck.** Copy the link and paste it into Ember instead of using Open in Ember.
- **Offline Training and the game's own replays still follow the Smooth frame rate setting.** The frame meter and replay playback are most accurate with the game's Frame rate on Fixed.

Report problems in [Discord](https://discord.gg/uPNqF5A5uq) with `sf4e.log` and `launcher.log` from `%APPDATA%\sf4e\logs`, the time it happened and what you were doing. For installer or update problems, include `%TEMP%\sf4-netplay-update.log` too.

## Testing

The full build passed all of its automated tests.
