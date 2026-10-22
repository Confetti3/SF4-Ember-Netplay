# Arcade rooms

A private room can be created as an Arcade room: sixteen one-player
cabinets in eight pairs, as single player cabinets stand in an arcade.

## What a player sees

1. Create: Room type Classic or Arcade. Arcade adds the Arcade room view row
   (Floor, Real Arcade rows, Real Arcade columns). The rules chosen on Create
   are the room's for its life and apply at every table.
2. The room board draws the cabinets in the room's view, the same for
   everyone:
   - Floor: two columns of facing cabinets, a row per table.
   - Real Arcade rows: two rows of eight tall cabinets over a drawn arcade;
     cabinet n on top faces n+8 below it.
   - Real Arcade columns: four columns of stacked pairs.
   Each table has its own colour; a pair with anyone at it lights up in it
   and a band between the two cabinets shows the score, or VS while one
   waits. An empty cabinet says Insert coin.
3. A member sits at the cabinet they choose (press an empty one, or the seat
   chooser). Sit down without a cabinet puts them opposite someone waiting,
   else at an empty table. To play someone else they move to the free
   cabinet opposite them, between games only. Nobody is moved for them.
4. After a set the pair plays on until one stands up.

## How it is built

- Every room has eight tables (room protocol 4). A Classic room plays at
  the first four (TablesInPlay).
- Snapshot::arcade is a room::ArcadeView: off, or the view. It is chosen on
  Create and fixed for the room's life. ArcadeKindValid keeps it private:
  never public, never a tournament's, no table queue.
- RoomModelArcade.cxx picks the table for a Sit down with no cabinet.
  ApplyQueue lets an Arcade member between games move by naming another
  seat. SetRules is refused in an Arcade room.
- RoomPanel.cxx draws the floor and Real Arcade; RoomCards.cxx draws the
  cabinets and the backdrop.

## Open

- Not run in the real game since the eight table rework.
- King of the hill: a separate mode, its own PR on top of this one.
- Before a PR: fold the history into clean commits, run the reviewer, move
  the Arcade drawing out of RoomPanel.cxx (977 lines), have the machine
  translations checked.
- Public Arcade rooms.
