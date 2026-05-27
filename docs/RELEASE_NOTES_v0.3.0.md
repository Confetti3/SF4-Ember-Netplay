# SF4 Netplay Launcher v0.3.0

Transport + TLS release — HTTPS broker, VPS port hardening, and GGPO transport groundwork (legacy path remains default on VPS).

> **Not production-ready.** Experimental unofficial port for a small friends group.

## What's new

### VPS / broker
- **TLS via Caddy** — broker and dashboard at `https://74-208-200-95.nip.io` (no plain `:8787` from the internet)
- **Port lockdown** — broker/dashboard/relay-manager on localhost only; public ports are 443 + game relay ranges
- **GGPO UDP relay binary** on VPS (for future `BROKER_GGPO_TRANSPORT=auto`; still **legacy** by default)
- Match coordinator APIs + dashboard panel (operator use)

### Launcher
- Default broker URL: **`https://74-208-200-95.nip.io`** (auto-migrates from `http://74.208.200.95:8787`)
- **HTTPS required** for public broker URLs (`SF4E_ALLOW_HTTP_BROKER=1` for dev only)
- NetplayConfig **v5** — GGPO transport fields, connect-plan, legacy fallback
- Dev overlay transport stats (when enabled)

## Upgrade

1. Download **sf4-netplay-launcher-*.zip** from [Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest)
2. Extract over your existing folder (both players need the **same zip**)
3. Broker URL should show `https://74-208-200-95.nip.io` in Advanced (or delete `%APPDATA%\sf4e\config.json` to reset)

## Operator

- Dashboard: `https://74-208-200-95.nip.io/login`
- See [docs/VPS_TLS_SETUP.md](VPS_TLS_SETUP.md)

## Testing

- Legacy relay (Simple mode) should work unchanged with this build + current VPS
- UDP/auto transport: keep `BROKER_GGPO_TRANSPORT=legacy` until soak tests pass ([docs/TRANSPORT_REGRESSION.md](TRANSPORT_REGRESSION.md))

## Bug reports

Include `BUILD_INFO.txt` Git line, `%APPDATA%\sf4e\logs\sf4e.log`, and broker URL from Advanced settings.
