# Crash dump upload review, round 1 (bdb5bd50)

Scope: commit bdb5bd50 "Crash reports carry the small dump; symbols go up with every publish", judged against
dumps-r1-prompt.md and CRASH_DUMP_UPLOAD_DESIGN_2026-10-31.md. The working tree matches the commit for every file
in scope (src/platform, src/netplay, scripts, server, locales).

## Checks run

- `cargo test --locked` in WSL for server/ember-reports: every suite passed (lib 12, config 3, delivery 3, event 8,
  intake 11, isolation 2, storage 5). The review target dir was deleted afterwards.
- `python3 tests/deploy_setup.py` in WSL: 7 tests OK, including the three new SymbolInstall cases.
- `pwsh scripts/test-build-provenance.ps1`: all PASS, including the two new symbol cases.
- `ctest --test-dir build\current -R "ProblemReport|SettingsStore|ApplicationServices"`: 3 of 3 passed (binaries
  built 22:39 to 22:42, before the 23:14 commit, from the same tree).
- WSL path handling: `wslpath -a 'C:\Users\Kate\some dir\dependencies/x86-windows-wchar-filenames/bin\GGPO.dll'`
  returns `/mnt/c/Users/Kate/some dir/dependencies/x86-windows-wchar-filenames/bin/GGPO.dll`, so spaces and mixed
  separators survive. dump_syms 2.3.9 is installed in WSL; `dump_syms -s <scratch> zlib1.dll` printed nothing on
  stdout and wrote `zlib.pdb/<id>/zlib.sym` with 175 FUNC records (the sibling PDB was found). Scratch removed.
- Not checked: the zh-Hans and ja strings against the UI atlas (UiRenderLocales not run), anything on the VPS.

## Findings, in priority order

### 1. server/ember-reports/src/config.rs: Config::validate still accepts four workers

`MEMORY_BYTES` (worker.rs) went from 64 MiB to 512 MiB, eight times, while the cgroup went from 512M to 1536M, three
times. The new comments in worker.rs, the README and ember-reports.service all rest on "two workers at this limit",
but only the defaults changed: `validate` still has `|| l.workers > 4`. setup.sh keeps an existing
/etc/ember-reports/config.json, which was written from the old example with `"workers": 4`, so the deployed service
will most likely start with four children of 512 MiB address space each inside a 1536M cgroup, and nothing says so.
RLIMIT_AS is address space, not resident memory, so this is not an immediate OOM, but the invariant the commit
documents is not the one the code enforces, and a cgroup OOM kills intake along with the walk.

Remedy: tie the bound to the constant instead of a comment. Lower the bound to 2 (or validate
`workers as u64 * MEMORY_BYTES` against one declared budget constant that the unit file comment quotes), add a
`tests/config.rs` case that 3 is refused, and add the `workers` edit to the deploy README's upgrade steps so a stale
config fails startup loudly instead of overcommitting silently.

### 2. scripts/publish-nightly.ps1 and scripts/github-release.ps1: the symbol upload is hidden inside the Package operation

NightlyOperations.ps1 states its own contract in its header: "Invoke-NightlyPublish owns preparation and phase
ordering; the entry script supplies the lock, transcript and the build/package operations." This commit puts a
network side effect (ssh, scp, a commit on the VPS) inside the `Package` scriptblock of both entry scripts, as two
copies of the same three lines. Consequences that follow from the code as written:

- The orchestrators do not know the step exists, so test-nightly-publication.ps1 cannot cover its ordering, its
  dry-run behaviour or its failure handling. Nightly's `-WhatIf` guard works only because the scriptblock resolves
  `$WhatIf` through dynamic scope.
- Nightly uploads before the installer, the room-host build, the packaged-revision check and `Assert-NightlySource`.
  A run that fails after packaging leaves a manifest on the VPS for a label that was never published, and the retry
  takes the next label and uploads again.
- A build without symbols is discovered late. For Stable, `Invoke-StablePublish` repackages first and only then
  learns, inside Package, that the receipt has no symbols; nothing at the top of either orchestrator asks.
