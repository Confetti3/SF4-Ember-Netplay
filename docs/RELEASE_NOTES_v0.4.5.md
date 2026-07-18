# SF4 Netplay Launcher v0.4.5

**Rematch / UDP reliability release** — fixes the crash where one player hits a 5s GGPO sync timeout and falls into the legacy tunnel while the other stays on UDP.

> **Experimental unofficial port** — unsigned build. Download only from this GitHub release page.

## Install

1. Download **`sf4-netplay-launcher-*-0.4.5.zip`** from [Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest) (Assets — not Source code).
2. Extract the **entire** zip on **both** PCs.
3. Run **`preflight.cmd`** — you should see `[OK]` for each file and `RESULT: PASS`.
4. Run **`Launcher.exe`**. Confirm both players show **v0.4.5** in the launcher header (or matching `BUILD_INFO.txt` Git line).

**Ops:** Redeploy updated **`sf4e-ggpo-udp-relay`** on the VPS for rematch slot fixes (same release window).

## What's in v0.4.5

### Netplay (client)
- Wait for UDP relay **`SF4R`** (both peers registered) before starting GGPO — stops racing ahead on `SF4W`.
- No automatic fallback to the legacy session tunnel; VPS play stays on **UDP**.
- Sync watchdog extended; on failure **abort to lobby** instead of restarting on the tunnel.
- Safer GGPO start/abort (close leftover sessions, fail cleanly on bind errors).
- `CloseBattle` defer ordering fixed so rematch is less likely to leave GGPO open.

### Netplay (VPS relay binary)
- Rematch rebind **invalidates the peer slot** so the first registrant gets `SF4W` until the other re-registers (no false `SF4R` from stale slots).
- Slot lookup prefers IP and rejects ambiguous same-port matches.

### Also
- README refresh for v0.4.4+ UX (from prior polish).

## Netplay (VPS)

- Default broker: `https://74-208-200-95.nip.io`
- Host → **Get code** → share `SF4-XXXX` → **Start game**
- Joiner → paste code → **Start game** after host starts
- Both **Ready** in the in-game lobby; rematch should stay on UDP

## Troubleshooting

See [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

## Upgrade from v0.4.4

Replace the **whole** extracted folder on both PCs. Redeploy the VPS `sf4e-ggpo-udp-relay` binary from this release’s ops build. Both players must use the same zip.
