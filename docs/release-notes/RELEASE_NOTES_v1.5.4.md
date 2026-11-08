# SF4 Ember Netplay v1.5.4

v1.5.4 fixes full Arcade rooms. With every cabinet taken, the same two players could only play each other again and again; now New opponent always works, and you get someone new as soon as anyone is free. Rematch and New opponent are on the room's bar after a set, a won set says who it was won against, and the post-match prompt fits its content again.

Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **From 1.5.3, update in Ember.** Ember offers the update at launch. Your settings, recordings and replays are kept.
- **The zip and setup.exe are on the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.5.4)** with their `.sha256` files.
- **Everyone in a room needs 1.5.4,** spectators included.

## Changes

### Arcade rooms

- **New opponent works in a full room.** In 1.5.3, with all sixteen cabinets taken, New opponent only said nobody else was free, so the same two players kept playing each other. Now New opponent is always there after a set:
  - If someone is waiting alone or a table is free, you part at once, as before.
  - If someone at another table has also asked for a new opponent, the two of you trade places, so both pairs get someone new.
  - Otherwise your request waits and you keep your seat. The next set to end anywhere sends its loser to your old opponent, and you play its winner. If a player leaves the room first, you play the one they left behind.
  - Nobody is pulled away from a table that did not ask, and a Rematch chosen at another table is never undone.
  - Choosing Rematch instead takes your request back.
- **Wait in Training.** While a request waits, both you and your old opponent can go to Training from the room. You're called back when someone new sits opposite you.
- **Rematch and New opponent are on the room's bar.** After a set, Ready reads Rematch and New opponent sits beside it, so you no longer have to find your own cabinet to choose. Both are greyed out while the room is busy with another action.
- **A won set names the opponent.** The room chat says "Name won the set against Name (3 - 1)" in Arcade and King of the Hill rooms, instead of naming only the table.

### Post-match prompt

- **No more scroll bar hiding the first player.** 1.5.3 added the Ultra row to the prompt without making the panel taller, so a scroll bar hid player 1's row at 720p and 1080p. The panel now sizes itself to what it shows, from its first frame.
- **New opponent can be taken back.** After choosing New opponent, Rematch and your Ultra stay open on the prompt, as they do on the cabinet.
- **After a swap, the prompt says where each player went** ("vs Name", waiting, or left), instead of the winner-stays text, which only fits when a set's result moved the players.

## Thanks

Gagapa for the full-room report, the Rematch and New opponent buttons and the set-result request; Exis and Bogart Willey for the prompt's scroll bar (#185); FRaccie for the first full-room fix (#182); FinFreezer and Exis for the Arcade feedback.