- upload-symbols.ps1 runs a full `Assert-BuildReceipt` (fresh fingerprint) right after New-EmberPackage validated
  the same receipt.
- The resume paths (a pending Nightly record, a pending Stable draft) skip Package, so they also skip the upload.
  Today that is harmless only because the upload happens before the pending record exists; the moment the order
  changes it becomes a silent gap.

Remedy: make it an explicit operation and phase. Add a `Symbols` operation (`param($Label)`) to both operation
tables, call `Assert-ReceiptSymbols` on the receipt right after the build is checked (Invoke-NightlyPublish next to
`Assert-NightlyBuildSnapshot`, Invoke-StablePublish before `& $Operations.Package`), and invoke the upload at one
named point: for Nightly inside the `!$WhatIf` block after `Assert-NightlySource` (or as a phase of
Complete-NightlyPublication so a resumed run retries it), for Stable immediately before `Publish-EmberRelease`. Then
add the operation to the fakes in test-nightly-publication.ps1 and assert it is skipped by `-WhatIf` and runs once.
The Package scriptblocks go back to one line each.

### 3. src/platform/ReportWorkflow.cxx: ReportWorkflow::Execute, the `settled` lambda

The fallback works and the tests pin it, but it is a special case grafted onto the automatic branch rather than one
flow. `settled` returns one bool that means three things (the build was refused, the dump was in the way, the send is
settled), and the retry is sequenced by the short circuit in `if (!settled(true) && !settled(false))`. A reader has
to simulate the lambda twice to see that the second call can only return false when the dump-free build fails. Two
consequences of that shape:

- The preview path now also sends the dump by default (`Submission::includeDump = true`), yet it has neither the
  build fallback nor the 408/413/timeout fallback, and it computes `record.dump` with its own copy of
  `includeDump && !minidump.empty()`. A player on a slow uplink who sends from the preview gets "reports.failed",
  spends one of the day's sends, and is not told that turning Small dump off would help. The design scoped the
  retry to the automatic branch, so this is a consistency question rather than a deviation, but the commit is what
  made the preview path a dump path.
- If the first upload was answered 408 and the dump-free build then fails, the branch records `notSent("invalid")`
  for a report that was already uploaded once. Practically unreachable (it needs the log text to contain the second
  random boundary), but the bool cannot express it correctly.

Remedy: one helper in the anonymous namespace, for example
`std::optional<Record> Send(const std::shared_ptr<const Report>& report, bool withDump, const Record& attempt,
WorkflowState& state, cancelled, dependencies)`, which builds with the dump when asked and present, falls back to a
build without it, uploads, and repeats once without the dump when `DumpInTheWay`; nullopt means nothing could be
built. Both the automatic branch and the Submission branch call it (the preview passes `submission.includeDump`);
the lambda, the short-circuit sequencing and the second `record.dump` formula disappear. Sending less than the
player approved is always within consent, and the Sent reports list already tells the truth through `record.dump`.

### 4. server/ember-reports/deploy/install-symbols.sh: commit is not all-or-nothing and is not locked

- Validation and `mv` are interleaved in one loop. An invalid entry after some valid ones exits 4 with those
  modules already moved into the store and no manifest written. Prune deletes only what manifests list, so these
  folders stay forever.
- There is no lock. A Stable and a Nightly commit running together can interleave so that one keeps an
  already-present shared module (the vcpkg DLLs keep their DEBUGID across builds, for example `GGPO.pdb/<id>`) and
  lists it, while the other's prune has already computed `kept` without that new manifest and deletes the folder.
- `module_pattern` admits a first component of `..` or any leading dot. Manifests are only written from glob
  output, which never yields those, so this is hardening rather than a live hole, but the prune `rm -rf` is guarded
  by that pattern alone when a manifest is edited by hand.

Remedy: two passes in `commit` (collect and validate every entry, failing before any move; then move), wrap commit
and prune in `exec 9>"$manifests/.lock"; flock 9` (katie owns that directory, so no root is needed), and require
the first component to start with an alphanumeric character. Add a deploy_setup.py case where the second of two
entries is invalid and assert the store stays empty.

