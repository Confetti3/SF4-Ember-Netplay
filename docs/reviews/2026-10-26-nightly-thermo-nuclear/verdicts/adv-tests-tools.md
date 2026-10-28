# Adversary verdicts: tests.md (F-TEST-1..14) and tools.md (F-TOOLS-1..22)

Read at sf4-nightly b9ce0e31 (branch nightly). Nothing was built or run. The only thing executed outside the repo was a read of the installed VS 18 BuildTools vsdevcmd_start.bat (for F-TOOLS-17).

## tests.md

### F-TEST-1: PLAUSIBLE  (severity: P2)
- Decisive invariant/evidence: tests.rs is 7384 lines, and the seven submodules all `use super::*` (tests.rs:7378-7384). The split is NOT mechanical along the proposed map, because many fixtures are used across the proposed module boundaries and by the existing submodules:
  - `led_host` (1161) is used by probes (5103) and by ReservationFixture (5459).
  - `BoundHost`/`bound_host` (1179/1192) are used by `LearnerFollower` (4852, 4863) and by tests/match_watch.rs.
  - `complete_next` (2919) is used by departure (2973-2986), probes (5187), reservations (5739-6127) and match_watch.rs.
  - `send_admission` (1239) is used at 4606 (join_failures).
  - `GameSide`/`game_connection_pair` (5201/5315) are used by dials (6184, 6199, 6221, 6702).
  - `ReservationFixture` is used by dials (6267, 6452, 6621) and by wire (7313). The proposal's own example, "ReservationFixture goes to reservations.rs", is wrong.
  - `joined_invite` (4638) is used by dials (6662) and by public_admission, public_rooms and server_owned.
  - `leads` (6987) is used by server_owned.rs.
  - `run_probe` is used by reservations (5684).
- Adjustment: put every shared fixture listed above in a `tests/support.rs` (or keep it in tests.rs) visible to all siblings. The shared fixtures come to about 650 lines (GameSide, two ReservationFixture impl blocks, the BoundHost family, test_actor and its helpers), so the "tests.rs under 400 lines" target holds only if that support module is separate. Only the single-family fixtures (ProbeSide, LearnerFollower, the short-link helpers, `nested_json`, `unconfirmed_dial`, `phase`) move verbatim. The relay dedup is confirmed (21 `default_relay_map` hits, 1 of them in `test_relay` itself, so 20 inline). It saves about 40 lines, not 60. P1 is too high for test organization.
- Duplicate of: no. It overlaps F-TEST-7 and F-TEST-14 in the same file; do them in the same change.

### F-TEST-2: PLAUSIBLE  (severity: P2)
- Decisive invariant/evidence:
  1. The 2485 test checks only the test-only copy (tests.rs:5-31). That copy also drops current voters that are missing from `admissions`, which `admission_voters` (members.rs:595-615) does not do. The hand calculation `admission_voters({1,2,3},[1,2,3,4],3,excl 2|3,true) = {1,4}` is correct. However, production `admission_voters` is NOT untested: members.rs has its own `mod tests` (members.rs:737-797) that calls it directly, and it covers candidate exclusion (`|id| id == 3`, 779-787). The case that is missing against production is "an excluded *current* voter is dropped".
  2. The 3007 test is not "testing nothing". Its named invariant (an aged-out tombstone makes room) lives in production `apply_confirmed_retirements`/`record_retirement` (members.rs:50-101), and the shortcut calls that production code. The shortcut replaces only the computation of the confirmed set and skips `close_retired_strangers` (members.rs:497-500).
  3. The sub-claim that the copy "reads a different source for the applied set" is wrong. `applied_member_ids` → `store.applied_members()` and `applied_membership_provenance().0` both read `machine.membership.nodes()` (coordination.rs:536-545 and 550-561).
- Adjustment:
  - For 2485, add the {1,4} case to the existing members.rs `mod tests`. That needs no visibility change and no actor. Then delete `stable_voters`. Neither form proves that members.rs:443 passes `pending_retired || retired`, so say so.
  - For 3007, `start_membership_reconciliation` does not exist. The entry points are `spawn_membership_operation` (as used at tests.rs:5061) and `schedule_pending_membership_reconciliation` (members.rs:158). The completion is silently dropped unless the key's epoch, room, term and committed revision all match (members.rs:483-496). The rewrite must therefore pass the committed revision, and if it is wrong the assertions fail, so the test cannot silently pass. A cheaper option keeps the unit test by calling `apply_confirmed_retirements` directly with an explicit set.
