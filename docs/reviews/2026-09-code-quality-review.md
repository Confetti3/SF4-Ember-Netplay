# Code quality review, September 2026

Base: `experiment/degraded-connection-recovery` at `c9a9ed5`. Work branch: `review/code-quality`.
Scope: `src/`, `rust/sf4-net`, `CMakeLists.txt`, `cmake/`, `scripts/`. Excluded: `src/Dimps`
(layout follows the game binary), `src/ui/backends/imgui_impl_win32.cpp` (vendor), `vcpkg-ports`.

The rule for the cleanup that follows: bodies move verbatim, wire formats and user-visible strings
do not change, and anything that changes behaviour is listed under "Actual issues" and fixed in its
own commit.

## Baseline (before any edit)

| Check | Result |
|---|---|
| `ctest --test-dir build/current` | 49 of 49 pass, 64 s |
| `RoomHostBench` | step p50 6.25 ms, propose p50 5.36 ms, apply_commit p50 1.47 ms, decode p50 10.88 ms, import p50 1.13 ms |
| `RecoveryBenchmark --rift --continuous` | 3 runs, exit 0, outputs kept for comparison |
| `cargo fmt --check` | clean |

## What is good and should stay that way

- `src/common` and `src/netplay` are true leaves with no upward includes and no logging.
- `MenuNavigation` is a renderer-free model and nearly every screen goes through
  `MenuEntry` rows plus `GameMenu::Draw`. Controller input has one path.
- `SessionServer` and `SessionClient` share almost no code (one 15-line parse preamble).
  `IrohRoom` and `RoomModel` share none. The problem there is size, not copy-paste.
- No TODO debt, no commented-out code. Long "why" comments are the strongest convention in the
  repo and must travel with the code they explain.
- Rust: one `Arc<Mutex>` in the whole crate, 8 non-test `unwrap`/`expect`/`unreachable!`, no FFI
  (the helper is a separate x64 process behind a length-prefixed JSON pipe).

## Findings, ranked

### 1. Structural: giant dispatch functions

These functions are each a message or event dispatcher whose branches were written inline. Every
branch is independent (no fallthrough), so each one can become a named handler with its body moved
unchanged. This is the largest single improvement available and it needs no redesign.

| Function | Lines | Shape | Remedy | Stage |
|---|---|---|---|---|
| `Actor::completed` `service.rs:3466` | 874 | `match` over 16 `Completion` variants | one method per variant, in the module that owns that state | 3 |
| `TickRuntime` `sf4e__Application.cxx:948` | 853 | 8 sequential phases | one static function per phase, same order | 5 |
| `SessionServer::Step` `sf4e__SessionServer.cxx:1397` | 814 | poll, departures, then a 15-branch `else if` on message type | `Handle<Type>` methods; `Step` keeps poll, parse, guards | 4 |
| `SessionClient::Step` `sf4e__SessionClient.cxx:565` | 505 | 11-branch chain | same | 4 |
| `GameMenu::Draw` `GameMenu.cxx:110` | 347 | list, detail, legend, 3 modals, flyout; 12 parameters | split by part; options struct | 6 |
| `ApplicationShell::Draw` `ApplicationShell.cxx:61` | 320 | 13-screen string chain plus 20-id action chain | per-screen row builders, one dispatcher | 6 |
| `RoomAuthority::Apply` `RoomModel.cxx:964` | 308 | 20-branch `if` chain on `ActionKind`, no exhaustiveness check | `switch` plus `Apply<Action>` methods | 4 |
| `Actor::command` `service.rs:3172` | 293 | `match` over 17 `Command` variants | method per variant where the arm is long | 3 |
| `Publish` `sf4e__Application.cxx:331` | 285 | fills `RuntimeSnapshot` section by section | per-section fillers | 5 |
| `Actor::graceful_leave` `service.rs:2467` | 260 | 4 copies of a poll-until loop | `poll_until` helper | 3 |
| `IrohRoom::Poll` `IrohRoom.cxx:960` | 209 | chain on event `type` string | per-event methods | 4 |
| `fSystem::BattleUpdate` `Battle__System.cxx:775` | 201 | hot path | leave the body alone; only the file is split | 5 |

`SessionServer::Step` handlers share a few locals (`conn`, `msg`, `incoming`, `cid`,
`bSendLobbyAllReady`, `bSendBattleSynced`, `deferredRoomEvents`) and leave with `continue`. They
move behind a small per-message context struct and `continue` becomes `return`.

