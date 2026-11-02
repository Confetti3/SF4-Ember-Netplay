# SF4 Ember Netplay v1.5.0

v1.5.0 gives Ember a new look, with a new Home, Settings and room board, and adds Arcade and King of the hill rooms, where players sit at cabinets and the winner stays on. After a room game both players can choose a rematch that starts at once, without going back to the room. Ember's menus can be read aloud, and Training opens on a select screen for your fighter, the dummy and the stage. Room chat hides swearing on your screen, and crash reports can now carry a small crash dump.

Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **From 1.4.1 on Windows, update in Ember.** Ember offers the update at launch. Settings, recordings and replays stay.
- **On Linux and Steam Deck, update from 1.4.1 in Ember too.** From 1.3.x, update with the zip, as for 1.4.0.
- **The zip and setup.exe are on the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.5.0)** with their `.sha256` files. Setup now also updates an existing Ember folder, through the same updater Ember uses, so from an older version on Windows you can run `sf4-ember-netplay-1.5.0-setup.exe` over your install. For a new install, use an empty folder.
- **Everyone in a room needs 1.5.0.** This includes spectators. 1.4.1 and 1.5.0 cannot share a room.
- **Ember asks again before sending problem reports.** Reports now carry a small crash dump, so the old Send problem reports choice no longer counts. If it was on, Ember asks at your next crash.
- **Going back to 1.4.1 keeps your settings.** 1.4.1 then asks before sending a problem report, since 1.5.0 retires its setting, and it ignores the export video choices 1.5.0 remembers.

## Changes since v1.4.1

### A new look

- **Every menu has a new look.** (FRaccie) Home shows your profile card and news from Ember's releases. Settings is six tabs (Player, Matches, Interface, Accessibility, Online, About), each a list of panels beside the open panel's rows and a preview. Online play, Create, Join, Public rooms, the room board, Training controls, Replays, Rankings and News are redrawn, with new line icons. The old menus are gone, and with them the choice between the two looks.
- **Fighter select is one board.** Fighter, costume, color and Ultra are rows on it. Picking a fighter from a room closes it and puts you on Ready.
- **Messages arrive as notifications.** (FRaccie) Ember's messages show as notices with up to two actions and stay on a Notifications screen for the session. A Discord invitation arrives as a notification and a row on Home, and no longer switches your screen. A press that answers a notification does only that, and a notification never takes the focus while you type or answer a question.
- **Pads and keyboards reach every screen.** LB and RB move between tabs and wrap from the last to the first. On a DirectInput stick, Light punch and Heavy punch step the tabs, and the hint bar shows the buttons your device really has. The right mouse button is Back. On a pad, text boxes, room chat included, open an on-screen keyboard.
- **TV mode, under Settings > Interface > Look,** makes everything a quarter larger and keeps it inside a safe area at the screen's edges. Long headings and labels wrap instead of being cut, in every language and at every size.
- **The in-game interface scales with the picture's height too.** (FRaccie) Costume previews are lit like the training stage and draw each costume's special materials and cloth.

### Rooms

- **Instant rematch.** (FRaccie) After each room game both fighters choose Rematch, Character select or Back to room over the WIN screen, for 10 seconds. When both choose Rematch, the next game starts in place, without going back to the room. It is a new table rule, Instant rematch, on by default. There is no rematch while someone waits in the queue.
- **Room templates and table names.** (FRaccie) Room settings > Save as template keeps the room's settings and each table's rules and name, up to 12 templates. Create room > Template opens a room from one. The host can name each table, and the name shows on its card.
- **A private room can have 1 to 8 tables.** The host sets it on Create or later in Room settings. A table goes away only when nobody plays, waits or watches at it.
- **Chat filter.** Settings > Interface > Room layout > Chat filter, on by default, hides swearing and slurs behind stars on your own screen, in all of Ember's languages. Messages are still sent as written.
- **Chat shows who is typing, and pings show their route,** such as Direct 44 ms or Relay 62 ms. (FRaccie) A Ready still on its way can be taken back, and a held Ready drops once the opponent's seat empties.
- **Arrange the room board.** (FRaccie) The watched game's picture can be Small, Big or Off, and Members, Chat and the picture can each be hidden. The column and the members and chat split can be resized with the mouse or under Settings > Interface > Room layout. These work inside a room, also while you watch a game, and are saved at once.
- **Auto input delay has three styles.** (FRaccie) Under Input delay, Balanced works as before (1 to 3 frames), Responsive plays one frame less (0 to 2, more rollback) and Smooth one frame more (1 to 4, less rollback).
- **Edition Select rooms remember the edition you last played each fighter in.** In a room without Edition Select, your pick follows the room's rule.
- **Classes show on the match HUD and your profile.** Each fighter's class shows on the match HUD, and yours on your profile card, without opening Rankings first. Settings > Online > Rankings > Class emblems turns them off on your screen.
- **Ember says why a public room could not be made:** you already have one open, or your network has reached its limit.

