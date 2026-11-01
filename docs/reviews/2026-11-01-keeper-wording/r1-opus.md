Review by Opus (Sol had reached its usage limit), against r1-prompt.md.

The change uses the right predicate (room::KeepsFighter, the one the room and the fighter select enforce); the table page reads it only after the game, when the room has the result.

P2: the post-match prompt opens over the WIN screen before the room has this game's result, and CompleteSet (the only writer of table.keeper) runs only once it is accepted. In an FT2 Arcade or King room where A won set 1 and B wins the deciding game of set 2, A reads "you keep this fighter" and B the ordinary text until the result lands. Fix: a KeeperKnown(t, generation) helper beside MidSet: the result is in (lastSet.generation == generation), or the game cannot have ended the set (neither seat a win from the goal); not plain !MidSet, which would hide the sentence from a real keeper mid-set.

P3: the keeper lands on a locked grid cell; open the selection on "ultra" for a keeper (FighterSelector already handles it), on both rows and the X shortcut.
P3: after a King rotation the detail says "You won" twice, and the table page loses "Ready up"; leave for the next wording pass (no new strings).
P3: the post-match half has no test; extend the King post-match case in shell_room_journey_test (keeper after the result, not before it).

VERDICT: CHANGES REQUIRED
