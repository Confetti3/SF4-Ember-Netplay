# SF4 Ember Netplay v1.0.2

v1.0.2 fixes rooms that stopped responding for good after a player dropped, joins that got stuck after someone left, and a crash during rollback.

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Changes since v1.0.1

- **A room no longer freezes when a player drops.** When a room was down to two players and one of them crashed or lost their connection, the other could no longer change anything in the room, and nobody could join or rejoin it. The only way out was to close the room and open a new one. Long-running rooms hit this the first time someone dropped while only two people were left. The room now keeps working, removes the player who dropped after a short wait, and lets players join again. If the room's host leaves normally, the other player takes over the room as before.
- **Joining right after someone left no longer hangs.** A player who joined or rejoined a room shortly after another player left or dropped could get stuck partway into the room and never finish joining. They now join normally.
- **Fewer crashes during matches.** A rollback could write past the end of one of the game's effect objects and corrupt the heap, which crashed the game a little later. This is the same fix that was planned for 1.1.0.
- **"Until the room closes" links work for everyone.** A link set to last until the room closes was refused as broken by players whose PC clock ran even a second behind the host's. Clocks up to a day apart are now accepted.

One case is unchanged: if the host's game crashes while only two players are in the room, the other player still has to open a new room.

Everyone in a room needs v1.0.2. v1.0.1 and v1.0.2 cannot join each other's rooms, as with any other package change.

## Testing

The full build passed all 88 automated tests. The room recovery tests passed every scenario directly and through relays, including three new ones: a room that shrinks to two and then loses a player, the same with the player dropping during the shrink, and the host of a room of two leaving. The room, spectator and late-spectator tests passed. These fixes have not yet had a two-PC test.
