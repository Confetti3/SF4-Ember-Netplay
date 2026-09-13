# Iroh and ImGui migration status

**Migration incomplete.** Ordinary launcher startup now selects the Idle
bootstrap for in-game ImGui Home. Qt remains packaged for the explicit
`--legacy-wizard` comparison flow; full UI parity and recovery are outstanding.
Launcher and overlay now share a versioned settings store with legacy backups
and background saves. Shared Home/Settings/About views now live under `src/ui`;
profile/input delay, room defaults and P1 lobby settings use game-thread commands.
Home Ready drives room-authorized gameplay setup and
the game-side GGPO handoff uses the resulting loopback endpoints. Four native
GGPO participants pass process tests with normal and forced-relay routing on
this PC. Actual SF4 simulation and interactive UI validation remain unverified;
no new release, real fight or two-PC gameplay result is claimed.

## Specification and baseline

The full accepted specification is [IROH_IMGUI_MIGRATION_PLAN.md](IROH_IMGUI_MIGRATION_PLAN.md),
copied verbatim from the user-supplied September 7 plan. Keep its complete M0–M6
scope and all acceptance gates. Do not count a helper connection, a synthetic
rematch loop, or a unit-test pass as a real-fight acceptance result.

- Baseline: `53750e69069aa1a45dc6da09ad8b7fa8000497cb` (`origin/main` at clone).
- Working branch: `feat/iroh-imgui-migration`.
- Local baseline checkout: `../sf4-baseline`, detached at the baseline commit.
- Local dependency checkout: `../vcpkg` (developer tooling, not shipped).
- Rust library and executable: `rust/sf4-net`, Iroh `=1.1.0`, Rust `1.98.0`, checked-in lockfile.
- Game/Sidecar architecture remains x86; CMake builds the helper for x64 Windows.
- Publication, process injection, and game tests have not been performed.
- User confirmed only this PC is available. Two-PC/different-network gameplay
  acceptance remains unverified; local processes and public relay tests do not
  close that gate.

## Implemented components

| Component | Current evidence | Integration still needed |
| --- | --- | --- |
| `src/netplay/SessionController.*` | x86 MSVC unit test: repeated rooms/rematches, cancellation, readiness, stale callbacks, control loss, gameplay loss, Offline | Host/Join/Leave/Offline/Ready effects, copied snapshots and GGPO handoff connected; real game evidence pending |
| `src/netplay/BoundedMailbox.hxx` | Item/byte limits, ordered delivery across threads, close behavior; used by native helper client | Used by the new runtime command queue; full event integration pending |
| `src/ui/ApplicationShell.*`, `PlayerPreferences.hxx` | Shared Home/Settings/About consumes copied views and emits generation-tagged actions; validated profile/input delay/defaults and P1 room settings | Actual rendering/navigation, controls/graphics/diagnostics, updater and shared recovery still pending |
| `rust/sf4-net/src/wire.rs` | Partial reads, bounded network and IPC framing, invalid versions/IDs, raw bytes and stale generation rejection; C++ IPC interoperates | Complete gameplay authorization integration |
| `rust/sf4-net/src/invite.rs` | Capability/room/build/expiry checks, URL allowlist, malformed/oversize tokens, no private IPs in serialized invitation | Clipboard UI; host-owned room policy and expiry UX |
| `rust/sf4-net/src/transport.rs` | Real local/public-relay connections, peer/capability rejection, interrupted-stream poisoning, independent connections | C++ room authority connected; full external network matrix pending |
| `rust/sf4-net/src/control.rs` | Bounded workers; consumer overflow closes control; IPC actor reserves room for lifecycle events | Visible UI failure handling |
| `rust/sf4-net/src/bridge.rs` | Bidirectional UDP bridging, stale/wrong-source rejection, authenticated registration, statistics, survives control close | Runtime GGPO packet sizing and UI |
| `rust/sf4-net/src/service.rs` | Bounded IPC actor, host/join/leave, opaque control payloads, match authorization, stale-generation rejection, cancellation and shutdown | C++ authority, game coordinator and GGPO handoff connected; runtime/error-matrix proof pending |
| `rust/sf4-net/src/ipc.rs`, `src/platform/Helper*` | Hidden x64 child from x86 supervisor, kill-on-close job, inherited bootstrap secret, current-user pipe ACL, nonce and both PID checks, bounded background C++ I/O | Versioned launcher/game bootstrap and missing-helper Offline recovery tested; real game lifecycle pending |
| `src/session/SessionTransport.hxx`, `Legacy*Transport.cxx` | Client/server interfaces with opaque connections, state and send results; GNS API/callbacks isolated in legacy adapters; failures returned after Step | Retire compatibility adapters after full application parity |
| `src/session/IrohRoom.*` | Bounded C++ helper-event router, host-local in-process client, explicit application IDs, stale room protection; actual C++ lobby protocol over normal/forced-relay helper processes | Host/Join/Leave controller and clipboard connected; game-ready/closed/statistics consumers tested; authority-issued capabilities and native GGPO streams now tested; actual game lifecycle/UI validation pending |
| `SessionClientTransportTest` | Handshake/automatic join, exact readiness acknowledgment, callbacks, repeated connections and safe failure reporting | Runtime simulation/hash behavior still needs game evidence |
| `SessionServer` mock test | Host settings, build/name/full/duplicate rejection, spectator restrictions, 50 synthetic rematch resets, leave/rejoin and send failure | Complete lobby/game integration and interactive legacy parity |

