# SF4 Ember Netplay v1.1.0

v1.1.0 adds public rooms, tournament matches, short room links and automatic input delay. Rooms gain first-to sets with queue rotation, a chat conversation and a new match HUD layout, and Ember now has a Windows installer, an update channel for pre-releases and Play in the background for streamers.

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **Everyone in a room needs 1.1.0.** 1.0.x and 1.1.0 cannot join each other's rooms, and the release candidates cannot join 1.1.0 rooms either. Public rooms and tournaments need an Ember ID.
- **From v1.0.2 or 1.1.0-rc2, use the updater.** Ember offers 1.1.0 in game. You can also download the zip below and extract it over your Ember folder. Your settings carry over; Input delay moves to Auto as described below.
- **From 1.1.0-rc1, install it by hand.** The rc1 updater does not see this release. Extract the zip over your rc1 folder.
- **New installs can use setup.exe.** `sf4-ember-netplay-1.1.0-setup.exe` installs Ember for your Windows account into an empty folder, with a Start menu entry and an uninstaller. It also installs a newer Visual C++ runtime when yours is too old, which asks for administrator rights. To update an existing folder, use the updater or the zip.

## Public rooms

- **Find players from the room list.** Open Online play > Public rooms and choose Set up public rooms to get started. Room cards show the host, players, free seats and lock status, and the list refreshes on its own. Select a room to see its rules and how long it has been open.
- **Join a nearby room quickly.** Quick join looks for an open room near you with a free seat. Show filters the list to All rooms, Near me or Free seats.
- **Choose the rules before opening a room.** Create room lets you set Visibility to Public and choose the room's name, capacity and table rules. Public rooms default to First to 2 and Winner stays, and your rules apply to all four tables.
- **The room outlives its creator.** The oldest remaining member takes over as host. A public room closes when its last member leaves. The server holds up to 20 public rooms at once.

## Tournaments and Ember ID

- **An Ember ID that stays with you.** Open Ember ID from Home to create yours, connect accounts and find tournament matches. Backup and restore saves your ID and carries it to another PC.
- **Connect Discord for tournament sites.** Connect Discord links your Discord account to your Ember ID so sites such as BluMint can find you.
- **Open your assigned match and play.** Tournament matches lists your matches, and Paste match link accepts a link from an organizer or tournament site. Choose Play to enter the match room and ready before each game. Results are reported automatically and the set ends when a player reaches the required wins.

## Rooms and matches

- **Play a set, then rotate.** Set length offers First to 1, 2, 3 and 5. After a set offers Winner stays, Loser stays or Both rotate; outgoing fighters go to the back of the queue and waiting players take their seats.
- **Follow the score and streak.** Table cards and the match HUD show the score, and consecutive set wins appear under the winner's name.
- **Check your Ultra after an opponent changes fighter.** If you are ready and your opponent changes fighter, Ember takes back your Ready and names the new fighter on the table card.
- **See who a start is waiting for.** A start held for a spectator names them and counts down for up to 10 seconds; either fighter can choose Cancel the start.
- **See when someone is idle.** Members who have not readied show an idle time after a minute without activity.

## Input delay