### Arcade and King of the hill rooms

- **Two new room types.** (FRaccie) Create room > Room type offers Arcade and King of the hill beside Classic. Both seat up to sixteen players at cabinets: sit at any free one. In King of the hill a set's winner stays seated and plays the next challenger, the longest current win streak holds the top slot, and with nobody else seated the loser challenges again.
- **Arcade rooms rotate like an arcade.** When a set ends and someone is waiting alone at a cabinet, the winner plays them and the loser waits for the next player. Rematch or New opponent is asked only when nobody is waiting. In a room with no set length each win ends the set, so a player waiting alone always gets the next game. The post-match lines say who goes where, and member cards show each player's sets in the room (2 W / 1 L).
- **The post-match prompt keeps your cabinet.** While you two decide, its buttons are Rematch, New opponent, Character select and Back to room. Back to room never gives up your seat; Leave seat is on the board. In King of the hill with no set length, a drawn game can be played again in place.
- **Winner keeps fighter.** A new Arcade and King of the hill room option: as at a real arcade, a set's winner stays on the fighter and edition they won with and can still change the Ultra, costume and color. Their fighter select says so and opens on the Ultra.
- **Cabinets show the table's banner** (held start, Ready timeout, Training call), and members show the cabinet they are at. A player moved to another cabinet by someone else's action no longer has their first game there fail.

### Training

- **Training starts from its own select.** (FRaccie) Play offline > Training asks for your fighter, the dummy and the stage first, so Training loads without the game's character select.
- **Hitboxes can show the fighters' skeleton.** (FRaccie) The Hitboxes page has a switch for each layer: hurt boxes, throwable boxes and the skeleton.
- **The frame meter leaves hitstop and freeze frames out of its bars.** (FRaccie) The Classic meter shows each hit's frame advantage between its bars, shows free frames apart from empty cells, and colors a result by its frames.
- **Stun follows the game's own timing.** (FRaccie) A hit's stun follows the game's reaction script, knockdowns and the first getting up included, and a charge reply keeps its charge through the stun.
- **The training controls are tabs** with every value in a panel, and R3 opens them on a pad. (FRaccie)
- **The pause menu and View behave.** (FRaccie) A pad's Back under the game's pause menu no longer resets or saves the position. A reset keeps a recording made with the pause menu's Record and starts a running playback from its first frame. A new View on the pad option can leave View to the fight, for a plink on a button that also sends View.

### Replays

- **Share my replays.** (FRaccie) Settings > Matches > Training and replays > Share my replays, on by default, sends the games you play from then on to your bridge's public replay archive, with your name and country. An opponent is named only if they share too. Only games you played go, never ones you watched, and the replays already on your PC are not sent. Turning it off stops sending from then on. Organized tournament games are always shared, with both players' names; sets played through a partner site such as BluMint are not archived as tournament games. Each game is sent once, and one that fails to send waits without holding up the rest. See Known issues for when this starts working on Ember's main bridge.
- **Tournament sets in Replays.** (FRaccie) Replays > Tournaments lists the sets you played in tournaments, each with all of its games. Watch them in order, pick one, or save the set as one `.emberpack` file. Imported set packs show who played.
- **Export set videos** sets up a video of each game of a set in turn, its caption counting the set so far. (FRaccie)
- **Export video remembers your caption.** Player names, Match line, Set score, Ember mark and Frame meter keep your last choice between exports. Names and Set score still start off for a replay whose names or set Ember does not know. A side's set wins can be typed: Select opens a box, and Left and Right still step it.
- **The frame meter stays on the frames a slower playback speed holds.** (FRaccie)

### Accessibility

