Review by Opus (Sol had reached its usage limit), against r3-prompt.md.

Earlier findings: r1 P1 and P2, r2 P1-1, P2-1 and P2-2 all fixed; r2 P3s fixed apart from test coverage.

Paths traced that hold: switching fighters under either rule; an edition edit under Edition Select remembered in the same frame; joined but not attached (selection locked, nothing runs); one frame of a wrong rule corrected the next; canReady implies an editable, fitted pick; Clamp's Remember(false) on every save changes nothing; frame and thread order; the device-reset test's premise.

Structure: no regression; fitting the pick in one place each frame and remembering it in one place is the simpler model. sf4e__Overlay.cxx is near 1k lines; the next feature there should decompose first.

P1: none. P2: none.

P3-1: under a rule without Edition Select the memory can hold an edition with an Ultra that edition lacks (AE with Double Ultra). Keep the stored Ultra unless the lobby's is allowed for the kept edition.
P3-2: Data::lobbyEditionSelect no longer affects anything (legacy key).
P3-3: FollowEditionRule takes a whole Data but reads only fighters.
P3-4: the frame test restates DrawOverlay's sequence; missing a fighter switch, the Ultra refit, a refused save and the Reset path.
P3-5: the two halves of the per-frame contract re-derive the rule and the gate 235 lines apart; compute both once.
P3-6: "selection unavailable" can show while the selection is locked after the host turns Edition Select off; accurate and clears itself.

VERDICT: APPROVE

Addressed after approval: P3-1 (Remember keeps a valid edition and Ultra pair, tested), P3-3 (FollowEditionRule takes the fighters array), P3-5 (pickEditable and pickEditionSelect read once per frame). P3-2, P3-4 and P3-6 are left as they are.