### 2. Structural: files that hold several subsystems

| File | Lines | Split along | Stage |
|---|---|---|---|
| `service.rs` | 7788 | `protocol`, `checkpoint`, `probe`, `membership`, `control`, `entry`, `tests` modules; all `impl Actor` blocks so visibility does not change | 3 |
| `sf4e__Game__Battle__System.cxx` | 2545 | `SaveState` (670 lines), GGPO lifecycle and callbacks, state hashing, `RollbackStress` harness (225 lines, env-gated) | 5 |
| `sf4e__SessionServer.cxx` | 2373 | recovery checkpoint and quorum; custom-room tables | 4 |
| `sf4e__Application.cxx` | 1803 | the name matches nothing inside; it is the second half of `NetplayFacade`. Rename to `sf4e__NetplayRuntime.cxx` | 5 |
| `sf4e__DeveloperOverlay.cxx` | 1682 | one file per inspector window; off by default, so verify with `-DSF4E_DEVELOPER_UI=ON` | 5 |
| `RoomModel.cxx` | 1446 | bounded JSON readers and `to_json`/`from_json` (230 lines) out of the room rules | 4 |
| `IrohRoom.cxx` | 1314 | `ServerAdapter`/`ClientAdapter`; checkpoint pipeline | 4 |
| `github_release_client.cxx` | 1154 | validation, hashing, download | 6 |
| `SessionRecovery.hxx` | 456 | nearly all inline bodies, including a portable SHA-256; move to a `.cxx` | 4 |

### 3. Duplication worth removing

- Rust checkpoint header `{epoch, room, transfer, term, base_revision, revision}` spelled out 11
  times in `service.rs`. It is why three functions need `#[allow(clippy::too_many_arguments)]`. An
  internal struct fixes the signatures. The wire variants stay flat because `serde(flatten)` does
  not work with `deny_unknown_fields`.
- `timeout(.., loop { ..; sleep(25ms) })` copied 4 times (`service.rs:1682, 2501, 2535, 2631`); the
  `25 ms` literal 8 times and `5 s` 6 times without a name.
- Constants defined twice with the same value: `MAX_CHECKPOINT` (`coordination.rs:35`) and
  `MAX_CHECKPOINT_BYTES` (`recovery.rs:23`); `MAX_CHECKPOINT * 6 + 65536` (`coordination.rs:41`,
  `coordination_iroh.rs:26`); credit window 4 (`recovery.rs:25`, `coordination.rs:43`).
- `GameMenu.cxx`: three near-identical modal scaffolds (`:366`, `:395`, `:422`) plus a fourth as a
  flyout; text elision written four times (`GameMenu.cxx:20`, `:34`, `RoomPanel.cxx:314`,
  `MenuGlyphs.hxx:43`).
- 45 test files each define their own `CHECK` in 8 variants; headless ImGui setup 5 times; one-frame
  driver 5 times; unique temp dir 4 times. `session_client_mock.hxx:6` has a comment working around it.
- CMake compiles `MatchAuthority.cxx`, `sf4e__SessionServer.cxx`, `sf4e__SessionProtocol.cxx` three
  times (`Session`, `SessionServerTransportTest`, `RoomHostBench`). `sf4e_fonts.cmake` and
  `sf4e_brand.cmake` share a copy-pasted hex embed.
- Scripts: the target preamble is repeated in 5 scripts, the `vcvarsall` import in 2, and
  `$workspace` is assigned and unused in 3.

### 4. Boundary and layering problems

- `src/common/MenuInputCapture.hxx:9-21` reads and writes live game memory with raw offsets from
  the leaf value-type library. Belongs in `src/sf4e`.
- `sf4e__NetUtil` (WinHTTP client, 433 lines) is compiled into `sf4e_common` and so links
  `winhttp` into the injected DLL. Its only caller is the launcher's update client.
- `sf4e__NetplayFacade.hxx:2` includes `ui/ControllerNavigation.hxx`, so the session layer sees UI
  types. `platform/ApplicationServices.hxx:2` includes a launcher header.
- `session` and `sf4e` include each other; `training` is compiled into the `sf4e` target to hide a
  second cycle. Recorded, not fixed here (needs an interface, which is redesign).
- `SessionClient` exposes `_lobbyData`, `_matchData`, `_cid`, `_ggpoPort` as public fields that
  `sf4e__Application.cxx` reads directly.
