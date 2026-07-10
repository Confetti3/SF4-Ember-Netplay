# SF4 Netplay Launcher v0.4.3

**Stability and security release** — the launcher UI no longer freezes during network actions, the in-app updater now verifies downloads, and several relay/broker crash conditions are fixed.

> **Experimental unofficial port** — unsigned build. Download only from this GitHub release page.

## Install

1. Download **`sf4-netplay-launcher-*-v0.4.3.zip`** from [Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest) (Assets — not Source code).
2. Extract the **entire** zip on **both** PCs.
3. Run **`preflight.cmd`** — you should see `[OK]` for each file and `RESULT: PASS`.
4. Run **`Launcher.exe`**. Confirm both players show **v0.4.3** in the launcher header (or matching `BUILD_INFO.txt` Git line).

## What's in v0.4.3

- **Responsive UI** — the launcher no longer freezes while getting a room code, starting a game, refreshing your IP, trying UPnP, listing rooms, checking for updates, or downloading an update. Backend work now runs off the UI thread.
- **Safer updater** — downloaded update packages are now verified against the release's published SHA-256 before anything is installed. A corrupted or tampered download is rejected instead of applied.
- **Update install confirmation** — installing an update now asks for confirmation first (it replaces the install folder and restarts the launcher), so a stray click can't wipe an in-progress session.
- **Room browser fix** — picking a room from **Open rooms** in Advanced mode no longer fails with a "needs an IP:port" error; the connection method is set to relay automatically.
- **Crash hardening** — an unexpected error while handling a UI action now surfaces as a message instead of crashing the launcher.

## Relay / broker (server-side)

Operators running the VPS relay and room broker get several reliability fixes:

- Relay manager no longer crashes if a relay binary fails to spawn, and now force-stops hung relay processes so their ports are reclaimed.
- Room broker no longer crashes if the relay manager is briefly unreachable, and a port-allocation race that could break a concurrently-created room is fixed.

## Netplay (VPS)

Unchanged from v0.4.2 — same broker, room codes, and session flow:

- Default broker: `https://74-208-200-95.nip.io`
- Host → **Get code** → share `SF4-XXXX` → **Start game**
- Joiner → paste code → **Start game** after host starts
- Both **Ready** in the in-game lobby

## Troubleshooting

See [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

## Upgrade from v0.4.2

Replace the **whole** extracted folder on both PCs. Do not copy only `Launcher.exe`. Both players must use the same zip.
