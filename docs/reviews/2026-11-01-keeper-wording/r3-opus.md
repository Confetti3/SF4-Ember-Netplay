Review by Opus (Sol had reached its usage limit), against r3-prompt.md.

Round 2: the P1 (KeeperKnown after a game inside the set) is fixed by ResultIn, shared with Readied and covered by a test; the helper placement and the test message are fixed; the Ultra focus test is deferred as agreed.

Checked: KeepsFighter is the right test on both rows (gated by KeeperKnown on the post-match row); the rows still open the page; the X shortcut and hint need no change, all three openers share RoomSelectionStart; file sizes fine.

P3: the lastSet.generation condition in KeeperKnown never decides anything in real play (EndMatch sets lastSet and leaves Playing together); it exists only for a fixture that keeps Playing with the set ended. Drop it and set the fixture's phase to Waiting.
P3: the branch that shows the sentence before the result of a game that cannot end the set is untested.
P3: a rematch played on in the same battle stays Playing at the same generation, so during its Starting seconds the score test runs on the score after the game; small, the choice is made; fix or say so in the comment.

VERDICT: APPROVE

Addressed after approval: KeeperKnown is ResultIn or the score test, the fixture is Waiting for the set's result, the before-result true case is tested, and the comment notes the short rematch lag.
