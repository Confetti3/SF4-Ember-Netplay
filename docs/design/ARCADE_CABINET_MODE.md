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
   - Arcade: Real Arcade rows, sixteen tall one-player cabinets in two rows
     of eight, each over its stool with the player's back on it: marquee,
     screen (fighter and score, Waiting, the Rematch choice or Insert coin),
     name plate with the tag, control panel. Each pair has its own colour on
     marquee and plate, and a line along the aisle between the rows. Where
     they would be too small to read they keep their size and scroll.
   - King of the hill: the same under the top slot banner: the longest
     current win streak, its holder and the streak. Each marquee shows its
     streak by a crown; the top slot's marquee is gold.
   The Classic look shows the same rows plainly.
3. Arcade: sit at any free cabinet. First come, first paired: whoever sits
   down plays the player who has waited longest. Between games a player may
   move to another free cabinet, keeping opponent and streak.
4. After a set, as at a real arcade, the winner stays at the table and plays
   whoever has waited alone longest; the loser waits for the next one who
   sits down, not the winner. With three players the winner plays the third
   and the loser waits. Only when nobody waits alone does the pair stay:
   each chooses Rematch or New opponent. Both Rematch: they play again. New
   opponent from either: each plays whoever waited longest (not each other),
   or waits for the next one who sits. One who sits down while a pair
   chooses waits alone until a set ends or the pair parts.
5. King of the hill rotates the same way, with streaks: every pair plays at
   once. When nobody waits alone the winner still waits for the next one free
   while anyone else sits at a cabinet; with nobody else there the loser
   challenges again. The top slot stays with its holder until someone beats
   that streak or beats them.

## How it is built

- Snapshot::arcade is the room::RoomType. Snapshot::cabinets holds who sits
  at each cabinet, Snapshot::streaks each cabinet's streak, topSlot the
  crown. A table is a pair, not a place: Table::deciding and rematch hold the
  choice after a set nobody waited alone for, waitingSince orders the players
  waiting alone. RoomAuthority::ArcadeSetEnded rotates both types through one
  path; only the streaks and what happens with nobody waiting differ.
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
