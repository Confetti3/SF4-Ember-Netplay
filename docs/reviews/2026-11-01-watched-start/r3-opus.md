Review by Opus (Sol had reached its usage limit), against r3-prompt.md.

Earlier rounds: both r1 P1s, the two-detector P2, the inline rule, r2's stale-sample P1, the report panel's text field, the untested arm and the r2 P3s are fixed. Checked: native pause never sees the closing Start while held (MenuInputCapture blocks until all buttons are up); Publish drops Holds() only after the request is answered.

P2: the "should Start ask" rule is still decided in two places on two threads: StartPress returns !menuTakesPads || ControllerClosesMenu(context) on the game thread, and AnswerStart decides again on the drawing thread. They differ only when Holds() lags (Start pressed at the main menu in the frame the menu closed is swallowed). Fix: StartPress a pure edge detector (allowed, physical), AnswerStart the single decision.

P3: the overlay maps every request kind, None included, to an answer and then re-checks the kind; branch on the kind first.
P3: StartPress takes the Start bit as a parameter only for the test; use NativeStart.
P3: AnswerStart's comment says "on-screen keyboard's Done" but TextEntryOpen is any editor; say "while a text field is being edited".

VERDICT: CHANGES REQUIRED
