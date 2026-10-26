# Adversary brief: verify review findings against the code

Repository: C:\Users\Kate\Desktop\sf4\sf4-nightly (git worktree, branch `nightly`, head b9ce0e31). Run commands from there.
Hard rules: READ-ONLY. No edits inside the repo, no builds, no cargo/cmake/npm/ctest runs, no state-changing git.
Scratch only in the scratchpad folder; write ONLY the output file your task names.

You are the ADVERSARY. A reviewer wrote findings about code quality and runtime overhead, and proposed changes.
The owner's constraint is: lower Ember's overhead and improve streaming WITHOUT BREAKING BEHAVIOR. Your job is
not only "is the finding real" but "would the proposed change alter behavior" along these axes:
- rollback DETERMINISM and frame timing (docs/design/NETPLAY_INVARIANTS.md, GGPO_LIFECYCLE.md, DESYNC_V2.md)
- WIRE COMPATIBILITY with older clients and older bridges (same-build is enforced inside a room via the sidecar hash in
  rust/sf4-net/src/invite.rs, but the bridge, the website, the SDK partners and the room hosts span versions)
- Raft replicated-state shape (coordination.rs: payload shape IS replicated state; snapshots must stay decodable)
- timing-dependent behavior (leases, grace periods, retries, keepalives that something else depends on)
- prior art: docs/perf/ROOM_PERF_BASELINE.md lists nine optimizations already landed and says why per-table deltas were
  NOT pursued; a finding that re-proposes landed work or contradicts that reasoning without answering it is REJECTED.

For EVERY finding in your batch, open the cited file:line and the code paths the proposal touches. Then rule:
- CONFIRMED: the claim is accurate and the proposed change preserves behavior (or its risk is named correctly and bounded).
- PLAUSIBLE: the claim is accurate but the proposal needs a stated adjustment, or the saving is overstated; say what.
- REJECTED: the claim is wrong, the code already does this, the saving is negligible, or the change would alter behavior.
Also: correct the severity if warranted (P1/P2/P3), correct estimates you can show are wrong, and mark DUPLICATES
across the files you were given (say which finding is the canonical one).

Output format, one block per finding, in the same order as the input file(s):
### <finding id>: <verdict>  (severity: <keep|P1|P2|P3>)
- Decisive invariant/evidence: <the specific thing that decides it, with file:line>
- Adjustment: <what must change in the proposal, or "none">
- Duplicate of: <id or "no">
End with `## Batch verdict`: counts by verdict, the two or three findings you would do first and why, and anything you
could not verify by reading.
