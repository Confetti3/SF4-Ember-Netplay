# Casual netplay (experimental setup)

SF4 Netplay Launcher supports **Simple mode** (default): relay room codes in an **experimental** friends-only setup, plus optional Advanced direct IP / UPnP.

## Tester quick start

1. **Simple mode** (checkbox on home): Host → **Create relay room** → copy `SF4-XXXX` → Start game.
2. Joiner: paste the same code → Start game.
3. v0.2.7.3+ ships with broker **`http://74.208.200.95:8787`**. The **session relay runs on the VPS** — host and joiner connect outbound; no host port forward.

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
| **Find match** | Same broker | Experimental — prefer Host + room code for friends testing |
| **Open rooms** | Same broker | Experimental — browse list; not primary testing path |

## Invite links

Register `sf4e://` with Launcher.exe, then share:

```text
sf4e://join/SF4-XXXX
```

CLI: `Launcher.exe --join SF4-XXXX`

## VPS checklist (~$10/mo)

- **Broker + session relay** on the VPS: ports **8787/tcp** and **23456–23475/tcp+udp**
- **IONOS (or provider) control-panel firewall** must allow **inbound UDP 23456–23475** to the VPS — `ufw` on the server is not enough if the cloud panel blocks UDP
- Host and joiner use the same release zip (matching `Sidecar.dll`)
- No port forward on the host PC for Simple mode relay
- `MAX_ROOMS=20`, monitor bandwidth

Verify from your PC: `powershell -File scripts\relay-diag.ps1`. If relay-diag passes but in-game connect fails, open IONOS inbound UDP **23456–23475** and retest.

See [TEAM_QUICKSTART.md](TEAM_QUICKSTART.md) for deploy notes.
