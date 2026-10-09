# Security policy

This policy describes the Ember 1.1.x architecture on the `release` branch.
Use the latest compatible Ember release for all participants. The retired
0.6.x launcher, broker, RelayHost and Qt documentation does not describe this
product. Its previous policy is retained as [historical material](../docs/archive/SECURITY-pre-ember.md).

## Reporting a vulnerability

Do not post exploitable bugs, private invitations, credentials or crash dumps
in public issues. Contact the maintainer (Katie / Confetti3) privately through
the project's [support community](https://discord.gg/uPNqF5A5uq). Include the
affected build, impact, reproduction steps and a redacted diagnostic export.
The maintainer's existing targets are acknowledgement within seven days and a
fix or mitigation plan within thirty days for critical or high severity reports;
these are targets, not guarantees. Coordinated disclosure and attribution are
preferred unless the reporter requests anonymity.

## Current boundaries

- The launcher injects Sidecar into an owned Steam copy of USFIV. The supported
  executable fingerprint is checked before fixed-offset hooks are installed.
  A matching executable or Sidecar hash is compatibility checking, not anti-cheat.
- Private rooms use Iroh authenticated transport and invitation capabilities.
  Anyone given a valid invitation may be able to join; keep invitations private.
  Room peers can still be modified or malicious. Authentication does not make
  game results or player-controlled state inherently trustworthy.
- Public rooms depend on bridge admission tickets and a hosted room authority.
  Identity proofs, bridge sessions and tournament integrations have separate
  credentials. Public hosting depends on the deployed service and its operators;
  a local client test does not audit that deployment.
- Helpers use authenticated, current-user local IPC. The networking helper and
  optional Discord companion are separate processes. Discord account linking
  and SDK presence are separate features with separate trust boundaries.
- Updates validate package inventory and hashes, but still trust the configured
  GitHub release channel. A checksum beside a download is not independent
  publisher authentication. Current local development builds are unsigned;
  inspect the actual release's signatures rather than assuming signing is active.

## Diagnostics and safe use

Use **Help & About → Export diagnostics** for redacted reports. Raw logs and
especially crash dumps can contain invitations, session material, chat and
other process memory. Share them privately only when needed. See
[Saving logs](../docs/guides/SAVING_LOGS.md).

Keep all files from one build together, close the game before updating, and
obtain releases from the project's [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases).
Do not disable antivirus protection or add blanket folder exclusions. A
detection needs investigation; signing alone does not prove a file harmless.
See [Windows Defender guidance](../docs/guides/WINDOWS_DEFENDER.md).

USFIV and Steam vulnerabilities belong to their respective vendors. This
experimental unofficial project does not promise anti-cheat, protection from
a compromised local account, or availability against denial-of-service attacks.
Historical audits of the retired stack do not certify the current code.
