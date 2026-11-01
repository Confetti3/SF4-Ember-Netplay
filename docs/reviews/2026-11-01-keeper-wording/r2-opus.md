Review by Opus (Sol had reached its usage limit), against r2-prompt.md.

Round 1: the Ultra start and the post-match test are fixed; the wording pass is deferred as agreed; the timing P2 is only partly fixed.

P1: KeeperKnown treats "the room has this game's result" as lastSet.generation == generation, which is true only for a game that ends the set. After the result of a game inside the set, EndMatch adds the point to t.score without CompleteSet, so the score test runs on the new score: in an FT2 King room where A kept the fighter from set 1, game 1 of set 2 lands at 1-0 and the row flips back to "Choose another fighter" while A still keeps it. Fix: ResultIn(t, generation) = the game is no longer live at this generation (the test Readied already uses); KeeperKnown = ResultIn || (format not Unlimited && neither seat a win from the goal); Readied uses ResultIn too. Add the test.

P3: RoomSelectionStart was inserted between RoomTypingMs and its comment.
P3: a test message names a rule the fixture never touches; it is about the keeper.
P3: no test checks the selection opens on the Ultra for a keeper.

Other: KeepsFighter is the right test on both rows (post-match once KeeperKnown is right); the rows should still open the page; the X hint can stay; file sizes fine.

VERDICT: CHANGES REQUIRED
