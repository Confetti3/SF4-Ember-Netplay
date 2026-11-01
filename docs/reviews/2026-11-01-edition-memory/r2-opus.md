Review by Opus (Sol had reached its usage limit), against r2-prompt.md.

Round 1: P1 (rule change while locked) fixed as implemented; P2 (live memory tied to the save queue) fixed within a session, but a display Reset reloads the saved picks (P2-2).

P1-1: canReady (sf4e__Overlay.cxx ~343) and selectionError (~446) still check snapshot.lobbySettings.editionSelect, which outside a room is the player's own hosting default (preferences.lobby), while the follow and the selector use EditionSelectNow (Edition Select outside a room). A player who hosts without Edition Select leaves an Edition Select room with AE Ryu: Home brings AE back and then says "selection unavailable", disabling Training select's Start. Fix: use EditionSelectNow at both.

P2-1: EditionRule is a state machine the canEditSelection gate makes unnecessary. Every writer of lobbyConditions is gated on canEditSelection and FollowEditionRule is idempotent, so: while editable, every frame FollowEditionRule then Remember under EditionSelectNow; while locked, neither. Delete EditionRule.

P2-2: a device Reset calls InitializeOverlay, which reloads the picks from Load over the live s_prefs and lobbyConditions; and Clamp normalizes the lobby under the file's legacy lobbyEditionSelect. Fix: load the picks once per process; clamp the lobby with true, leaving the rule to the follow.

P3-1: stale invariant comment on Data::fighters. P3-2: EditionRule is a UI policy in the persistence header (moot if deleted). P3-3: document that the Ultra remembered beside the edition is normalized when the edition returns. P3-4: tests miss true to false while locked, the rule agreement and the Reset path.

VERDICT: CHANGES REQUIRED
