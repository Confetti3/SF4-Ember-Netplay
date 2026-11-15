# SF4 Ember Netplay v1.6.0

v1.6.0 turns public rooms into a place with a history. Every rated game earns fighter points (FP) and account points (BP), your card becomes a profile with fighters, sets and milestone awards, you can add players as friends and join their rooms, and the public replay archive opens inside the game. Busy rooms no longer drop players, Training gets dummy reply slots, a camera and a phone remote, and FRaccie's tournament desk now runs on Ember's own bracket site.

Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **From 1.5.7, update in Ember.** Ember offers the update at launch. Your settings, recordings and replays are kept.
- **The zip and setup.exe are on the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.6.0)** with their `.sha256` files.
- **Everyone in a room needs 1.6.0,** spectators included.
- **Points (FP/BP), friends and player cards need an Ember ID connected to Discord,** the same as ranking. Connect it under Settings > Online > Discord account.
- **Points start from zero with this release.** Games played before it are not counted back.

## Changes

### Points and tiers

- **Fighter points (FP) and account points (BP).** Every rated public-room game earns 1 point for both players, win or lose. A win adds 1 more when your opponent's class is the same as yours or higher, or when either of you has no class yet. A win over a much lower class earns the same 1 point as any other game. Points never go down and have no cap, and they never change your class.
- **FP belong to a fighter, BP to your account.** FP count the games you played with that fighter; BP count every rated public game. They are drawn like the game's own PP and BP: the number in the game's digits, then FP or BP. They show on room seats, Arcade cabinets, member rows, a member's page and your card.
- **Five tiers.** Ash, Copper, Silver, Gold and Ember. A fighter reaches them at 200, 700, 1,500 and 2,500 FP; your account at 500, 2,000, 5,000 and 10,000 BP. A tier shows as the colour of a frame, and is always named in words as well.
- **After eight games against the same player in one sitting, further games earn no points** and move your rating only a little. A sitting ends after three hours without a game between the two of you.
- **Every game of a set counts.** In a set played on in place (Rematch, first to 2 or 3), only the set's first game was rated before; now each game is.
- **A loss to a clearly lower-rated player costs less while your rating is still settling.** Your rating moves far less for it than before.

### Profiles, player card and awards

- **Every name opens a profile.** A room member, a friend, a Rankings row, either side of a replay and Find a player all open the player's profile, with four tabs: Card, Fighters, Sets and Awards.
- **A fuller card.** It shows main, FP and BP and how far each is from its next tier, rated games, wins and best streak, most played opponent, recent sets, and how you play: device, connection, area and what you are looking for. A new player sees the whole card at 0 and how to start earning points. Narrow windows get a compact card.
- **Edit card.** Profile > Card > Edit card sets what your card says and which awards it shows. The preview is the card as others see it. More options (Triangle or Y) on a player's Card tab opens that player's choices.
- **Fighters.** All 44 fighters, the ones you have played first. Select on one to make it your main. Fighter select moved here from Home; it is also in rooms.
- **Sets.** Your own Sets tab lists your sets from the public archive, and so does any other player's profile.
- **Find a player.** Search by name or friend code, or pick someone you played lately.
- **Awards are milestones now.** The old awards are replaced by nine families: games played, wins, win streak, class reached, fighter mastery, fighters played, friends, a rivalry with one opponent, and opponents faced. Each family has one icon, coloured by its level from Ash to Ember. You choose which award your card wears (its title, its icon or both) and up to four to show on it.
- **Add friend after a set.** The post-match prompt has Add friend on your opponent's line (View on a pad, C on a keyboard). It never moves your focus or answers the rematch for you.

### Friends and private rooms

- **Friends has its own row on Home,** under Online play. It shows how many requests are waiting, or how many friends are online. The same list opens from Online play, from a room's More menu and from Notifications, and Back returns to where you opened it.
- **Add friends** from a player's card in a public room (Add friend) or with their friend code. The list shows requests first, then friends in a room, online and offline, with what each is doing. Join their room, watch their game, or invite them to yours. More has Remove and Block.
- **Requests and invites pop up while you are not fighting.** A request toast has View, Accept and Later. A friend playing another Ember game shows which one ("Playing SFV"); you can only join friends who are in SF4.
- **Settings > Online > Friends:** appear offline, your friend code, or turn friends off.
- **Every friend action answers.** Adding a friend between rematch games shows its confirmation even when the menu closes, and a request that cannot be sent says so.
- **Private rooms you host start with Privacy: Friends.** The friends of anyone in the room see its name and how full it is, and can Join it from their Friends list without an invite, unless the room is locked or full. Your saved templates start as Friends too.
- **To make a private room invite-only, set Privacy to Hidden.** It is on Create room under Visibility and in Room settings, where the host can change it at any time. Friends then see only "In a private room".
- **Hidden stops new joins from the list; it does not shut out a player who already joined or holds an invitation.** Set Admission to Locked for that.

