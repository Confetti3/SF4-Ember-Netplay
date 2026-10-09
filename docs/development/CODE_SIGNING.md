# Release authentication

Current development builds are unsigned. There is no active SignPath workflow
in this checkout. The retired configuration is [archived](../archive/signpath-legacy.json);
the old application checklist is historical, not proof of enrollment or signing.

SHA-256 package checks and build receipts establish consistency with their
records. They do not establish publisher identity independently of GitHub.
Authenticode requires a trusted publisher certificate and timestamping service,
or an approved signing provider. No certificate or provider credentials are
included here. Signing does not guarantee that antivirus accepts a file.

## Required signing order

1. Build and pass the test gate from the final source state.
2. Copy the verified stage to a separate signing area. Preserve the original
   build receipt; signing changes the binary hashes.
3. Sign the project binaries, including `Launcher.exe`, `Sidecar.dll`,
   `sf4-net.exe`, `Updater.exe`, and `ember-discord.exe` when included.
   Preserve vendor signatures and redistribution terms for third-party files.
4. Verify every expected signature and timestamp, then generate a separate
   signing receipt relating unsigned and signed hashes to the same source.
5. Generate manifests and archives from the signed files, then validate the
   final package and installer signatures before publication.

The current package script validates the original build hashes; it intentionally
rejects a stage modified by signing. A signed-release promotion path and signing
receipt validation must be implemented and tested when a signing provider is
available. Do not hand-edit a passing build receipt to bypass this check.

`scripts/sign-release-binaries.ps1` is a low-level certificate tool, not a
complete signed-release pipeline. It requires explicit certificate credentials.
Never commit those credentials or print them in build logs.

Inspect an artifact in PowerShell with `Get-AuthenticodeSignature`. A successful
signing command alone is insufficient: require `Valid` on every intended file
and verify the publisher identity. See [build instructions](BUILDING.md) and
[Defender guidance](../guides/WINDOWS_DEFENDER.md). Do not ask testers to disable
protection or add folder exclusions.