- Unnamespaced macros in public headers: `NUM_SAVE_STATES`, `MAX_SF4E_PROTOCOL_USERS`.
- Two readers for env switches: `sf4e::EnvFlag` and raw `getenv`.

### 5. Dead code and leftovers (each re-verified by grep before removal)

- `src/common/agent_debug_log.hxx`: a debugging logger with a hardcoded session id `592d59` that
  writes `debug-592d59.log` to `%APPDATA%\sf4e` and the working directory when `SF4E_AGENT_DEBUG`
  is set. Three calls in `sf4e__UserApp.cxx`; `sf4e__NetplayFacade.cxx:15` includes it and never
  calls it.
- `transport::bind_endpoint` (`transport.rs:48`): no references. `accept_game` and
  `accept_game_stream` are only used by tests. Two `#[cfg(test)]` methods sit in the middle of
  `impl Actor` (`service.rs:815`, `:864`).
- Zero-caller candidates: `SessionServer::AddConnection`, `SessionServer::RebindMember`,
  `SessionClient::Forward`, `SessionClient::SelectRoomTable`, `RoomAuthority::SetLocalMember`,
  `RoomAuthority::SetRoomEpoch`.
- Stale comments about a `GgpoRelay` class that no longer exists (`Battle__System.cxx:1392`) and a
  VPS relay deployment (`sf4e__SessionProtocol.hxx:248`).
- `static sf4e::RollbackHud rollbackHud;` declared inside the include block (`Battle__System.cxx:31`).
- `src/tests/ui-polish/` is built by nothing; only `docs/validation/UI_POLISH_VALIDATION.md` mentions it.
- Two claims from the first pass were wrong and are withdrawn: `scripts/upgrade/Install-Upgrade.ps1`
  is used by `package-upgrade.ps1` and `test-upgrade-recovery.ps1`; `MT_PUNCH_GO` still has a
  message struct.

### 6. Legibility

- Forty colour literals outside `Theme`, several repeated byte for byte (`ImVec4(.5f,.25f,.1f,1)`
  four times). Only identical values get a name; near-duplicates would change pixels and are left.
- Mixed tabs and spaces inside single files (`sf4e__Application.cxx`, `IrohRoom.cxx`,
  `RoomModel.cxx`). Not reformatted: a mass reformat would bury the moves and conflict with the
  unmerged experiment.
- `run-package-tests.ps1` defaults to `msvc-build/display` and `recovery-benchmark.ps1` hardcodes
  build paths, both disagreeing with `build-target.json`.

## Actual issues (behaviour changes, fixed separately)

| # | Issue | Evidence | Fix |
|---|---|---|---|
| A1 | The "connection check unavailable" error never clears for pt-BR and es-419 users | `sf4e__Application.cxx:1322` sets `loc::T("runtime.connection_check_unavailable")`; `:1323` clears it only if the text equals the English sentence | compare against the same `loc::T(...)` value |
| A2 | Room rejection messages are English only | 21 sentences in `RoomRejectText`, `sf4e__SessionClient.cxx:43-68`, reach the status line unchanged; every other UI string uses `loc::T` | add `room.reject.*` keys to the three catalogs. Needs translations, so this waits for a decision |
| A3 | Same for `"Could not open the room. Try again."` | `sf4e__Application.cxx:1299` | new key, same decision as A2 |
| A4 | `sf4e__Game__Battle__Vfx.hxx` has no `#pragma once` | every other header has one | add it |
| A5 | Internal tokens can reach the status line | `_roomError = "invalid_room_snapshot"` and two siblings, `sf4e__SessionClient.cxx:660, 681, 742` | confirm where `_roomError` is shown, then decide |

Checked and dismissed:
- `RoomModel` readers throw `std::invalid_argument`, but every room parse site in `SessionServer`
  (`:1532, :1557, :1588`) and `SessionClient` (`:660, :681, :742`) catches `std::exception`.
- `service.rs:4136` `get_mut(&peer).unwrap()` follows a validity check with an `.await` between, but
  the actor holds `&mut self` across it, so nothing else can remove the slot. `:4093`
  `self.room.unwrap()` is guarded at `:4057`. Both can become `let Some(..) else` for clarity.
- Raw string matches for `game_prepared`/`game_ready` next to unused enum values is drift, not a
  bug. Wire names are frozen, so it stays.

## Future work (not part of this cleanup)