### Public rooms

- **Busy rooms keep their players.** A full lobby no longer drops players with "Room connection failed" after a burst of activity. The score, chat and post-match answers keep up. If a room does have to drop someone, the log says what it was waiting on. Please report any drop in a room of 12 or more with your logs.
- **Your area replaces "region".** Region was the relay you happened to connect through, so everyone in the Americas showed as US East. You now have one of twelve areas (NA West, NA Central, NA East, South America, Europe West and so on), taken from Windows' time zone. Change it or hide it under Settings > Country and area. A hidden area is kept from others, but your own list still sorts by it.
- **Rooms, cards and Quick join use areas.** A room shows where its creator plays from, and the "near you" mark, the filter and Quick join go by area.
- **Relays are named as relays.** Online play shows "Relay: Ember US (Kansas)", "Ember Europe" or "n0 Asia", never a place you are in.
- **Clearer ping.** In a public room the figure beside other players is measured through a relay and says so ("Relay ~80 ms"), without the input delay colours. Your opponent's row shows the real figure from the connection check on the path your match will use, such as "Direct 9 ms".
- **Ranked play says what it needs.** Public rooms tell you at the top, and on Create, when ranked play needs Discord and you are not connected.
- **Arcade and King of the Hill rooms.** A room has a table count (1 to 8 pairs) and a line for whoever finds no table; the line is seated in order as tables free up. Two players who both chose New opponent are no longer seated together again while anyone else can come free, and a full King of the Hill room no longer rematches the loser against the winner.
- **A game that never finishes loading is called off after a minute** for both players and the table frees, instead of leaving everyone on Now Loading.
- **The opponent's answer shows at once.** Over the WIN screen, their Rematch, Character select or Back appears on the post-match prompt as they choose. Over the WIN screen, Character select and Back no longer replace a Rematch you chose.
- **Chat and menus.** A typing indicator is no longer followed by a refused "stopped typing" after you send a line (FRaccie). Ending a Join or Watch question over a card returns you to the same row.

### Replays and the archive

- **Archive tab.** Replays > Archive lists the public archive's sets in the game, with filters for player, fighter, class, round and event, pages, and Watch. A profile lists a player's archived sets the same way.
- **Better cards.** (FRaccie) Set and match cards show the fighters' portraits, Ultras and costumes, the score after each round, and faces on set cards. The Sets tab has Sort & filter with a fighter grid, and Search.
- **Import takes one replay** (an `.emberreplay` from the archive site or a game `.usf4replay`) as well as a set pack. An Import replay row opens a file picker, and files can be dropped on Ember. If files were skipped, the notice says how many and why. Files still waiting when you quit are kept for the next start.
- **A greyed-out replay choice says why** ("Leave the room first", or "Available at the main menu").
- **Seek, step back and loop.** (FRaccie) A replay can jump forward or back, step back a frame and loop, without restarting.
- **A replay keeps its match's classes,** and Watch and the exported video's name plates show them.
- **A fast video export that stalls** ends with the normal failure instead of holding the game.

### Training (FRaccie)

- **Dummy reply slots with a built-in moves list.** Pick any move for the dummy's wake-up reply without recording it. The reply's first move can be a button let go, and a jump typed after another move comes out.
- **Charge moves.** A Charge row in the reply move list sets how the charge is held. Charge moves come out at once after any stun, and "xx" after a dash cancels in the dash's cancel window. A charge reply set to wake-up no longer comes out after an ordinary hit.
- **Data panel.** It shows Hakan's oil, with a switch to keep it full, charge moves as one stacked bar each (including El Fuerte, Balrog and Juri's stock), and the juggle count the foe is under.
- **Frame meter.** Startup, active and recovery now follow the game's own timing, including fireballs and specials that take over from a normal. The classic meter adds a Total reading for counting framekill setups, and leaves the dummy's numbers off.
- **Camera panel (F12).** Orbit, pan, zoom, roll, lens, saved views, glides and effects, with a free camera, mouse and pad control and a keyframe timeline for replays. The camera never carries into an online match.
- **Phone remote.** Training > Phone remote shows a QR code; scan it, or open the address and enter the PIN. Your phone can change the dummy, replies and HUD settings, show the frame meter, frame data and Data panel, save and share combos, and drive the camera. It is off until you turn it on, reachable only on your local network, and Select on its address opens it in this PC's browser. Frame data is Ember's own table of every fighter's moves.
- **The training guide notes that EX moves need a quarter of the Super gauge.**

