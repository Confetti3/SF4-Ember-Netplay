# Random fighter

Players asked for a random character select. Ember's fighter grid has a
Random tile, as USFIV's own select has its "?" box. Choosing it makes the
player's pick Random: every game they play gets a freshly rolled fighter,
rematches included.

## The tile

- The tile is the grid's last cell, after the 44 fighters, where USFIV puts
  its own Random. The pad and the keyboard reach it as any other card.
- It is on every grid that edits the player's own pick: a room's Fighter
  select and the Training select page's Fighter. Training's dummy keeps its
  own pick and has no tile.
- Select on it saves Random. A Random pick already saved opens the rolled
  fighter's page, so the costume, Ultra, stage and options stay in reach.
  Select on a fighter saves that fighter and ends Random.
- A winner who keeps their fighter (Arcade's winner-keeps rule) cannot choose
  it, as they cannot choose another fighter.
- Random is saved as its own flag beside the pick
  (`OverlayPrefs::Data::lobbyRandom`, "randomFighter" in the settings file),
  never as a fighter id. The pick keeps the rolled fighter, so everything that
  reads the pick (Ready, Training, the summaries) reads a real fighter.

## The roll

`common/RandomFighter.hxx` holds the roll; it takes its random source as an
argument, and its tests pass their own.

- It is uniform over the fighters the player may play now: those their Steam
  licences cover (`FighterLocked`), each with a valid pick under the edition
  rule the pick plays by (`Normalize`, `Valid`). Values that would favour the
  first fighters are drawn again.
- The rolled fighter plays in the look last used on them (the per-fighter
  memory), fitted to the rule and to what is unlocked, as choosing that
  fighter on the grid would.
- A winner who keeps their fighter keeps the fighter they rolled and won with:
  nothing rolls while they keep it.
- A tournament table has no fixed fighter of its own in this build; the
  winner-keeps rule is the only fixed fighter, and the host's
  `RoomAuthority::FighterAllowed` is the one check.

## When it rolls

`session/RandomFighterWatch.hxx` decides. The fighter in hand is always one
not yet played:

- Random rolls when it is chosen, and when it is loaded with the saved picks.
- It rolls again as soon as each game the player fights at a room table is
  over: its WIN is on screen, or the room has its result, whichever comes
  first. A seat taken later finds that fresh roll; sitting down rolls
  nothing more.
- In a room whose winners keep their fighters, a game that may end the set
  waits for the room's result, which says whether the player keeps theirs.
- Training from Home plays the fighter in hand; it rolls again after.
  Training from a room's table does not: the player practises the fighter
  they will play.

A seat's fighter reaches the room host only through the pre-battle message
(`SessionServer::HandleSetChara`), refused while that seat is Ready or the
table's game is still on, and a changed fighter withdraws the opponent's
Ready (`SetMemberChara`). Ember sends it with Ready, and for a Random pick
also on its own:

- The runtime sends the roll the room does not show yet while the player sits
  between games and has not readied (`RandomRollToShow`), at most once a
  second until the room shows it. The host takes it as soon as the room has
  the game's result, while the opponent is still tearing their game down,
  so it arrives before their next Ready and never takes one back.
- One race is left: an opponent whose Ready reaches the host first (for
  example, a player sitting down at a table where the opponent already
  readied with an older roll on show). The change then withdraws their
  Ready, as any changed fighter does, and the room says so.

## Rematches

A rematch both fighters choose over the WIN usually goes on in the same
battle with the same fighters (`Battle::System::GetGameMode`). That would
replay the old roll, so a Random player's Rematch does not take part in it:
the battle ends after its WIN as it would without a rematch, and the Rematch
is the room's Ready, with the new roll. Both games read the same inputs, so
neither PC plays in place. The opponent waits through the WIN and a VS load.

## What others see

The player's own summaries (Home, the room's Fighter row, Training select)
and their own seat on the board say "Random · Ryu"; the fighter's page is
titled the same. Others see the rolled fighter on the board, as for any pick.

Later: hiding the roll from others until the VS screen needs room host
support (a hidden pick in `RoomAuthority`'s member state, shown at the
match's start), so the board shows the rolled fighter for now.

## Out of scope

Offline and the game's own modes: the game's own select has its own Random.