- `struct Runtime` (`sf4e__Application.cxx:68`) carries about 25 bools, 12 deadlines and 5 pending
  slots next to `netplay::SessionController`, which was meant to own that state.
  `IrohMatchSession` has 11 flags beside its `Phase` enum. Both are implicit state machines.
- Screen ids are strings compared by prefix in about 20 places.
- Four error idioms coexist (exceptions, bool, `int` -1, enum results, string codes).
- `sf4e::Game::Battle::System` is a class of 17 static members with no instances.
- The `MT_LOBBY_*`/`MT_PUNCH_*` protocol is partly vestigial; `Lobby_ReportResults` is only called
  by tests. Removing it changes the wire vocabulary, so it needs a version plan.
- No CI builds the product; `CMakePresets.json` is unused by every script.
- `transport.rs` has 16 connect/accept entry points that differ by one injected argument.

## Order of work

Stage 2 dead code, 3 Rust `service.rs`, 4 session layer, 5 `src/sf4e`, 6 UI and launcher, 7 tests,
CMake and scripts. Each stage ends with the full build, ctest, the Rust gate where relevant, and
the network fixtures for stages 3 to 5. A real two-PC match is still owed after stages 4 and 5.

## Outcome (added after the cleanup)

All work is on `review/code-quality`, one commit per step, nothing pushed.

### Done

| Area | Before | After |
|---|---|---|
| `rust/sf4-net/src/service.rs` | 7788 lines, `impl Actor` 3783, `completed` 874 | `service/` with `mod.rs` 1070, `checkpoints` 559, `controls` 645, `members` 803, `probes` 573, `refresh` 376, `games` 321, `protocol` 342, `entry` 140, `tests` 2894. `completed` is a flat dispatcher over 12 `completed_*` methods |
| `transport.rs`, `coordination.rs` | 1183 and 1965 lines with inline tests | 636 and 1314 lines; tests in `transport/tests.rs`, `coordination/tests.rs` |
| `sf4e__SessionServer.cxx` | 2373 lines, `Step` 814 | main 544, `__Recovery` 958, `__Handlers` 661, `__Rooms` 330. `Step` dispatches to 15 `Handle*` methods |
| `sf4e__SessionClient.cxx` | 1229 lines, `Step` 505 | main 815, `__Room` 467. `Step` dispatches to 9 `Handle*` methods |
| `RoomModel.cxx` | 1446 lines, `Apply` 308 | `RoomModel` 667, `RoomModelActions` 352, `RoomModelJson` 377, shared `RoomModelDetail.hxx`. `Apply` keeps the shared checks and calls 17 `Apply<Action>` rules in the original order |
| `IrohRoom.cxx` | 1314 lines | 862, plus `IrohRoomCheckpoint` 327 and `IrohRoomAdapters` 159 |
| `sf4e__Game__Battle__System.cxx` | 2545 lines | 1512, plus `__SaveState` 704, `__RollbackStress` 250 and one internal header for what they share |
| `sf4e__Application.cxx` | 1803 lines, `TickRuntime` 853, `Publish` 285 | renamed `sf4e__NetplayRuntime.cxx`. `TickRuntime` is 52 lines calling 16 phase functions in the original order. `Publish` is 81 lines plus four section helpers |
| Dead code | | agent debug logger, six zero-caller methods, `bind_endpoint`, two stale comments removed; test-only transport entry points are `#[cfg(test)]` |
| Tests | 45 private `CHECK` macros | 23 equivalent ones replaced by `src/tests/test_support.hxx` |
| Build | session server sources listed three times | one CMake list; `run-package-tests.ps1` points at `build/current` |

Actual issues fixed (each its own commit): A1 the connection check error that never cleared outside
English; A2 and A3 room rejections and 24 runtime status sentences now go through the catalogs with
pt-BR and es-419 text (43 new keys, drafts for native review like the rest of those catalogs); A4
the missing include guard. A5 turned out to be harmless: those tokens pass through `loc::T`
unchanged and are only ever shown for malformed server data.

### Findings withdrawn after checking the code

- `sf4e__NetUtil` and `winhttp` in the game DLL: the update client is compiled into
  `sf4e_app_services`, which the game uses for in-app updates. The dependency is real.
- `MenuInputCapture.hxx` in `src/common`: it is header-only there so `controller_navigation_test`
  can check the cache offsets against a fake buffer without game headers.
