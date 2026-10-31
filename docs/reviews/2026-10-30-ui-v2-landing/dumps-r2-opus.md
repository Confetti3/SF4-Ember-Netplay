# Crash dump upload review, round 2 (bdb5bd50 and 0fc71347)

Scope: `git diff bdb5bd50~1..0fc71347` over scripts, server, src/platform, src/netplay, the four named tests, docs and
locales, judged against dumps-r2-prompt.md. HEAD is 0fc71347 and none of the files read from the checkout have
working-tree changes. An uncommitted edit to src/netplay/PlayerPreferences.hxx appeared during the review (the
separate task in progress); it was not considered, and that file's change was read from git only.

## Checks run

- `cargo test --locked` in WSL for server/ember-reports: every suite passed, including the new
  `workers_fit_the_service_memory_budget` and `symbol_file_at_the_cap_symbolicates_within_the_worker_memory_limit`.
  The review target dir was deleted afterwards.
- `python3 tests/deploy_setup.py` in WSL: 9 tests OK, including the invalid second entry, the hand-edited manifest
  and the shared-dependency interleaving with an expired Nightly manifest.
- `pwsh scripts/test-build-provenance.ps1`, `test-nightly-publication.ps1` (42 of 42 groups),
  `test-nightly-paths.ps1`, `test-package-files.ps1`: all pass.
- `ctest --test-dir build\current -R "ProblemReport|SettingsStore|ApplicationServices"`: 3 of 3 passed. The
  binaries were linked at 23:48, the commit is 23:53; they appear to be from the same tree, but I did not rebuild.

## Round 1 findings

Opus round 1:

1. Workers against the memory budget: resolved. worker.rs derives `MAX_WORKERS` from `SERVICE_MEMORY_BYTES`,
   `INTAKE_MEMORY_BYTES` and `MEMORY_BYTES`; `Config::validate` refuses more; setup.sh stops on a kept config.json
   with more; tests/config.rs ties the constant to the unit's `MemoryMax`, the example config and setup.sh.
2. Symbol upload hidden in Package: resolved. Both Package callbacks are one line, `Assert-ReceiptSymbols` runs
   right after the build check in both orchestrators, and the upload is the Symbols step of
   NightlyOperations.ps1:Complete-EmberStaging, after the record is saved, so a resume repeats it.
3. The `settled` lambda: resolved in shape. ReportWorkflow.cxx:Build and :Send are one path for the automatic and
   the preview sends, and a failed dump-free build leaves the first answer standing. The new classification has
   its own problem (finding 1 below).
4. install-symbols.sh atomicity and locking: resolved. `commit` validates every entry before any move, holds
   `flock` on the store from the existence checks through the moves, the manifest and the prune, and the module
   pattern requires an alphanumeric first character.
5. Second copy of the staged-upload protocol: resolved. NightlyOperations.ps1:Invoke-EmberStagedUpload serves the
   room hosts and the symbols and keeps the `$stageError` guard; the fixture with a failing cleanup after a failing
   commit proves the commit error is the one thrown.
6. Old consent key: resolved as a recorded deferral. BoolPreferences.hxx and SettingsStore.cxx name the shared
   `%APPDATA%\sf4e` reason and the Stable release that retires it.
7. Limit constants coupled only by comments: resolved by the Linux test in tests/event.rs.
8. Lower items: resolved. `New-EmberSymbols` sends dump_syms output to the host; SymbolPublication.ps1 keeps a
   per-label archive in dist.

Sol round 1:

1. Upload inside packaging, no recovery: resolved, with fixtures for a symbol upload that fails once on both
   channels and completes on the next run without rebuilding or repackaging.
2. Installer not atomic across publishers: resolved (Opus 4).
3. No local archive: resolved. `Save-EmberSymbolArchive` pins the archive in the record, `Expand-EmberSymbolArchive`
   checks it, and upload-symbols.ps1 uploads from it without reading the build folder.
4. Timeout after a status line: addressed, but the rule it led to is wrong for this transport; see finding 1.
5. Generator not pinned: resolved. `$EmberDumpSymsVersion` is checked in `Get-EmberDumpSyms` before configure and
   again in `New-EmberSymbols`, and the fixture refuses 2.3.8.

## Findings, in priority order

### 1. src/platform/ReportWorkflow.cxx: Classify and DumpInTheWay resend reports the intake already holds

`DumpInTheWay` now returns true for `Outcome::TimedOut` whatever the status and for `Outcome::Incomplete` (a 202
whose body did not arrive whole), and `Send` then uploads the same report again without the dump. Two facts in the
code show that, once a status line exists, the dump cannot have been in the way:

- src/common/sf4e__NetUtil.cxx, the `exchange` lambda in the bounded request: `statusCode` is assigned only after
  every body chunk was written and `transport.Receive` succeeded. A nonzero status with `HttpErrorKind::Timeout` or
  `ReceiveFailed` therefore means the whole multipart, dump included, reached the server, and what timed out was
  reading a receipt under 16 KiB.
