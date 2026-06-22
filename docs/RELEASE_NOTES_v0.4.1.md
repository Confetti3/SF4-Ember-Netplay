# SF4 Netplay Launcher v0.4.1

**Stability release** — safer netplay teardown on connection loss, longer GGPO disconnect tolerance, coordinated P2P punch, and hardened UDP relay fallback.

> **Experimental unofficial port** — unsigned build. Download only from this GitHub release page.

## Install

1. Download **`sf4-netplay-launcher-*-v0.4.1.zip`** from [Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest) (Assets — not Source code).
2. Extract the **entire** zip on **both** PCs.
3. Run **`preflight.cmd`** — you should see `[OK]` for each file and `RESULT: PASS`.
4. Run **`Launcher.exe`**. Confirm both players show **v0.4.1** in the launcher header (or matching `BUILD_INFO.txt` Git line).

## What's in v0.4.1

- **Safer disconnect handling** — session/GGPO failures tear down cleanly instead of leaving dangling pointers that could crash the game.
- **Graceful GGPO abort** — rollback buffer exhaustion and sync failures end the match with an overlay alert instead of continuing into a crash.
- **Connection resilience** — `CONNECTION_INTERRUPTED` / `CONNECTION_RESUMED` alerts; default disconnect timeout raised to **3 s** (derived from host input delay).
- **Coordinated P2P punch** — session server signals both players before UDP hole punch (`punch_ready` / `punch_go`).
- **UDP relay hardening** — health probe and registration retries; visible alert when falling back to legacy session tunnel.
- **Broker updates** — broader P2P NAT heuristics; GGPO relay restart retry on connect-plan.
- **`NetplayConfig` v8** — both players must use v0.4.1 `Sidecar.dll` (**not compatible** with v0.4.0).

## Netplay (VPS)

- Default broker: `https://74-208-200-95.nip.io`
- Host → **Get code** → share `SF4-XXXX` → **Start game**
- Joiner → paste code → **Start game** after host starts
- Both **Ready** in the in-game lobby

## Troubleshooting

See [TROUBLESHOOTING.md](TROUBLESHOOTING.md) — new **Connection lost behavior** section for in-game alert meanings.

## Upgrade from v0.4.0

Replace the **whole** extracted folder on both PCs. Do not copy only `Sidecar.dll` or `Launcher.exe`.
