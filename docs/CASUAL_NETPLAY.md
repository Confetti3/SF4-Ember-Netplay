# Casual netplay (budget setup)

SF4 Enhanced launcher supports **Simple mode** (default): relay room codes, no port-forward talk, and optional Advanced direct IP / UPnP.

## Tester quick start

1. **Simple mode** (checkbox on home): Host → **Create relay room** → copy `SF4-XXXX` → Start game.
2. Joiner: paste the same code → Start game.
3. Requires a running [room broker](../services/room-broker/README.md). For production, use an [Oracle Cloud broker](ORACLE_BROKER.md); the **relay runs on the host’s PC** (`RelayHost.exe`), not on the VPS.

Set broker URL once in Advanced, or:

```text
set SF4E_BROKER_URL=http://YOUR_VPS:8787
```

## Modes

| Mode | Cost | When to use |
|------|------|-------------|
| **Relay room** | ~$5 VPS + broker | WAN friends, CGNAT, no port forward |
| **Direct + UPnP** | $0 | Same network or router supports UPnP |
| **Direct IP** | $0 | LAN party or you already forwarded ports |
| **Find match** | Same broker | Unranked queue on broker |
| **Open rooms** | Same broker | Small lobby list |

## Invite links

Register `sf4e://` with Launcher.exe, then share:

```text
sf4e://join/SF4-XXXX
```

CLI: `Launcher.exe --join SF4-XXXX`

## VPS checklist (~$10/mo or $0 Oracle)

- **Broker only** on a small VM (Oracle Always Free works): port **8787**
- **RelayHost.exe** on the **host’s Windows PC**: forward **23456 TCP+UDP** (or enable UPnP in Advanced)
- Set `SF4E_BROKER_URL=http://YOUR_ORACLE_IP:8787` on all clients
- `MAX_ROOMS=20`, monitor bandwidth

See [ORACLE_BROKER.md](ORACLE_BROKER.md) for step-by-step Oracle setup.
