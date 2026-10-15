# Nightly builds

Nightly is the daily test build of SF4 Ember Netplay. It may break, and its
rooms work only with other Nightly players. FRaccie and other contributors
merge feature branches into `nightly`. After each Stable release, merge
`release` into `nightly`; keep Nightly's CMake version one minor version ahead
of Stable.

Nightly requires **PowerShell 7 or later** (`pwsh`). The publisher, importable
Nightly operations and both Nightly PowerShell tests declare `#Requires -Version 7`,
so Windows PowerShell 5.1 rejects them before any script code or fixture creation.

The Windows build PC runs `scripts/publish-nightly.ps1` at 04:00 local time
from `C:\Users\Kate\Desktop\sf4\sf4-nightly`. The parent `build-target.json`
must select that checkout through `channels.nightly`:

```json
"channels": {
  "nightly": {
    "sourceDirectory": "sf4-nightly",
    "buildDirectory": "build/current",
    "installDirectory": "build/current/stage",
    "branch": "nightly",
    "githubRepo": "Confetti3/SF4-Ember-Netplay-Nightly"
  }
}
```

Use the channel-aware `Get-EmberBuildTarget` on `nightly` for the existing
build/package scripts. The publisher requires its returned `.channel` to be
`nightly`; merge the channel-aware `BuildEnvironment.ps1` change before running.

The job requires a clean checkout on `nightly` and skips while `SSFIV.exe`
runs. After fetching and fast-forwarding, it captures a clean HEAD and the
canonical source fingerprint. Build and packaged receipts must match that
snapshot, and cleanliness, HEAD and the fingerprint are checked once more just
before the pending record is written. From then on remote steps use only the
hashed files in that record, so the checkout is not consulted between them.
Unchanged published source skips building unless forced,
but still runs retention maintenance. Labels use the CMake version and UTC date, for example
`1.2.0-nightly20261008`, with `.2`, `.3`, etc. for existing tags that day.
It runs the normal build and local tests, packages the full ZIP and installer
with SHA-256 sidecars. Publication has four phases: prepare the artifacts,
verify host staging, verify and publish the GitHub draft, then finish retention.
It keeps the newest 14 matching Nightlies; unrelated releases are excluded
from both the changelog baseline and retention.
The Nightly repository must exist with **one initial commit on its default
branch** before the first publish; its tags name that branch and the release
notes' `Source:` line identifies the actual client source commit.

Prepare the [normal Windows build tools](BUILDING.md), authenticate `gh`, and
set `SF4E_DISCORD_SDK_ARCHIVE`, `SF4E_VISUAL_STUDIO_PATH` (if discovery is
insufficient) and `VCPKG_ROOT` in the task user's environment. WSL Ubuntu,
user `kate`, needs the [Linux room-host prerequisites](../../server/roomhost/README.md).
SSH alias `vps` must work with `BatchMode=yes`; `server1` is optional. If its
initial probe succeeds, server1 becomes a required destination for that run
and its retries.
The job builds both Linux binaries from the same committed source and stages
them under the lowercase SHA-256 of the staged `Sidecar.dll`. Each pair is
uploaded to a temporary sibling outside the scanned `builds` directory,
verified against both SHA-256 hashes, and renamed into place on the same
filesystem. An existing build ID is accepted only for identical bytes;
different or incomplete pairs are rejected without overwriting them. A staging
failure prevents public visibility. The owner then
installs the staged VPS build with:

```powershell
ssh -t vps "sudo bash ~/ember-rooms/setup.sh"
```

Register the task from PowerShell on the build PC (runs only while Kate is
logged on, without storing a password; no concurrent task instances):

```powershell
$checkout = 'C:\Users\Kate\Desktop\sf4\sf4-nightly'
$action = New-ScheduledTaskAction -Execute (Get-Command pwsh.exe).Source `
  -Argument "-NoProfile -File `"$checkout\scripts\publish-nightly.ps1`"" `
  -WorkingDirectory $checkout
$trigger = New-ScheduledTaskTrigger -Daily -At '04:00'
$principal = New-ScheduledTaskPrincipal -UserId ([System.Security.Principal.WindowsIdentity]::GetCurrent().Name) `
  -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName 'SF4 Ember Nightly' -Action $action `
  -Trigger $trigger -Principal $principal -Settings $settings
