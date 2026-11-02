Reviewed `c4aa95a71..59e98611f` on `fix/archive-duplicates`. Two findings block approval.

**F1 — P1 bug — [server/ember/bridge/src/archive/duplicates.rs:67](/C:/Users/Kate/Desktop/sf4/web-archive-dedupe/server/ember/bridge/src/archive/duplicates.rs:67): Destructive folding loses names when the keeper changes.**

`fold` applies naming rules against the earliest **identified** copy, then marks rejected donors as duplicates. An earlier row with `game = NULL` can subsequently become the real keeper, but those donors are excluded from reconsideration and may already have been deleted.

Concrete reproduction with three legacy copies of the same game, in arrival order:

- A: player A, side 1, named Ann.
- B: player B, side 0, unnamed.
- C: player B, side 0, named Bob.

When reads complete **B→C→A**, B initially keeps the game. C cannot name B’s empty side because B is its first sender, so C becomes a duplicate. When A is identified, B contributes no name and C is ignored. The final A row remains unnamed on side 0. With completion order **A→B→C**, the final row correctly names Bob.

I reproduced both results using the branch’s SQL in an in-memory SQLite harness. This makes durable naming depend on concurrent GET completion order.

**Concrete fix:** separate identity backfill from destructive consolidation. Establish the final keeper before applying naming rules and discarding donors, or retain the original donor observations until they have been evaluated against that keeper. Add a deterministic regression covering both completion orders and removal between folds.

**F2 — P2 structural — [server/ember/bridge/src/delivery/duplicates.rs:45](/C:/Users/Kate/Desktop/sf4/web-archive-dedupe/server/ember/bridge/src/delivery/duplicates.rs:45): Read turns failed validation into a successful identity backfill.**

`Replay::parse` failure silently assigns `game = digest` and reports delivery success. Because Read only leases rows with `game IS NULL`, repairing the bucket object later never causes that row to be reconsidered. This conflates a valid short replay—which the protocol already handles—with an unreadable or malformed stored export.

The successful parse path also never checks that the returned replay’s digest matches the requested key. A valid replay returned under the wrong key can therefore drive destructive folding using another game’s identity.

**Concrete fix:** require a successful parse and matching `digest_hex()` before calling `fold`. Return a diagnostic retry on either failure and leave `game` unset. Cover malformed and valid-but-mismatched responses, followed by a repaired response.

The two delivery queues otherwise earn their place through the existing durable retry machinery. The SQL probes passed live-upload deadline preservation, stale-record fencing, and set-row preservation. No file crosses from below 1,000 lines to above it, and `git diff --check` passed. These probes are not Rust integration tests; I reused your reported test results without rerunning the suites or claiming the in-progress full run passed. No files were edited.

VERDICT: CHANGES REQUIRED