The controller's room epoch is process-local callback ownership. Its match epoch
is also local orchestration identity, **not** the shared wire match generation.
The room owner must allocate and distribute `wire::MatchKey` plus fresh gameplay
capabilities before both peers configure their bridges.

Control frames are big-endian `body_length:u32`, `version:u16`, `message_id:u64`,
then the original payload (1–65536 bytes). Admission uses ID 1; application IDs
start at 2, increase monotonically per sender/connection, and fit signed 64-bit
readiness fields. Gameplay envelopes are `version:u16`, `room:[u8;16]`,
`generation:u64`, and unchanged GGPO bytes. Every send checks the current Iroh
datagram ceiling including all 26 header bytes. No fragmentation or reliable
fallback is implemented; an over-limit packet fails the bridge explicitly.

Default endpoint construction disables port mapping. Unit tests use Iroh's
Minimal preset without public relay/address lookup. Separate process tests use
the production endpoint configuration and public infrastructure. Diagnostic
`--relay-only` clears all IP transports; status verifies zero bound IP sockets.
This forces QUIC traffic through the public relay without changing the protocol.

IPC uses the same versioned frame layout with a separate 512 KiB limit to hold
JSON-escaped network control payloads. The network control limit remains 64 KiB.
The helper accepts at most three remote peers and 16 pending tasks. Native IPC
and actor queues are bounded; saturation and process loss are observable failures.
The capability is sent through an inherited bootstrap pipe, never the command
line, environment, or a file. The helper pipe permits the current Windows user
and rejects remote clients; each side verifies the other process ID before use.

The extracted server also fixes forwarded identity spoofing, invalid result
indices, spectator result reports, duplicate registration and stale connection
map entries. The legacy adapter retains GNS delivery IDs and its 512 KiB message
ceiling for compatibility. Iroh uses explicit application IDs and its own bound;
every payload producer must be reconciled with that bound before release. The
Iroh client explicitly rejects the old unreliable JSON GGPO tunnel; gameplay
must use its separately authorized UDP bridge.

## Validation recorded on September 7, 2026

- Baseline source compiled directly with x86 MSVC 19.51 `/O2 /EHsc /std:c++14`:
  `RollbackDiagnostics`, `GgpoGate`, `PacingController`, `StateHash`,
  `SaveStateOwnership` all passed.
- Baseline `GgpoUdpValidation` compiled with the repository's patched
  `adanducci/ggpo@c88b667` vcpkg port and passed: malformed UDP rejected and valid
  traffic accepted. No game process was used.
- New `SessionController` CTest passed using the game-independent CMake build.
- Rust suite: 19 tests passed, including 50 synthetic match generations over the
  same pair of local endpoints. These are **not 50 SF4 rematches**.
- Rust Clippy with `--all-targets -- -D warnings` passed.
- `scripts/test-migration.ps1` passed as a whole: x86 `HelperProcess` and
  `SessionController` CTests, x64 Release helper build, Rust formatting, Clippy
  and 19 Rust tests. Log: `build/helper-migration-checks.log`.