```

Run by hand from the same checkout:

```powershell
Set-Location C:\Users\Kate\Desktop\sf4\sf4-nightly
pwsh -NoProfile -File ./scripts/publish-nightly.ps1 -WhatIf -Force
pwsh -NoProfile -File ./scripts/publish-nightly.ps1 -WhatIf -Local -Force
pwsh -NoProfile -File ./scripts/publish-nightly.ps1
```

`-WhatIf` still fetches/fast-forwards, builds, tests, packages, writes notes
and builds Linux room hosts for a new preparation; it skips publication,
pruning, remote staging and creation of pending state. When pending work exists,
it reports the retry and preserves that state without rebuilding.
Add `-Local` to skip all fetches and the fast-forward and use the checkout's
current HEAD, including before `origin/nightly` has been pushed. It still
requires a clean checkout on `nightly`. Without a previous Nightly, local mode
uses the existing `origin/release` ref for notes; fetch that ref beforehand if
it is absent. `-Local` alone still publishes and stages; use `-WhatIf -Local`
for a local dry run.

A new run first asks GitHub's GraphQL API whether the Nightly repository exists
and has a default-branch commit. Absence is read from the response's JSON (a
`NOT_FOUND` error on the repository), never from error text; authentication,
network and any other failures are errors for every run. Under `-WhatIf`, a
missing or empty Nightly repository is logged and the run continues with no
previous Nightly and no remote tag-collision check. A repository with no
releases also uses `origin/release` for notes while checking any existing tags.
Without `-WhatIf`, a missing or empty repository is an error: create it with an
initial default-branch commit first. A dry run's planned tag is checked again
when publishing.

Before the first remote mutation, `dist/nightly-pending.json` records the
label, clean source snapshot, artifact hashes, host destinations with their
upload tokens, and a pinned copy of `stage-nightly-room-hosts.sh` (LF line
endings, hashed, kept beside the room-host binaries). SSH always receives that
pinned copy, never the checkout's script. An ordinary retry finishes this
recorded release using the same files and label, including after an uncertain
draft/upload/publication result. It does not fetch or rebuild unfinished
publication work, even with `-Force`. A retry checks once, at the start, that the
clean checkout still matches the recorded HEAD and fingerprint and that every
recorded file still has its hash; if they changed, restore that committed source
and the original files before retrying. Completed publication is recorded before
retention, so retention-only retries can use a newer clean source checkout.
The pending record is removed only after retention succeeds.

Each host destination gets its upload token when the pending record is built, so
the token is saved before any remote operation and stays recorded with the
pending work. Every staging attempt runs cleanup, prepare, upload and commit,
then cleanup again: the first cleanup removes whatever an interrupted attempt
left in that exact directory. If the final cleanup fails after a successful
commit, the run stops before publication and a retry repeats the staging.
Cleanup is idempotent and never sweeps other temporary directories.

GitHub publication reads the release and its tag, takes the one step the
recorded release still needs (create the draft, remove an interrupted zero-byte
upload, upload a missing asset, or publish), then reads the release again. Every
read checks the notes, prerelease flag and asset hashes against the record. The
notes file is read and hash-checked once; the draft is created from that text
and later reads are compared with it. A
step that is still needed right after it ran stops the run instead of repeating.

Retention selects one plan per run and saves it to `dist/nightly-cleanup.json`
(exact release IDs and tag-object hashes) before any deletion. Each target is
read, both identities are checked, one thing is deleted (the release by its
recorded ID, never by tag, then the tag), and it is read again; a target is
retired from the plan only once neither remains. A replaced release or a
repointed tag stops the run before anything of it is deleted, and so does a
deletion that the read-back does not show. Retries finish only the recorded
plan, even if a release has already disappeared; they do not sweep orphan tags,
and Nightlies that age out meanwhile wait for the next run. Deletion works only
from these identities and does not read the checkout. Preserve
both state files after failures. A local file lock prevents concurrent publishers.

`-SkipRoomHosts` skips Linux building/staging for a new preparation; it does not
change a pending run's destinations. `-Force` rebuilds unchanged source only
when no pending publication exists. `-VisualStudioPath` and `-DiscordSdkArchive` override the corresponding
environment variables. Existing local package destinations are never
overwritten: move aside a previous dry run's artifacts before reusing its label.
Every run appends a transcript in `dist/nightly-logs/yyyyMMdd-HHmmss.log`;
notes and artifacts remain in `dist`. Local test/build success does not
establish live gameplay acceptance or installation of the public room hosts.

`publish-nightly.ps1` only takes the lock, starts the transcript and calls
`Invoke-NightlyPublish` in `NightlyOperations.ps1` with the build, package,
installer and WSL steps as replaceable operations. Focused tests call the
import-safe operations, and the whole run, with every external command and
build step faked; they check ordering, dry runs, pending resumes and recovery.
The optional Bash checks use inert files in a temporary directory.
`build-current.ps1` runs both test scripts:

```powershell
pwsh -NoProfile -File ./scripts/test-nightly-publication.ps1
pwsh -NoProfile -File ./scripts/test-nightly-paths.ps1 -BashPath 'C:\Program Files\Git\bin\bash.exe'
```
