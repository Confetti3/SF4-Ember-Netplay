# SF4 Ember Netplay — troubleshooting

- **Game not found:** choose the directory containing SSFIV.exe in recovery, then Retry launch. Steam must be installed and the game owned.
- **Missing networking helper:** extract the complete package, including sf4-net.exe. Offline remains available. Restart Launcher after restoring files.
- **Launch or injection failed:** close recovery, run preflight.cmd and review `%APPDATA%\sf4e\logs\launcher.log`. Retry through recovery. No unrelated processes are terminated.
- **Character art missing:** portraits come from the installed game, with the packaged original outfit as a fallback. Most alternate colors have no preview picture; those show "Preview not available" by design. A picture that fails to load is retried, then named in `%APPDATA%\sf4e\logs\sf4e.log` as `Selection art ... unavailable` with the file and error. Include that line in a report.
- **Display problems:** use the native game Options menu. Ember does not override fullscreen or window behavior.
- **Invalid invitation:** paste the entire private invitation. Old broker room codes and direct-IP addresses are no longer supported.
- **Room rejected:** use the same build as the host, a distinct player name, and a room with free capacity. Cancel or Leave, fix the cause, then retry Host/Join.
- **Connection lost:** check the connection text in Help & About. Leave and rejoin with a current invitation. A continuing fight may have unavailable control-plane verification; its health should not be inferred from the HUD alone.
- **Ready pending:** wait for the other player and preparation acknowledgement. Selections remain locked while readiness is pending. Leave is explicit.
- **Preferences could not be saved:** keep the window open, resolve a read-only or busy settings directory, then Apply preferences again. Original settings and migration backups remain under `%APPDATA%\sf4e`.
- **Update failed:** retry from Help & About or `Launcher.exe --updates`. Installation requires orderly game shutdown. Logs are in `%TEMP%\sf4-netplay-update.log`; previous product files are preserved in `.ember-update-backups`. Do not delete those recovery copies while investigating.

Before reporting a problem, follow [Saving logs for a bug report](SAVING_LOGS.md) to save the full logs folder and export diagnostics from Help & About. For online match issues, collect logs from both players. The diagnostic export intentionally omits invitations, capabilities, player names, credentials, arbitrary log text and settings backups. Never paste a private invitation into a public issue.

Old VPS, Qt and direct-IP instructions are under `docs/archive` and do not describe this build.
