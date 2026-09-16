# SF4 Ember Netplay v0.8.7

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

This release follows a code audit of the rollback core, the room and result
flow, and the interface after v0.8.6. Its changes have not yet been observed
in gameplay; the validation section says exactly what was run.

## Changes

### Rollback and stability

- **A netplay error inside a rollback no longer risks a crash.** When GGPO
  rejected input while re-simulating, the session was closed from inside GGPO's
  own callback, and GGPO then kept calling into the closed session. An error
  raised inside a callback now waits until GGPO has finished the current call.
- **Offline play works again after a netplay error.** An aborted or failed
  online match left the simulation gate closed; the next offline Versus or
  Training battle never advanced a frame until a new online match was started.
  The gate reopens when the session is retired.
- **A spectator dropping no longer ends the fighters' game.** GGPO reports a
  spectator's disconnect with the same event as a fighter's, and the game
  treated every such event as the opponent leaving. The same applied to a
  spectator whose state diverged: its snapshot mismatch could end the players'
  match. Both are now spectator-only conditions.
- **Remote inputs are read right before they are needed.** The only network
  poll ran after the frame had been rendered, so an opponent input that arrived
  during the frame was applied one frame late and predicted for one frame more
  than the connection required. The game now also polls immediately before
  simulating, which shortens rollbacks by up to a frame.
- **Cheaper saves.** The per-frame save recorded sound state into two maps that
  were rebuilt from scratch every simulated and re-simulated frame, and a
  restore could insert zeroed entries for adapters the save never recorded.
  Both are flat, capacity-retaining records now, and a restore never invents
  state. The rollback load no longer allocates its scratch buffer per rollback.
- The interface snapshot is rebuilt only when something changed (or every
  sixth frame) during a match, instead of every frame; the helper is polled
  once per tick instead of up to three times; the legacy snapshot map is
  bounded; the rollback stress harness never runs alongside a live session;
  and GGPO's internal logging no longer formats every line and reads two
  environment variables per call when logging is off.

### Rooms and results

- **Rematch lockout fixed.** A player who missed the end-of-match event (lost
  while their connection was recovering) could never acknowledge it, so the
  table stayed fenced with "Waiting for all players and spectators to finish
  returning from the previous match." The room now replays outstanding
  end-of-match receipts when a member rebinds and when it acknowledges the
  wrong game, and a client stops retrying an acknowledgement the room has
  rejected three times.
- **A table can no longer be stuck "In game" forever.** The finish report was
  dropped when the local room projection was one checkpoint behind; the table
  then never armed its 30 second result timer. The report is now retried until
  the table moves on, and a host who is not one of the fighters can cancel a
  game that never reported a result.
- **A parked Ready or table edit is reported instead of forgotten.** Pressing
  Ready during the previous match's teardown retired GGPO and parked the press;
  a recovery blip discarded it silently. It now expires with a message after
  15 seconds.
- **Refused room actions read as sentences.** Tokens such as
  `table_not_ready_for_action` and `match_generation_expired` no longer reach
  the status line, and the expected duplicate reply to a retried result report
  no longer shows as an error. A result the room can no longer record says so.
- A match that ended before GGPO reached Running left the room in "Preparing"
  and refused Ready; it now ends normally. The survivor of an opponent who
  left the room mid-game is told so.

### Interface

- **Every in-match message is now shown.** "Connection unstable", "Connection
  restored", "Opponent disconnected", every abort reason, room-connection loss
  and the join-rejection messages were written to a queue nothing read. The
  match HUD has a state line above the telemetry that shows them with a
  severity colour, names a prediction stall ("Waiting for opponent..."), and
  counts down to the disconnect timeout during a connection warning. With the
  match HUD hidden the line still appears on its own. A desync now says
  "Match ended: the two games diverged" instead of returning to the menu
  without a word.
- Recovery and update messages are rendered in the launch-recovery screen (they
  never were); "Export diagnostics" shows its own result instead of writing it
  under "Check for updates"; "Exit to recovery" is disabled with a reason while
  in a room instead of doing nothing.
- Status and notices expire: a shell error clears after 20 seconds and a
  transient runtime error after 30, so they no longer hide save feedback for
  the rest of the session. Notices outrank routine "Saving..." text, and the
  replace-room warning is drawn as a caution rather than a success.
- Wording: "Result unresolved" now points at Abandon rather than only the
  host; Leave room describes what is lost (an unresolved game, a seat, a queue
  place, the host role); Leave seat says the queue takes the seat; the room and
  table screens use one "Change fighter" label; the five "could not be queued"
  messages are one. Ready explains every refusal.
- The wide room board exposes elided and second-line text on hover; the wide
  Home breakpoint scales with the interface size; the training frame meter's
  "unavailable" tooltips work again.

## Validation and limits

The designated Windows build runs the native test set: 39 of 39 suites pass,
including the DX9 render harness (6534 frames across ten viewport and DPI
configurations). The Iroh network plan (`scripts/test-current-network.ps1`)
passed every case on both the direct and the forced-relay route: room,
queue-acks, game, authorized, terminal-recovery and recovery. Two relay cases
each timed out once on the public relay and passed when rerun. The network
harness itself had a stale completion marker for the queue-acks case since
v0.8.1, so that case could not report a pass before this release.

New coverage in this release:

- the callback abort latch: immediate outside a callback, deferred and drained
  exactly once inside one, nested callbacks, and reset by a new session;
- the match HUD state line wording and precedence, and three rendered HUD
  states at every viewport and DPI;
- a match ending from Preparing returns the room controller to PostMatch;
- the room replays an unacknowledged end-of-match receipt to a rebound member
  and to a member acknowledging the wrong game, once, together with the
  native game_end.

**Not yet observed in gameplay:** the pre-simulation poll's effect on rollback
depth (compare `rollbackCallbacks` with `SF4E_ROLLBACK_DIAGNOSTICS=1` at the
same delay and route), the spectator-disconnect behaviour on a live room, the
terminal-receipt replay after a real reconnect, and the frame-time effect of
the allocation changes (`scripts/capture-frame-pacing.ps1`). If a problem
appears, keep both players' `%APPDATA%\sf4e\logs` along with
**Help & About → Export diagnostics**.

## Install or upgrade

For a fresh installation, download **sf4-ember-netplay-0.8.7.zip** and its `.sha256` sidecar. Extract the complete package into a new writable folder, run **preflight.cmd**, then run **Launcher.exe**.

For an intact published v0.8.6 installation, download **upgrade-sf4-ember-netplay-0.8.6-to-0.8.7.zip** and its checksum. Extract it into a separate folder, close SF4 and Ember, run **Install Upgrade.cmd**, and select your existing v0.8.6 Ember folder.

Everyone in a room must use the same Ember release; the room refuses a different build. Preferences and your win/loss record remain under `%APPDATA%\sf4e`. Requires Windows 10 or later (x64), an owned Steam copy of Ultra Street Fighter IV and the Microsoft Visual C++ x86 runtime.

See [the player guide](USER_NETPLAY.md) and [troubleshooting](TROUBLESHOOTING.md). Keep invitations private.
