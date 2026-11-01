1. **P1 — The helper loses the legacy fallback when paired with an old game.** [rust/sf4-net/src/tournament/client.rs:572](/C:/Users/Kate/Desktop/sf4/sf4-nightly/rust/sf4-net/src/tournament/client.rs:572)

   **Scenario:** A new bridge refuses creation with `reason = room_limit` and `cause = own_room` or `network`. The new helper replaces the base reason with the composed code. An old game recognizes neither code: its `PublicRoomsPanel::FailureText` falls through to `IdentityPanel`, displaying **“That did not work (room_limit_own_room).”** or its network equivalent. Preserving the bridge’s reason protects old helpers, but does not protect old games behind new helpers. This fails the requested compatibility bar.

   **Fix:** Preserve `room_limit` through the helper-to-game boundary and carry the cause as additive structured metadata. Let the new game select the specific wording while old games retain their existing sentence. Alternatively, emit specific codes only after explicit client capability negotiation. Add a regression covering a new bridge and helper with the legacy game consumer; the current tests exercise each updated layer separately and miss this combination.

2. **P2 — String composition can turn an unrecognized reason into a recognized refusal.** [rust/sf4-net/src/tournament/client.rs:569](/C:/Users/Kate/Desktop/sf4/sf4-nightly/rust/sf4-net/src/tournament/client.rs:569)

   **Scenario:** The rooms reader receives a valid error envelope with `code = stale_revision`, `details.reason = room`, and `details.cause = full`. Neither field describes a recognized room refusal, but concatenation produces `room_full`, which is whitelisted. The game now claims the room is full instead of taking the unknown-reason fallback. Likewise, `room` plus `limit_network` produces the new network-limit message. Underscores in flat identifiers have silently become a parsing grammar across every caller of this shared reader.

   **Fix:** At minimum, require the base reason to be recognized before considering a cause. Prefer an explicit mapping of the supported room-limit pairs at the room boundary if retaining this representation. Test an unknown base reason whose concatenation matches a recognized code; the existing “cause on another reason” test only covers a combination that cannot collide. Carrying the cause separately, as in finding 1, also avoids manufacturing failure identities from spelling.

The compatibility trace for creator/address limits is:

| Bridge | Helper | Old game | New game |
|---|---|---|---|
| Old | Old | Legacy room-limit sentence | Legacy room-limit sentence |
| Old | New | Legacy room-limit sentence | Legacy room-limit sentence |
| New | Old | Legacy room-limit sentence | Legacy room-limit sentence |
| New | New | **Generic failure with raw code** | Specific own-room/network sentence |

The remaining scoped paths look sound on inspection:

- **Reconcile and retry:** `open()` still checks the unchanged `details.reason`. It re-polls and retries for both causes, and preserves the original refusal on timeout. Existing tests cover stale creator and address limits, shared polling, and bounded waiting.
- **Partner path:** Creator-limit precedence and conditional disclosure of `room_id` remain intact. A connection’s own limit carries no cause. Existing partner tests cover those behaviors, but do not assert the new cause contract.
- **Wording and size:** Both entries are present, unique, and nonempty in all 14 catalogs. The wording distinguishes the intended limits, and the existing CJK fonts contain the required glyphs. No scoped source file crosses from below 1,000 lines to above it.

This was a static review against the working-tree diff and unchanged `HEAD` consumers. No files were edited, no build was performed, and no test suites were run.

VERDICT: CHANGES REQUIRED