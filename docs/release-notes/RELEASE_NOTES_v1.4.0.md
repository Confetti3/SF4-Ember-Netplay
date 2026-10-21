# SF4 Ember Netplay v1.4.0

v1.4.0 shows USF4's own classes beside player names in public rooms, with a Rankings page under Online play. It adds sign-in to tournament sites such as Challonge and start.gg, watching tournament matches from their room, a Borderless window mode, hitboxes in Training, and extra pad buttons that copy a layout button. It also stops Windows taking the game for hung when only a pad is used, and from 1.4.0 on, updates inside Ember work on Linux and Steam Deck too.

Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **From 1.3.0 or 1.3.1 on Windows, update in Ember.** Ember offers the update at launch, and setup.exe is not needed. Settings, recordings and replays stay.
- **On Linux and Steam Deck, update to 1.4.0 with the zip.** The in-app update of 1.3.x still uses tar.exe, which Wine and Proton do not have. From 1.4.0 on, Ember extracts updates itself, so later updates work in Ember there too.
- **The zip and setup.exe are on the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.4.0)** with their `.sha256` files. For a new install, run `sf4-ember-netplay-1.4.0-setup.exe` into an empty folder (it refuses a folder that already has files), or extract `sf4-ember-netplay-1.4.0.zip` into a new empty folder as the README describes.
- **Everyone in a room needs 1.4.0.** This includes spectators, and a join needs the same build as the room. 1.3.x cannot join these rooms.
- **Nightly players can go back to Stable** as before.
- **Classes need a linked Discord account.** Without one your public room games do not count, and no class shows beside your name.
- **Going back to 1.3.1 keeps your settings.** 1.3.1 ignores the extra pad button choices and drops them the next time it saves your settings.

## Changes since v1.3.1

### Public room classes

- **Public rooms show each player's class.** The classes are USF4's own: D, D+, C, C+, B, B+, A, A+, Master and Grand Master, drawn with the game's emblems beside the name on table seats and member cards. Ember reads the emblems from your game install. Private rooms show none.
- **Classes come from public room games and belong to a Discord account.** Ember's server rates each game a public room records as won, for the Discord account linked to each fighter's Ember ID. Draws, cancelled games and dropped connections do not count, and nor does a game with a fighter who has no linked Discord account. A new Ember ID keeps your class once the same Discord account is linked. Games against players far from your class count for less. Ember shows the class only, never points.
- **A "?" means the class is still being placed.** It becomes a class after enough games against players who have one. A class moves only after a clear run of results and never drops on its own. After a long time without a game the emblem turns faint, and a few games settle it again. The tooltip on a faint emblem says the player has not played lately.
- **Rankings, under Online play, shows your class and the players by class.** The page shows your class, how you appear on the board, and how classes work. Players by class lists up to 500 players taking part with a settled class, highest class first and the most recent to play first within a class. It shows the in-game name and country flag that rooms show, never Discord names, and no points or positions. Faint and placing players are not listed. You are listed once you have opened the page, which shows how you appear and how to stop. If your service is not showing classes yet, the page says so and your games still count.
- **Stop taking part, or take part again, on the Rankings page.** Stopping removes your class from rooms and the board and your games stop counting for you and your opponents. Your rating is kept and resumes when you come back. You can change this once a day, and the page asks first.
- **Ember's developers show a Dev mark** in place of a class. It is Ember's own flame DEV art. Developers are not on the board, and their games are rated like anyone's.
- **Hide the emblems on your own screen.** Settings > Interface > Class emblems starts on. Off hides the emblems and marks in rooms, for you only; the Rankings page still shows your own class. Others still see yours and your games still count.

### Tournaments

