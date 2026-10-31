# Crash dump upload review, round 3 (bdb5bd50, 0fc71347 and 0309bd07)

Scope: `git diff bdb5bd50~1..HEAD` over scripts, server, src/platform, BoolPreferences.hxx, SettingsStore.cxx, the
four named tests, docs/development, SAVING_LOGS.md and locales, judged against dumps-r2-prompt.md. HEAD is
0309bd07; the commit between it and 0fc71347 (a27ad22c, room settings) touches none of these paths. None of the
files read from the checkout have working-tree changes.

## Checks run

- `ctest -R "ProblemReport|SettingsStore|ApplicationServices"` on build\current: 3 of 3 passed. The binaries were
  not rebuilt for this review; ApplicationServicesTest.exe (which includes report_workflow_tests.hxx) contains the
  new assertion text `state.record->status == Status::Received && state.record->reason == "incomplete"` and not the
  old `Status::NotSent ... "incomplete"` one, so it was built from the round 3 test.
- `pwsh scripts/test-nightly-publication.ps1`: 43 of 43 groups, including the new manual-upload group.
  `test-build-provenance.ps1`, `test-nightly-paths.ps1`, `test-package-files.ps1`: all pass.

## Round 2 findings

1. Classify and DumpInTheWay resent reports the intake already held: resolved. ReportWorkflow.cxx:Classify now
   decides by the status line once one exists: 429 is RateLimited, 202 is Incomplete, 408 and 413 are
   BodyRefused, any other status is Refused, and only status 0 splits into TimedOut or NoConnection. Upload records
   Incomplete as `Status::Received`, reason "incomplete", no id, phase Failed with the "may already have accepted"
   message, and returns the Outcome; Send switches on that Outcome, and DumpInTheWay is true only for TimedOut and
   BodyRefused. The `(202, Timeout)`, `(202, ReceiveFailed)` and `(500, Timeout)` cases moved to the no-retry loop,
   `(503, Timeout)` is now a refusal, and the preview test pins the Received record without an id. This matches
   the transport: in sf4e__NetUtil.cxx the exchange sets `statusCode` only after every body chunk was written and
   `Receive` succeeded, and PostReport marks a non-ok 202 only when the body did not arrive whole.
2. Store-wide policy inside every archive: resolved. Publish-EmberSymbols takes `$Installer`;
   Complete-EmberStaging passes Get-EmberArchivedSymbolInstaller (one release, one installer across retries),
   upload-symbols.ps1 passes Get-EmberSymbolInstaller of the checkout, and the header, the script comment and
   NIGHTLY.md say why. The store path is no longer spelled in PowerShell: Invoke-EmberStagedUpload takes the folder
   install-symbols.sh prints from `prepare`. Both installer reads go through Read-EmberSymbolArchive, which checks
   the pinned SHA-256, so reading the archive twice on the publication path cannot let the two reads diverge. The
   new fixture installs an old archive with a different current installer, checks the archive is unchanged, runs
   the archived installer for the publication case, checks the scp destination is the printed folder and rejects
   a wrong installer.
3. Lower items: the in-flight sentence is in NIGHTLY.md (resolved). test-nightly-publication.ps1 is 1189 lines; it
   was 1044 before bdb5bd50, so these commits did not cross the threshold, but the symbol and Stable groups remain
   the natural split the next time it grows.

## Findings, in priority order

None blocking.

### 1. src/platform/ReportWorkflow.cxx:SamePreview: a report the intake already holds can be sent again from the preview

After an Incomplete 202 (and, as before, after a 202 with no valid id) the record says Received but `state.id` is
empty. SamePreview accepts a new Submission for that preview whenever `state.id.empty()`, and
src/ui/ProblemReportPanel.cxx:Rows keeps the Send row enabled on the same test. One more click therefore makes
exactly the intake duplicate that round 2 removed from the automatic path, and the guide's "nothing is sent again"
holds only for what the program does on its own. The player is warned ("It may already have accepted your
report") and has to choose to send, and the no_id half predates these commits, so this is not blocking.

Remedy: make "the intake took this preview" part of the workflow state rather than inferring it from the id. Set
a marker on WorkflowState in Upload's Received case (with or without an id) and its Incomplete case; Execute resets
`state.record` on entry, so the record cannot carry it. SamePreview refuses a preview so marked, and the panel uses
the same predicate in place of `state.reporting.id.empty()` to hide Send and offer Close (the panel half is in
src/ui, outside this diff). Add the preview case to report_workflow_tests.hxx:Records: after the `(202,
ReceiveFailed)` send, a second Submission of the same preview is refused and does not upload.

### 2. scripts/NightlyOperations.ps1:Invoke-EmberStagedUpload: an empty $Incoming is a second mode of the shared helper

The room hosts pass an explicit folder and the symbols pass `''` and take the last line `prepare` printed. That is
a nullable mode on the one helper both uploads share, and the reader has to know which caller is which.

Remedy: let scripts/stage-nightly-room-hosts.sh `prepare` print its folder as install-symbols.sh does, drop the
`$Incoming` parameter, and always use the printed path. A room-host record already at `prepared` carries the old
pinned staging script, which does not print; that is the same in-flight case NIGHTLY.md already describes and
needs one more clause there. Not blocking.

### 3. Lower item

- report_workflow_tests.hxx:Automatic: the no-retry loop checks one upload and one history entry for the two 202
  cases but not that the stored record is Received with reason "incomplete". The preview test covers it and both
  paths go through Upload, so this is a tightening only.

## Accepted as is

- A timeout at status 0 after the whole body was written can still be a report the intake stored but had not yet
  answered, and the lighter resend then duplicates it. The client cannot tell that apart from a body still in
  flight on a slow link (WinHttpWriteData completes when the bytes reach the local stack), so retrying without the
  dump is the right default. Removing the window needs an idempotency key in the multipart and deduplication in
  the store, the follow-up named in round 2.
- Incomplete could fold into Received and let the body parse decide (a truncated body never yields a valid id),
  dropping one enum value. The separate "incomplete" reason has diagnostic value, so keeping it is reasonable.

## The three commits as a whole

The publication model is one shape for both channels: prepared (every local file pinned, the symbol archive
included), staged (room hosts and symbols confirmed by Complete-EmberStaging), published. Assert-ReceiptSymbols
stops a build without symbols before packaging, the archive is hashed once per read and checked against the
record, the installer validates every entry under one lock before moving any, and the manual path now applies
today's store policy while a publication retry stays on its own. On the server, the worker count is a computed
constant tied by tests to the unit, the example config and setup.sh. On the client, Classify is now a direct
mapping from the status line to a typed Outcome, Upload returns it once, Send has one fallback path for the
automatic and the preview sends, and DumpInTheWay is a one-line predicate over that type. No file in scope crosses
1000 lines in these commits (NightlyOperations.ps1 880, ReportWorkflow.cxx 380, SymbolPublication.ps1 143).

VERDICT: APPROVE