- **Auto input delay, contributed by fabeloper.** Auto came from [fabeloper's pull request #20](https://github.com/Confetti3/SF4-Ember-Netplay/pull/20). It checks the connection when a new opponent sits down and picks 1 to 3 frames; both fighters use the higher of their two delays.
- **A bounded wait before Ready.** Auto retries a failed check once and falls back to 2 frames after about 12 seconds. Rematches and fighter changes keep the measurement, and delay never changes during a fight.
- **Existing profiles move to Auto.** To keep a fixed delay, press Right on Input delay to leave Auto, then choose 1 to 10 frames. 0 frames is no longer offered, and a saved 0 becomes 1.

## Chat, links and languages

- **Read the conversation in one place.** Chat shows a scrolling conversation with joins, departures, host changes and results. The message box is ready when you open Chat, and an unread count appears while you are elsewhere.
- **Share a short private-room link.** Copy short link gives a link or short code another player can paste into Join room. Invitations now last about a week while the room stays open, replacing 1.0.2's link length choice, and short links follow the room when its host changes.
- **Open public rooms and matches from a link.** Copy room link shares a public room through embernetplay.link/r, and the room and tournament pages offer Open in Ember. A link that arrives while you are busy waits instead of pulling you out of your match.
- **Room links from community chat.** Where the community's bot is available, Discord's /room command and Twitch's !room (for the broadcaster or a moderator) open a public room and post its link.
- **Translations.** The new menus and messages are translated across all 14 languages.

## Controllers, HUD and streaming

- **Launch through Steam with Steam Input.** Ember accepts being started through the game's Steam launch options, so Steam Input controllers reach the game.
- **A familiar fighter grid.** Fighter select and Choose your main follow USFIV's character-select order, and Ultra choices have I, II and W badges with written notation.
- **A new default match HUD.** Split puts each player's name over the game's PLAYER label and the score, Ping, Rollback, Delay and spectator count in a small panel. Match HUD layout still offers Ember strip, and Match HUD position and Edge spacing place the panel.
- **Name height.** If you changed the game's own HUD position option, the names can miss the PLAYER labels. Interface > Name height moves them up or down to match. If moving the names up leaves too little room for a readable panel at the top, the panel moves to the bottom corner on its side.
- **Play in the background.** Player > Play in the background keeps the game's sound and your controller working while another window, such as OBS, is in front. The keyboard and Ember's menus still need the game window, and fullscreen still minimizes when it loses focus. It is off by default.
- **The HUD follows the game's resolution.** When the game's resolution differs from its window, Ember's overlay now lays itself out at the game's resolution, and the mouse follows.

## Updates and installing

- **Choose your update channel, contributed by FRaccie.** The updater window shows Update channel and Installed version. Stable offers finished releases; Pre-release also offers release candidates. Switching a pre-release installation back to Stable offers the latest finished release as the way back.
- **Uninstalling keeps your files.** The uninstaller removes only the files Ember installed, plus the updater's backups. Replays, notes and art you added yourself stay.
- **Updates check package files more carefully.** An update stops with a message when the installed file list cannot be read, instead of guessing which files are Ember's.

## Fixes

- **Connection checks no longer depend on matching PC clocks.** A difference between the players' clocks could make a check time out and interrupt the room connection.
- **Readying or watching during a check no longer breaks the room connection.** An outdated check now ends without disconnecting anyone.
- **More reliable room recovery over relays.** After a player disappeared, the remaining room could repeatedly stop responding. The remaining players now keep communicating while the missing player is removed.
- **Fresh public rooms connect more promptly**, particularly when a relay is needed.
- **Tournament results are not silently lost.** A result that cannot be saved is retried; if it still fails, the match stops with a message.
- **Steam installations are found more reliably** when Steam uses a differently named folder.
- **Unexpected exits are reported more clearly.** The launcher records fatal game exits that previously left no crash record.
- **Akuma's Raging Demon input** now shows forward instead of back.

## Known issues

- **A message sent just as its sender leaves can be missed.** Messages already received stay in Chat.
- **A two-player private room cannot survive its host's game crashing.** The remaining player still has to open a new room. Leaving normally is fine.
- **Auto input delay still needs long-distance testing.**
- **Browser links need a fallback on Linux and Steam Deck.** Copy the link and paste it into Ember instead of using Open in Ember.

Report problems in [Discord](https://discord.gg/uPNqF5A5uq) with `sf4e.log` and `launcher.log` from `%APPDATA%\sf4e\logs`, the time it happened and what you were doing. For installer or update problems, include `%TEMP%\sf4-netplay-update.log` too.

## Testing

1.1.0 is 1.1.0-rc2, which testers have been playing, plus Name height. The full build passed all 105 automated tests, including new checks that Name height moves the names by the chosen amount and that the HUD panel stays readable, on screen and clear of the names, moving to the bottom corner when the names leave too little room above them. The installer test installed setup.exe into an empty folder and uninstalled it while keeping player files. The network room tests passed for rc1 and were not run again, since nothing after rc1 changes room or match code.

Play in the background was tried in game behind OBS. Name height was checked against a stream frame from a player whose game HUD position was changed. This exact build has not been tested between two PCs over the internet.
