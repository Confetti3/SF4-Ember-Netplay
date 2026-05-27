# SF4 Netplay Launcher v0.3.1

Auto GGPO transport + NAT/connect-plan fixes. Pairs with VPS `BROKER_GGPO_TRANSPORT=auto`.

> **Not production-ready.** Experimental unofficial port for a small friends group.

## What's new

### VPS (already deployed)
- **`BROKER_GGPO_TRANSPORT=auto`** — rooms spawn session + GGPO UDP relay pairs
- NAT probe **8790/udp** fixed (correct `SF4E_PROBE` handling)
- GGPO relay health probe fixed (`SF4OK` response)

### Launcher / Sidecar (this zip)
- **DNS resolution for NAT probe** — works with `https://74-208-200-95.nip.io` (not IP-only)
- Connect-plan + register-endpoint for **udp_relay** / **p2p** when broker is in auto mode
- Legacy session tunnel still used as fallback if UDP/P2P fails

## Upgrade from v0.3.0

1. Download **sf4-netplay-launcher-*-v0.3.1.zip** from [Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest)
2. Extract to a **new folder** (or replace old files completely)
3. **Both players** must use this exact zip (check `BUILD_INFO.txt` Git line matches)
4. Broker URL: `https://74-208-200-95.nip.io`

## Testing

- **Simple / relay room** → Start game; overlay should show `udp_relay` or `p2p` when auto path succeeds
- Fallback to legacy tunnel is normal if UDP registration fails
- See [docs/TRANSPORT_REGRESSION.md](TRANSPORT_REGRESSION.md)

## Bug reports

Include `BUILD_INFO.txt` Git line, `%APPDATA%\sf4e\logs\sf4e.log`, room code, and broker URL.
