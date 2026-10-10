# Saving logs for a bug report

Save your SF4 Ember Netplay logs immediately after a problem, before launching another session. For an online match issue, **both players should follow these steps on their own PCs**.

## Save the logs folder

1. Note the approximate time the problem happened and what you were doing, such as joining a table, finishing a match or starting a rematch.
2. If Ember is still responsive, use **Help & About → Export diagnostics** before leaving the session. Wait for the saved-file message. If the game crashed or the menu is unavailable, continue with the logs below.
3. Close the game and launcher. If `Launcher.exe` or `SSFIV.exe` is still listed in Task Manager, end it. Avoid starting another session until you have saved this one.
4. Press **Windows + R**, paste the following path, and press **Enter**:

   ```text
   %APPDATA%\sf4e
   ```

   On Linux (Proton or Wine), the folder is inside the game's prefix: `steamapps/compatdata/45760/pfx/drive_c/users/steamuser/AppData/Roaming/sf4e`.

5. Right-click the **logs** folder and choose **Compress to ZIP file**. On Windows 10, or under **Show more options**, choose **Send to → Compressed (zipped) folder**.
6. Give the ZIP a descriptive name, such as `P1-rematch-stuck-2026-09-14.zip`. The other player can use `P2-rematch-stuck-2026-09-14.zip`.
7. Copy the ZIP somewhere you can find it, such as your Desktop, and keep it until the report is resolved.

**Include the entire logs folder.** Depending on your build and session, it can contain:

| File | What it helps investigate |
| --- | --- |
| `session-*.log` | Session events from individual processes |
| `sf4e.log` and numbered copies | Game-side logging and previous log files, including `Selection art ... unavailable` lines when character pictures fail to load |
| `launcher.log` and numbered copies | Launcher startup and launch failures, and picture failures on the launcher's recovery screen |
| `sidecar_bootstrap.log` | Sidecar startup and loading |
| Other `.log` files | Additional startup or diagnostic information |

Some files may be absent or older than your latest session. Sending only `sf4e.log` can miss the relevant events. Keep the existing filenames inside the ZIP, and do not delete older logs before collecting them.

## Sending a report from Ember

Open **Help & About > Report a problem**, describe what happened, and review
the preview before choosing **Send**. After a crash, the launcher's message
offers **Send this report**, which opens the same preview for that crash.
The preview lists every part and its size, the version/build/Windows and crash
facts, and the redacted log text. Reports include up to the last 192 KiB of
each available `sf4e.log`, `launcher.log`, `sf4e-crash.log`, and `sf4-net.log`.
Invitations, tokens, network addresses, emails, your Windows user name and
profile folder (including its short 8.3 form), your PC name and your player
name are redacted; review the text for anything else you want to keep private.
Problem reports require a comment, limited to 2000 characters. The comment is
sent as written, and a report with a comment always waits for **Send**. Keep
the copyable report ID after a successful submission.

A crash report can include a **Small dump**. It is off for every report until
you turn it on in that report's preview. It contains unfiltered thread stack
bytes and module paths, which can expose private memory, complete credentials
or invitations, usernames, directory paths and other identifiers. Log
redaction does not sanitize these bytes. The dump contents are not shown in
the text preview; the final **Send** confirmation repeats this warning
whenever the dump is included. Full local dumps are never uploaded. Small
dumps over 4 MiB are not offered. Raw dumps are kept on Ember's server for up
to 30 days; processed reports and pending delivery have separate retention.

Sending crash reports without asking is opt-in. **Settings > Problem reports >
Send problem reports** defaults to off. Choosing **Always send** in the
launcher's crash message, after its confirmation, turns it on and sends that
crash's report. With it on, the launcher sends each crash's report without
asking: logs only, never a dump, and at most 3 reports in 24 hours. Every
report sent counts toward that limit, including ones sent from a preview, and
while the record of sent reports cannot be read nothing is sent without
asking. Turn it off on the same screen. **Settings > Problem reports > Sent reports** lists each
report this PC sent, how it went, its report ID, and whether a dump went with
it.

If reporting is unavailable or rate limited, the preview shows the error and
when to try again. You can still save logs manually using this guide.

## Include the diagnostic export

If **Export diagnostics** succeeded, press **Windows + R** and open:

```text
%APPDATA%\sf4e\diagnostics
```

Copy `ember-diagnostics.txt` alongside your log ZIP and include both in your report. Rename the copy with the same player and incident label so it is easy to match to the ZIP. Each export replaces the file at its original location, so save a copy before exporting again.

