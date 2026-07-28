# Security Policy

## Supported versions

| Version | Supported |
|---------|-----------|
| 0.6.x   | Yes (current releases; HTTPS broker on official VPS) |
| 0.5.x   | Best-effort only — prefer latest 0.6.x |
| 0.4.x   | Best-effort only |
| ≤ 0.3.x | No |

Security fixes are published as GitHub releases on [Confetti3/SF4-Netplay-Launcher](https://github.com/Confetti3/SF4-Netplay-Launcher).

## Reporting a vulnerability

**Do not open public GitHub issues for exploitable security bugs.**

Email or DM the maintainer (Katie / Confetti3) with:

1. Description and impact
2. Steps to reproduce
3. Affected version
4. Optional: suggested fix

We aim to acknowledge within **7 days** and provide a fix or mitigation plan within **30 days** for Critical/High issues.

## Scope

**In scope**

- SF4 Netplay Launcher (`Launcher.exe`, `Updater.exe`, `RelayHost.exe`, shipped DLLs)
- Official room broker and VPS relay operated for the project
- GitHub release packages (`sf4-netplay-launcher-*.zip`)

**Out of scope**

- Upstream [sf4e](https://codeberg.org/adanducci/sf4e) on Codeberg (report to upstream)
- Ultra Street Fighter IV / Steam client vulnerabilities
- Cheating in ranked Steam matchmaking (sidecar replaces vanilla online by design)
- Physical access to a user's PC
- DDoS at scale against the public broker

## Known limitations (experimental)

This is an **experimental unofficial port** for casual friends-only netplay — **not production-ready software**:

- Official VPS broker uses **HTTPS** (`https://74-208-200-95.nip.io`); custom broker URLs may still be HTTP (requires `SF4E_ALLOW_HTTP_BROKER=1` on the client)
- Room broker has **no room authentication** (friends-only codes)
- Room codes are short; active rooms may be listed publicly
- **Sidecar.dll hash** ensures matching builds between players; it is **not** anti-cheat or code signing
- Updates trust **GitHub releases** without separate code signatures
- **Windows Defender** may flag `Sidecar.dll` / `Launcher.exe` as `Program:Win32/Wacapew.A!ml` (heuristic **false positive** on unsigned game hooks). See [docs/WINDOWS_DEFENDER.md](docs/WINDOWS_DEFENDER.md).

Use only with people you trust until room auth and signed releases are in place.

## Audit status (0.6.x)

A full-stack launcher → VPS security audit was performed against **v0.6.5** (2026-07-28). Critical WebView-era client issues (`applyUpdate` client URLs, unrestricted `openUrl`, default HTTP broker) are mitigated in the current Qt + HTTPS path. Follow-up hardening on this line closes session post-HELLO ACL gaps, broker queue/health rate limits, update redirect host checks, broker post-DNS SSRF filtering, optional relay-manager token + port allowlists, and dashboard cookie compare / body limits. Residual accepted risks remain as listed above (unsigned updates, room-code join model, plaintext game UDP). Detailed findings are kept private and are not published in this repository.

## Safe usage

- Download only from official GitHub Releases
- If Defender quarantines files, verify release SHA256 hashes and follow [docs/WINDOWS_DEFENDER.md](docs/WINDOWS_DEFENDER.md) — do not weaken Defender with exclusions; wait for signed releases
- Keep `Launcher.exe`, `Sidecar.dll`, and Qt runtime files together from the **same zip**
- Do not point the broker URL at untrusted servers
- Close the game before applying in-app updates

## Disclosure

We prefer coordinated disclosure. Credit will be given in release notes unless you request anonymity.
