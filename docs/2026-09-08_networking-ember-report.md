# Room migration and GGPO hardening: implementation status

**Partial implementation. Live room migration is not enabled and this is not a migration release.**

Work is confined to the designated `sf4-current` integration checkout. Existing UI, profile, controller, room, Discord, and training changes are preserved. No SF4 installation or launch is authorized or performed. This report distinguishes executable component tests from a complete room-recovery implementation and from native SF4 acceptance.

## Implemented components

| Component | Implemented behavior | Remaining boundary |
| --- | --- | --- |
| Rust consensus | Exact OpenRaft 0.9.25 pin; bounded checkpoint proposals; term/revision checks; payload-bound deduplication; snapshots; 250 ms heartbeat and 1.5–3 s election configuration | Not instantiated by the helper service's live room actor |
| Coordination transport | Separate authenticated Iroh endpoint; room/source/target binding; Raft sender binding; bounded RPC concurrency, payloads and chunks; endpoint admission map | Admission must come from committed membership; process-incarnation and learner promotion lifecycle still need integration |
| C++ room checkpoint | Rules, roster, seats, queues, watchers, scores, chat, bans, deduplication, counters, pending reports and time; atomic validation before import | Cross-machine monotonic-clock rebasing and recovery-time result suspension remain necessary |
| C++ server checkpoint | Native setup, connection/CID mappings, selected tables, authenticated identities/bans, battle readiness, and MatchAuthority handshake state; no pair capabilities or socket handles | A server-wide candidate/effect commit gate must be wired before replacing live authority |
| Commit primitive | RoomAuthority candidate copied privately; effects withheld until matching term/request/revision acknowledgment; old-term candidate discarded | This alone does not gate every SessionServer mutation or outgoing native/control message |
| Host-role primitive | Host transfer changes room ownership without advancing gameplay generation | Transfer UI, graceful departure, automatic failover and invitation refresh are not wired |
| GGPO confirmed frame | Read-only fork accessor; exact GGPO state/input frame boundary gates semantic checkpoint exchange; spectator consumed-confirmed stream supported | Native replay/hash acceptance still requires actual SF4 testing |
| Zero input delay | Validation, settings load/persistence, menu value range and GGPO test support 0–10; default stays two | Per-fighter advisory probe and immutable Ready-time delay selection are not implemented |
| Dependency provenance | Build owns `build/current/dependencies`; current GGPO port patches checked against vcpkg ABI record; staged DLL parity and Rust lock hash recorded | A final migration archive requires completion of the remaining feature and acceptance gates |

## Evidence and findings

Observed on 2026-09-08. Commands below run from this checkout; Rust commands require the x64 MSVC developer environment used by the helper resource compiler.

| Evidence | Source and reproduction | Observation |
| --- | --- | --- |
| E-001 | `src/tests/room_authority_test.cxx`; `build/current/RoomAuthorityTest.exe` | Checkpoint round-trip retains bans/deduplication and pending result timeout; old-term proposal cannot commit; matching reports score once; host-role transfer leaves the active match generation intact |
| E-002 | `src/tests/session_server_transport_test.cxx`; `build/current/SessionServerTransportTest.exe` | Started-match checkpoint round-trip preserves full server state; malformed imports leave it unchanged; no gameplay grants or transport close are emitted by import; duplicate handshake acknowledgment does not restart the match |
| E-003 | `rust/sf4-net/src/coordination.rs` and `coordination_iroh.rs`; `cargo test --locked --lib` in `rust/sf4-net` | 28 tests passed, including new one/two/three/sixteen-node commitment fixtures and authenticated Iroh host-loss election. Three-node recovery retains the committed checkpoint; a two-node partition cannot commit a replacement owner |
| E-004 | `src/tests/ggpo_confirmed_frame_test.cxx`; `build/current/GgpoConfirmedFrameTest.exe` | Two real loopback GGPO peers run 120 zero-delay frames; confirmed input N corresponds to saved state N+1; repeated accessor reads cause no saves or advancement; null arguments are rejected |
| E-005 | `scripts/build-current-dependencies.ps1`; `build/current/dependencies/x86-windows-wchar-filenames/share/ggpo/vcpkg_abi_info.txt` | GGPO 1.0.0 port revision 5 rebuilt from fork c88b667 and all three local patches in the designated dependency tree |
| E-006 | Baseline `build/migration-baseline-20260908`; previous `dist/sf4-netplay-launcher-ember-current-profile-recovery-20260908.zip` | Prior source and package preserved for comparison; no before/after performance claim is made |

