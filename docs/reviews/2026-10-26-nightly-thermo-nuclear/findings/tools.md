# Findings: build tooling, packaging, CMake, partner SDK, deploy (area TOOLS)

Reviewed at b9ce0e31 (nightly). Scope: scripts/ (PowerShell, shell, Python), cmake/, CMakeLists.txt,
CMakePresets.json, sdk/typescript/src and tools, server/*/deploy. Nothing was built or run except one
read-only timing of `Get-SourceFingerprint` (2.0 s warm, 3.0 s cold over 7,650 files, 298.6 MiB).

### F-TOOLS-1: Packaging scripts are dot-sourced and "return" through $script: variables
- File: scripts/package-team.ps1:98
- File: scripts/package-upgrade.ps1:8
- File: scripts/package-upgrade.ps1:157
- File: scripts/package-installer.ps1:52
- File: scripts/github-release.ps1:43
- File: scripts/github-release.ps1:46
- File: scripts/github-release.ps1:49
- File: scripts/release-team-build.ps1:12
- File: scripts/publish-nightly.ps1:26
- Category: boundary
- Severity: P2
- Claim: The three packaging steps are whole scripts that callers dot-source into their own scope, then read
  `$script:PackageZipPath`, `$script:PackageFolderPath`, `$script:PackageGitRev`, `$script:UpgradeZipPath`
  and `$script:InstallerPath` as their result. Dot-sourcing also leaks every local (`$repo`, `$target`,
  `$destination`, `$hash`, `$revision`, `$entries`) and every preference into the caller. Concretely,
  package-upgrade.ps1:8 runs `Set-StrictMode -Version Latest`; dot-sourced from github-release.ps1:46 it turns
  strict mode on for the rest of github-release, so package-installer.ps1 (line 49) runs strict for a Stable
  release and non-strict for a pre-release (which skips the upgrade). publish-nightly.ps1 wraps the same
  scripts in scriptblocks (lines 26-35) only to re-shape these globals into an object.
- Proposed change: Turn the bodies into functions in one importable module (for example
  `scripts/Packaging.ps1` with `New-EmberPackage`, `New-EmberUpgradePackage`, `New-EmberInstaller`) that
  return `[pscustomobject]@{zipPath; folderPath; gitRev}` and `@{zipPath}` / `@{installerPath}`. Keep
  package-team.ps1, package-upgrade.ps1 and package-installer.ps1 as three-line entry points that import and
  call them. github-release.ps1, release-team-build.ps1 and the Nightly `Package`/`Installer` operations call
  the functions and use the returned object; the `$script:` variables and the scriptblock re-shaping go.
- Behavior preserved: Every check, file name, sidecar and output path; the AGENTS.md packaging validations
  (target, CMake source, fresh fingerprint, staged hashes, provenance receipt in the package) stay inside
  `New-EmberPackage` exactly as today.
- Behavior at risk: The strict-mode leak disappears, so package-installer runs non-strict in both Stable
  paths (today's accidental strictness on Stable only). Test fakes in test-nightly-publication.ps1 that
  replace the `Package` operation keep working because the operation contract (an object) is unchanged.
- Estimated saving: Removes 5 script-scope globals and the dot-source coupling; about 15 lines of glue.
- Confidence: high

### F-TOOLS-2: Manifest, sidecar and zip-hash primitives are copy-pasted across packaging scripts
- File: scripts/package-team.ps1:81
- File: scripts/package-team.ps1:96
- File: scripts/package-upgrade.ps1:14
- File: scripts/package-upgrade.ps1:34
- File: scripts/package-upgrade.ps1:132
- File: scripts/package-upgrade.ps1:155
- File: scripts/package-installer.ps1:50
- File: scripts/github-release.ps1:40
- File: scripts/tester-preflight.ps1:57
- File: scripts/NightlyOperations.ps1:152
- Category: judo
- Severity: P2
- Claim: "List every file, sort ordinal, write `<lowercase sha256>  <relative>`" is written twice
  (package-team 81-86, package-upgrade 132-137). "Hash a file and write `<hash>  <name>` to `<file>.sha256`"
  is written three times (package-team 96-97, package-upgrade 155-156, package-installer 50-51). Reading a
  manifest back is written twice with different strictness (package-upgrade `Read-Manifest` rejects
  duplicates; tester-preflight 57-67 parses its own way). Sidecar verification exists only in
  package-upgrade (`Assert-ZipSidecar`), while github-release.ps1:40 re-hashes the base zip inline, and
  NightlyOperations has a third artifact-hash shape (`Get-NightlyArtifact`). The formats must stay
  byte-identical for the updater and preflight, yet nothing ties the copies together.
- Proposed change: One `scripts/PackageFiles.ps1` with `Write-HashManifest <dir> <name>`,
  `Read-HashManifest <path>`, `Write-Sha256Sidecar <file>`, `Assert-Sha256Sidecar <file>` and
  `Get-FileSha256 <path>` (lowercase), used by package-team, package-upgrade, package-installer,
  github-release and NightlyOperations. tester-preflight.ps1 and upgrade/Install-Upgrade.ps1 ship inside
  packages and run standalone, so they keep their own copies; add a CTest-run check that their parsers
  accept `Write-HashManifest` output (one fixture folder) so the shipped copies cannot drift silently.
- Behavior preserved: Exact manifest and sidecar bytes (ordinal order, two spaces, lowercase hex, ASCII
  sidecars, UTF-8 manifest).
- Behavior at risk: Encoding of MANIFEST.txt (`Set-Content -Encoding UTF8` writes a BOM on Windows PowerShell
  but not on pwsh 7); the helper must pin the encoding the current pwsh 7 path produces.
- Estimated saving: About 30 duplicated lines removed; one definition of the package hash format.
- Confidence: high

### F-TOOLS-3: Stable publication has no channel guard, so the Nightly checkout can package for Stable
- File: scripts/github-release.ps1:6
- File: scripts/github-release.ps1:43
- File: scripts/package-team.ps1:8
- File: scripts/BuildEnvironment.ps1:12
- Category: boundary
- Severity: P2
- Claim: `Get-EmberBuildTarget` deliberately accepts either designated checkout and reports `.channel`.
  Invoke-NightlyPublish enforces `channel -eq 'nightly'` (NightlyOperations.ps1:434), but github-release.ps1
  never looks at the channel and package-team.ps1 accepts both. A build from sf4-nightly (receipt
  `stagingBridge: true`, helper trusting the staging bridge) passes every github-release check as long as the
  tag matches HEAD and CMake's VERSION, and would be uploaded to `Confetti3/SF4-Ember-Netplay` as a Stable
  release. Today the existing v1.4.1 release makes github-release.ps1:24-25 refuse, but for the next new
  version tag only the operator stands between the Nightly build and the Stable repository.
- Proposed change: In github-release.ps1, resolve `Get-EmberBuildTarget` and require
  `channel -eq 'stable'`; in package-team (or the new `New-EmberPackage`) refuse a receipt whose
  `stagingBridge` disagrees with the channel (true only for nightly). Both are one-line asserts next to the
  existing ones.
- Behavior preserved: Every legitimate Stable and Nightly run.
- Behavior at risk: none identified.
- Estimated saving: n/a (removes a footgun; no lines deleted)
- Confidence: high

### F-TOOLS-4: Nightly's "one minor ahead of Stable" rule is documented but not enforced, and not true at head
- File: CMakeLists.txt:7
- File: scripts/NightlyOperations.ps1:507
- File: docs/development/NIGHTLY.md:7
- Category: boundary
- Severity: P3
- Claim: NIGHTLY.md and AGENTS.md say Nightly's CMake version is one minor ahead of Stable. At b9ce0e31
  `VERSION 1.4.1` on nightly equals `release` (also 1.4.1), apparently carried in by the release merge.
  Invoke-NightlyPublish parses the VERSION line (507-511) but never compares it with `origin/release`, so the
  next publish would be labelled `v1.4.1-nightlyYYYYMMDD`, which orders below the published `v1.4.1` under
  semver prerelease rules. Past practice may already accept equal versions right after a promotion (a
  `1.4.0-nightly` was published the day before Stable 1.4.0), so either the code or the document is wrong.
- Proposed change: Decide which is true. If the rule holds, in Invoke-NightlyPublish read
  `origin/release:CMakeLists.txt` (the ref is already fetched for the notes baseline) and refuse unless
  Nightly's minor is Stable's minor + 1, and bump nightly to 1.5.0. If equal versions are acceptable after a
  promotion, amend NIGHTLY.md and AGENTS.md to say so.
- Behavior preserved: Label format and every existing check.
- Behavior at risk: A publish that would currently succeed now stops until the version is bumped; whether the
  updater's channel logic actually mis-orders `1.4.1-nightly…` against `1.4.1` was not verified here.
- Estimated saving: n/a
- Confidence: medium

### F-TOOLS-5: Stable and Nightly publication are two pipelines; only Nightly verifies what it published
- File: scripts/github-release.ps1:51
- File: scripts/NightlyOperations.ps1:250
- File: scripts/NightlyOperations.ps1:290
- File: scripts/github-release.ps1:16
- File: scripts/github-release.ps1:20
- Category: judo
- Severity: P2
- Claim: Nightly publishes through a resumable read-step-read state machine (`Get-NightlyTagState`,
  `Get-NightlyNextStep`, `Publish-NightlyRelease`) that creates a draft, uploads, checks every asset digest
  against the recorded SHA-256 and only then publishes. Stable does one `gh release create … --latest` with
  all assets and never reads the release back, so an interrupted upload or a wrong asset leaves a public
  Latest release with nothing checking it. The two flows also re-implement the same preconditions differently:
  clean tree (`git diff --name-only HEAD` plus `ls-files --others` vs `git status --porcelain
  --untracked-files=all`), the CMake VERSION read (github-release:20, no count check, vs
  NightlyOperations:507 with one), and tag regexes.
- Proposed change: Generalize `Publish-NightlyRelease`/`Get-NightlyNextStep`/`Assert-NightlyReleaseIdentity`
  into `Publish-EmberRelease -Repository -Tag -Prerelease -Latest -Assets -Notes`, keeping the repository
  allow-list per channel (Nightly repo for nightly, Stable repo for stable) instead of the single hard-coded
  Nightly name. github-release.ps1 builds the same `{tag, label, assets[{path,name,sha256}], notes}` record
  and calls it; it gains the draft-then-verify-then-publish order and the digest read-back for free. Move
  `Assert-CleanHead` and `Get-CMakeProductVersion` into BuildEnvironment.ps1 and use them from both.
- Behavior preserved: Stable's assets, title, notes, `--latest`/`--prerelease` flags and refusal of existing
  releases; Nightly's behaviour unchanged.
- Behavior at risk: Stable becomes draft-first, so for a minute the release exists as a draft before it is
  public. The tag rule differs: Nightly refuses a tag that exists without its release, while Stable requires
  the pushed tag to exist (`--verify-tag`), so the generalized `Get-NextStep` takes a tag policy. Retention
  must not generalize: `Invoke-NightlyRetention` and `Remove-NightlyRetentionTarget` delete releases and tags
  by ID and keep their own assert confining them to the Nightly repository; only create, upload, verify and
  publish are shared.
- Estimated saving: One release publisher instead of two; about 20 lines of duplicated preconditions.
- Confidence: medium

### F-TOOLS-6: build-current.ps1 runs the publisher's test suites outside CTest, on every build
- File: scripts/build-current.ps1:9
- File: scripts/build-current.ps1:12
- File: scripts/build-current.ps1:13
- File: CMakeLists.txt:902
- Category: structural
- Severity: P2
- Claim: Before compiling anything, every build-current run executes test-build-provenance.ps1,
  test-nightly-publication.ps1 (761 lines of mocked publisher scenarios) and test-nightly-paths.ps1 (which
  needs Git Bash and fails the whole build without it). They run for Stable builds, for `-SkipTests`, for
  `-SelfTest`, serially before configure, and their result is not in the junit files or the receipt's
  component list. Meanwhile the repository already registers comparable PowerShell suites as CTest entries
  (UpdaterDownloadValidation, NetworkCompletion, UpgradeRecoveryPowerShell at CMakeLists.txt:902-911). Two
  test mechanisms for the same kind of script test.
- Proposed change: Register `NightlyPublication`, `NightlyPaths` and `BuildProvenance` with `add_test` (via
  `find_program(SF4E_PWSH pwsh REQUIRED)`, since the Nightly scripts need PowerShell 7; NightlyPaths gets
  `-BashPath` from a cached `SF4E_GIT_BASH`). Delete lines 9, 12 and 13 of build-current.ps1. Add the three
  names to the `$missing` list in `Invoke-DefaultTests` if the receipt must prove they ran.
- Behavior preserved: The same scripts run on every gated build and a failure still blocks the receipt;
  ctest's `-E` exclusions do not match them.
- Behavior at risk: `-SkipTests` builds no longer run them (consistent with every other test). They would
  now run in parallel with other CTests; they use their own temp fixtures so no lock is needed.
- Estimated saving: Removes a second test path; on `-SkipTests` development builds, the serial pre-build
  time of three PowerShell suites (not measured; the 761-line suite builds and tears down many fixtures).
- Confidence: high

### F-TOOLS-7: SDK wire types are kept in sync with server/ember/protocol by hand, and have drifted
- File: sdk/typescript/src/events.ts:5
- File: sdk/typescript/src/events.ts:96
- File: server/ember/protocol/src/event.rs:20
- File: server/ember/protocol/src/event.rs:63
- File: server/ember/protocol/src/rooms.rs:202
- Category: boundary
- Severity: P2
- Claim: The 39 event family names exist three times in Rust (the `Kind` enum, `Kind::ALL`, the `name()`
  match) and once more in TS (`EVENT_TYPES`). They match today (39 each), but no test compares them.
  Field shapes already drifted: Rust `RoomSummary` has `own_delay` (serialized when true), `host_name`,
  `fighters`, `locked`, `set_format` and more optional fields; TS `RoomSummary` has none of them. Today this
  is latent rather than visible: the provider route takes `ConnectionCreateRoom` (rooms.rs:430, no
  `own_delay`), so a connection's rooms never carry `own_delay`, and the detail fields need `detail=1`, which
  the SDK never sends. Nothing would flag it when either changes, because every TS response type is asserted
  with `parseStrict(text) as T`, so drift is silent at run time too.
- Proposed change: Add a protocol `examples/contract.rs` (the BridgeSdk CTest already builds the ember
  workspace into `rust-ember-target`) that prints a JSON contract: `Kind::ALL` names plus, per exported wire
  struct, its field names (a sample value serialized with every optional field set). The SDK test reads that
  JSON and asserts `EVENT_TYPES` and the key sets of `RoomSummary`, `ConnectionRoom`, `Lobby`, `Tournament`,
  `PlayerRecord` declared in a small `keys` table next to each TS interface. In Rust, derive the `ALL`
  array and `name()` from one macro table so the enum is listed once.
- Behavior preserved: No wire change; the SDK's public types only gain optional fields.
- Behavior at risk: none identified (test-only plus additive optional TS fields).
- Estimated saving: Removes two of the four hand-kept event lists; turns silent drift into a failing test.
- Confidence: high

### F-TOOLS-8: BridgeClient's typed contract stops at the central objects and mixes casing conventions
- File: sdk/typescript/src/client.ts:353
- File: sdk/typescript/src/client.ts:358
- File: sdk/typescript/src/client.ts:413
- File: sdk/typescript/src/client.ts:450
- File: sdk/typescript/src/client.ts:469
- File: sdk/typescript/src/client.ts:251
- File: sdk/typescript/tools/selftest-checks.ts:7
- File: sdk/typescript/tools/selftest-challonge.ts:24
- Category: boundary
- Severity: P2
- Claim: `getMatch`, `cancelMatch`, `recordGame`, `voidGame`, `getLinkIntent`, `approveLinkClaim`,
  `rejectLinkClaim` and `getCapabilities` return `Body` (`{[key]: Json}`), although the match is the SDK's
  central object and `server/ember/protocol/src/matches.rs` defines it. As a result the SDK's own tools
  re-validate match shapes by hand (`selftest-checks.ts` `completedProblems`, `finishedProblems`,
  `deliveryProblems` poke `Body` fields), with the same `isBody` guard copy-pasted in two tool files.
  Request shapes are inconsistent: `MatchSpec`, `LobbySpec`, `TournamentSpec` are wire-shaped snake_case
  (TournamentSpec is spread straight onto the wire at 570), while `RoomSpec` (`buildId`, `creator.emberId`),
  `joinLobby`, `registerEntrant` and `approveLinkClaim` take camelCase and translate; responses are wire
  snake_case except `listEvents`, which renames `next_cursor` to `nextCursor`. `createMatch` casts
  `participants as unknown as Json[]`. There is also a detached duplicate JSDoc block at 434-442.
- Proposed change: Add `Match`, `LinkIntent`, `Capabilities`, `MatchMutation` interfaces mirroring
  matches.rs (covered by the F-TOOLS-7 key check) and return them. Pick one convention for inputs: wire
  snake_case objects everywhere (the existing Spec types) and deprecate the camelCase parameter objects
  behind overloads; keep `nextCursor` as an alias while adding `next_cursor`. Replace the `as unknown as`
  cast by typing `Body` values as `Json`-compatible interfaces (`Participant` is already JSON).
  Move `isBody` into the SDK (`canonical.ts`) and import it in both tools.
- Behavior preserved: Identical HTTP requests and parsed values; existing callers compile if overloads keep
  the camelCase forms.
- Behavior at risk: Public SDK type changes for external partners (BluMint); additive types are safe,
  renames need a minor version and a deprecation period.
- Estimated saving: Deletes the ad-hoc shape checks in selftest-checks.ts that a typed `Match` makes
  redundant (several dozen lines); one input convention.
- Confidence: medium

### F-TOOLS-9: TestPlayer re-implements BridgeClient's HTTP and error envelope, without its safeguards
- File: sdk/typescript/src/testing.ts:338
- File: sdk/typescript/src/client.ts:315
- File: sdk/typescript/src/signed.ts:104
- File: sdk/typescript/src/signed.ts:112
- File: sdk/typescript/src/signed.ts:133
- File: sdk/typescript/src/signed.ts:149
- Category: judo
- Severity: P3
- Claim: `TestPlayer.#read` (testing.ts 338-350) is a copy of the error-envelope parsing in
  `BridgeClient.#request` (client.ts 334-349), and TestPlayer's six raw `fetch` calls (163, 170, 277, 293, 314, 330) omit the client's
  `redirect: "error"` and `AbortSignal.timeout`, so the test stand-in follows redirects and can hang where the
  client cannot. In signed.ts the PKCS#8-from-seed construction is written three times (104, 112, 247), and
  the general encoding helpers `prefixedId`, `counter` and `isBridgeOrigin` live in signed.ts although
  events.ts, results.ts and client.ts import them for non-signature reasons; Rust keeps them in
  `protocol/src/encoding.rs`.
- Proposed change: Extract `bridgeRequest(fetch, origin, {method, path, body, token, headers, timeoutMs,
  expected})` into a new `http.ts` used by both classes (BridgeError construction in one place). Add
  `privateKeyFromSeed(seed)` in signed.ts. Move `prefixedId`, `counter`, `isBridgeOrigin` to a new
  `encoding.ts` mirroring encoding.rs and re-export from signed.ts for compatibility.
- Behavior preserved: Same requests, same errors; BridgeClient unchanged.
- Behavior at risk: TestPlayer gains a timeout (15 s default) and refuses redirects; a partner test that
  relied on a redirecting bridge origin would now fail fast, which matches the real client.
- Estimated saving: About 40 lines; one error-envelope parser.
- Confidence: high

### F-TOOLS-10: The Linux room-host source list duplicates four CMake targets by hand
- File: server/roomhost/build-linux.sh:53
- File: CMakeLists.txt:143
- File: CMakeLists.txt:538
- File: CMakeLists.txt:554
- File: CMakeLists.txt:583
- File: scripts/build-nightly-room-hosts.sh:4
- Category: spaghetti
- Severity: P2
- Claim: build-linux.sh's `SOURCES` (about 30 files) restates the sources of `sf4e_session_server`,
  `sf4e_iroh_room`, `sf4e_room_model`, part of `sf4e_selection` and `sf4e_common`, and `sf4e-room-host`
  (with the Posix helper files swapped in). Its header says it is "the same files as" CMake, but nothing
  checks it; a file added to `sf4e_session_server` is found missing only when the Nightly WSL build, the
  Docker image build or test-full-quality-linux fails at link time, long after the Windows build passed.
  Prior art: the 2026-09 review collapsed "session server sources listed three times" into one CMake list
  (docs/reviews/2026-09-code-quality-review.md:196); this shell list is the copy that remained. Separately, build-nightly-room-hosts.sh extracts every revision to
  `$HOME/ember-nightly-build/<rev>` and never removes it, so WSL accumulates one source tree per Nightly.
- Proposed change: Keep the no-CMake constraint on Linux. Move the shared lists into
  `server/roomhost/sources.txt` (one path per line, with a `# windows`/`# posix` split for the two helper
  files); CMake reads it with `file(STRINGS …)` to populate the four targets, build-linux.sh reads it with
  `grep -v '^#'`. In build-nightly-room-hosts.sh, `trap 'rm -rf "$build_root"' EXIT` (the binaries are copied
  out at the end; the shared `CARGO_TARGET_DIR` keeps cargo incremental).
- Behavior preserved: Same sources compiled, same flags, same binaries.
- Behavior at risk: CMake target source order changes nothing in MSVC output; Docker build context must
  include the new file (it already copies `server/`). The Dockerfile strips CR from build-linux.sh before
  running it (Dockerfile:43), so a sources.txt checked out with CRLF must be read with `tr -d '\r'` too.
- Estimated saving: One source list instead of two; WSL disk growth of one extracted tree per Nightly
  (src+rust+server, tens of MB each) stops.
- Confidence: high

### F-TOOLS-11: CMakeLists.txt is 974 lines with tests interleaved through product targets
- File: CMakeLists.txt:28
- File: CMakeLists.txt:282
- File: CMakeLists.txt:609
- File: cmake/sf4e_tests.cmake:35
- Category: size
- Severity: P3
- Claim: The root file holds 168 `sf4e_add_unit_test` calls and 50 raw `add_test` registrations in 18
  `if(BUILD_TESTING)` blocks woven between library definitions, plus `install()` rules dropped into the middle
  of `sf4e_ui` (282-293, where the target's sources accreted over six `target_sources` calls). The network
  test block under `SF4E_BUILD_IROH_HELPER` (609-744, with `SF4E_NETWORK_TESTS` from 670) alone is about 135 lines. Several registrations are
  repeated triplets (`add_test` + `set_tests_properties TIMEOUT` + `sf4e_test_locks` for the four
  GgpoDelayRollback cases, the relay/non-relay Iroh pairs) where the RoomPerf block already shows the
  table-driven `foreach` form. `sf4e_add_unit_test` spends about 45 lines hand-parsing `ARGV<n>` that
  `cmake_parse_arguments(PARSE_ARGV 0 arg "" "NAME;STD;TIMEOUT;SKIP_RETURN_CODE" "SOURCES;LIBS;LOCKS")`
  does, including empty-value detection via `arg_KEYWORDS_MISSING_VALUES` (CMake 3.15+, minimum here 3.26).
- Proposed change: Move tests into `cmake/sf4e_test_targets.cmake` (core tests before the
  `SF4E_CORE_TESTS_ONLY` return) and `cmake/sf4e_network_tests.cmake`, included after the libraries they
  link (sf4e_add_unit_test checks `TARGET` at call time, so order matters). Gather `sf4e_ui` sources into one
  `add_library` list. Table-drive the repeated test triplets. Replace the parser body with
  `cmake_parse_arguments` plus checks for `arg_UNPARSED_ARGUMENTS`, `arg_KEYWORDS_MISSING_VALUES` and a
  duplicate-keyword scan, keeping the source-exists, library-name and numeric checks.
- Behavior preserved: Same targets, tests, properties and locks (verifiable with `ctest -N` and
  `--show-only=json-v1` before and after).
- Behavior at risk: `cmake_parse_arguments` treats a repeated single-value keyword as last-wins; the explicit
  duplicate scan must stay to keep today's fatal error.
- Estimated saving: Root file to roughly 450 lines; about 40 lines of parser; about 30 lines of repeated
  test boilerplate.
- Confidence: medium

### F-TOOLS-12: Stale scripts and configs that point at other checkouts or retired layouts
- File: scripts/build-discord-local.ps1:3
- File: scripts/build-discord-local.ps1:18
- File: scripts/run-package-tests.ps1:9
- File: scripts/sign-release-binaries.ps1:5
- File: scripts/desktop-sf4e-helper.ps1:5
- File: scripts/run-offline-test.ps1:1
- File: CMakePresets.json:7
- File: build-target.json:5
- Category: spaghetti
- Severity: P3
- Claim: build-discord-local.ps1 hard-codes the VS BuildTools path, a vcpkg tree in another checkout
  (`sf4-display-dxvk/msvc-build/ember-clean`), an install prefix in `sf4-discord`, the archive path and the app
  ID; it bypasses `Get-EmberBuildTarget`/`Get-EmberToolPaths` and builds exactly the cross-checkout tree that
  build-current.ps1:52 refuses and AGENTS.md forbids, yet docs/guides/DISCORD.md still points at it for
  `-RustChecks`. run-package-tests.ps1 hard-codes the VS ctest path and runs ctest without the public-network
  exclusions build-current uses (prior art: the 2026-09 review already moved its build directory to
  build-target.json, docs/reviews/2026-09-code-quality-review.md:137 and :274, but not the VS path).
  sign-release-binaries.ps1 defaults to `msvc-out\relwithdebinfo`, pins two SDK
  versions, shadows the automatic `$pwd` and passes the PFX password on signtool's command line.
  desktop-sf4e-helper.ps1 (`c:\Users\ptwar\…`) and run-offline-test.ps1 have no references.
  CMakePresets.json still targets `msvc-build`/`msvc-out` and was already called unused in
  docs/reviews/2026-09-code-quality-review.md:169. The checkout's own build-target.json carries an 11-item
  feature list that diverged from the workspace file's 30 items (it is only read when no parent exists).
- Proposed change: Delete desktop-sf4e-helper.ps1, run-offline-test.ps1 and CMakePresets.json. Reduce
  build-discord-local.ps1 to its `-RustChecks` cargo part (or fold it into test-full-quality.ps1) and update
  DISCORD.md. Make run-package-tests.ps1 use `Get-EmberToolPaths` and `Invoke-DefaultTests`' exclusion string
  (export it from BuildProvenance.ps1). Point sign-release-binaries.ps1 at the designated stage directory,
  discover signtool under the Windows Kits root, and rename `$pwd`. Either drop the feature list from the
  checkout's build-target.json or document that only the workspace file's list is authoritative.
- Behavior preserved: build-current, packaging and publication are untouched.
- Behavior at risk: Anyone still running the deleted helpers by hand.
- Estimated saving: About 230 lines and three stale path conventions.
- Confidence: high

### F-TOOLS-13: BridgeSdk CTest reinstalls node_modules on every test run
- File: cmake/RunSdkTests.cmake:20
- File: cmake/sf4e_helper.cmake:96
- Category: overhead
- Severity: P2
- Claim: The BridgeSdk test (part of every gated build, and required by `Invoke-DefaultTests`) runs
  `npm ci --ignore-scripts`, which deletes `sdk/typescript/node_modules` and reinstalls it from the lockfile
  every time, then `npm run build` and `npm test`. The lockfile changes rarely; the reinstall is repeated work
  on the critical test path and writes into the source tree during a test.
- Proposed change: In RunSdkTests.cmake, hash `package-lock.json` (`file(SHA256 …)`) together with
  `node --version`, compare with a stamp in `${EMBER_TARGET}/sdk-npm.stamp`, and run `npm ci` only when the
  stamp differs or `node_modules/.package-lock.json` is missing; write the stamp after success.
- Behavior preserved: Same lockfile-exact dependency tree; same build and test commands.
- Behavior at risk: A hand-modified node_modules with an unchanged lockfile is no longer repaired
  automatically; deleting the stamp restores today's behaviour.
- Estimated saving: Payload: about the SDK's dev dependencies (typescript, @types/node; tens of MB of
  files). Interval: every ctest run (each build-current and each Nightly). Transport: local disk plus npm cache.
  Saves an `npm ci` per run, typically several seconds to tens of seconds (not measured here).
- Confidence: medium

### F-TOOLS-14: Two ExternalProjects rebuild unconditionally on every build
- File: cmake/sf4e_discord.cmake:29
- File: CMakeLists.txt:172
- Category: overhead
- Severity: P3
- Claim: `EmberDiscord` and `DiscordBridgeFixture` use `BUILD_ALWAYS TRUE` with the Visual Studio generator,
  so each `cmake --build` (including a no-change rebuild) launches two nested MSBuild builds. sf4e_helper.cmake
  already shows the alternative: a `GLOB_RECURSE CONFIGURE_DEPENDS` source list as explicit dependencies.
- Proposed change: Drop `BUILD_ALWAYS`; add `file(GLOB_RECURSE … CONFIGURE_DEPENDS src/discord/*)` (and the
  fixture's folder) and `ExternalProject_Add_StepDependencies(EmberDiscord build ${sources})`. The SDK
  archive and pin are already configure inputs.
- Behavior preserved: The companion and fixture rebuild whenever their sources or CMake arguments change.
- Behavior at risk: A change outside the globbed folder that the companion includes (for example a shared
  header) would need adding to the dependency list; check src/discord's includes first.
- Estimated saving: Per incremental build, two MSBuild no-op invocations (typically a few seconds each; not
  measured). Local CPU only.
- Confidence: medium

### F-TOOLS-15: Every build compares 228 MiB of selection art, half of which never ships
- File: CMakeLists.txt:933
- File: CMakeLists.txt:927
- Category: overhead
- Severity: P3
- Claim: The `SelectionAssets` custom target runs `copy_directory_if_different` over `assets/selection`
  (5,861 files, 228 MiB) on every build; `copy_if_different` compares sizes and then file contents, so a
  no-change build reads both copies. 958 of those files (123 MiB) are the masking photographs
  (`color-N.jpg/png`) that the install rule at 927-930 and package-team.ps1:68 exclude because the runtime
  never reads them. The build folder therefore carries 123 MiB that the product does not use and pays for
  comparing it each build.
- Proposed change: Replace the selection line with a small `cmake -P` script (or a configure-time
  `file(GLOB_RECURSE … CONFIGURE_DEPENDS)` list) that mirrors the install rule's filter, skipping
  `color-[0-9]+\.(png|jpg)$` and copying with `copy_if_different` per file. Keep the brand/input/flags lines.
- Behavior preserved: A launch from the build folder sees the same files a package has.
- Behavior at risk: A developer workflow that opens the photographs from the build folder (none found;
  UiRenderTest and SelectionArtTest read `assets/selection` from the source tree).
- Estimated saving: Payload: 123 MiB of photographs (about 246 MiB of reads per no-change build when the
  comparison reads both sides). Interval: every build. Transport: local disk. Also 123 MiB of build-folder disk.
- Confidence: medium

### F-TOOLS-16: The source fingerprint is recomputed about 7 times per Nightly publish
- File: scripts/NightlyOperations.ps1:462
- File: scripts/NightlyOperations.ps1:548
- File: scripts/NightlyOperations.ps1:554
- File: scripts/NightlyOperations.ps1:592
- File: scripts/build-current.ps1:22
- File: scripts/build-current.ps1:67
- File: scripts/BuildProvenance.ps1:2
- Category: overhead
- Severity: P3
- Claim: `Get-SourceFingerprint` hashes every tracked and untracked file with one `Get-FileHash` cmdlet call
  per file (7,650 files, 298.6 MiB; measured 2.0 s warm, 3.0 s cold). A Nightly run calls it at the
  snapshot (462), before and after the build (build-current 22, 67), in Assert-NightlyBuildSnapshot after the
  build (548), inside package-team's Assert-BuildReceipt, in Assert-NightlyBuildSnapshot after packaging
  (554) and in Assert-NightlySource (592). Most placements are deliberate (NIGHTLY.md; AGENTS.md requires
  packaging to check a fresh fingerprint). The call at 554 is redundant: package-team fingerprinted seconds
  earlier and Assert-NightlySource at 592 fingerprints again before the pending record.
- Proposed change: Keep every boundary check. At 554, compare only the packaged receipt and HEAD (pass a
  `-SkipFingerprint` to the receipt check, or split `Assert-BuildReceipt` into its fingerprint and
  binary-hash halves) since 592 re-establishes freshness. Optionally implement `Get-SourceFingerprint` with
  `[IO.File]::OpenRead` + `SHA256.HashData` in one loop (no per-file cmdlet or `Test-Path`), producing the
  identical serialization.
- Behavior preserved: Fingerprint value and serialization (ordinal paths, uppercase hex, WORKSPACE_TARGET
  line); every AGENTS.md packaging check.
- Behavior at risk: none identified for the 554 trim; the faster implementation must keep `DELETED` lines and
  uppercase hex exactly (test-build-provenance.ps1 pins the format).
- Estimated saving: About 2-3 s per removed call, roughly 15-20 s of a multi-minute job overall if the
  cmdlet overhead dominates. Small; listed so nobody re-proposes removing the deliberate ones.
- Confidence: high

### F-TOOLS-17: vcvarsall runs three times in one Nightly process and grows PATH each time
- File: scripts/BuildEnvironment.ps1:55
- File: scripts/build-current-dependencies.ps1:8
- File: scripts/build-current.ps1:24
- File: scripts/package-installer.ps1:39
- Category: overhead
- Severity: P3
- Claim: `Enter-EmberVcEnvironment` runs `vcvarsall.bat` and copies its environment into the process. In one
  publish-nightly process it is called by build-current-dependencies (same process, `&`), again by
  build-current, and again by package-installer. vcvarsall prepends to the existing PATH/INCLUDE/LIB each
  time, so the third call sees doubled entries; vcvarsall is known to fail with "input line is too long" once
  PATH grows enough. Prior art: the 2026-09 review introduced `Enter-EmberVcEnvironment` to replace three
  copies of the import (docs/reviews/2026-09-code-quality-review.md:274); this builds on it.
- Proposed change: Make `Enter-EmberVcEnvironment` idempotent: return early when `$env:VSINSTALLDIR` equals
  the requested installation and `$env:VSCMD_ARG_TGT_ARCH` equals the architecture.
- Behavior preserved: Same compiler environment for every step.
- Behavior at risk: none identified (all three calls request the same installation and x86).
- Estimated saving: Two vcvarsall runs per Nightly (a few seconds each) and the PATH duplication.
- Confidence: medium

### F-TOOLS-18: ember-rooms setup.sh re-implements the supervisor's config in 13 inline Python snippets
- File: server/ember-rooms/deploy/setup.sh:154
- File: server/ember-rooms/deploy/setup.sh:244
- File: server/ember-rooms/deploy/setup.sh:298
- File: server/ember-rooms/deploy/setup.sh:380
- File: server/ember-rooms/src/config.rs:48
- Category: judo
- Severity: P3
- Claim: setup.sh (595 lines) shells out to `python3` 13 times, including a 42-line config sync heredoc, to
  read and edit config.json, and restates the supervisor's defaults (`bind` 127.0.0.1:47830 three times,
  `max_rooms` 8, `port_range` [45800, 45899]) that `server/ember-rooms/src/config.rs` owns. A default changed
  in Rust silently diverges from the installer's view of an unset field (for example `wait_reloaded` comparing
  against the wrong default).
- Proposed change: Add subcommands to the ember-rooms binary that setup.sh already stages next to itself
  (`$SRC/bin/ember-rooms`): `config get <path> <field>` (resolved with defaults), `config sync --builds …
  --prune --max-rooms N` (the SYNC heredoc) and `config check-ports`. setup.sh calls the staged binary, so the
  installed version's own config model applies. Keep the bash for files, systemd and rollback.
- Behavior preserved: Same config edits, same refusal points, same rollback.
- Behavior at risk: setup.sh would execute the staged binary before installing it (today it only checks it
  exists and is ELF), so a binary built for the wrong glibc fails earlier, which is arguably better;
  test-setup.sh needs a stand-in binary that answers the new subcommands.
- Estimated saving: About 70 lines of embedded Python and one set of duplicated defaults.
- Confidence: medium

### F-TOOLS-19: Three deploy scripts carry three different backup/rollback and nginx-include implementations
- File: server/ember-reports/deploy/setup.sh:50
- File: server/ember-short/deploy/setup.sh:79
- File: server/ember-rooms/deploy/setup.sh:200
- File: server/ember-reports/deploy/setup.sh:90
- File: server/ember-short/deploy/setup.sh:106
- Category: spaghetti
- Severity: P3
- Claim: ember-reports backs up an indexed `targets` array and restores it in an EXIT trap; ember-short has a
  hand-written `restore()` listing four paths plus `unstage()`; ember-rooms has a bundle with `had_*` flags and
  `put_back`. Injecting an `include snippets/…;` line into the nginx site is done twice with different
  algorithms: a Python brace-counting search for the server block (reports) and an awk insert before the first
  `location / {` (short).
- Proposed change: A shared `server/deploy-lib.sh` with `backup_targets`/`restore_targets` (the reports
  pattern, which is the most general) and one `nginx_include_once <site> <line>` helper; each setup.sh copies
  or sources it from beside itself (they are staged as folders, so ship the file with each). Keep each
  service's own steps.
- Behavior preserved: Same files changed, same refusal and restore semantics per service.
- Behavior at risk: The two nginx insertion rules pick different places today; unifying them must keep each
  site's include in the same server block (verify with `nginx -T` diff on the VPS).
- Estimated saving: About 60 lines; one rollback model.
- Confidence: medium

### F-TOOLS-20: idle-close.py polls the supervisor every 30 s for a rule the supervisor could own
- File: server/ember-rooms/deploy/ember-rooms-idle.timer:6
- File: server/ember-rooms/deploy/idle-close.py:100
- File: server/ember-rooms/src/config.rs:38
- Category: judo
- Severity: P3
- Claim: A systemd timer starts a Python process every 30 s that GETs `/rooms` (with details) over loopback,
  keeps per-room "quiet since" state in a JSON file, and DELETEs rooms that had at most one member and no table
  for 15 minutes. The supervisor already enforces the neighbouring rule itself (`empty_close_secs`) and has the
  member and table counts in memory. The script samples, so a member who comes and goes between polls is
  missed (its own docstring says so). Prior design: server/ember-rooms/deploy/README.md:188-208 runs it as a
  separate throwaway user that only gets the config and the secret; the supervisor already holds both, so
  moving the rule inside grants nothing new.
- Proposed change: Add `lone_close_secs` (default 900, 0 to disable) to the supervisor config next to
  `empty_close_secs` and close lone rooms in the same place empty ones are closed. Delete idle-close.py,
  install-idle-close.sh, ember-rooms-idle.service/.timer and the state directory.
- Behavior preserved: The closing rule (one member at most, no table playing, 15 minutes) and the close path.
- Behavior at risk: Timing becomes exact instead of 30-second sampled, so a brief second member now resets
  the clock where the script would have missed it. Not verified whether server1's Docker deployment
  (ember-rooms-docker.service) runs the timer too.
- Estimated saving: Payload: GET /rooms JSON with details, roughly 300-600 bytes per room (about 15-25 KB at
  45 rooms). Interval: 30 s. Fan-out: one host. Transport: loopback HTTP. About 2,880 Python process starts a
  day (tens of ms CPU each, so a few CPU-minutes a day) and about 60 MB/day of loopback reads; about 250 lines
  deleted.
- Confidence: medium

### F-TOOLS-21: NightlyOperations.ps1 and its 761-line test are single linear units
- File: scripts/NightlyOperations.ps1:428
- File: scripts/test-nightly-publication.ps1:1
- File: scripts/NightlyOperations.ps1:21
- File: scripts/NightlyOperations.ps1:441
- File: scripts/NightlyOperations.ps1:524
- Category: size
- Severity: P3
- Claim: NightlyOperations.ps1 (619 lines) mixes four concerns: command plumbing (4-18), GitHub release
  state machine and retention (37-390), room-host staging (182-226) and preparation/orchestration in one
  190-line `Invoke-NightlyPublish` (428-619) that interleaves validation, git, notes text, Linux build and
  pending-record construction. test-nightly-publication.ps1 is one 761-line script with home-grown
  `Check`/`Reject`/`Pass` and a single try block, so a failure reports the first broken assertion and stops.
  The Nightly repository literal appears in `Assert-NightlyRepository` (21), again at 441, and in
  build-target.json; the tag regex is in `Test-NightlyTag` (27) and again at 524 without the `v`.
- Proposed change: Split into `NightlyGitHub.ps1` (query, tag state, publish, retention),
  `NightlyHosts.ps1` (destinations, staging) and keep `NightlyOperations.ps1` for preparation, with
  `Invoke-NightlyPublish` delegating to `New-NightlyNotes`, `Build-NightlyRoomHosts` and `New-NightlyPending`.
  Split the test along the same seams (one file per module, run from the CTest entries of F-TOOLS-6). Keep the
  repository literal only in `Assert-NightlyRepository` and derive the label regex from `Test-NightlyTag`.
- Behavior preserved: Identical operation order, state files and retries (the existing faked end-to-end
  scenarios are the regression check).
- Behavior at risk: none identified.
- Estimated saving: No line reduction; each file under 300 lines with one concern.
- Confidence: high

### F-TOOLS-22: Configure-time cost: font hex embedding and a forced re-find of every package
- File: cmake/sf4e_embed.cmake:4
- File: cmake/sf4e_fonts.cmake:14
- File: scripts/build-current.ps1:49
- Category: overhead
- Severity: P3
- Claim: build-current reconfigures on every run. Configure converts 2.7 MB of fonts (two Inter weights and
  three CJK subsets) into hex with two `string(REGEX REPLACE)` passes over a 5.5 MB string, then writes an
  EmbeddedFonts.hxx of roughly 17 MB, every configure. The configure line also passes `-U*_DIR` and
  `-UZLIB_*`, discarding every package location so each configure re-runs all `find_package` searches; this
  guards against a cache from another checkout, which line 52 checks separately.
- Proposed change: Generate EmbeddedFonts.hxx (and the brand embeds) in a build-time `add_custom_command`
  that runs a `cmake -P` script with the fonts as `DEPENDS`, so configure no longer reads them and the header
  is regenerated only when a font changes. Replace the blanket `-U*_DIR` with a check that each cached
  `*_DIR` lies under this checkout's build or dependency root, unsetting only those that do not.
- Behavior preserved: Same header bytes and the same cross-checkout protection.
- Behavior at risk: The hash checks in `sf4e_embed_font` move to build time; a wrong font then fails the build
  instead of the configure.
- Estimated saving: Not measured; likely a few seconds per build-current run. Low priority.
- Confidence: low

## Area summary
Overall the tooling is careful about provenance and recovery (pinned inputs, receipts, read-back publication,
token-scoped staging), and it is well commented, but the structure lags the rigor: packaging scripts talk
through dot-sourced globals, the same hash/sidecar/clean-tree/version primitives are re-implemented across
four scripts, Stable and Nightly have two publishers, and the room host, SDK and deploy scripts each keep a
hand-copied mirror of something another layer owns. Biggest judo move: turn packaging into one module that
returns objects (F-TOOLS-1/2) and let Stable publish through the Nightly read-back state machine (F-TOOLS-5),
which removes the script globals, a second publisher and most duplicated preconditions. Biggest boundary
items: the SDK's hand-kept wire types have drifted from server/ember/protocol with no parity test, though the
drift is latent for current SDK calls (F-TOOLS-7/8), Stable publication has no channel guard (F-TOOLS-3),
and Nightly's documented version rule is neither enforced nor true at head (F-TOOLS-4).
Biggest overhead items are build-side only: npm ci on every test run (F-TOOLS-13) and 123 MiB of unshipped
photographs compared each build (F-TOOLS-15). No hot-path recurring network sends live in this area; the
"large JSON every 15 seconds" is not in scripts, sdk, cmake or deploy (the only recurring job here is the
30-second loopback idle-close poll, F-TOOLS-20). Not assessed: actual timings of npm ci, MSBuild no-ops and
configure (no builds allowed), the updater's version ordering for F-TOOLS-4, and whether server1's Docker
deployment runs the idle timer.