### Tournaments (FRaccie)

- **The tournament desk runs on Ember's own bracket site.** Make, set up, start and stop an event in the game. Organizers can hold a set and call its players again, or void or decide a game from the site, and an event's own Discord channel gets its news.
- **Players are found by their Discord account.** Challonge and start.gg are no longer used, and the Ember ID screen lists Discord alone.
- **Stream host.** `Launcher.exe --write-stream-host` writes the stream PC's control panel to `%APPDATA%\sf4e\stream-host`.
### Themes and HUD

- **In-game themes.** (FRaccie) Settings > Interface > In-game themes picks a theme folder for each part of the game: main menu, title, character select, stage select, versus, pause menu, network menus, edition marks, buttons, menu sounds and the battle HUD. Ember never changes the game's own files. Away from the main menu a pick is a preview; a changed battle HUD loads in the next battle. See `docs/guides/GAME_THEMES.md`.
- **A broken theme cannot lock you into a crash.** A battle HUD theme that names pictures this edition does not have is drawn without them, and Settings says so. If a theme does close the game while a battle loads, Ember turns it off at the next start, tells you, and marks it in Settings. You can pick it again after fixing its files.
- **HUD editor.** (FRaccie) From the phone remote's Layout tab you can move, rotate, scale and hide HUD elements, replace, export and restore pictures, and save the result as a new theme. Player names, character names and portraits only show after a game restart; the page says so and has a Restart game button. Restart and Apply wait until no match is on.

### Menus, settings, News and Notifications

- **News/Notifications share one place.** Home's row has tabs for Notifications, All, Updates and Change log. Friend requests and room invites are listed under Notifications and can be answered there. Long release notes and titles wrap instead of overlapping.
- **Interface size runs from 0.70x to 1.50x** for monitors where everything was too big.
- **Settings > Online has a Discord account row** to connect, see which account is connected, or disconnect.
- **The mouse pointer hides during a fight** (FRaccie) and comes back in menus.
- **Button prompts follow the device you use.** They change only on a key or pad press, so they should no longer flicker to keyboard keys while you play on a pad. If yours still do, tell us which pad or board.
- **The Ember tier has its own colour,** and every tier is named wherever it was shown by colour alone. "Accept friend" replaces "Accept request". Switches that are off show an outline, and long text on cards is set smaller before it is cut.
- **Friends and profile updates stop while Ember is hidden during your matches.**

### Updates, installer and connection messages

- **The updater runs under an administrator account.** It no longer refuses when Windows gives Ember an administrator token (UAC off, or the built-in Administrator account).
- **A refused join says when your Ember is out of date** and where to update.
- **"UDP blocked" and "Could not reach the host" name Windows Firewall** as the usual cause and say how to allow Ember. Setup's firewall option now allows the match program as well as the phone remote.
- **Crash reports carry more of what is needed to read a crash,** and a game that fails to start is reported as a start failure, not a crash.

## Known issues

- **A first-to-1 set still loads between sets.** Rematch restarts in place only inside a longer set.
- **There is no US West relay yet.**
- **Wake-up replies after Hakan's command grabs and the slide follow-up still do not come out.** If you can, set the environment variable `SF4E_TRAINING_TRACE=1` before starting the game, reproduce it once and send your `sf4e.log`.
- **Translations of the new text in 13 languages need a native read.** Better wording is welcome.
- **A controller plugged in after the game started is not seen until a restart.**

## Thanks

FRaccie for the tournament desk on the bracket site, Training (dummy replies, the camera, the phone remote and frame data), the replay cards and seeking, in-game themes and the HUD editor, and the stream host. Exis, waistman3s and Axythetaxi for the reports of being dropped from busy lobbies; lariat tubman for the charge reversal and Hakan reports; Bogart Willey for the card options, button prompt, HUD editor and News reports; Gagapa for the rematch loading and European lag reports; gonnajump and others for the US East and West report.