- Native supervisor tests cover hidden startup, authenticated status/shutdown,
  incorrect nonce, wrong client/server PID and child cleanup on job closure.
- Two explicitly invoked production-helper process tests passed: normal endpoint
  configuration and forced public relay, each with three match generations and
  150 exact 1024-byte round trips. Control closure did not stop gameplay UDP.
  Observed round-trip p50/p95/p99 were 765/987/1620 microseconds normally and
  68980/72206/74410 microseconds with relay-only configuration. These are a
  one-PC debug-helper test sample, **not** game latency budgets or direct-path
  proof on different networks. Log: `build/helper-network-tests.log`.
- `SessionServerTransportTest` compiled with x86 MSVC and passed without any
  GNS library. `LegacyServerTransportTest` passed using actual GNS socket pairs:
  two servers, 20 lifecycle cycles, preserved readiness IDs and correct callback
  routing. Logs: `build/session-server-test*.log`, `build/legacy-server-test*.log`.
  These are also registered with CTest in the full product configuration. The
  direct build used installed baseline JSON/GNS dependencies and checksum-verified
  spdlog 1.17.0 headers; the subsequent full build used vcpkg's packaged libraries.
- Full migration branch RelWithDebInfo x86 product build passed, including the
  unchanged Qt launcher flow, Sidecar, RelayHost, SessionInteractiveTest and the
  new x64 Release helper. **All ten CTests passed** (six original regressions
  plus helper, controller, mock server and legacy server tests). Logs:
  `build/product-configure.log`, `build/product-build.log`, `build/product-ctest.log`.
  Local configuration reuses the baseline's installed dependency tree with
  `VCPKG_MANIFEST_INSTALL=OFF`; a build-local `vcpkg_installed` junction also
  supports the existing Qt plugin-copy script's conventional path.
- Full untouched baseline RelWithDebInfo x86 build passed, including Launcher,
  Sidecar, RelayHost and SessionInteractiveTest. All six baseline CTests passed.
  Logs in `../sf4-baseline`: `baseline-configure.log`, `baseline-build.log`,
  `baseline-ctest.log`. Interactive/gameplay baseline observations remain open.
- Helper import audit (`dumpbin /dependents`) found Windows system libraries,
  UCRT and **x64 VCRUNTIME140.dll** in the earlier build. This is superseded by
  the static-CRT production helper audit below. Clean-machine validation is still
  open. Historical log: `build/helper-runtime-dependencies.log`.

### Client and C++ Iroh room integration checkpoint

- Complete RelWithDebInfo product build passed after the client extraction.
  **All twelve CTests passed**, including the six original regressions, helper,
  controller, mock client/server, legacy server routing and legacy IP integration.
  Logs: `build/client-seam-build.log`, `build/client-seam-ctest.log`.
- `SessionClient` no longer exposes GNS handles, address structs or `EResult`.
  Its owner supplies a transport and receives connection failures after `Step`
  returns. Existing snapshot/hash reconciliation and player/spectator behavior
  remain in the C++ client; this extraction does not establish gameplay proof.
- `SessionClientTransportTest` passed 20 connection lifetimes, automatic
  hello/join, exact application readiness acknowledgment and safe loss reporting.
- `IrohRoomIntegrationTest` passed with **both normal and forced-relay** helper
  policies. Each mode uses the real x86 C++ client/server, two supervised x64
  helpers, three room lifetimes and 30 Ready/load/rematch cycles. It does not
  initialize GNS. Malformed invitation rejection, cancel/retry and destruction
  of an old client after a newer room starts are included. Logs:
  `build/iroh-room-integration.log`, `build/iroh-room-relay-integration.log`.
- Rejected invitations now consume an accepted attempt's helper epoch before
  reporting failure. Previously the error used the preceding epoch, so a
  correctly guarded C++ consumer could ignore it and wait forever.
- `LegacySessionIntegrationTest` passed actual loopback-IP connection, hello/join,
  30 rematch cycles and loss reporting across three connections per client.
  GNS listener shutdown can be ungraceful even with connection-first cleanup;
  the test allows its keepalive probes and four unanswered replies to finish.
  Only the test shortens the connected timeout; product timeouts are unchanged.
  Optional `SF4E_TEST_GNS_TRACE=1` enables the diagnostic trace.
