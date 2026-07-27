# SF4 Netplay Launcher v0.6.5-rc1 (testing)

> **Experimental unofficial test build** — not production-ready software.
> Both players must install the same complete release zip.

This release candidate fixes a memory-corruption bug that crashed the game to
desktop mid-session, with no error dialog and nothing in the log. It was
reachable on every match restart and every disconnect teardown.

**The fix has not been validated in live two-PC play.** That is the whole point
of this RC — see [What to watch for](#what-to-watch-for).

## Install

1. Download `sf4-netplay-launcher-*-0.6.5.zip` from this release.
2. Extract the complete zip into a new folder.
3. Run `preflight.cmd`, then `Launcher.exe`.
4. Confirm both players show **v0.6.5**.

Mixed builds are rejected by the room compatibility check. The netplay config
version is unchanged at 9 — no wire format or launcher payload changed — but
run the same zip on both PCs anyway, since the fix has to be on both sides to
help.

## The crash

Symptom: the game vanishes mid-session. No crash dialog, no error in
`sf4e.log` — the log just stops on an ordinary line. The other player sees
"Opponent disconnected" 30–45 seconds later and may keep playing normally.

Cause: the rollback savestate system moves ownership of the engine's memento
payloads into a savestate when it saves, and hands ownership back when it
frees. The "free" path took a scratch snapshot of the live state, restored it
afterwards — and then still released it, freeing memory the engine had just
taken back and was still using.

One such call was survivable. But match teardown frees every occupied slot in
a loop, so the damage compounded, and the pool was never reset when the next
match started. A rematch would then build on an already-corrupted pool.

That last part explains the worst case seen in testing: both players' logs
ending on the identical line, `Netplay: starting match`, at the same instant.

Fixed:

- Savestates now track whether they still own their key payloads, so the free
  path can hand ownership back without releasing it.
- The savestate pool is explicitly reset when a session starts and swept after
  battle close. This matters because the deferred-close path retires a session
  outside the normal battle-close route, so the old cleanup loop never ran at
  all on a rematch.
- Three safety checks that existed only in debug builds — and so were absent
  from every build players actually run — now log and recover in release:
  saving into an occupied slot, savestate work on the wrong thread, and GGPO
  handing back a buffer outside the pool.

## Also fixed

- **Spectator slot overflow.** The loop that fills the GGPO player array was
  bounded only by the member count reported by the room server, with no check
  against the array size. A malformed or hostile room reply could write past
  the end of a stack array. Both call sites are now clamped, as are the two
  player counts passed into GGPO.

## Diagnostics

Because the crash left no evidence, this build adds breadcrumbs that land in
`sf4e.log`:

- Savestate pool occupancy on entry to match start, battle close, and abort.
- A warning naming any leaked slot that gets reclaimed, with its frame numbers.

Nothing here changes gameplay. If you do crash again, these lines plus a crash
dump are what make it diagnosable.

**Enabling crash dumps is worth doing before you test.** Windows Error
Reporting is disabled on some systems, which silently suppresses crash dumps
entirely. In an **Administrator** PowerShell, on both PCs:

```powershell
New-Item -Path 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\SSFIV.exe' -Force | New-ItemProperty -Name DumpFolder -Value 'C:\SF4Dumps' -PropertyType ExpandString -Force | New-ItemProperty -Name DumpType -Value 2 -PropertyType DWord -Force
```

If a crash happens, send `C:\SF4Dumps\SSFIV.exe.*.dmp` along with the logs.

## What to watch for

The success signal is in `sf4e.log`. On each match start you should see:

```
SaveSlots [start_ggpo_entry]: 0/10 occupied
```

**`0/10` on every rematch is the fix working.** If you instead see

```
SaveState: reclaiming leaked slot 3 (start_ggpo) used=true keys=90 ...
```

then something is still leaking savestates — the build has caught it and
recovered rather than corrupting the pool, but please report that line.

Priority test path, since this is where the crash lived:

1. Play a match, then **rematch several times in a row** without returning to
   the launcher. This is the path that crashed most reliably.
2. Force a disconnect mid-match (unplug one side), then rematch cleanly.
3. Play a long session, 30+ minutes, and confirm nothing terminates silently.

## Validation

- Native x86 RelWithDebInfo build of Launcher, Sidecar and session targets;
  clean, no new warnings.
- Full test suite passes (6/6), including a new `SaveStateOwnership` test that
  models the ownership protocol, reproduces the original bug, and asserts the
  corrected path does not release payloads still in use.
- Netplay config version verified unchanged at 9; no wire struct or launcher
  IPC field was touched.

Not yet done, and the reason this is an RC rather than a release:

- **No live two-PC session.** The unit test covers the ownership protocol, not
  the engine integration. Only real play exercises that.
- **No debug-configuration build.** The newly-live asserts have been exercised
  only in RelWithDebInfo.
