# SF4 Ember Netplay v0.8.4

This release adds a readable, compact match HUD, improves training-meter feedback, moves routine file output to background workers, and adds recovery for interrupted updates.

## What's changed

- The match HUD shows both fighter names, current gameplay **Ping**, recent **Rollback**, and the **Delay** accepted for your local fighter. Its quiet, opaque panel sits at bottom center and measures 520 by 62 logical pixels at 1080p with Standard size. It does not capture gameplay input.
- Settings > Interface now offers **Small / Standard / Large** HUD sizes and **Normal / Raised** bottom spacing with a live preview. Existing HUD On/Off choices are preserved. Standard and Normal are the migration defaults.
- Ping is sampled up to four times per second and becomes unavailable after a failed query or two seconds without a fresh sample. Rollback is the largest successful replay burst in the preceding second. Spectators see **Spectating** and unavailable fighter ping. Metrics reset between matches.
- Training startup measurements retain the beginning of an attack across internal action changes and preserve completed startup through recovery-only actions. The training panel explains unavailable measurements. Synthetic cancel/dash sequences verify that advantage waits for the observed actionable state; exact DP, Flash Kick and FADC variants still need recorded native comparisons.
- Routine game logs and lifecycle file writes use bounded background queues. Diagnostics report dropped records, trace write duration, and additional CPU timing groups. Unchanged lifecycle records avoid serialization, repeated unchanged presence updates do less work, and the runtime consumes its published state without immediately copying the full snapshot again.
- Recovery-journal appends track serialized size incrementally; existing payload shedding and commitment rules remain in use at the limits.
- Both update paths preserve occupied destination files and record recoverable transactions before replacing live files. Recovery validates journal paths and all required backups before restoring anything. The readers accept each other's journals, replacements use unique temporary siblings, and the product manifest is written last. Incremental **CheckOnly** remains read-only and reports pending recovery.
- Optional offline training capture now includes complete fighter samples and loss counters. Its writer starts outside DLL initialization and stops during normal shutdown. A PresentMon runner records usable display or presentation intervals with QPC timestamps and explicitly reports unavailable display measurements.

## Install or upgrade

For a fresh installation, download **sf4-ember-netplay-0.8.4.zip** and its `.sha256` sidecar. Extract the complete package into a new writable folder, run **preflight.cmd**, then run **Launcher.exe**.

For an intact published v0.8.3 installation, download **upgrade-sf4-ember-netplay-0.8.3-to-0.8.4.zip** and its checksum. Extract it into a separate folder, close SF4 and Ember, run **Install Upgrade.cmd**, and select your existing v0.8.3 Ember folder. The upgrade reuses unchanged artwork and verifies the resulting package.

If an update was interrupted, retain the transaction and backup. Run `Install-Upgrade.ps1 -InstallDir "C:\Games\Ember" -RecoverOnly` from the extracted upgrade, or `Updater.exe -InstallDir "C:\Games\Ember" -RecoverOnly` from an extracted full package. Close SF4 and Ember first. Missing or damaged evidence requires manual recovery; do not delete backups or treat a legacy backup directory as a completed transaction.

Everyone in a room must use the same Ember release. Preferences remain under `%APPDATA%\sf4e`. Requires Windows 10 or later (x64), an owned Steam copy of Ultra Street Fighter IV and the Microsoft Visual C++ x86 runtime.

## Validation and limits

The project owner reported that the compact HUD candidate works in gameplay on 15 September 2026. That report applies to `0.8.4-readable-match-hud-compact-20260915-r2`. Subsequent release-audit corrections cover update recovery, optional capture lifecycle, measurement provenance, and diagnostic reporting; they do not change the tested HUD layout.

The designated Windows build runs the 39-test suite, including telemetry availability/expiry, applied delay, settings/navigation, training sequences, DX9 rendering and update recovery. The renderer exercises 6,444 frames across ten viewport/DPI configurations. Update fixtures cover interruption at each incremental replacement, damaged evidence, temporary-name collisions, read-only inspection, idempotent recovery, and native/PowerShell journal interoperability. Packaging verifies provenance and hashes and performs a temporary upgrade from the published v0.8.3 package.

This release does not claim measured stutter or input-latency improvements. Matched five-minute PresentMon comparisons and recorded move-by-move native frame-data checks remain outstanding. The larger audit plan also includes further room-snapshot caching, structured diagnostic records/error prioritization, projection caching, differential journal testing and broader failure injection; v0.8.4 delivers the changes listed above rather than claiming that entire program is complete. See [release audit](https://github.com/Confetti3/SF4-Ember-Netplay/blob/v0.8.4/docs/RELEASE_AUDIT_v0.8.4.md).

Ember remains experimental. For problem reports, collect logs from both PCs using the [log collection guide](https://github.com/Confetti3/SF4-Ember-Netplay/blob/v0.8.4/docs/SAVING_LOGS.md).

## Attribution

Based on **[sf4e](https://codeberg.org/adanducci/sf4e)** by **Anthony Danducci and contributors**, under the MIT license. This is an unofficial port; Anthony Danducci does not maintain, endorse or support this build. Original licenses and dependency and artwork attribution are preserved.
