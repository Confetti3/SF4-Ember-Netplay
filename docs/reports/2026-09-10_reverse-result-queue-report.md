# Result delivery and lobby exit failure

The second play test captured a native winner but could not send it because the room router had failed. Replacing winner detection alone did not address this failure. A read-only inspection of the running package established the queue failure and the related exit loop.

## Scope and evidence

[Authorized local scope](../../build/current/debug-result-210347/scope.md). No process memory was changed. Existing packages were preserved.

E-001: [Live state summary](../../build/current/debug-result-210347/evidence.json), SHA-256 `f5ffdb8d31a12ee77468765b95f16f6ebc6c99de90f6bf6733fd45f570a94e10`. The game loaded Sidecar from package `ember-result-rematch-fix-20260910-210347`; its SHA-256 was `D07ADE9756C3A6D9389FA5229CD6A08D94ACA13E162B0E126ADB53401308728C`. ReadProcessMemory and the matching PDB showed:

- `room_server_receive_queue`, with room state Failed but writable/rebound still true.
- Winner captured and pending, result/finish action IDs zero: neither report had been queued.
- Leave requested, native GGPO socket released, coordinator Ending, helper close not dispatched.
- The pending proposal contained one battle hash. Its 176-entry history retained 82 hashes and 40 battle snapshots.

The [retained probe](../../build/current/debug-result-210347/read-stalled-state.py) is specific to PID 27048 and that module/PDB layout; its live run cannot be replayed after the process exits. The JSON summary is the portable evidence.

E-002: `room_message_queue_test.cxx` drove the production queue policy with 300 verification frames while its consumer was paused. Before the repair it failed on the queue limit. After the repair it passed and preserved ordered result, finish, and leave messages.

## Findings and repair path

F-001 (high confidence, E-001/E-002): diagnostic traffic could exhaust the same 64-message queue needed for results. The server committed one diagnostic per quorum round trip while the fight continued producing more. Queue policy now retains a recent diagnostic window, preserves ordered control messages, and batches diagnostic prefixes. The shared recovery journal retains at most 32 historical diagnostic identities without dropping result/lifecycle receipts to make that space.

F-002 (high confidence, E-001): fatal room failure retained writable coordination flags, so the UI could appear healthy. Failure now clears these flags.

F-003 (high confidence, E-001): Leave repeatedly entered match Ending, but EndMatch rejected the failed room. `CloseFailedRoom` now retires the helper epoch once native socket ownership ends. The next room remains blocked until helper shutdown is acknowledged.

P-001 callflow: battle verification -> queue held during checkpoint commit -> receive-queue failure -> unsent result -> Leave -> rejected generation teardown. The changes address the queue pressure, failure status, and final cleanup boundaries.

## Validation

The focused queue regression changed from failure to success. Additional coverage exercises diagnostic commit batching, journal compaction, and failed-room retirement with and without native socket ownership. Run `scripts/build-current.ps1` for the designated build and local suite; its build-provenance receipt and `build/current/result-queue-build.log` record the final outcome. New two-PC gameplay acceptance remains pending.

Timeline: the prior synthetic winner fix passed but failed live; this run inspected the still-running process, reproduced queue exhaustion, implemented bounded diagnostic handling and failed-room retirement, then prepared regression/build validation.
