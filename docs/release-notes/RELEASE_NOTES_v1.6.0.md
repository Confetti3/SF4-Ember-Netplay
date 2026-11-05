# SF4 Ember Netplay v1.6.0

v1.6.0 is built from what players asked for on Discord and GitHub. Changing your Ultra takes one press, from the room bar or the post-match prompt. Costumes, colors and Ultras are pictures again, each Ultra with its input. Public rooms can be filtered by region, type, open seats and connection, and a room can be set to wired players only. The Training controls button can be moved off R3. Spectators of an instant rematch now see the set score move.

Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **From 1.5.2, update in Ember.** Ember offers the update at launch. Your settings, recordings and replays are kept.
- **The zip and setup.exe are on the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.6.0)** with their `.sha256` files.
- **Everyone in a room needs 1.6.0,** spectators included.

## Changes

### Changing your Ultra

- **One press from the room.** While you are seated, an Ultra button beside Ready up opens your fighter's Ultra cards, and choosing one returns to the room. You no longer need to open Table options for it. The button says only Ultra I, Ultra II or Ultra Double, so the room bar stays on one line.
- **After a game.** The post-match prompt shows your Ultra under your answer, and Left and Right change it before you choose Rematch or Next game. An instant rematch keeps the Ultras the game loaded, so with a new Ultra the next game loads first.
- **Real Ultra names.** Ready reads, for example, "Ready with Ryu, Metsu Hadoken, against Ken." The fighter page names each Ultra too, not just U1 and U2.
- **Your opponent's first pick counts.** Until a player picks, their seat shows their profile's main. A first pick of another fighter now takes back a Ready given against the main, as a later change already did, so the game no longer loads with an Ultra you chose for the wrong matchup. The table card says what they picked, and the focus moves to your Ultra.
- **Table options are back on the room bar** (Y still opens them). Copy room link moved into More.

### Costume, color and Ultra pictures

- **Costumes and colors are pictures again, all at once.** On a fighter's page, Select on Costume shows a card for every costume you own, each in the color you wear now. Select on a costume shows all of its colors, and Select on a color saves both. The pad moves across the cards in every direction, and Back steps out without changing anything. Left and Right on the Costume and Color rows still change them in place.
- **From a room, Appearance opens the costume grid straight away,** and picking a color returns to the room.
- **The Ultra page is back, full size.** Select on the Ultra row of a fighter's page shows a large card for each Ultra, as the old Ultra page did: its picture with the game's own I, II or W in the corner and SAVED on the one you have, its name, and its input drawn as motion arrows and buttons, with the same input in words under them that read the same on either side, such as QCF x2 + PPP or Charge B, F, B, F + KKK. A move with two ways in, like Gen's stances or Decapre's anti-air version, shows both, and Ultra Double shows a large W and both Ultras' inputs. The input follows the edition, so SSFIV Bison and Omega Decapre show their own. Select saves the Ultra and Back leaves it as it was. Left and Right on the Ultra row still change it in place.
- **From a room, the Ultra opens these cards too:** the Ultra button beside Ready up and the Ultra row in Table options. Picking one returns to the room.
- **Arcade cabinets show each player's Ultra** in the corner of the cabinet's screen, once the player has shared it.

### Public rooms

- **Filters.** Public rooms has one Filters row that says what is on. Select opens the Filters page: Region (all regions, Near me, or one region), Room type (all, Classic, Arcade or King of the Hill), Open seats only, and Connection (all rooms or wired only rooms). Your filter is saved with your settings. When it hides every room, the list says so and Clear filters shows them all.
- **Quick join follows the filter.** It takes the best room the filter lists, near you unless you chose a region.
- **Wired players only.** A new room setting, off by default, when you create a room and in Room settings. While it is on, players on Wi-Fi can join, watch and chat but cannot take a seat, queue or sit at an Arcade cabinet, and the closed seat says why. A connection Ember cannot tell, as on Linux and Steam Deck, can always play. Turning it on moves nobody who is already seated, and a Wi-Fi player still in the queue is passed over when their turn comes. After plugging in a cable, leave and join the room again. Public rooms with the setting show a Wired only mark.

### Training

- **The Training controls button can move.** Controllers > your pad > Button layout has a Training controls row. It starts on Automatic: R3 opens the controls unless R3 is one of your attacks, and then it is off and your attack is left alone. Pick another button or turn it off, and your choice wins. F6 and Back with Start always open the controls. A button chosen here opens the controls in Training and never attacks there, and keeps its attack everywhere else.

## Fixes

- **Spectators see the set score move during an instant rematch.** When both players chose Rematch, the next game went on in the same battle, and a spectator's score stayed where it was when they started watching (often 0 - 0). The score now counts each game as the spectator sees it end.
- **The tournament match page no longer tells current players to update.** It cannot see which Ember a PC has, so Open in Ember is always the main button, and the page says both players need the same version. When Ember finds the versions differ, its notice now has a Check for updates button.
- **New opponent in a full Arcade room.** With every other table in use and nobody waiting, New opponent could only pair the same two players again. The row now says that there is nobody else to play yet.