- **Sign in to Challonge and start.gg from Ember.** Online play > Ember ID shows a row for each site your tournament service offers, beside Discord. Choose it, then Connect, and the site opens in your browser. Approve there with the account your tournament uses, and Ember shows the connected username. You can connect another account or disconnect later. One sign-in runs at a time, and Ember says when a site is busy or the service no longer offers it.
- **Settings > Discord names your tournament Discord account.** The new Tournament account row shows the Discord account your tournament site knows, which can differ from the one in the Discord app. Select it to connect or change it.
- **Watch a tournament match from its room.** Open the match link, then choose Watch this match on Tournament matches. Organizers and the staff they list can watch, and an organizer can also let anyone with the match link watch. You need an Ember ID, no room of your own open, and the same Ember version as both players. At most 6 people watch one match, staff included, and at most 2 of them through the match link. Watchers cannot sit, queue, chat or lock in, and Stop watching leaves the room. Ember says when the organizer has not opened a match to watchers. Matches on BluMint cannot be opened to watchers.
- **A tournament match keeps you from opening another room.** While you play or watch one, Ember will not open a room by hand until you finish or stop it under Tournament matches. Leaving the match room, or switching to a room from a Discord invite, ends your part in the match.
- **The final score stays up after a set, and Ember finds a restarted game sooner.** The room waits for the last game to close, up to 60 seconds, before it leaves, and the Ember menu returns whenever a game is cut short. After a lost connection Ember says so, then checks for the tournament's new match every 5 seconds for 3 minutes.
- **A set keeps its score through a drop.** (Joseph O'Connor) A player who drops and returns finds the set at the score it had reached. A set a site starts again after it ended early, as BluMint does, begins from the games already won. A fighter whose match room was locked by its host can still rejoin. Both players need 1.4.0 for a set that resumes.
- **BluMint sees each fighter's own Ready.** (Joseph O'Connor) The site can tell a player who has not pressed Ready from one waiting for them, and a game's Ready is cleared when the game is decided.
- **Paste a connect link that names its site.** (Joseph O'Connor) Paste link accepts the link as a site sends it in a message or email, with the site's name after the service ID.

### Display and controls

- **Borderless gives a frameless 16:9 window.** (FRaccie, #107) Settings > Player & controller > Borderless starts off. The game runs in a window without a frame at the largest whole 16:9 size of its monitor, with black bars beside it on a wider monitor. If the window cannot be set up, the game keeps its own window. While it is on, Alt+Enter and the game's own resolution and Full Screen options do nothing, and the game keeps your choice there for when it is off. Changes apply the next time the game starts.
- **Alt+Enter no longer drops you from a watched game.** (FRaccie, #135) Switching between window and full screen stops the game for about a second, which was long enough for the players' side to drop a spectator. Ember now keeps Alt+Enter from the game while you watch a game in a room, and it works again between games.
- **The touchpad, PS or Home button, the extra button some pads have (Share, or Mute on a DualSense) and paddles can copy a layout button.** Button layout lists each extra button your pad reports. Each can be Off or work as one of the ten layout buttons, as a Steam Input binding did. Choices are saved for each controller.
- **Every costume color has a render in the gallery.** (FRaccie, #132) The previews are drawn from the game's own models, textures and materials. They replace the mixed photographs and the "Preview not available" colors of fourteen fighters.
- **The menu art runs on behind the legend.** Full-screen menus no longer cut the backdrop off along a hard line above the key legend. The band under the list now fades in over the art and stays solid over the game.
- **The game is no longer taken for hung when only a pad is used.** With no keyboard or mouse input, Windows could put its Not Responding ghost in front of the game every few seconds. In full screen the game then lost focus, flickered to the taskbar, and a click on the ghost could have Windows close it. Ember now sends the game window an empty message about once a second. A game that really stops is still reported as not responding.

### Training

- **See the fighters' hitboxes.** (FRaccie) Hitboxes on the HUD page of the training controls draws the game's own boxes over the fight: green where a fighter can be hit, yellow where it can be thrown, red where an attack or a fireball hits, and blue where a throw connects. It starts off and shows with the HUD (F5 hides both) in offline Training only.
- **Meter settings moved to the Frame data page.** It is now the last row there, so the HUD page fits on one screen.

### Rooms and updates

- **Rejoining quickly no longer removes you again.** (JimboFGC, #115) If your game restarted and rejoined a room before the room had noticed you left, the room could retire your new session along with the old one. It now retires only the old one.
- **Update packages are extracted inside Ember.** (FRaccie, #109) Ember used tar.exe, which Wine and Proton do not have, so an in-app update on Linux or Steam Deck downloaded the package and stopped at "Could not extract update package". It now extracts the zip itself on every platform.

### Guides

- **Input lag guide.** (FRaccie) `docs/guides/INPUT_LAG.md` lists the Ember, game and PC settings that matter, recommends a window over Full Screen, and shows how to measure a change with a phone's slow-motion mode.
- **Firewall and Defender steps in troubleshooting.** (FRaccie, #93) `docs/guides/TROUBLESHOOTING.md` has PowerShell commands to allow Ember in Windows Firewall, check whether Defender acted on an Ember file, restore a quarantined one and add exclusions, with commands to undo them.

## Known issues

- **If the game still stops responding after switching to another window,** send `sf4e.log` and `launcher.log` and say which input you were using. The fix above for a game used with a pad is expected to cover the reports from 1.3.0 and 1.3.1.
- **Shared position save and reset are unavailable in rollback matches,** including Practice rooms. Offline Training keeps them.
- **Video export can stay unfinished** if the game stops rendering during export. Later replay requests can stay blocked until the game restarts.
- **Two rare mid-match crashes are still under investigation:** one during heavy rollback and one shortly after a spectator is dropped for falling too far behind.
- **Picking your fighter, costume, color or Ultra does not count as activity for the 90 second Ready timeout.** A chat message does count.
- **A two-player private room cannot survive its host's game crashing.** The remaining player needs a new room. Leaving normally is fine.
- **Browser links need a fallback on Linux and Steam Deck.** Copy the link and paste it into Ember instead of using Open in Ember.
- **Offline Training and the game's own replays still follow the Smooth frame rate setting.** The frame meter and replay playback are most accurate with the game's Frame rate on Fixed.

Report problems in [Discord](https://discord.gg/uPNqF5A5uq) with `sf4e.log` and `launcher.log` from `%APPDATA%\sf4e\logs`, the time it happened and what you were doing. For installer or update problems, include `%TEMP%\sf4-netplay-update.log` too.
