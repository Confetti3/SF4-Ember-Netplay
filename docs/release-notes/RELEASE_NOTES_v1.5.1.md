# SF4 Ember Netplay v1.5.1

v1.5.1 fixes what players reported on 1.5.0. Every game of an instant rematch now keeps its replay, Arcade rematches no longer drop both players back to the room, and the Training menu moves the way it is drawn. It also brings the work tested on Nightly since 1.5.0: fighters' faces on the room board, a Random fighter tile, following a room as a spectator, one Sets tab in Replays, and Training's Data panel and Combo check.

Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **From 1.5.0, update in Ember.** Ember offers the update at launch. Settings, recordings and replays stay.
- **The zip and setup.exe are on the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.5.1)** with their `.sha256` files.
- **Everyone in a room needs 1.5.1.** This includes spectators. 1.5.0 and 1.5.1 cannot share a room and say so when they meet.
- **Nightly players on 1.5.1-nightly** can stay on Nightly or switch Settings > Updates > Update channel to Stable.

## Fixes

- **Every game of an instant rematch keeps its replay.** The game saves one replay per battle, so on 1.5.0 the games after the first in an instant rematch were never saved, in Ember or in the game's Battle Log: an FT2 set kept one replay. Ember now saves each of those games itself, with both names and its set. Replays already lost on 1.5.0 cannot be recovered.
- **Arcade Rematch.** When both players chose Rematch near the end of the countdown, the game's WIN screen could end first and send both back to the room. The WIN screen now waits while either player is still choosing. In an Arcade room, choosing Rematch when you two were asked Rematch or New opponent now readies you.
- **Back or Character select takes back an Arcade Rematch.** Choosing Rematch and then Back to room no longer readies you when the other player picks Rematch.
- **The Rematch prompt shows the set as it stands,** counting the game just played, and says Rematch rather than Next game after a set's last game.
- **The set's loser gets the Rematch prompt too** in Arcade and King of the hill rooms, where they are moved to another cabinet before the WIN screen ends.
- **A Rematch you pressed is taken back if the opponent changes fighter,** with a notice naming their new fighter, so a game never starts against a fighter you did not see.
- **Leaving a public room returns to the public room list,** not Home.
- **Training menu navigation follows the menu.** Down moves one slot at a time on the recording page, Left and Right cross between the two columns on Frame data, HUD and Dummy pages, and Counter hit, Dummy reply and Inputs shown offer all their choices instead of an On/Off switch.
- **News:** the release title scrolls with the notes instead of staying over them.
- **Rankings:** the Master and Grand Master crowns are as big as the class bubbles and centred.
- **Room board:** the grips that resize the Members and Chat panels are no longer covered. Settings > Interface > Room layout's preview follows Tables: One per line.
- **Watch now under Replays > Sets plays the set.**
- **Nightly updates:** a Nightly compares with another Nightly by its date, so the Nightly line's move from 1.6.0 back to 1.5.1 no longer stops updates.
- **Export video:** the match line starts with the date alone, so it no longer gives away the set score.

## Changes since v1.5.0

### Rooms

- **Fighters' faces on the room board.** Each seat shows the fighter's face, and Pictures shows each player's costume, color and Ultra, or the game's portraits. Members show each player's mains. Tables are one per line by default; Settings > Interface > Room layout > Tables: Grid brings the grid back.
- **Random fighter.** Fighter select has a Random tile that rolls a fighter for every game.
- **Follow the room.** A spectator can follow the room to its next game, also while Ember is hidden. Stop watching ends it.
- **Your own seat shows your costume and Ultra before Ready.**
- **Arcade cabinets glow in their pair's colour.**
- **Public rooms show their type,** Classic, Arcade or King of the hill, on each card.
- **Notifications stay off the game** and stay up only briefly.

### Training

- **Data panel.** (FRaccie) Training shows life, stun, the interruptible frame and frame advantage.
- **Combo check.** (FRaccie) Plays Player 1's recorded combo against other fighters as the dummy, from Training with its own page.
- **The frame meter matches the game's frame data on all 44 fighters** and draws the first active frame red. (FRaccie)
- **Hitboxes stay off the game's pause menu.** (FRaccie)

### Fighter select

- **The grid alone, and each fighter's choices on their own page.** (FRaccie) Speech reads both.

### Replays

- **One Sets tab** for tournament and room sets. (FRaccie) A set's videos follow one another when exported.
- **Keep, Show and Search.** (FRaccie) Keep marks a replay to find again; Show and Search narrow the list.
- **A battle the opponent leaves keeps its replay.** (FRaccie)
- **Online games are shared to the replay archive only while signed in with Discord.**
- **Export video:** each player's class on their name plate, Ember's mark in the corner, and the game's button guide left out of the video.

### Other

- **A frozen game's threads are saved in the crash dump before Windows ends it.** (FRaccie)
- **Settings' open panel reaches the right edge** like the tabs. (FRaccie)

## Known issues

- Cody's knife: a player saw online Practice sessions end in a desync after picking up or throwing the knife. We could not reproduce the cause from the code yet. Please send both players' `sf4e.log` from `%APPDATA%\sf4e\logs` if it happens to you.
- The game's own Battle Log still holds only the first game of an instant rematch; Ember's Replays has them all.
