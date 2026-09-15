# Saving logs for a bug report

Save your SF4 Ember Netplay logs immediately after a problem, before launching another session. For an online match issue, **both players should follow these steps on their own PCs**.

## Save the logs folder

1. Note the approximate time the problem happened and what you were doing, such as joining a table, finishing a match or starting a rematch.
2. If Ember is still responsive, use **Help & About → Export diagnostics** before leaving the session. Wait for the saved-file message. If the game crashed or the menu is unavailable, continue with the logs below.
3. Close the game and launcher. Avoid starting another session until you have saved this one.
4. Press **Windows + R**, paste the following path, and press **Enter**:

   ```text
   %APPDATA%\sf4e
   ```

5. Right-click the **logs** folder and choose **Compress to ZIP file**. On Windows 10, or under **Show more options**, choose **Send to → Compressed (zipped) folder**.
6. Give the ZIP a descriptive name, such as `P1-rematch-stuck-2026-09-14.zip`. The other player can use `P2-rematch-stuck-2026-09-14.zip`.
7. Copy the ZIP somewhere you can find it, such as your Desktop, and keep it until the report is resolved.

**Include the entire logs folder.** Depending on your build and session, it can contain:

| File | What it helps investigate |
| --- | --- |
| `session-*.log` | Session events from individual processes |
| `sf4e.log` and numbered copies | Game-side logging and previous log files |
| `launcher.log` and numbered copies | Launcher startup and launch failures |
| `sidecar_bootstrap.log` | Sidecar startup and loading |
| Other `.log` files | Additional startup or diagnostic information |

Some files may be absent or older than your latest session. Sending only `sf4e.log` can miss the relevant events. Keep the existing filenames inside the ZIP, and do not delete older logs before collecting them.

## Include the diagnostic export

If **Export diagnostics** succeeded, press **Windows + R** and open:

```text
%APPDATA%\sf4e\diagnostics
```

Copy `ember-diagnostics.txt` alongside your log ZIP and include both in your report. Rename the copy with the same player and incident label so it is easy to match to the ZIP. Each export replaces the file at its original location, so save a copy before exporting again.

The export contains Ember's version and connection-state information. It omits invitations, credentials, player names, raw log text and settings backups. **It is an additional report; it does not include the logs folder.**

When rollback diagnostics are enabled, the export includes twelve fixed CPU-work timing groups: complete outer call, outer tick, room runtime, session-client step, session-server step, GGPO idle, rollback callback, save state, load state, pacing wait, diagnostic enqueue and lifecycle-trace enqueue. Their sample counts, mean and maximum durations, and over-25-ms counts describe where Ember spent CPU time. The export also reports dropped asynchronous game-log/lifecycle records and the latest lifecycle writer duration. These are CPU and queue indicators, not displayed-frame measurements; use the repository's PresentMon capture runner for presentation pacing.

`Recovery checkpoint builds` counts checkpoint serialization attempts during the lifetime of the current hosted room. A value of zero means that the available room counter observed no builds; `Unavailable` means there was no hosted-room counter to read, so it must not be interpreted as zero. Starting a different hosted room starts a different counter lifetime.

## Send a useful report

Include:

- The log ZIP from each affected PC, labelled so the players can be distinguished.
- Each player's `ember-diagnostics.txt`, if available.
- The Ember version shown in **Help & About**, or `BUILD_INFO.txt` from the folder containing `Launcher.exe`.
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
- **An update says recovery is required:** extract the matching upgrade package outside the installation and run `Install-Upgrade.ps1 -InstallDir <Ember folder> -RecoverOnly`. `-CheckOnly` only reports the pending transaction and never changes installed files. Preserve the transaction and backups if recovery reports damaged evidence.

For fixes to common problems, see [Troubleshooting](TROUBLESHOOTING.md).
