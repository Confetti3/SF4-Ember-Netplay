No P1, P2, or P3 findings in the scoped working-tree changes. Both Round 1 findings are resolved.

The earlier mixed-helper/game P1 is withdrawn. The launcher resolves `Sidecar.dll` within its package and starts `<installRoot>/sf4-net.exe` explicitly; `HelperProcess::Start` passes that executable path directly to `CreateProcessW`. I found no supported launch path that selects another package’s helper. See [launcher.cxx:739](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/launcher/launcher.cxx:739), [launcher.cxx:776](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/launcher/launcher.cxx:776), and [HelperProcess.cxx:81](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/platform/HelperProcess.cxx:81).

The four shipped combinations behave sensibly:

| Bridge | Helper + game package | Room-limit wording |
|---|---|---|
| Old | Old | Existing generic message |
| Old | New | Existing generic message |
| New | Old | Existing generic message; additional `cause` is ignored |
| New | New | Specific own-room/network message; generic message for partner and server limits |

The generic composition is reasonable in the existing architecture. [client.rs:566](/C:/Users/Kate/Desktop/sf4/sf4-nightly/rust/sf4-net/src/tournament/client.rs:566) requires **both the base reason and composed code** to appear in the route’s reason list. Unknown, absent, or non-string causes preserve the base reason; an unknown base reason preserves the error-code fallback. The room route owns which composed codes it accepts. A narrower implementation would not clearly simplify this without adding room-specific handling to the shared decoder or another response-processing path. The two unknown-reason regressions are explicitly covered in [rooms.rs:394](/C:/Users/Kate/Desktop/sf4/sf4-nightly/rust/sf4-net/src/tournament/rooms.rs:394).

The reconcile-and-retry path remains correct. [create.rs:84](/C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/routes/rooms/create.rs:84) still checks `details.reason == room_limit`, so either cause triggers reconciliation. A timeout returns the original refusal with its cause; a completed reconciliation reruns the reservation and computes the current refusal afresh.

The partner path preserves its existing contract: an existing creator room receives `own_room`, with `room_id` disclosed only to the connection that created it. A connection’s aggregate limit receives no cause, and a supervisor’s refusal receives no cause. Reservation and limit checks remain within the existing transaction.

Test inspection covers both causes, generic fallback, unknown-cause fallback, unknown-reason collisions, server-limit cause absence, reconciliation after creator/address rooms close, bounded polling, and game wording for both new codes. All 14 locale files contain exactly one nonempty entry for each new message; I found no substantive wording mismatch. No scoped source file crosses from below 1,000 lines to above it; `client.rs` was already above that threshold.

No files were edited. No build or test suite was run. The scoped whitespace check passed; conclusions are based on source and test inspection.

VERDICT: APPROVE