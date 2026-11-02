# Round 2 (Opus; Sol's sandbox failed to start, see r2-sol.md)

Round 1 F1 (fold order) and F2 (bad file accepted): fixed. Races checked, none found.

- F1 P1: a set's copy found another player's online copy by `game` and was dropped as Held (sets::held `None` arm, sent_by != sender), then marked archived; fold kept set copies, admit kept the online one. Fix: one rule.
- F2 P2: the fold ran only from `identified`, so an operator removing an unreadable row never started it. Fix: fold from a state check in maintenance.
- F3 P2: Read and Remove as two queues duplicate the secret check, lease helper, SQL-fragment parameter, spawns and `key()`. Fix: one queue with an enum item; share `key` with delivery::archive.
- F4 P3: a body exactly 0x320 long is known by its fighters alone. Fix: require one round record (0x88).
- F5 P3: "each lease starts the retry schedule afresh" is wrong: attempts carries on.
- F6 P3: name_side takes two arrays and an index; pass the one name and country.
- Tests missing: a fold keeping the set copy; Remove waiting for a live upload lease.

VERDICT: CHANGES REQUIRED
