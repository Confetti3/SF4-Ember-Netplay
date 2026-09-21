# SF4 Netplay Launcher v0.6.5

> **Experimental unofficial port** — friends-only test software. Both players
> must install the same complete release zip.

This release fixes a memory-corruption bug that crashed the game to desktop
mid-session, with no error dialog and nothing in the log. It was reachable on
every match restart and every disconnect teardown.

**If you are on v0.6.4 or earlier, update.** This is the fix for the
disappearing-game crash.

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

Nothing here changes gameplay. On a healthy match start you will see:

```
SaveSlots [start_ggpo_entry]: 0/10 occupied
```

If you ever see `SaveState: reclaiming leaked slot ...` instead, the build has
caught a leak and recovered rather than corrupting the pool — please report
that line.

**Enabling crash dumps is worth doing if you hit any crash.** Windows Error
Reporting is disabled on some systems, which silently suppresses crash dumps
entirely. In an **Administrator** PowerShell:

```powershell
New-Item -Path 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\SSFIV.exe' -Force | New-ItemProperty -Name DumpFolder -Value 'C:\SF4Dumps' -PropertyType ExpandString -Force | New-ItemProperty -Name DumpType -Value 2 -PropertyType DWord -Force
```

If a crash happens, send `C:\SF4Dumps\SSFIV.exe.*.dmp` along with the logs.

## Validation

- Live two-PC play confirmed, including rematches.
- Native x86 RelWithDebInfo build of Launcher, Sidecar and session targets;
  clean, no new warnings.
- Full test suite passes (6/6), including a new `SaveStateOwnership` test that
  models the ownership protocol, reproduces the original bug, and asserts the
  corrected path does not release payloads still in use.
- Netplay config version verified unchanged at 9; no wire struct or launcher
  IPC field was touched.

This remains experimental software for small friend groups. See
[docs/guides/SCOPE_AND_LIMITATIONS.md](../guides/SCOPE_AND_LIMITATIONS.md) and
[docs/guides/TROUBLESHOOTING.md](../guides/TROUBLESHOOTING.md).