- Duplicate of: no.

### F-TEST-3: CONFIRMED  (severity: P2)
- Decisive invariant/evidence:
  - `match_expiry_hours: config::DEFAULT_MATCH_EXPIRY_HOURS` appears in exactly 5 files outside src, including blumint_command.rs.
  - sf4-net (rust/sf4-net/Cargo.toml:41), brackets and notifier already dev-depend on ember-bridge.
  - The server/ember workspace uses resolver 3, so dev-dependency features do not reach `cargo build -p ember-bridge`, the build that BridgeSdk ships to the SDK test (RunSdkTests.cmake:16). The shipped bridge is unaffected.
- Adjustment: none in substance. Note that the shared `rust-ember-target` will hold two builds of the ember-bridge lib (feature on for `cargo test`, feature off for BridgeSdk's `cargo build`). That costs one extra compile after each bridge change; the two builds do not thrash. P1 is too high for test duplication.
- Duplicate of: no. It is the canonical home for F-TEST-4's and F-TEST-6's shared pieces.

### F-TEST-4: CONFIRMED  (severity: P2)
- Decisive invariant/evidence: rooms.rs:291-296, 312-317, 339-345 and 355-360 `break` on success and fall through silently on timeout. `rooms_settle_at` (rooms.rs:120-130) panics correctly. ember-rooms is its own workspace (it has its own Cargo.lock), so it needs a local copy, as the finding says.
- Adjustment: convert only the positive polls. The fixed sleeps that follow them are F-TEST-5's business, and several of them are negative windows.
- Duplicate of: no. It overlaps F-TEST-5 on rooms.rs; F-TEST-5 is canonical for the sleeps.

### F-TEST-5: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence:
  - rooms.rs:297, 319 and 369 are negative windows: they prove the host was not killed for bad, deep or maximum-size details. They are not "settle" waits.
  - rooms.rs:337 is a deliberate "do not look early" delay. Its comment says the first list "is then the only look at the room". Replacing it with polling would let the list see members=1 and would change what the test proves.
  - The twitch window has an upper bound as well as a lower one. Keepalives stop after 8×500 ms (twitch_room.rs:301-306), and a reconnect follows 1 s + `KEEPALIVE_GRACE` (2 s, eventsub.rs:29) later. A derived "+ margin" that runs past that bound would turn the test red.
  - The value 1200 ms already equals `empty_close*4`.
- Adjustment: do not convert 297, 319, 337 or 369 to `until`. Bound the twitch window by the keepalive run. Leave the 16 s C++ window as is unless a specific constant is identified: for a room of two the freeze is permanent (iroh_recovery_integration_test.cxx:1043-1044), so no lease value brackets it.
- Duplicate of: no.

### F-TEST-6: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: `relay` (`older_bridge_in_front`) uses `read_raw` and `answer` (public_rooms_support/mod.rs:280-288), so the raw parser has to stay anyway.
- Adjustment: replace only `Fake`/`handle`. The saving is well under 200 lines.
- Duplicate of: no. It depends on F-TEST-3.

### F-TEST-7: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: a diff of entry.rs:95-156 against tests.rs:229-293 shows exactly three differences (`endpoint`, `coordination_port`, `latency`). The counts are confirmed: 68 `test_actor(`, 80 `mpsc::channel(IPC_QUEUE_CAPACITY)` and 39 `Invite::create(`.
- Adjustment: none. `Actor::new` is a trivial production change: same field values, with `latency` passed in.
- Duplicate of: no.

### F-TEST-8: CONFIRMED  (severity: P2)
- Decisive invariant/evidence: session_server_transport_test.cxx has 2365 lines. Corrections:
  - `main()` (2089) first runs TestParticipantDepartureCheckpoint (×10), TestRejoinAfterPreStartDeparture and TestServerOwnedRoom, then the inline scenario (2095-2346), then 12 more named tests (2347-2363).
  - TestRetiringSpectatorProjection (599) is missing from the proposed clusters.
- Adjustment: assign TestRetiringSpectatorProjection to the barriers group. Five executables add link time to every build.
- Duplicate of: no.

### F-TEST-9: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: room_panel_navigation_test.cxx is one `main() try` from line 29 to 1405, with a local `Check` that throws (24-28). The risk the finding names, blocks that depend on state an earlier block set, is the real one, and finding those blocks needs a careful read.
- Adjustment: none beyond the stated care. The value is diagnostic, not a saving.
- Duplicate of: no. It shares its `Check` part with F-TEST-10.

### F-TEST-10: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: a recursive recount over src/tests gives 29 files defining `CHECK`, 118 of 222 .cxx files including test_support.hxx, and 15 defining `Check(`. These are close to the finding's numbers.
- Adjustment: none.
- Duplicate of: no.

### F-TEST-11: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: blumint_reports.rs has 1597 lines and 34 tests. Correction: only two `#[path]` includes cross binaries (blumint_reports→discord/support.rs and startgg→challonge/support.rs). challonge.rs:9 and discord.rs:7 include their own support files.
- Adjustment: none beyond that count.
- Duplicate of: no.

### F-TEST-12: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: `INSERT INTO discord_accounts` appears 6 times in 5 files. The `att_silent` INSERT is identical at blumint_reports.rs:286 and partner.rs:708.
- Adjustment: prefer SQL helpers over store functions, unless those functions have no side effects such as events or emits.
- Duplicate of: no.

### F-TEST-13: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: HelperRust, EmberRust and BridgeSdk are each a single 1800 s entry (cmake/sf4e_helper.cmake:75-99), and no test anywhere carries `LABELS`.
- Adjustment: the targeted-retest script must not write into `rust-test-target` while a gated ctest is running.
- Duplicate of: no.

### F-TEST-14: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: 22 `timeout(Duration::from_secs(60), async` wrappers; `phase` is at tests.rs:206.
- Adjustment: none.
- Duplicate of: no. Do it together with F-TEST-1.

## tools.md

### F-TOOLS-1: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: package-upgrade.ps1:8 is the only `Set-StrictMode` among the scripts. It is dot-sourced at github-release.ps1:46, so package-installer (dot-sourced at line 49) runs strict only on Stable non-prerelease releases. The `$script:` outputs are at package-team.ps1:98-100, package-upgrade.ps1:157-158 and package-installer.ps1:52.
- Adjustment: pin strictness explicitly inside the new functions rather than dropping it. Every AGENTS.md check must stay inside `New-EmberPackage`.
- Duplicate of: no.

### F-TOOLS-2: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: the duplication is confirmed (package-team.ps1:83-89 and package-upgrade.ps1:132-137, plus three sidecar writers). github-release.ps1 and package-team.ps1 carry no `#Requires -Version 7` (only the Nightly scripts do). Under Windows PowerShell 5.1, `Set-Content -Encoding UTF8` writes a BOM.
- Adjustment: the helper must pin the encoding explicitly, and the shipped-parser fixture check should cover both BOM and no-BOM input.
- Duplicate of: no. It overlaps F-TOOLS-5 on the digest helpers.

### F-TOOLS-3: CONFIRMED  (severity: P2)
- Decisive invariant/evidence:
  - github-release.ps1 never calls `Get-EmberBuildTarget` and never reads `.channel`. It checks a clean tree, tag == HEAD, tag == CMake VERSION, the notes file and the previous zip (lines 12-41), then dot-sources package-team.
  - package-team accepts either channel (package-team.ps1:8-16).
  - build-current derives `stagingBridge` from the channel (build-current.ps1:31). A package built from sf4-nightly therefore ships a helper that trusts the staging bridge, to Stable's Latest release.
  - Nightly commits reach `origin`, so a `vX.Y.Z` tag on a nightly commit satisfies `--verify-tag`.
- Adjustment: require `channel -eq 'stable'` in github-release.ps1 (and in release-team-build if it is used for Stable). package-team must keep accepting both channels, because Nightly packages through it. The stagingBridge-vs-channel assert is cheap and only strengthens provenance.
- Duplicate of: no.

### F-TOOLS-4: CONFIRMED  (severity: P3)
- Decisive invariant/evidence:
  - CMakeLists.txt:7 reads `VERSION 1.4.1`, the same as origin/release.
  - History shows the rule is practiced: after each Stable release there is a renumber commit (15ce7078 → 1.2.0, dbd58042 → 1.3.0, 90b529c1 → 1.4.0). None follows f427c279 (1.4.1, 2026-10-21), so this is a missed bump. The document is right; the code does not enforce it.
  - Updater impact: `CompareVersions` orders a prerelease below its release (github_release_validation.cxx:62-73). The Nightly channel accepts only Nightly kinds from the Nightly repo (UpdateChannel.hxx:30), so Nightly players still update normally (1.4.1-nightly > 1.4.0-nightly). The visible effect is that a Stable 1.4.1 → Nightly switch is presented as "go back" (github_release_client.cxx:536-538). The impact is cosmetic and on process.
- Adjustment: bump nightly to 1.5.0 now. Add the guard in `Invoke-NightlyPublish`, failing closed when `origin/release` is unavailable (the `-Local` path does not fetch it).
- Duplicate of: no.

### F-TOOLS-5: PLAUSIBLE  (severity: P2)
- Decisive invariant/evidence: Stable publishes with one `gh release create … --latest` and never reads it back (github-release.ps1:51). Nightly verifies asset digests (NightlyOperations.ps1:250-290).
- Adjustment:
  - Bind the repository to the channel returned by `Get-EmberBuildTarget`, never to a parameter. Ship it after F-TOOLS-3.
  - Keep retention Nightly-only, as stated.
  - Draft-then-verify-then-publish also gives a resumable path for the known HTTP 500 that gh hits on a 393 MB upload.
  - Keep `Latest` set only at publish, and keep Stable's previous-asset digest check.
- Duplicate of: no.

### F-TOOLS-6: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: all three suites throw on failure (`$ErrorActionPreference='Stop'` and throw-based checks), so today every receipt implies they passed, including `-SkipTests` and Stable receipts. They run before the `$before` fingerprint (build-current.ps1:9-13 vs 22), so that attestation is not bound to the fingerprint. After the move, only `testsPassed` receipts imply it. Packaging rejects any receipt without `testsPassed` (package-team.ps1:15), so packaged builds stay covered, but only under three conditions:
  1. NightlyPublication, NightlyPaths and BuildProvenance are added to `$missing` in `Invoke-DefaultTests` (BuildProvenance.ps1:63). This is MANDATORY, not optional, or a CMake edit could drop them silently.
  2. test-build-provenance.ps1's fake-ctest fixture (line 86) is updated to list the new names.
  3. The component text at build-current.ps1:75 names them.
- The suites write only under TEMP, so running them inside the fingerprint window does not trip "Source changed during build". A broken publisher test now fails after a full build instead of before it.
- Adjustment: the three conditions above. On those terms provenance is not weakened.
- Duplicate of: no. F-TOOLS-21 depends on it.

### F-TOOLS-7: CONFIRMED  (severity: P2)
- Decisive invariant/evidence: `Kind::ALL` is `[Kind; 39]` (event.rs:63) and TS `EVENT_TYPES` has 39 entries. Rust `RoomSummary` has `own_delay`, `host_name`, `fighters`, `locked`, `set_format`, `rotation` and more (rooms.rs:217-238). TS `RoomSummary` stops at `created_at` (events.ts:96-106).
- Adjustment: none. This is a test plus additive optional fields; there is no wire change.
- Duplicate of: no.

### F-TOOLS-8: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: 8 methods return `Promise<Body>` (client.ts:358-505).
- Adjustment: the public partner API changes, so keep the change additive. Renames need a deprecation period.
- Duplicate of: no.

### F-TOOLS-9: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: TestPlayer's 6 raw fetches (testing.ts:163-330) lack `redirect:"error"` and a timeout, which client.ts:322-325 has. PKCS8 construction is repeated at signed.ts:104, 112 and 247.
- Adjustment: TestPlayer is a public export that partners use. Make the timeout an option, and note the change in the SDK release notes.
- Duplicate of: no.

### F-TOOLS-10: CONFIRMED  (severity: P2)
- Decisive invariant/evidence: the hand list is build-linux.sh:53-83. build-nightly-room-hosts.sh:4-8 extracts each revision to `$HOME/ember-nightly-build/<rev>` and never deletes it.
- Adjustment: the trap must run after the `cp` (line 21). That is safe because the pending record hashes the copied binaries.
- Duplicate of: no.

### F-TOOLS-11: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: the counts are confirmed (974 lines, 168 `sf4e_add_unit_test`, 50 `add_test`, 18 `if(BUILD_TESTING)` blocks). The parser at sf4e_tests.cmake:43-75 also rejects an empty token anywhere in SOURCES or LIBS. `cmake_parse_arguments(PARSE_ARGV)` keeps empty list elements, and `KEYWORDS_MISSING_VALUES` flags only keywords that have no values at all. An empty source would resolve to the source directory and pass an EXISTS check.
- Adjustment: keep the explicit empty-token and duplicate scans, or skip the parser rewrite (about 25 lines saved in a gate helper). The file split is fine.
- Duplicate of: no.

### F-TOOLS-12: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: desktop-sf4e-helper.ps1 and run-offline-test.ps1 have no references. sign-release-binaries.ps1 is referenced by docs/development/CODE_SIGNING.md. The checkout's build-target.json has 11 features and the workspace file has 30.
- Adjustment: update CODE_SIGNING.md along with DISCORD.md.
- Duplicate of: no.

### F-TOOLS-13: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: RunSdkTests.cmake:21 runs `npm ci` every time. The lockfile has 3 packages (typescript, @types/node, undici-types). node_modules is gitignored (.gitignore:18), so it is outside the source fingerprint either way.
- Adjustment:
  - With a warm npm cache the saving is a few seconds, not tens.
  - The stamp must cover the npm version too, and must be checked against `node_modules/.package-lock.json`. node_modules lives in the source tree, not the build dir.
  - Small attestation change: a BridgeSdk pass no longer proves that a lockfile-exact tree was installed in this run.
- Duplicate of: no.

### F-TOOLS-14: REJECTED  (severity: P3)
- Decisive invariant/evidence:
  - The companion includes `../session/RoomModel.hxx` and `../netplay/SessionController.hxx` (src/discord/Presence.hxx:2-3) and their transitive headers. It also reads cmake/version.rc.in and the root CMakeLists.txt VERSION (src/discord/CMakeLists.txt:3, 20-26).
  - The fixture's only source is src/tests/discord_bridge_fixture.cxx, outside its folder (discord-fixture/CMakeLists.txt:6), and that file includes src/discord/BridgeServer.hxx.
  - A folder glob would therefore miss real inputs. A header change would then stage a stale ember-discord.exe, and the receipt would hash it against newer source. That weakens provenance.
  - `BUILD_ALWAYS` hands correctness to MSBuild's complete dependency tracking, at the cost of two MSBuild no-op launches.
- Adjustment: keep `BUILD_ALWAYS` for EmberDiscord. For the test-only fixture, use explicit DEPENDS only if the cost is ever measured.
- Duplicate of: no.

### F-TOOLS-15: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: confirmed 5861 files and 958 `color-N` photographs (about 123-126 MiB) under assets/selection, copied by `copy_directory_if_different` (CMakeLists.txt:934-935). The receipt hashes only staged .exe and .dll files, and install already excludes the photographs (927-930), so provenance is unaffected.
- Adjustment: reads after the first build mostly hit the page cache, so the time saved is likely about a second, not "246 MiB of reads". Keep the compare on every build, done at build time with a per-file `copy_if_different` under the install regex. Delete the photographs already in build folders once.
- Duplicate of: no.

### F-TOOLS-16: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: 7 calls per Nightly, at NightlyOperations.ps1:117, build-current.ps1:22 and 67, package-team's `Assert-BuildReceipt`, NightlyOperations.ps1:548 and 554 (both through `Assert-BuildReceipt`, BuildProvenance.ps1:76), and 592. Trimming the call at 554 is safe: package-team fingerprinted fresh during Package (549), and 592 rechecks fingerprint and clean HEAD.
- Adjustment:
  - The 554 check must keep receipt == snapshot, packaged receipt == snapshot, the staged binary hashes and the clean HEAD check.
  - Correct the estimate: the trim saves one call, about 2-3 s. The 15-20 s figure is the total of all 7 calls.
  - A faster implementation must keep the `Test-Path -PathType Leaf` → `DELETED` semantics and uppercase hex.
- Duplicate of: no.

### F-TOOLS-17: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: there are three calls in one pwsh process (build-current-dependencies.ps1:8, build-current.ps1:24, package-installer.ps1:39). The installed vsdevcmd_start.bat:51-57 saves `__VSCMD_PREINIT_PATH` only on first entry and restores it only with `-clean_env` (line 46), so each re-entry prepends again. PATH growth is real.
- Adjustment: compare normalized `VSINSTALLDIR` (it has a trailing backslash) and `VSCMD_ARG_TGT_ARCH`. package-installer's `VCToolsRedistDir` persists across an early return.
- Duplicate of: no.

### F-TOOLS-18: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: 13 `python3` calls. The defaults appear again at setup.sh:156, 248, 298 and 319, and match config.rs:47-56.
- Adjustment: test-setup.sh needs a fake binary, as the finding says. Run the staged binary only after its ELF check.
- Duplicate of: no.

### F-TOOLS-19: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: three rollback models exist (reports setup.sh:51-79, short setup.sh:84-123, rooms setup.sh:203-211), plus two nginx insertion algorithms.
- Adjustment: low value; verify with an `nginx -T` diff on the VPS as stated.
- Duplicate of: no.

### F-TOOLS-20: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: the timer runs every 30 s (ember-rooms-idle.timer). The install docs are VPS-only (deploy/README.md:188-208), and ember-rooms-docker.service (server1) has no idle timer.
- Adjustment:
  - Default `lone_close_secs` must be 0 (off), with 900 set in the VPS config. Otherwise server1 newly starts closing lone rooms.
  - Deploy the supervisor everywhere before removing the timer.
  - Accept the exact-timing behavior change explicitly.
- Duplicate of: no.

### F-TOOLS-21: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: test-nightly-publication.ps1 has 761 lines with throw-based `Check`/`Reject` (line 19). The repository literal appears at NightlyOperations.ps1:21 and 441.
- Adjustment: none.
- Duplicate of: no. It depends on F-TOOLS-6.

### F-TOOLS-22: PLAUSIBLE for the fonts, REJECTED for `-U*_DIR`  (severity: P3)
- Decisive invariant/evidence:
  - The header is written with `file(GENERATE)` (sf4e_fonts.cmake:43), which writes only when the content differs. "Writes 17 MB every configure" is wrong: the configure cost is the in-memory hex conversion (sf4e_embed.cmake:6-9).
  - `-U*_DIR`/`-UZLIB_*` (build-current.ps1:49) is the cross-checkout guard. The check at line 52 only greps for `sf4-display-dxvk`.
- Adjustment: the build-time font generation is acceptable. Replacing `-U*_DIR` with a path heuristic weakens a provenance guard, so that half is rejected.
- Duplicate of: no.

## Batch verdict
- Counts:
  - tests.md: CONFIRMED 9 (F-TEST-3, 4, 7, 8, 10, 11, 12, 13, 14); PLAUSIBLE 5 (F-TEST-1, 2, 5, 6, 9); REJECTED 0.
  - tools.md: CONFIRMED 9 (F-TOOLS-1, 3, 4, 7, 10, 12, 16, 17, 21); PLAUSIBLE 11 (F-TOOLS-2, 5, 6, 8, 9, 11, 13, 15, 18, 19, 20); split 1 (F-TOOLS-22: fonts PLAUSIBLE, `-U*_DIR` REJECTED); REJECTED 1 (F-TOOLS-14).
  - Severity corrections: F-TEST-1 and F-TEST-3 P1→P2; F-TEST-2 P1→P2; F-TEST-7 P2→P3; F-TOOLS-1 P2→P3; F-TOOLS-6 P2→P3; F-TOOLS-13 P2→P3.
- Do first:
  1. F-TOOLS-3: a one-line channel guard closes a real path for a build that trusts the staging bridge to reach Stable Latest.
  2. F-TOOLS-4: bump nightly to 1.5.0, a missed step in a practiced rule, and add the guard so it cannot be missed again.
  3. F-TEST-2: cheap. It removes two test-only copies of production rules, but with the corrected framing: 3007 already exercises production tombstone aging, and `admission_voters` is already unit-tested in members.rs.
- Could not verify by reading:
  - every timing estimate (npm ci, MSBuild no-ops, vcvarsall, configure, fingerprint);
  - whether the rewritten 3007 test can reach a matching committed revision;
  - Cargo's handling of a self-referencing dev-dependency with features;
  - which production constant, if any, the 16 s C++ window brackets;
  - whether server1 runs the idle timer outside the repo's units.