The export contains Ember's version and connection-state information. It omits invitations, credentials, player names, raw log text and settings backups. **It is an additional report; it does not include the logs folder.**

When rollback diagnostics are enabled (see below), the export includes fifteen fixed CPU-work timing groups: complete outer call, outer tick, room runtime, session-client step, session-server step, GGPO idle, rollback callback, save state, load state, pacing wait, diagnostic enqueue, trace enqueue, free state, effect restore and VFX restore. It also counts rollback callbacks, prediction stalls and prediction-skipped frames. Their sample counts, mean and maximum durations, and over-25-ms counts describe where Ember spent CPU time. The export also reports dropped asynchronous game-log/lifecycle records and the latest lifecycle writer duration. These are CPU and queue indicators, not displayed-frame measurements; use the repository's PresentMon capture runner for presentation pacing.

To turn on rollback diagnostics, open Command Prompt in the Ember folder, run `set SF4E_ROLLBACK_DIAGNOSTICS=1`, then run `Launcher.exe` from the same window. During a match, `sf4e.log` then gets a `RollbackDiag` summary about every 10 seconds and at match end. It includes `sound_sync` and `limiter_wait`; a `limiter_wait` near zero means the PC cannot keep 60 fps.

`Recovery checkpoint builds` counts checkpoint serialization attempts during the lifetime of the current hosted room. A value of zero means that the available room counter observed no builds; `Unavailable` means there was no hosted-room counter to read, so it must not be interpreted as zero. Starting a different hosted room starts a different counter lifetime.

## Heap checkpoints for testers

For a heap-corruption test, close Ember and the game, then open
`%APPDATA%\sf4e\settings.json` in a text editor. Add or merge this object
inside the existing `overlay` section, preserving the other preferences:

```json
"diagnostics": { "heapCheckInterval": 1 }
```

Restart Ember. `sf4e.log` should show `HeapCheck: validating every process
heap` with `(settings.json)`. The interval is the number of save-state
operations between checks; `1` checks every save, load and release. Match
boundaries are also checked. A check can take milliseconds and affect match
performance, so use this only for reproducing a problem. Set the interval to
`0` or remove the key and restart to disable it. The existing
`SF4E_HEAP_CHECK=<n>` environment variable still works and takes precedence,
including `SF4E_HEAP_CHECK=0` to disable checks. A failed checkpoint reports
the operation and frame in `sf4e.log`; collect the whole logs folder.

## Send a useful report

Include:

- The log ZIP from each affected PC, labelled so the players can be distinguished.
- Each player's `ember-diagnostics.txt`, if available.
- Which package you use: the name of the ZIP you extracted, such as `sf4-ember-netplay-v0.9.8-rc3`, or `BUILD_INFO.txt` from the folder containing `Launcher.exe`. Test builds can still report the previous release's version number, so the ZIP name matters.
- The approximate time and time zone of the problem on each PC.
- What you expected, what actually happened, and the steps that led to it.
- Whether one or both players saw the issue, and whether it repeats.

For example: “At about 8:15 p.m. Eastern, P1 won the first match. P1 stayed on the result screen while P2 returned to the room. This happened twice when trying to rematch.”

Use the repository's [Issues page](https://github.com/Confetti3/SF4-Ember-Netplay/issues) to report the problem, or send the files through the support conversation where they were requested. Raw logs are separate from the redacted export: review them before posting publicly, and keep private invitations and credentials out of your report. Share the log ZIP and diagnostic file rather than the entire `%APPDATA%\sf4e` folder, which also contains settings.

## If something is missing

- **The logs folder does not exist:** check that you are using the same Windows account that ran Ember. Report that no logs were created and include the launch error and build information if available.
- **The logs look old:** sort the folder by **Date modified**, check the `session-*.log` files too, and include the whole folder with the incident time. Do not assume an old `sf4e.log` describes the latest match.
- **Windows cannot zip a file because it is in use:** make sure the game and launcher have closed, then try again.
- **The problem is an update failure:** also copy `%TEMP%\sf4-netplay-update.log`, if present. Keep `.ember-update-backups` in the install folder for recovery.
- **An update says recovery is required:** start `Launcher.exe` again; it restores an interrupted update before the game starts. If that fails, extract the matching upgrade package outside the installation and run `Install-Upgrade.ps1 -InstallDir <Ember folder> -RecoverOnly`. If the install folder contains `.ember-update-transaction-v1.json.failed`, include it in your report. `-CheckOnly` only reports the pending transaction and never changes installed files. Preserve the transaction and backups if recovery reports damaged evidence.

For fixes to common problems, see [Troubleshooting](TROUBLESHOOTING.md).