- Rust formatting, Clippy with warnings denied and all 19 Rust tests passed
  again after the epoch fix. Log: `build/client-seam-rust-checks.log`.

After building the complete product, invoke the external C++ network tests from
the repository root (or enable `SF4E_NETWORK_TESTS` to register them with CTest):

```powershell
$bin = Join-Path $PWD 'msvc-build/x86-msvc-ninja-relwithdebinfo'
& "$bin/IrohRoomIntegrationTest.exe" "$bin/sf4-net.exe"
& "$bin/IrohRoomIntegrationTest.exe" "$bin/sf4-net.exe" --relay-only
```

These room tests exercise the lobby protocol. The separate gameplay bridge
tests below consume game-ready/closed/statistics events. The authorization checkpoint below adds
authority-issued match capabilities and native GGPO integration. No real fight, two-PC
validation, public matchmaking parity or completed ImGui migration is claimed.

### Runtime bootstrap, Home and C++ gameplay bridge checkpoint

- Full product build and all **13 CTests** passed after runtime bootstrap/Home
  integration. `RuntimeBootstrapTest` covers payload magic/version/length,
  deferred authenticated helper startup, missing-helper Offline recovery,
  rejection of room commands before game readiness and orderly helper exit.
  Logs: `build/runtime-home-build.log`, `build/runtime-home-ctest.log`.
- The launcher starts the owned x64 helper for the suspended game's PID and
  remains its invisible supervisor until game exit. Sidecar validates the exact
  versioned payload and copies the bootstrap under the loader lock; worker
  creation/authentication and shutdown run from normal platform hooks. The
  helper startup window is 60 seconds to accommodate game initialization.
- The runtime owns a bounded command queue, copied UI snapshots and actual
  SessionClient/SessionServer Iroh adapters. Partial Home offers display name,
  Host/Join, invitation copy, members, cancel/leave and Offline. Keyboard/gamepad
  navigation is enabled and Home consumes captured Win32 input. Room opening is
  restricted to the main menu. Rendering, controller capture, focus/device reset
  and input-leak behavior still require interactive game validation.
- At this checkpoint default startup still used Qt and Ready was guarded. The
  following checkpoint connects Ready and GGPO handoff. Default Qt replacement,
  settings, updates, full lobby controls and public discovery remain in scope.
- CMake now applies static CRT flags to the x64 production helper. Its import
  audit contains only Windows system DLLs, with no VCRUNTIME or UCRT DLL import.
  Log: `build/helper-static-imports.log`. This is not clean-machine proof.
- Rust formatting, Clippy with warnings denied and all 19 Rust tests passed
  after the startup-window change. Log: `build/runtime-rust-checks.log`.
- `IrohRoom` now submits explicit per-peer match authorizations and consumes
  generation-guarded ready/closed/statistics events independently of control
  health. Ending a match immediately invalidates the exposed virtual ports;
  acknowledgment closes each mapping before replacement. The authority must
  allocate/distribute the shared generation and capabilities; that connection
  is implemented in the following checkpoint.
- `IrohGameIntegrationTest` passed normal and forced public relay: three fresh
  generations and 30 exact 1024-byte UDP round trips per mode, with statistics,
  invalid/self/duplicate registration checks, stale-generation rejection and
  teardown. The last generation closes control before exchanging UDP and
  continues receiving statistics in the failed guest room state. Logs:
  `build/cpp-game-network.log`, `build/cpp-game-relay-network.log`.
- Local room-enqueue rejection settles cancellation without waiting for an
  epoch the helper never received. Leave retries temporary command backpressure
  on later ticks while still draining events; it never waits on the game thread.
  The full product rebuild, C++ lobby test, forced-relay bridge test and three
  affected CTests passed afterward. Logs: `build/runtime-teardown-build.log`,
  `build/runtime-teardown-room.log`, `build/runtime-teardown-game.log`,
  `build/runtime-teardown-ctest.log`. Queue-saturation fault injection for this
  runtime boundary is still outstanding.
- The actual C++ lobby integration was rerun against the static-CRT helper in
  both modes and passed. Logs: `build/runtime-iroh-room.log`,
  `build/runtime-iroh-room-relay.log`. No SF4 gameplay was exercised.

After the full build, run the additional process tests explicitly:

```powershell
$bin = Join-Path $PWD 'msvc-build/x86-msvc-ninja-relwithdebinfo'
& "$bin/IrohGameIntegrationTest.exe" "$bin/sf4-net.exe"
& "$bin/IrohGameIntegrationTest.exe" "$bin/sf4-net.exe" --relay-only
```

### Authorized GGPO match checkpoint

- `MatchAuthority` runs in the existing C++ SessionServer. It issues monotonically
  increasing match generations, a captured member/role order, exact endpoint
  identities and a fresh random capability for each player/spectator edge.
  P1 owns the two spectator streams. Pair capabilities are sent only to their
  two endpoints, and the helper still contains no SF4 lobby policy.
- The sequence is permission distribution, listener-registration acknowledgment,
  connect instruction, every-participant bridge-ready acknowledgment, then
  start. Old generations, duplicate acknowledgments and connections outside
  the admitted roster cannot advance it. Results/reset requests also carry the
  active generation. Repeated results cannot rotate the roster twice. Roster
  updates precede the room end acknowledgment, and local mapping closure alone
  cannot re-enable Ready. This fixes an observed fast-rematch role race.
- `IrohMatchSession` validates the permission/roster, reserves a fresh loopback
  game port and starts listeners before allowing dialers. It waits for helper
  events without blocking, checks setup deadlines, and exposes only authorized
  endpoints. A pending next grant waits for the previous GGPO socket and helper
  mappings to retire. The room endpoint remains alive across matches.
- Runtime Home now sends character/stage/RNG/Ready through the game-thread queue.
  Match setup waits for authorization before entering the existing Versus
  lifecycle. Players and spectators use a separate Iroh branch at the actual
  GGPO start hook; it has no legacy endpoint fallback. The existing battle-loaded
  synchronization callback is retained. Rematch waits for old mappings to close,
  including an explicit handoff from deferred spectator draining.
- Control loss is reflected in the runtime controller and existing degraded
  verification policy. The host also detects departure from the captured match
  roster. External legacy teardown hooks cannot leave a dangling coordinator
  referencing a destroyed SessionClient. Actual in-game failure timing remains
  part of the open lifecycle matrix.
- The admitted packet ceiling is **1064 bytes**: a conservative full-queue bound
  from the pinned fork's 32-byte packet prefix, 64 pending input frames and two
  8-byte player inputs plus an encoding flag per frame. A compile-time assertion
  binds this calculation to SF4's input structure. Every QUIC path must admit the
  full ceiling. This is source-derived sizing, not an actual-game maximum-size
  measurement or completion of the impairment/backlog gate. The pinned fork also
  retains its existing compressed-bit assertion; extreme backlog behavior needs
  explicit stress coverage.