### F-001: migration needs more than a UI snapshot

Status: validated at the component boundary. Evidence: E-001, E-002. Location: RoomAuthority and SessionServer checkpoint APIs. The old room snapshot does not contain all authority state. Recovering from it alone would lose deduplication, pending reports and native match coordination. The new private checkpoints preserve these fields without distributing pair capabilities. Call path P-001: committed state → checkpoint export → bounded consensus payload → validated temporary import → replacement control state. The last two steps are tested as components, not connected to automatic live recovery.

### F-002: aged engine frames are not an exact confirmation API

Status: corrected, synthetic validation only. Evidence: E-004 and the prior SessionClient aging gate preserved in E-006. Location: `ConfirmedCheckpoint.hxx`, `CaptureHashCheckpoint`, `SessionClient::Step`, and the GGPO fork patch. The old gate used thirty elapsed engine frames. The replacement records the GGPO save-frame number and queries the backend's last confirmed input frame. Call path P-002: successful GGPO advance/save → capture exact state-frame identity → read confirmed input boundary → send/compare eligible hash. No prediction-window, pacing, savestate allocation/ownership or disconnect-timeout rewrite was made.

### F-003: shared dependency installations could retain an older fork

Status: corrected build path, final receipt required. Evidence: E-005 and the baseline build script in E-006. The earlier workflow configured another checkout's dependency installation with manifest installation disabled. Call path P-003: designated manifest and overlay port → vcpkg ABI/rebuild → designated CMake dependency paths → staged DLL hash check → build receipt. Old cache paths are cleared and checked. The build uses an explicit MSVC environment because automatic discovery rejected the installed VS 18 Build Tools; this follows the documented [vcpkg triplet environment controls](https://learn.microsoft.com/en-us/vcpkg/users/triplets#vcpkg_load_vcvars_env).

## Reproduce the checks

```powershell
# In sf4-current; this builds/stages locally, without running SF4.
.\scripts\build-current.ps1

# Explicit public Iroh connectivity tests; never interpreted as native gameplay proof.
.\scripts\test-current-network.ps1 -IncludeLargeRooms

# In rust/sf4-net, with the x64 MSVC developer environment initialized:
cargo clippy --locked --lib -- -D warnings
cargo test --locked --lib
```

The network script records each executed test, route policy, exit code and log in `build/current/network-test-results.json`; it fails at the first failing case. Default-route policy is not evidence of a direct route. Forced relay is fallback-connectivity coverage, not a latency guarantee. A CTest filter matching zero registered tests is not accepted as a network test pass.

## Work required before a migration package

1. Wire authenticated coordination admission, caught-up learners, voting changes and fresh-process incarnation handling into the helper actor and C++ IPC. In-memory voters must never restart under their old identity after losing state.
2. Gate the entire SessionServer candidate and its outgoing effects on consensus commitment. Discard uncommitted native preparation effects on leadership changes; do not replay freshly generated pair capabilities into a live game.
3. Rebind only control authority, preserving the existing IrohMatchSession, GGPO session, UDP sockets, gameplay mappings/capabilities, generations and pacing. Add host transfer, recovery state, term-bound invitations and Discord refresh. Do not call the existing whole-room Leave/Begin methods to migrate.
4. Add safe preparation cancellation, buffered two-fighter result reconciliation, quorum-loss fallback and explicit replacement-room UI. Preserve identity-based menu focus and default destructive confirmations to Cancel.
5. Implement the authenticated five-second pre-match probe, sample sufficiency and route invalidation; expose Recommended/Selected delay and lock each fighter's choice at Ready.
6. Run the missing seeded migration/fault/congestion matrix and identical baseline/candidate performance workloads. Record forwarding/frame-cost percentiles, rollback depth, prediction stalls and resource growth. Existing synthetic transport tests are not those benchmark comparisons.
7. Finish the networking/GGPO audit, render/test all affected migration menus, rebuild and verify the final archive. Two-PC native fights, physical controllers, rollback determinism and real-network host loss remain user runtime acceptance checks.

No claims of full network-stack correctness, optimal GGPO performance, successful live migration, or native determinism are made by this component work.
