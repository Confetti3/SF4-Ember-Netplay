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
   with others at the cabinets each chooses Rematch or New opponent (the
   board's Ready is the Rematch); alone in the room they just ready again.
   Both Rematch: they play again. New opponent from either: each plays
   whoever waited longest (not each other), or waits for the next one who
   sits. One who sits down while a pair
   chooses waits alone until a set ends or the pair parts.
5. King of the hill rotates the same way, with streaks: every pair plays at
   once. When nobody waits alone the winner still waits for the next one free
   while anyone else sits at a cabinet; with nobody else there the loser
   challenges again. In a full room, every table a pair, the pair stays and
   the loser asks for a new opponent at once: the next set to end elsewhere
   trades the losers, or both choose Rematch. The top slot stays with its
   holder until someone beats that streak or beats them.
6. After a set the WIN screen's prompt asks the room's question too. While
   the pair decides it offers Rematch and New opponent (Y), the same choices
   as the cabinet, and a Rematch chosen there readies the player once both
   chose it, so the next set starts without another press; a pair still
   seated together (nobody else to play, or the result not in yet) gets the
   ordinary Rematch, which readies again; once one of them has left the
   table it says who is next, the loser too, whom the room has already
   moved to another table.
   Nobody leaves the cabinet from the prompt: Back to room and no answer keep
   it.
7. The cabinets carry what a Classic table card's banner says about a seat
   (a start held for a spectator, the Ready timeout, a Training call) on the
   cabinet's screen and in its row's words. The pad moves between cabinets,
   matchups, members and the room's actions by where they are drawn.
8. Winner keeps fighter, a room option set when it is created (off by
   default): as at a real arcade, a set's winner stays with the fighter they
   won with until they lose a set at that table or leave it. Fighter select
   locks the grid and the edition for them; the Ultra Combo, costume and
   color still change.
9. Every member's sets won and lost in the room since joining show on their
   member card ("3 W / 1 L"), whatever the set length.
10. Fighter select opened from the room (its Fighter shortcut, or the
   prompt's Character select) closes on a fighter picked and lands on Ready.

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
  cabinet-N in RoomPanel.cxx. Every cabinet and matchup goes through
  PressEntry, so MenuNavigation moves between them by their boxes.
- Rules::winnerKeepsFighter carries option 8 (written only when on);
  room::KeepsFighter is the one test, and the overlay passes it to
  FighterSelector. Member::setsWon and setsLost count in CompleteSet.
- The room host routes a member's fighter, stage and seed to the table they
  sit at now (SessionServer::RoomTableFor reads the seat first): rotation
  moves players by other members' actions, and the table they last acted at
  went stale, so a moved player's pick was dropped and they played as Ryu.
  This needs a room host build on the servers to take effect.
- The post-match prompt (ApplicationShell__PostMatch.cxx) reads
  Table::deciding: its Rematch and New opponent are RoomAction's, the cabinet's
  own answer; KeepsSeat holds every cabinet in an Arcade or King room.

## Following the room

A spectator watches a table, and in an Arcade room a table is a pair, so a
pair that parts left its spectators at an idle table while others played.
Follow the room moves a spectator by itself to the room's next game, in every
room type but a tournament's.

- Offered beside Watch in a table's options, and on the board's action bar
  while watching. It lasts while the member stays in the room with no seat
  or queue place: sitting, queueing, stopping watching or leaving ends it,
  as does Stop following, one press. It is not saved. Started while watching
  nothing, it first watches the table whose options it was chosen from.
- While following, the board's action bar says Stop following with the table
  followed, and the watched picture's chip says Following the room. A
  follower never locks in, so it never holds a start; a member locked in
  releases the lock-in first, and a lock-in that lands after following
  started (pressed on an older snapshot) ends following, with Release
  lock-in back in the table's options.
- It goes on while Ember is hidden (F10), as streamers hide it to show the
  games: ApplicationShell::Background runs it too, with the room and the
  watched game, and the overlay sends its room actions from there.
- The member's own watching comes first. Every Watch and Unwatch sent, the
  member's or following's, is one watching intent (NoteWatching), and the
  frame's presses are taken before following decides. Stop watching ends
  following at once and holds until the room shows the member watching
  nothing: a move still on its way may land after the member's Unwatch, so
  an Unwatch goes for whatever table the room shows it at, again after 5 s
  while one is refused or did not go, however often Stop watching was
  pressed. A Watch the member chooses afterwards, a place or leaving the
  room ends it.
- When the watched table has no game Playing or Ready, and some other table
  is Playing, it moves to the lowest-numbered such table that takes another
  spectator and whose last game is not still closing. Every client chooses
  the same one. It waits there for that table's next game (watchingNext).
- It stays at a table that just stopped: for 3 s after a game it watched
  while the same pair stays seated (an Arcade winner's next set starts at
  the same table), and for up to 20 s while both seats are filled at a table
  it came to for its next game. Without the second, a follower arriving
  mid-game would move on as that game ended, and could go from table to
  table without being shown a game.
- Client side only: no room host or protocol change, so public room hosts of
  every release take it. A move is one ordinary Watch, which already ends
  watching elsewhere (RoomAuthority::StopWatching), so no Unwatch goes first
  and a refused move leaves the follower watching where it was. Only one
  move is in flight; the next is decided once the room shows it, or after
  5 s, again from the snapshot, which shows what the room refuses for (a full
  table, a game over, a game still closing). Nothing moves while this PC
  still shows a watched game or the room cannot take an action.
- room::RoomFollow (session/RoomFollow.hxx) decides from each snapshot;
  ApplicationShell::UpdateFollow sends its step and FollowRow draws the
  row. The RoomFollow test (room_follow_test.cxx) covers each rule, and the
  follow journeys (shell_follow_journey_test.cxx) the drawn and hidden shell,
  a late lock-in and a Stop watching that races a move.

## Open

- Not run in the real game.
- The public room list does not show a room's type.
