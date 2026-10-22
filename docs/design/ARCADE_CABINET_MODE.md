# Arcade rooms

A room has a type, chosen on Create and fixed for its life: Classic, Arcade
or King of the hill. Any type can be private or public.

## What a player sees

1. Create: Room type Classic, Arcade or King of the hill. An Arcade room's
   rules are chosen on Create and apply at every table; a set ends at its
   length (one game when it has none).
2. The V2 room board lays the tables out by type (Arcade.dc.html):
   - Classic: today's tables as the floor, the two seats of each facing
     across the score.
   - Arcade: sixteen one-player cabinets in two rows of eight (two lines of
     four each where eight leave a name no room). Each pair has its own
     colour on both cabinets and a line between them wherever they stand.
   - King of the hill: the same under the top slot: the longest current win
     streak, its holder and the streak. Each cabinet shows its streak; the
     top slot's is gold.
   The Classic look shows the same rows plainly.
3. Arcade: sit at any free cabinet. First come, first paired: whoever sits
   down plays the player who has waited longest. Between games a player may
   move to another free cabinet, keeping opponent and streak.
4. After a set nobody leaves their seat. Each chooses Rematch or New
   opponent. Both Rematch: they play again. New opponent from either: each
   plays whoever waited longest (not each other), or waits for the next one
   who sits.
5. King of the hill: every pair plays at once. The winner stays and plays
   whoever waited longest; the loser waits for the next one. The top slot
   stays with its holder until someone beats that streak or beats them.

## How it is built

- Snapshot::arcade is the room::RoomType. Snapshot::cabinets holds who sits
  at each cabinet, Snapshot::streaks each cabinet's streak, topSlot the
  crown. A table is a pair, not a place: Table::deciding and rematch hold the
  choice after a set, waitingSince orders the players waiting alone.
- RoomModelArcade.cxx seats, pairs, parts and crowns. Queue names a cabinet
  as table + seat * TableCount; Unqueue stands up. Rematch and NewOpponent
  are table actions. SetRules is refused.
- Public: the service opens a Classic room; the creator, alone in it as host,
  sends SetRoomType with the type and the rules of all tables
  (netplay::CreatedRules). An emptied public room goes back to Classic.
- RoomPanelArcade.cxx draws the board from the V2 kit; the rows are
  cabinet-N in RoomPanel.cxx.

## Open

- Not run in the real game.
- The public room list does not show a room's type.
- On four lines, a pair two lines apart is linked behind the line between.
