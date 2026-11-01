1. **P1 — A rule change while selection is locked destroys the remembered edition.** [src/sf4e/sf4e__Overlay.cxx:895](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/sf4e/sf4e__Overlay.cxx:895)

   Rule-following waits for `snapshot.canEditSelection` at line 660, but `Remember` unconditionally uses the **current snapshot’s rule**.

   Concrete scenario: Ryu’s remembered edition is AE; a table without Edition Select correctly forces the lobby pick to Ultra. The player enters Training while waiting alone at that table, and the host enables Edition Select. While Training keeps selection non-editable, `FollowEditionRule` is deferred, but `Remember(prefs, true)` copies the still-forced Ultra into Ryu’s memory. A successful save commits that overwrite. Returning from Training restores Ultra because AE has already been lost.

   **Fix:** Preserve the distinction between a user-selected edition and a rule-constrained pick. While rule-following is deferred, do not remember the projected lobby edition as a new choice. Reconcile before recording against the new rule, or record editions only from explicit selection edits. Add a regression covering `false → true` while non-editable, several locked frames, and the eventual editable frame.

2. **P2 — Live edition memory depends on the save queue accepting the edit.** [src/sf4e/sf4e__Overlay.cxx:664](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/sf4e/sf4e__Overlay.cxx:664)

   `FollowEditionRule` restores from `s_prefs`, but line 896 updates `s_prefs` only when `Save` returns true. The new memory therefore represents the last accepted persistence snapshot, rather than the player’s latest choice.

   Concrete scenario: the accepted memory contains Ultra; the player selects AE under Edition Select, but `Save` rejects the snapshot—for example, because the persistence worker failed to start. Entering a room without Edition Select replaces the only live AE value in `lobbyConditions` with Ultra. Returning to Edition Select then restores the stale Ultra from `s_prefs`. The choice disappears within the running session, even if persistence later becomes available. This concerns **queue rejection**; an asynchronous disk-write failure after queue acceptance does not follow this path.

   **Fix:** Keep authoritative per-fighter memory in live selection state and update it independently of persistence success. Track the last accepted snapshot separately so failed submissions remain pending for retry. There is a useful structural simplification here: derive the room-constrained lobby pick from live remembered choices, rather than using the persistence snapshot as selection state. Add a regression that rejects saving an edition edit, crosses both rule boundaries, and verifies that the edited edition survives and can later be saved.

The added tests exercise the helpers directly, but miss both frame-level failure sequences. Ordinary fighter restoration and edition-dependent Ultra fallback use the canonical normalization path correctly. No scoped file crosses 1,000 lines. I inspected the tests and checked the scoped diff; I made no edits and ran no builds or tests. An unrelated concurrent overlay edit shifted the references above, which point to the latest file contents.

VERDICT: CHANGES REQUIRED