### 5. scripts/upload-symbols.ps1: a second copy of the staged-upload protocol, missing its error guard

`Invoke-NightlyHostStage` (NightlyOperations.ps1) already implements "send a script over ssh stdin, prepare, scp into
an incoming folder, commit, always clean up". upload-symbols.ps1 re-implements it, and drops the part that keeps the
real failure visible: in the host stage, a cleanup failure after a failed commit is downgraded to a warning
(`$stageError`); here the `finally` runs `& $ssh cleanup`, and if that throws it replaces the scp or commit error the
operator needed to see.

Remedy: extract one helper (script text, remote argument string, local paths, incoming folder) used by both, or at
minimum carry the `$stageError` guard over. This also fits finding 2: once the upload is an orchestrated operation,
it sits next to the host stage and can share its plumbing.

### 6. src/netplay/BoolPreferences.hxx: the old consent key is never removed

The rename itself is the one line the owner decided on, and SendReportsKey reads the key from the table, so
ReportsOn and TurnReportsOn follow it. But nothing removes `sendProblemReports` from settings.json: the new
report_workflow_tests.hxx `Consent` case asserts that it is still there after Always send, and the
problem_report_test.cxx comment "and never written again" overstates (it is not written, but the store keeps it).
SettingsStore.cxx `ObsoleteKeys` / `RetireLegacy` is the canonical retirement path.

There is a real reason to wait: `SettingsStore::DefaultDirectory()` is `%APPDATA%\sf4e` for every channel, so
retiring the key in Nightly would also switch off a dual-install player's Stable (logs-only) consent. Remedy: say
that in the PlayerPreferences.hxx comment now, and add `sendProblemReports` to `ObsoleteKeys` in the Stable release
that ships this change, flipping the `Consent` assertion at the same time.

### 7. server/ember-reports/tests/event.rs: the limit constants are coupled only by comments

The new test proves a file just over the old 8 MiB cap loads and one over `SYMBOL_BYTES` is refused before it is
read. Nothing proves that a walk near the new caps fits the new `MEMORY_BYTES`; that is asserted only in prose ("a
60 MiB file near 180 MiB"). The test worker runs the real binary with the real RLIMIT_AS, so the check is cheap.
Remedy: a Linux-only case that writes a store file a little under `SYMBOL_BYTES` (or files that sum to near
`TOTAL_SYMBOL_BYTES`) with the existing `launcher_symbols` padding helper and asserts `symbolication() == "ok"`, so
raising a cap without the memory limit fails a test.

### 8. Lower items

- scripts/BuildProvenance.ps1: New-EmberSymbols. `$modules = foreach (...)` collects every uncaptured output of the
  loop body, and the `$DumpSyms` runner does not discard the tool's stdout. dump_syms 2.3.9 prints nothing in store
  mode today (measured above), but any future stdout line would land in the receipt's `modules` and then fail
  Assert-ReceiptSymbols with a confusing message. Pipe the tool call to `Out-Host` inside the runner.
- No local copy of a published store. The design asked to keep `$build\symbols` per published label (for example
  zipped beside the package in dist). Today the next `-Symbols` build replaces it and the PDB is gone, so for Stable
  the VPS store is the only copy. Zip it in the Package step or in the new Symbols operation.

## What holds up

The consent reset is genuinely one line because the key comes from the BoolPreferences table. Get-EmberDumpSyms runs
before configure, so a missing WSL or dump_syms stops a `-Symbols` build before it compiles. New-EmberSymbols checks
each PDB-side DLL against the staged hash in the receipt and refuses a symbol file without FUNC records, and
Assert-ReceiptSymbols refuses a receipt without Sidecar or a store that lost a module. install-symbols.sh installs
each module by a rename inside the store's filesystem, works with the setgid ember-symbols store that setup.sh
creates, and needs no root. The intake tests now take worker and queue counts from `Limits::default()` instead of
literals. All 14 catalogs change the same five msgids.

VERDICT: CHANGES REQUIRED
