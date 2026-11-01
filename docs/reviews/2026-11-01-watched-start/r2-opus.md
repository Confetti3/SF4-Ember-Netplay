Review by Opus (Sol had reached its usage limit), against r2-prompt.md.

Round 1: P1s fixed in the game-thread arm; the editor check, inline rule fixed; the two-detector design not fixed.

P1: a Start that closes the menu over a fight reopens it on the next frame when the drawing thread's pad sample lags the request (the render thread draws a tick behind; Take() can see tick N's request with tick N-1's snapshot). ControllerNavigation's disarm on a visibility change then re-arms on the stale all-up sample, and the next frame's held Start sets OpenRequested, reopening the menu. Fix: delete the drawing thread's Start detector (menuArmed_, openRequested_, OpenRequested, the disarm and the overlay's Open line); the game thread's request covers every case it allowed.

P2: the game-thread arm, Start's owner, is untested inline state in a detour. Extract it as a small pure type next to ControllerClosesMenu and test it.

P2: the open-or-close answer to a Controls request is inline branching in DrawOverlay, and a request on a visible menu with a text field open falls through to Open. Make the decision one pure function (Open, Close, None) and test the editor case.

P3: TextEntryOpen should be const (add a const Navigation overload to ProblemReportPanel); ControllerSample's comment should say the game thread owns Start.

VERDICT: CHANGES REQUIRED
