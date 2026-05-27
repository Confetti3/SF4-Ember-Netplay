# Security Policy

## Supported versions

| Version | Supported |
|---------|-----------|
| 0.2.8+  | Yes (current team releases) |
| ≤ 0.2.7 | No |

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

## Known limitations (beta)

This is an **unofficial community port** for casual friends-only netplay:

- Room broker uses **HTTP** by default and has **no room authentication**
- Room codes are short; active rooms may be listed publicly
- **Sidecar.dll hash** ensures matching builds between players; it is **not** anti-cheat or code signing
- Updates trust **GitHub releases** without separate code signatures (see [docs/SECURITY_REVIEW.md](docs/SECURITY_REVIEW.md))

Use only with people you trust until HTTPS broker and auth land (planned v0.3.x).

## Safe usage

- Download only from official GitHub Releases
- Keep `Launcher.exe`, `Sidecar.dll`, and `launcher-ui/` together from the **same zip**
- Do not point the broker URL at untrusted servers
- Close the game before applying in-app updates

## Disclosure

We prefer coordinated disclosure. Credit will be given in release notes unless you request anonymity.

## Audit documentation

Full review: [docs/SECURITY_REVIEW.md](docs/SECURITY_REVIEW.md)  
Remediation plan: [docs/SECURITY_REMEDIATION.md](docs/SECURITY_REMEDIATION.md)