- Native GGPO runs exposed a local socket-retirement issue. Loopback UDP
  ConnectionReset/ConnectionRefused now count as local delivery drops; other I/O
  errors and QUIC failure still terminate the bridge. The UDP interpretation is
  documented in [Microsoft's WSASendTo reference](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsasendto).
  Normal match-ended notification precedes GGPO socket retirement, since control
  and QUIC close notifications can arrive in either order.
- `IrohAuthorizedMatchTest` uses four supervised helpers, actual SessionServer/
  SessionClient, actual GGPO with SF4-sized input records, and two spectator
  streams. Both spectators verify every received byte against the patterned
  player input records after the configured input delay. Three successive
  generations rotate the losing player and reassign
  the spectator mappings. Normal and forced public relay runs pass. The test uses
  a synthetic save/load callback; it does **not** execute SF4 battle simulation.
  Logs: `build/authorized-match-network.log`, `build/authorized-match-relay.log`.
- The full product build and all 13 CTests passed, including new mock authority
  checks for capability isolation, non-member votes, duplicates, stale votes,
  results permissions, loser rotation and cancellation. Logs:
  `build/match-final-build.log`, `build/match-final-ctest.log`. After the roster
  ordering fix, the full rebuild, both native network modes and four affected
  CTests passed: `build/match-roster-build.log`, `build/match-roster-ctest.log`.
  The explicit update-before-end regression assertion and departed-recipient
  handling also passed after the final full build: `build/match-order-build.log`,
  `build/match-order-ctest.log`.
- Rust formatting, Clippy with warnings denied and all 19 Rust tests passed after
  the local UDP error handling change. Log: `build/match-rust-checks.log`.

After the full build, run the native authorization/GGPO tests explicitly (or
register both using `SF4E_NETWORK_TESTS`):

```powershell
$bin = Join-Path $PWD 'msvc-build/x86-msvc-ninja-relwithdebinfo'
& "$bin/IrohAuthorizedMatchTest.exe" "$bin/sf4-net.exe"
& "$bin/IrohAuthorizedMatchTest.exe" "$bin/sf4-net.exe" --relay-only
```

This does not close the M3 real-fight gate. Late spectator admission, delayed
spectator teardown, actual GGPO rollback/state behavior, and failure timing in
SF4 still need runtime coverage. Default startup, complete settings/updater
services, discovery, UI parity and the remaining M0-M6 acceptance rows remain.

Run the migration component checks from PowerShell with MSVC C++ Build Tools,
CMake/Ninja, and rustup installed:

```powershell
./scripts/test-migration.ps1
```

It selects an x86 MSVC developer environment and the pinned Rust toolchain,
builds the helper, checks formatting/lints, then runs C++ component and Rust tests. It does
not install/inject the game or validate the complete package.

The two process network tests require public network access and are intentionally
excluded from the default offline-capable test suite. Run them explicitly from
`rust/sf4-net`:

```powershell
cargo test --locked --test helper_network -- --ignored --nocapture
```

## Shared settings and Home startup checkpoint

`src/netplay/SettingsStore.*` owns `%APPDATA%/sf4e/settings.json` with schema
version 1, separate `profile`, `netplay`, `overlay`, and temporary
`legacyLauncher` sections. The existing launcher and overlay serializers use
this store, so launcher writes preserve overlay selections and overlay writes
preserve launcher/profile preferences. Profile/input delay and room defaults
now have shared ImGui widgets and game-thread application; controls/graphics
and supported diagnostic settings still need their unified presentation.

On first access, both `config.json` and `overlay_prefs.json` are read before
publication. Each existing source receives a byte-identical `.pre-v1.bak`
backup; neither source is overwritten or deleted. A retry accepts an existing
backup only when it matches the source exactly. Stored legacy room codes,
host secrets and relay session ports do not enter active settings. The original
files/backups remain local recovery data and must not enter diagnostics exports.

The store limits files to 256 KiB and JSON nesting to 32 levels. Invalid JSON,
unsupported schema versions, inaccessible files and conflicting backups stop
the write rather than replacing the file with defaults. A cross-process file
handle lock covers read/merge/replace and fails promptly when busy. Saves flush
a fresh same-directory temporary file before replacement; failed replacement
keeps the previous complete document. Power-loss recovery and clean-machine
filesystem behavior are still part of the final packaging matrix.

`SettingsWriter` holds one pending snapshot per consumer (overlay and
profile/launcher), for two bounded pending slots. New UI edits replace older
pending edits, and disk writes/retries run on its worker. Errors appear in Home;
explicit Main teardown attempts the final save and logs a failed flush. The
worker survives D3D recreation, and cached selections prevent a device reset
from loading older disk contents over pending edits. Actual game reset/thread
ordering remains unverified.

Default Launcher startup now copies persisted choices into an Idle bootstrap;
`--legacy-wizard`, legacy host/join CLI and controller IPC remain explicit
comparison paths. Home drawing/input capture waits for the game main menu so
the title/boot screens can receive input. Missing-game/failed-launch recovery
still uses the old messages; shared ImGui recovery, updater and full settings
presentation are not complete.

Full product build and **14 CTests passed** after these changes:
`build/settings-final-build.log` and `build/settings-final-ctest.log`.
`SettingsStoreTest` uses fresh temporary directories and checks both backups,
selection preservation, cross-consumer updates, stale legacy files, lock
contention, failed replacement, corrupt/oversized/deep/newer-version data,
interrupted migration retry, and 100 coalesced edits with final worker flush.
The tests did not migrate the user's actual AppData files.

Read-only Steam inventory found app 45760 (build 834219) in
`G:/SteamLibrary/steamapps/common/Super Street Fighter IV - Arcade Edition`.
No game process was launched or injected for this checkpoint.

## Shared shell and runtime preference commands

`ApplicationShell` has no game, filesystem, transport or bootstrap dependency.
Its copied view includes the current room, roles, preferences and save status;
its actions carry the originating room/match generation. The overlay adapter
supplies the existing character/stage selection controls and translates actions
to the bounded runtime queue. The shell includes explicit clipboard paste with
size rejection, Home/lobby actions, Settings and basic version/build About.

`SavePreferences` is accepted only while idle at the main menu. A queued save
updates the owner-thread profile, input delay and hosted-room defaults, which
are used when constructing the next room/client. It does not change the immutable
injected bootstrap. The shared writer persists profile and overlay changes
without one consumer replacing the other's section.

`SetLobbySettings` is accepted only for P1 before readiness, with supported
round/time values. The server checks original JSON types/ranges before narrowing
into game fields and independently rejects P2/spectator edits, invalid values,
fractional timers, legacy training mode and changes after either player
is ready or authorized setup has begun. Ready stays disabled while a submitted
change waits for the authoritative lobby broadcast. Accepted room settings also
become defaults for later hosted rooms.
Character/stage controls are disabled once local Ready is pending or accepted;
the renderer cannot change its draft while displaying an already-submitted pick.

Post-match edits use the same explicit GGPO retirement boundary as Ready and
wait for old helper mappings/room end acknowledgment before sending new settings.
Stale command generations are checked before deferred retirement side effects.
These hooks still need real-game lifecycle proof; component tests do not prove
safe delayed spectator draining in SF4.

The full build and 14 CTests passed in `build/shell-final-build.log` and
`build/shell-final-ctest.log`. After the final post-match/generation adjustments,
the full build and five affected tests passed in `build/shell-lifecycle-build.log`
and `build/shell-lifecycle-ctest.log`. Added cases cover invalid preference ranges,
idle/setup/readiness command gates, before-game runtime guards, interleaved
profile/overlay saves, invalid/P2 room-setting requests and immutability after
readiness. Rendering, keyboard/mouse/controller input and actual game behavior
remain unverified.

The subsequent JSON-narrowing fix passed the full build and both server tests
(`build/shell-settings-validation-build.log`,
`build/shell-settings-validation-ctest.log`). The final selection-lock view
change passed the full build plus RuntimeBootstrap/SessionController checks
(`build/shell-selection-build.log`, `build/shell-selection-ctest.log`); this is
compile/component evidence, not an interactive input result.

## Startup crash correction and installed-game check (2026-09-07)

The first test package crashed approximately two seconds after injection.
An owned-process debugger captured an access violation at `SSFIV.exe+0x29a01`;
the packaged PDB traced its caller to `NetplayFacade::TickRuntime` publishing
the Home snapshot. Platform readiness was incorrectly treated as proof that
the game's root-event accessor was safe. That accessor dereferences native
state before returning, so checking its result for null could not protect it.

The runtime now also waits for the existing main-menu observer callback before
querying native event state. A regression test first failed on the early call,
then passed after the guard; it also verifies that the menu notification enables
the query and a null root still reports no menu. Full product rebuild and all
14 CTests passed (`build/startup-fix-build.log`, `build/startup-fixed-ctest.log`).

The corrected package reached the actual installed game's main menu with the
ImGui Home visible. The debugger launch reported helper error 50; launching
the same package normally started the helper and displayed Network ready.
The normal instance was left open for the user. This establishes startup,
Home rendering and helper readiness on this PC, not room admission, input
isolation, persistence, offline gameplay or real online fight acceptance.
The initial failing package is retained separately from the corrected snapshot.

## Remaining acceptance ledger

All rows below remain open unless a complete result is explicitly recorded.
The full plan is authoritative for the individual cases within each row.

| Plan gate | Evidence required to close it | Current status |
| --- | --- | --- |
| M0 baseline package | Full baseline build, named tests, SessionInteractiveTest, real private-room fight/rematch/leave-rejoin/spectators/offline/settings behavior, actual GGPO packet sizes and path latency | Full baseline build and six CTests passed; interactive/runtime measurements outstanding |
| M1 transport seam | Remove GNS handles/results/delivery IDs from session interfaces; keep legacy adapter behavior; mock-transport lobby coverage | Both interfaces extracted; mock client/server tests and real C++ Iroh lobby exchange pass; full runtime parity pending |
| M1 app services | Extract settings, preflight, update services from launcher; separate immutable bootstrap from mutable session config | Shared store/writer, typed preference and lobby-setting commands implemented/tested; preflight/update services pending |
| M2 packaged helper | Hidden supervised executable, current-user named pipe, nonce/peer-process authentication, exact loopback registration, bounded IPC, orderly exit/crash events | Launcher/bootstrap, supervisor and deferred game worker tested; actual injected-game lifecycle pending |
| M2 actual paths | Two helpers on different networks, direct and forced relay, no port mapping, no Rust required on player PC, measured latency/queues/MTU | Two processes on this PC passed normal/forced public relay; different networks and clean player machine unverified |
| M3 private-room slice | C++ host owns lobby rules; admission roles/hash/version/full/expiry rejection; raw GGPO bridge; persistent room with fresh match mappings | C++ authority, fresh mappings, runtime GGPO handoff and four-participant native GGPO tests implemented; SF4 gameplay proof pending |
| M3 real match gate | Two SF4 clients fight/rematch/leave-rejoin with VPS unreachable, both direct and forced relay, no one-sided fallback | Not run |
| M4 normal ImGui flow | Idle Home, runtime Host/Join/cancel/retry/lobby/match/rematch/Offline/settings/diagnostics, minimal bootstrap recovery | Default Home, shared shell, profile/input delay/defaults and lobby settings connected; interactive proof, remaining settings/diagnostics/updater and shared recovery pending |
| M4 persistence/updater | Atomic versioned migration of config and overlay prefs with backups; same update source; install only with binaries unloaded | Shared settings migration, backup/atomic-failure tests and background writes pass; actual game persistence/reset and updater handoff pending |
| M4 input/rendering | Keyboard/mouse/controller, focus/Alt-Tab/text capture, DPI/ultrawide, D3D9 device reset, no input leakage or UI pause of deterministic simulation | Not run |
| M5 parity | Spectator flow/admission/bandwidth isolation, advanced views, Open rooms/Find match with verified bootstrap/discovery service | Two native spectator streams and loser rotation pass; late admission, bandwidth isolation, advanced views and discovery pending |
| M6 lifecycle/network matrix | Every network and lifecycle case in plan §9, including healthy-gameplay/control loss, relay transitions and total failure | Local control/gameplay separation tested; external matrix outstanding |
| M6 performance | Matched RTT/loss/jitter/reordering grid; GGPO stalls/rollback separate from helper stats; p50/p95/p99 bridge delay, CPU/memory/frame cost; budgets from baseline | Not measured |
| M6 robustness | Fuzz invitations/control/game packets; stale identity/role/size tests; IPC impersonation; bounded pending handshakes; secret-redacted diagnostics/crashes | Parser, bounded admission, IPC authentication and lobby adversarial tests passed; fuzzing/full crash audit pending |
| M6 packaging | Cargo/CMake CI, helper runtime DLL audit, coherent version/hash/signing/manifests/updater, notices, clean-machine x64-Windows errors | Static-CRT helper import audit passed; CI, final package/versioning/notices and clean-machine validation pending |
| M6 legacy retirement | Audit all Qt/GNS/broker/UPnP/RelayHost consumers, remove from release, archive obsolete VPS docs, replace user instructions | Not started |
| Invariants | Preserve all `NETPLAY_INVARIANTS.md` behavior, game-main-thread ownership, undetoured rollback, nonblocking idle, bounded pacing, diagnostics and spectator-safe mismatch behavior | Deterministic simulation and rollback algorithms unchanged; runtime lifecycle hooks added and still need game regression proof |

## Next implementation work

1. Retain the passing baseline and migration builds for interactive/runtime
   comparison where one-PC testing is meaningful.
2. Validate the connected bootstrap and Home in the actual game when available;
   complete input capture, focus, device reset and launch-error recovery checks.
3. Exercise the connected authorization/GGPO lifecycle in SF4. Add delayed
   teardown, late spectator admission and failure-matrix coverage; prove actual
   input/rollback behavior and packet maxima rather than relying on synthetic
   process tests.
4. Finish shared settings widgets, preflight/update services and ImGui parity;
   verify default Home startup, persistence/reset behavior and bootstrap recovery.
5. Work through M3–M6 until the full ledger is proved. Do not mark the migration
   complete from component tests alone.