- **Ember's menus can be read aloud.** (FRaccie) Settings > Accessibility > Speech reads each row's name and value as you move. It uses SAPI 5 on Windows, and speech-dispatcher or a natural Piper voice on Linux and Steam Deck. F1 stops the voice.
- **One accessibility button: RT on pads and arcade sticks, F2 on the keyboard.** A tap reads the focused row's description or a notice's text, and a second tap stops it. A notice that opens by itself plays a chime.
- **Pre-rendered voice packs** download only when speech needs them, for English and seven more languages.

### Steam Deck and Linux

- **Guides for Steam Deck and Linux.** (FRaccie) `docs/guides/STEAM_DECK.md` and `docs/guides/LINUX.md` cover installing Ember under Proton.
- **Under Wine and Proton the room shows the right link,** Wi-Fi or cable. (FRaccie)

### Crash reports and updates

- **Crash reports can carry a small crash dump,** at most 4 MB of the game's thread memory, so a crash can be read in full. The old Send problem reports setting is retired, so if it was on, Ember asks again at your next crash. A report you send yourself shows its parts first, with Small dump on; turn it off there to send logs only. The texts that ask are translated in every language.
- **A crash report carries only the crash that happened,** not every crash since install. (FRaccie)
- **Update checks cannot hang the launcher.** (JimboFGC) They have one deadline and can be cancelled from the Updates window. A package that fails validation says why, and Ember says when GitHub is limiting update checks instead of calling it unreachable.

### Also new

- **Endless lobby music.** (oopswhats) The main menu plays a random stage or character theme, like the game's online lobby, and another one after every match. It is under Settings > Interface > Sounds and on by default; turn it off to get the menu's own track back.

### Fixes

- **A spectator dropped by the players no longer crashes.** (FRaccie)
- **A rollback save that runs out of memory ends the match** instead of crashing the game. (FRaccie)
- **The room host no longer closes the game** when a name in the room holds broken characters.
- **Ember's menus confirm on A and go back on B on every XInput pad,** even after Light kick was moved in the game's button settings, unless you choose other buttons for Menu confirm and Menu back. (FRaccie)
- **A pad or stick whose built-in mapping lacks a button is offered Set up controller.** Some built-in mappings leave out a button Ember needs, usually Heavy kick or the triggers (the Qanba Joystick Plus is one). Such a pad is now read through the game's own DirectInput until you run Set up controller, so Heavy kick and 3K can be bound. An Ember button layout saved for it applies again once the setup is done.
- **The "a participant left" warning shows in the first game of a new room too.** (FRaccie)
- **A watcher of a tournament match who joins through a relay gets five seconds to synchronize,** and is no longer dropped before the fight starts.

## Known issues

- **Ember's main bridge does not have the public replay archive yet.** Until the bridge is updated, Share my replays sends nothing from 1.5.0, and a tournament set's pack cannot be saved from it. Games played with sharing on can be sent once the archive is live there; turn Share my replays off if you do not want that.
- **A controller plugged in after the game started is not seen until a restart.**
- **If the game still stops responding after switching to another window,** send `sf4e.log` and `launcher.log` and say which input you were using.
- **Shared position save and reset are unavailable in rollback matches,** including Practice rooms. Offline Training keeps them.
- **Video export can stay unfinished** if the game stops rendering during export. Later replay requests can stay blocked until the game restarts.
- **A rare mid-match crash during heavy rollback is still under investigation.** The crash after a spectator is dropped is fixed. Crash reports with the small dump help find the rest.
- **Picking your fighter, costume, color or Ultra does not count as activity for the 90 second Ready timeout.** A chat message does count.
- **A two-player private room cannot survive its host's game crashing.** The remaining player needs a new room. Leaving normally is fine.
- **Browser links need a fallback on Linux and Steam Deck.** Copy the link and paste it into Ember instead of using Open in Ember.
- **Offline Training and the game's own replays still follow the Smooth frame rate setting.** The frame meter and replay playback are most accurate with the game's Frame rate on Fixed.
- **The non-English voice packs are provisional.**
- **Two Japanese Settings panel names are shortened to fit** (プレイヤーと操作, 練習とリプレイ). Better wording is welcome.

Report problems in [Discord](https://discord.gg/uPNqF5A5uq) with `sf4e.log` and `launcher.log` from `%APPDATA%\sf4e\logs`, the time it happened and what you were doing. For installer or update problems, include `%TEMP%\sf4-netplay-update.log` too.

## Testing

The full build passed all of its automated tests.
