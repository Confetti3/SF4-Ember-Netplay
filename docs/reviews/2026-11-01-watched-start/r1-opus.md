Review by Opus (Sol had reached its usage limit), against r1-prompt.md.

P1: The Start press that opens the menu over a fight closes it again. The game thread's Start (sf4e__Pad.cxx RequestMainControls) opens the menu through MenuPads::Take before ControllerNavigation::Update runs; Update then sees the same press with visible true and menuArmed_ still set, so CloseRequested fires and the overlay closes the menu it just opened. Fix: a press that meets a visibility change must be released first, or give Start over a fight one owner.

P1: Holding Start after it closes the menu reopens it. mainArmed in sf4e__Pad.cxx re-arms on any all-up sample, and a press while the menu captures does not use it up, so once the menu closes the still-held Start calls RequestMainControls. Fix: use up the arm on every Start press, captured or not.

P2 (structural, the cause of both P1s): two press-edge detectors on two threads own one button. Give Start over a fight one owner, the game thread's request, resolved by the drawing thread as open or close; delete CloseRequested.

P2: the editor check misses the problem report panel's own text field (ProblemReportPanel has its own MenuNavigation). Add an ApplicationShell accessor that also asks the report panel.

P2: the close rule is spelled inline in the overlay (ControllerOpensMenu && != MainMenu). Add input::ControllerClosesMenu next to ControllerOpensMenu and test it.

P2: tests cover only the isolated edge: missing the opening frame, a held Start after a close, the context rule, the editor case.

P3: the else adds nothing; give each accessor its own comment.

Checked: Start reaching the shell as MenuInput::Start in the closing frame does not matter (the shell is not drawn once the menu is hidden, and the gate rearms); MatchResult is excluded by ControllerOpensMenu.

VERDICT: CHANGES REQUIRED
