# Custom rooms status

**September 10 final source update:** the remaining gameplay dial/listener race
and over-eager recovery election were repaired. Started-match leader loss now
passes on default and forced-relay routes, and the sixteen-member/four-table
forced-relay run passes five match cycles. All 69 locked Rust tests, formatting
and Clippy pass. See the opening section of
[the validation record](RECOVERY_VALIDATION.md) for the final helper identity
and focused evidence. The detailed status below is retained development
history; the closing build/package receipt and uncompleted acceptance checks
are in `build/recovery-wrap-handoff.md`.

The designated `sf4-current` target now contains the implemented room-recovery,
membership, invitation, and route-probe repairs. The previous Rust candidate
passed its locked local gates, explicit direct and forced-relay process tests,
and the preserved four-participant terminal-recovery fixture. A later repair
keeps pending coordination writes out of the helper's service loop and passes
61 locked Rust tests, formatting, and Clippy. See [the recovery validation record](RECOVERY_VALIDATION.md) for exact
artifact hashes, test counts, selected routes, timings, and retained failure
logs.

> **Current integration status:** final combined acceptance is not complete.
> The latest designated build passed all 33 local tests, including the repair
> that commits a consecutive batch of terminal acknowledgments in one bounded
> candidate. Each acknowledgment retains its normal identity checks and exact
> committed reply. The sixteen-member, four-table matrix is running against
> these binaries. No deadline, payload limit, or acceptance predicate has been
> relaxed. Earlier timeout evidence remains preserved in the validation record.
> The admission publication fix passes its focused regression, all 61 locked
> Rust tests, formatting, Clippy, and exact-helper direct/relay process tests.
> Stable result and finish retries pass focused C++ and actual helper checks.
> The full room matrix and paired benchmarks remain open. Earlier failures and
> exact current evidence remain in the validation record. No final package,
> installed build, SF4 gameplay run, or two-PC native acceptance is claimed here.

The preserved September 8 package `ember-current-postmatch-ultra-20260908`
remains the comparison baseline. The room-worktree and combined Ember-room
totals later in this document are historical snapshot evidence and do not
replace current recovery validation.

## Current recovery behavior

| Area | Implemented behavior | Evidence boundary |
| --- | --- | --- |
| Invitations and admission | Room invitations bind the room, build, endpoint, capability, and expiry policy. Compact invitations bootstrap technical authority only through the authenticated invitation endpoint. A rejected join can leave and immediately retry on the same helper endpoint; a newly authenticated control replaces the stale same-endpoint control. | Wrong-build rejection followed by valid rejoin, three fresh rooms, and 90 Ready/Unready cycles passed. Final packaging and two-PC invitation use remain untested. |
| Membership and authority | A room admits at most 16 members. Technical coordination authority is separate from moderation ownership. Raft membership changes serialize removal and admission, preserve exact member history in snapshots, reject retired-incarnation replay, and stop admitting new history when the bounded provenance budget is full. Room mutations require a writable quorum; explicit replacement creates fresh authority when recovery is impossible. | Majority recovery, two-voter loss, moderator transfer, leader departure, snapshot provenance, delayed-admission, and full-room membership regressions pass. |
| Checkpoints and terminal results | Checkpoints are room-, term-, revision-, length-, and SHA-256-bound. Sender flow control allows four chunks in flight and advances only on an exact in-flight cumulative ACK. Native owners replay committed state and retain terminal results until exact recipients acknowledge them, including after journal eviction. | Near-limit actor transfer, native 1 MiB transfer, ACK saturation, terminal recovery, and retained-result fixtures pass. Final combined multi-table repetition remains open. |
| Route probes and gameplay preparation | A five-second probe is authorized only for the exact committed fighter pair, table revision, endpoints, and process incarnations. At least 80 of 100 unique replies are required. Duplicate or out-of-range sequences are rejected. A ready probe reserves the same authenticated Iroh connection for gameplay; closure or selected-path change invalidates advice, and invalidation is retained across native IPC backpressure. Prepared listeners have a renewable 60-second pre-stream wait and a separate 10-second gameplay proof handshake. | Direct and forced-relay helper tests, same-connection gameplay upgrade, closed-route invalidation, stale-completion fencing, delayed preparation, and recovery-pause deadline tests pass. |
| Live gameplay recovery | Started gameplay mappings remain independent of room-control reconnection. Technical-leader loss can preserve active GGPO sessions, capabilities, ports, frozen delay, and later result reconciliation. Incomplete preparation is cancelled or replaced through committed room state. | Focused started-game, preparation, committed-result, rematch, spectator, and terminal fixtures pass. Synthetic/helper evidence does not establish native SF4 gameplay. |

