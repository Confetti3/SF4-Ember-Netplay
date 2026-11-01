Review by Opus (Sol had reached its usage limit), against r4-prompt.md.

Earlier rounds: r1, r2 and r3 findings fixed; r3's overlay P3 half fixed.

Checked: one detector (StartPress, game thread) and one decision (AnswerStart, drawing thread); no Holds() lag left; Start at the shown main menu answered None; pad setup's binding capture cannot be closed by Start; MenuInput::Start owners all covered by TextEntryOpen; thread lag closes the menu that is on screen; the closing Start never reaches the game or native pause; NativeStart is the real bit; file sizes; tests cover every StartPress case, ControllerClosesMenu and AnswerStart.

P3: the overlay still maps every request kind to a StartAnswer and re-checks the kind for the reason and ShowPlay/ShowTraining. Use one switch on the kind.

VERDICT: APPROVE

Addressed after approval: the request block is one switch on the kind, with Controls switching on AnswerStart.