- server/ember-reports/src/lib.rs, the intake handler: 202 is returned only after `store.reserve(..).commit(..)`
  made the event and its minidump durable, and nothing in the service deduplicates by report or attempt.

So `answer(202, Timeout)` and `answer(202, ReceiveFailed)`, both pinned as retries in report_workflow_tests.hxx,
produce two stored reports: the first with the dump, the second without. Bugsink gets a duplicate event, the
per-address intake allowance is spent twice, and the local Sent reports entry says `dump=false` with the second id
while the server also holds the first report with the dump. For other statuses that time out on the body
(`answer(500, Timeout)` in the tests, or a 400 or 503) the server decided after reading everything, so the second
upload is pointless, and the comment "without the dump the same report may still get through" does not hold.
Round 1 Sol 4 assumed a timeout after the status line could be the dump; with this transport it cannot.

Remedy: decide by the status whenever one arrived. In `Classify`, return `TimedOut` only for `statusCode == 0` with
`HttpErrorKind::Timeout`; a non-ok 202 is `Incomplete`; any other status is `Refused` (or `RateLimited`). In
`DumpInTheWay`, return true only for `TimedOut` and for `Refused` with 408 or 413, so `Incomplete` ends like the
existing `no_id` case: one upload, phase Failed, and the record `Status::Received` with reason "incomplete" rather
than the current `Status::NotSent`, because the intake holds that report. (The day's allowance counts every entry by
time whatever its status, ProblemReport.cxx:AutomaticAllowed, so either status is safe for the limit; Received is the
one that tells the Sent reports list the truth.) Move the `(202, Timeout)`,
`(202, ReceiveFailed)` and `(500, Timeout)` entries from the retry loop to the no-retry loop and keep the
`(429, Timeout)` case. While there, let `Upload` return the `Outcome` it computed so `Send` switches on that instead
of classifying the same answer a second time. If a receipt after a broken-off 202 is wanted later, that needs an
idempotency key in the multipart (the record's `attempt` is already a suitable value) and deduplication in the
store; that is a follow-up, not part of this change.

### 2. scripts/SymbolPublication.ps1: store-wide policy travels inside every archive

`Save-EmberSymbolArchive` puts install-symbols.sh in each archive and `Publish-EmberSymbols` runs that copy. For a
retry inside one publication that is right, the same way the room-host staging script is pinned. But
install-symbols.sh is not per-release logic: it owns the store's lock, the manifest naming and the 45-day prune over
every release's manifest. upload-symbols.ps1 exists to restore the store from old archives, and each such replay runs
the prune rules of the release that made the archive. Today every archive has the same rules; the first change to
the manifest layout or the prune makes old archives unsafe to replay, and nothing would say so. The incoming folder
is also spelled twice (`$EmberSymbolStore/.incoming-$Channel-$Label/` in `Publish-EmberSymbols` and `incoming=` in
the script), although `prepare` already prints the path it made.

Remedy, smallest first: give `Publish-EmberSymbols` the installer text as a parameter, pass the archived copy from
`Complete-EmberStaging` and the checkout's current install-symbols.sh from upload-symbols.ps1, so a manual restore
always runs today's store policy. Use the path `prepare` prints as the scp destination instead of rebuilding it.
Not blocking.

### 3. Lower items

- Records in flight across this change: a Stable `release-pending-*.json` written by the old code has phase
  `staged` and no `symbols`, so `Complete-EmberStaging` returns and the release publishes with no Symbols step; an
  old Nightly record at `prepared` fails with "must include its symbol archive" and has no way forward except
  removing it. This matters only for a publication interrupted across the update; one sentence in NIGHTLY.md is
  enough.
- scripts/test-nightly-publication.ps1 was 1044 lines before bdb5bd50 and is 1165 now. It did not cross the
  threshold in this change, but the Stable and symbol groups are natural candidates for their own file the next time
  it grows.

## What holds up

The publication model is now one shape for both channels: prepared (every local file pinned, symbol archive
included), staged (room hosts and symbols confirmed by `Complete-EmberStaging`), published. Both orchestrators refuse
a build without symbols before packaging, and Stable also checks that the receipt it archived is the one it
packaged. `Invoke-EmberStagedUpload` is a real shared helper rather than a copy. The archive is hashed once and read
from memory, so the check and the use cannot diverge. The installer's lock covers exactly the relationship between
folders and manifests, and the interleaving test exercises it. On the server, the worker bound is now a computed
constant that a test ties to the unit file, the example config and setup.sh. On the client, `Build` and `Send` remove
the round 1 lambda and give the preview the same fallback; only the classification feeding the fallback needs the fix
above. No file in scope crosses 1000 lines in this change (NightlyOperations.ps1 is 874).

VERDICT: CHANGES REQUIRED