## Accepted room behavior

One private Iroh room admits up to 16 members and owns four independent versus
tables. Members can play, watch, queue, or remain idle. A game uses one local
instance at a time; queued members watch from the next game. The same two
fighters keep their seats through unlimited rematches until one leaves, with no
first-to limit or automatic set rotation. The next queued member fills a freed
seat, and both fighters ready before each game. Reconciled native results count
wins for the current pair without ending the room session.

Room text chat is bounded to 256 UTF-8 bytes and 100 retained messages, with
host moderation and local mute. Hosting is separate from P1. The recovery
candidate separates moderation ownership from technical coordination authority,
supports host transfer, and preserves healthy Started games during room-control
recovery.

Public discovery, online training, and mid-fight spectator catch-up remain
outside this implementation. Current recovery evidence is maintained in
[the recovery validation record](RECOVERY_VALIDATION.md); the historical runs
below establish only their preserved snapshot scopes.

## Room-worktree validation evidence

- The room worktree's Release x86 build passed for its Launcher, Sidecar, and
  x64 sf4-net helper targets.
- The room worktree's 28 CTest tests passed in 219.43 s. Focused results included
  NativeMatchResult (0.99 s), RoomAuthority (2.81 s), SessionClientTransport
  (1.12 s, including projection-client regression), and
  SessionServerTransport (0.75 s).
- Six games and rematches across four tables with 16 clients passed in 30.71 s
  in the room worktree.
- One table with two fighters and 14 spectators across six games passed in
  7.16 s.
- Six games across four tables with forced relay passed in 35.76 s.
- Rustfmt, all 20 Rust tests, and Clippy passed.
- The room worktree UI render run passed 1,215 frames across five DPI settings in
  22.81 s;
  the parent reviewed the resulting screenshots visually.
- The five room-worktree staged binaries matched their build outputs and passed PE
  architecture checks: x86 native binaries and the x64 sf4-net helper.

## Combined Ember-room validation

The parent integration validation subsequently completed the combined C++ build
with exit code 0. The combined CTest run passed all 29 tests in 161.61 seconds,
including CustomRoomFourTables (33.51 s), CustomRoomSpectators (7.98 s),
CustomRoomFourTablesRelay (36.06 s), and the SessionServerTransport test.
Rustfmt, 20 Rust unit tests, and Clippy with warnings denied also passed.

The final combined UI harness passed 2,016 DX9 frames across seven viewport and
DPI configurations with device resets after the room overview and Ready-footer
cleanup.
The completed build, room/helper tests, Rust checks, and UI harness do not
establish native SF4 gameplay, two-PC play, clean-machine installation, or a
packaged release.

The native result reader has static binary evidence and passing pure observer
tests, including rollback reset and long-frame confirmation. It has not had an
in-game or two-PC acceptance run. The package was staged locally; no game was
installed or modified.

## Workspace and ownership

The combined isolated tree uses Ember snapshot `4b0de7f` and room snapshot
`e8828d2`. The original dirty worktrees remain unchanged after hash checks of
2,315 Ember files and 2,388 room files. These are source snapshots, not a
publication or release receipt.

The earlier Luna work covered scoped authority/protocol, transport, and native
outcome investigation for these snapshots. Current repair ownership, frozen
artifact provenance, review findings, and combined validation are recorded in
[the recovery validation record](RECOVERY_VALIDATION.md). No game installation,
launch, or publication has been performed.