- `getenv` versus `sf4e::EnvFlag`: not the same semantics (CRT snapshot versus live environment),
  so they were not merged.
- `src/tests/ui-polish`: not dead. It is a passing 357-check fixture for `MenuNavigation` and
  `MenuFeedback` that is simply not registered in the main build. It was not deleted; registering
  it in the core-tests block is a three-line change that needs a decision. (Registered in the second pass.)

### Considered and rejected

- A shared `poll_until` helper for the leave path: the four loops break with different values and
  mutate captured state, so the helper read worse than the loops. The 5 s and 25 ms literals are
  named instead.
- A `CheckpointHeader` struct in Rust: the wire variants must stay flat, so the struct only moved
  the verbosity to every call site.
- Deduplicating the legacy lobby side lookup (seven copies): that path is only reachable from
  tests and is a candidate for removal, not polish.
- `SessionRecovery.hxx` to a `.cxx`: it is header-only so the dependency-free core tests can build
  it without a library.

### Not done, and why

All of these except `DrawRoomBoard` were done later; see "Second pass" below.

- `sf4e__DeveloperOverlay.cxx` (1682 lines): it only compiles with `-DSF4E_DEVELOPER_UI=ON`, so a
  split needs a second full build tree to verify.
- `GameMenu::Draw`, `ApplicationShell::Draw`, `DrawRoomBoard`: dense code with many shared locals
  and no cheap pixel oracle (a full `UiRenderTest` capture is about 45 GB). Only the byte-identical
  dialog colours were named.
- `github_release_client.cxx`: the update path is security sensitive and has no end-to-end test.
- `IrohRoom::Poll` (about 200 lines) and the GGPO lifecycle section of the battle system (about
  600 lines, coupled through 15 file-scope items) are left in place.

### Verification

- Rust: `cargo fmt --check`, `cargo clippy -D warnings`, 77 tests, same count as the baseline.
- C++: 49 of 49 ctest cases after every step, and the full `scripts/build-current.ps1` pipeline at
  the end.
- Network fixtures after stages 3 and 4 and at the end: `CustomRoomGameTest --late-spectator` and
  `IrohRecoveryIntegrationTest --scenario=preparing`, both exit 0.
- `RoomHostBench` after stage 4: step p50 6.39 to 6.53 ms against 6.25 ms before. The decode
  worker, which was not touched, moved by the same 2 to 4 percent, so this is machine noise.
- Not verified: a real match. The fixtures never call `TickRuntime` or the battle system, so the
  runtime and save-state moves are covered by compilation, `RuntimeBootstrapTest` and
  `SaveStateOwnershipTest` only. A two-PC match, direct and relay, is still owed before this branch
  or the experiment under it is promoted.

## Second pass (added after the first outcome)

The items left open above were picked up on the same branch, again one commit per step and nothing
pushed. Bodies were moved by script and each move was checked with
`git diff --color-moved --color-moved-ws=ignore-all-space`, so the only lines git did not classify
as moved are the ones listed here as deliberate.

### Done

| Area | Before | After |
|---|---|---|
| `IrohRoom::Poll` | 209 lines | 89 lines. `HandleConnected`, `HandleControlTraffic` and `HandleHelperError` return false where the branch used `return` and true where it used `continue`. Branches of ten lines or fewer stay inline |
| `sf4e__Game__Battle__System.cxx` | 1512 lines | 774, plus `__Ggpo` 740: session start and retire, abort latch, the seven callbacks, spectator policy, disconnect countdown and pacing. Four functions lost `static` and are declared in the internal header. No new extern variables |
| `github_release_client.cxx` | 1154 lines | 625, plus `github_release_validation` 274 and `github_release_download` 231 behind `github_release_client_internal.hxx`. Eleven helpers cross the files, the rest stay `static`. No logic edits |
| `GameMenu::Draw` | 347 lines | 217. The three modals, the flyout confirmation and the Home status line are private members that write `action` by reference |
| `ApplicationShell::Draw` | 320 lines | 98. `UpdateRoomTransitions`, `UpdateRoomFeedback`, `UpdatePreferenceSave`, `BuildRows`, `UpdateStatus`, `PublishPlayerCard`, `HandleActivate`, `HandleAdjust`, called in the original order |
| `sf4e__DeveloperOverlay.cxx` | 1681 lines | main 306 (menu inspectors and the page selector), `__Battle` 575, `__System` 537, shared `__Internal.hxx`. Three files rather than fifteen: the main menu inspector shares statics with a game callback and with the selector, so it stays with them |
| Dead code | | `PathExistsUtf8` and `kAllowedPackagePaths` in the release client; `DrawHelpWindow`, `DrawGGPOStatsOverlay`, `DrawHashOverlay`, `DrawCharaEditionDropdown`, `GetEditionLabel`, `compareTasks`, `lobbyConditions`, `clientAlerts` and an unused `WndProc` declaration in the developer overlay. Each had no reference outside its own definition |
| Tests | | `src/tests/ui-polish` is registered as `UiPolish` (50 ctest cases now). `imgui_test_support.hxx` replaces four identical headless setups, `temp_root.hxx` replaces four temp directory recipes and gives all four the delete guard only two had, and ten more tests use the shared `CHECK` |
| Build and scripts | | one `sf4e_hex_bytes` CMake function for the font and brand embeds (generated headers byte-identical); `Enter-EmberVcEnvironment` replaces three copies of the `vcvarsall` import; `recovery-benchmark.ps1` and `run-package-tests.ps1` take the build directory from `build-target.json` |
| Rust | | the two guarded `unwrap` calls are `let ... else` |

