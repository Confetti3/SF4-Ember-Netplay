# Nightly builds

Nightly is the daily test build of SF4 Ember Netplay. It may break, and its
rooms work only with other Nightly players. FRaccie and other contributors
merge feature branches into `nightly`. After each Stable release, merge
`release` into `nightly`; keep Nightly's CMake version one minor version ahead
of Stable.

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
runs. It fetches and fast-forwards, then skips unchanged published source
unless forced. Labels use the CMake version and UTC date, for example
`1.2.0-nightly20261008`, with `.2`, `.3`, etc. for existing tags that day.
It runs the normal build and local tests, packages the full ZIP and installer
with SHA-256 sidecars, and publishes a prerelease in the Nightly repository.
It keeps the newest 14 releases and only deletes older matching Nightly tags.
The Nightly repository must exist with **one initial commit on its default
branch** before the first publish; its tags name that branch and the release
notes' `Source:` line identifies the actual client source commit.

Prepare the [normal Windows build tools](BUILDING.md), authenticate `gh`, and
set `SF4E_DISCORD_SDK_ARCHIVE`, `SF4E_VISUAL_STUDIO_PATH` (if discovery is
insufficient) and `VCPKG_ROOT` in the task user's environment. WSL Ubuntu,
user `kate`, needs the [Linux room-host prerequisites](../../server/roomhost/README.md).
SSH alias `vps` must work with `BatchMode=yes`; `server1` is optional.
The job builds both Linux binaries from the same committed source and stages
them under the lowercase SHA-256 of the staged `Sidecar.dll`. The owner then
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
and builds Linux room hosts; it skips publication, pruning and remote staging.
Add `-Local` to skip all fetches and the fast-forward and use the checkout's
current HEAD, including before `origin/nightly` has been pushed. It still
requires a clean checkout on `nightly`. Without a previous Nightly, local mode
uses the existing `origin/release` ref for notes; fetch that ref beforehand if
it is absent. `-Local` alone still publishes and stages; use `-WhatIf -Local`
for a local dry run.

Under `-WhatIf`, a missing or empty Nightly repository is logged and the run continues
with no previous Nightly and no remote tag-collision check. A repository with
no releases also uses `origin/release` for notes while checking any existing
tags. Authentication and network failures remain errors. Without `-WhatIf`,
a missing or empty repository is an error: create it with an initial default-branch
commit first. A dry run's planned tag is checked again when publishing.

`-SkipRoomHosts` skips Linux building/staging. `-Force` rebuilds unchanged
source. `-VisualStudioPath` and `-DiscordSdkArchive` override the corresponding
environment variables. Existing local package destinations are never
overwritten: move aside a previous dry run's artifacts before reusing its label.
Every run appends a transcript in `dist/nightly-logs/yyyyMMdd-HHmmss.log`;
notes and artifacts remain in `dist`. Local test/build success does not
establish live gameplay acceptance or installation of the public room hosts.
