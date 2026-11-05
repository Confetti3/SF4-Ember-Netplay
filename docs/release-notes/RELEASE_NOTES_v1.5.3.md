# SF4 Ember Netplay v1.5.3

v1.5.3 is built from what players asked for on Discord and GitHub. Changing your Ultra takes one press, from the room bar or the post-match prompt. Costumes, colors and Ultras are pictures again, each Ultra with its input. Public rooms can be filtered by region, type, open seats and connection, and a room can be set to wired players only. Each player's own delay is now the default. Menus take fewer presses with a controller. The Training controls button can be moved off R3. Presses no longer go missing after Ember's menus or the post-match prompt while a direction is held, and spectators of an instant rematch now see the set score move.

Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **From 1.5.2, update in Ember.** Ember offers the update at launch. Your settings, recordings and replays are kept.
- **The zip and setup.exe are on the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.5.3)** with their `.sha256` files.
- **Everyone in a room needs 1.5.3,** spectators included.

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
- **Arcade cabinets show each player's Ultra** in the corner of the cabinet's screen, behind its scanlines, once the player has shared it.

### Rooms

- **Each player's own delay is the default.** A new room plays each fighter at their own delay unless you choose the same delay for both in Create room or Room settings. After updating, the setting starts on each player's own for everyone, because 1.5.2 did not save a choice of the same for both: if you want it, choose Same for both once more and Ember keeps it from then on.
- **Classic tables read like they did before 1.5.0.** On a wide enough board, 1280 wide at normal scale included, each table is one line: a fighter at each end, the players' names between them and the set's score small in the middle, where the big number was. The rules, queue and watchers are back along the bottom of the card, under the players, and the table's state and its name (if the host gave it one) sit beside TABLE 1. A player on a win streak at the table has it under their fighter again, and a seated player who has been waiting a while shows how long. Ready up and Leave seat on your own card, and the seat chooser, sit along the bottom of the card. Narrow boards and larger text scales stack the two players instead. Arcade and King of the Hill rooms are unchanged.


### Public rooms

- **Filters.** Public rooms has one Filters row that says what is on. Select opens the Filters page: Region (all regions, Near me, or one region), Room type (all, Classic, Arcade or King of the Hill), Open seats only, and Connection (all rooms or wired only rooms). Your filter is saved with your settings. When it hides every room, the list says so and Clear filters shows them all.
- **Quick join follows the filter.** It takes the best room the filter lists, near you unless you chose a region.
- **Wired players only.** A new room setting, off by default, when you create a room and in Room settings. While it is on, players on Wi-Fi can join, watch and chat but cannot take a seat, queue or sit at an Arcade cabinet, and the closed seat says why. A connection Ember cannot tell, as on Linux and Steam Deck, can always play. Turning it on moves nobody who is already seated, and a Wi-Fi player still in the queue is passed over when their turn comes. After plugging in a cable, leave and join the room again. Public rooms with the setting show a Wired only mark.

### Controller menus

- **A room opens where you need it.** Seated, the room opens on Ready up (Rematch after a set); watching, on the room's buttons; otherwise on the first table or cabinet with a free seat.
- **LB and RB jump between the room's areas:** the tables, the room's buttons, Members and Chat, each time back on the row you last used there. The chat box is two presses away, and the hint bar shows LB RB Areas. On a narrow screen they step its tabs, which remember their row too.
- **Lists go round.** A fresh Up on the first row goes to the last, and Down on the last to the first; holding a direction still stops at the end. Grids, the room board, the post-match prompt and Settings stop at their ends as before.
- **Fewer presses everywhere else:** Create room's button, Home's notifications, the fighter page's Ready and Table options' Leave seat are a press or two away instead of a long walk. Rows hidden by a scroll are no longer d-pad targets from outside it, so a move never lands on something you can't see.

### Training

- **The Training controls button can move.** Controllers > your pad > Button layout has a Training controls row. It starts on Automatic: R3 opens the controls unless R3 is one of your attacks, and then it is off and your attack is left alone. Pick another button or turn it off, and your choice wins. F6 and Back with Start always open the controls. A button chosen here opens the controls in Training and never attacks there, and keeps its attack everywhere else.

## Fixes

- **Presses no longer go missing after Ember lets go of the pads.** After Ember's menu, the Training controls or the post-match prompt closed, the game saw no input at all until every button was released, so holding back or a charge into the next game swallowed every other press. Now only the buttons still held from the menu wait for their release; anything pressed after it reaches the game at once. The netplay log also counts the frames where Ember held input back, and why, to help with any report of dropped inputs.
- **Spectators see the set score move during an instant rematch.** When both players chose Rematch, the next game went on in the same battle, and a spectator's score stayed where it was when they started watching (often 0 - 0). The score now counts each game as the spectator sees it end.
- **The tournament match page no longer tells current players to update.** It cannot see which Ember a PC has, so Open in Ember is always the main button, and the page says both players need the same version. When Ember finds the versions differ, its notice now has a Check for updates button.
- **New opponent in a full Arcade room.** With every other table in use and nobody waiting, New opponent could only pair the same two players again. The row now says that there is nobody else to play yet.