### Found to need no work

- `#[cfg(test)]` items inside `impl Actor`: already confined to `service/tests.rs` after the first pass.
- `rollbackHud` inside the include block: the file has one include now, and the static moved with
  the GGPO section, its only user.

### Considered and rejected

- `DrawRoomBoard` (165 lines): its parts are already eight named lambdas that share one mutable
  tooltip accumulator. Making them members needs a context struct and buys nothing.
- An options struct for the twelve `GameMenu::Draw` parameters: it changes every call site and is
  not a verbatim move.
- One modal scaffold for the three `GameMenu` dialogs: they differ in button count and sizing, in
  the feedback child and in the disabled handling, so a shared scaffold would be mostly parameters.
- One text elision helper: `FitLabel` measures with `CalcTextSize`, which rounds, and the other
  copies use `CalcTextSizeA`, so merging them changes pixels.
- The list and detail body of `GameMenu::Draw`: about fifteen shared locals. Parts were extracted
  only where they needed about six parameters or fewer.
- The remaining private checks in tests: five throw with a message, five count failures and keep
  going, one prints a caller variable. They differ from `CHECK` on purpose. The one-frame drivers
  share three calls around a different draw call each.
- The five-line build target preamble in the scripts: each script derives different paths from it.

### Verification of the second pass

- 50 of 50 ctest cases after every step. `UiRender` once took 92 s against its 90 s limit while
  other work ran on the machine; that binary had not been rebuilt, and it passed on every later run.
- Rust: `cargo fmt --check`, `cargo clippy -D warnings`, 77 tests.
- `IrohRoom::Poll`: `IrohRoomIntegrationTest`, `IrohRecoveryIntegrationTest --scenario=preparing` and
  `CustomRoomGameTest --late-spectator`, run one at a time from an isolated folder, all exit 0.
- Release client: `EmberUpdateTest` and `EmberUpdateTest --live` (a real `CheckForUpdate`).
- UI: a temporary patch hashed every draw list (vertices, indices, clip rectangles) after each
  `ImGui::Render()` in `ControllerNavigationTest`, `RoomPanelNavigationTest` and
  `NativeSelectionAvailabilityTest`. 8870 frames, stable across two baseline runs, and identical
  after the `GameMenu` commit and after the `ApplicationShell` commit. The patch was not committed.
- Developer overlay: a second tree, `build/devui`, configured with `-DSF4E_DEVELOPER_UI=ON`, built
  and linked `Sidecar.dll` before the change, after the dead code removal and after the split.
  `dumpbin /symbols` shows no reference to `ImGui::Begin` or `ImGui::End` in any of the three
  objects, so every moved body still binds to the local `Begin` and `End`.
- Still not verified: the GGPO split is covered by compile, link and `RuntimeBootstrapTest` only,
  `DownloadAndApplyUpdate` still has no test, and nobody has looked at the developer page in a
  running game. The two-PC match, direct and relay, is still owed before promotion.

### Still open

- Layering: `sf4e__NetplayFacade.hxx` includes a UI header, `platform/ApplicationServices.hxx`
  includes a launcher header, and `SessionClient` exposes four fields that three sources and six
  tests read. Recorded, not fixed.
- Everything under "Future work" above.
