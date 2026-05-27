# SF4 Netplay Launcher room broker

Lightweight HTTP broker for **~$10/month** casual netplay in an **experimental unofficial port**: short codes (`SF4-XXXX`), room caps, idle cleanup, and quick match queue. Not production infrastructure — friends-only testing scale.

## Run locally

```bash
cd services/room-broker
npm init -y   # optional if no package.json
node server.js
```

Default: `http://127.0.0.1:8787`. Point the launcher at it (Advanced → broker URL) or set:

```text
SF4E_BROKER_URL=http://127.0.0.1:8787
```

**Oracle Free deploy:** see [docs/ORACLE_BROKER.md](../../docs/ORACLE_BROKER.md).

## Host PC relay (with Oracle broker)

The broker returns room codes pointing at the **host's public IP**. When hosting:

1. Run **`RelayHost.exe`** (next to `Launcher.exe`) — launcher starts it automatically on **Start game**
2. Forward **TCP+UDP 23456** on the host router, or use launcher **Try UPnP**
3. Create room sends `relayHost` = your public IP to the broker

## VPS (~$5–6/mo)

On your VPS (same machine as the session relay):

1. Run a **SessionServer** listener on each allocated port (23456–23475), or one shared port for a single-room test.
2. Start the broker:

```bash
export RELAY_HOST=your.vps.ip
export RELAY_PORT_BASE=23456
export MAX_ROOMS=20
export ROOM_IDLE_MS=900000
node server.js
```

3. Open TCP/UDP **23456–23475** in the firewall.
4. Set testers’ `SF4E_BROKER_URL=http://your.vps.ip:8787`.

## Budget controls (env)

| Variable | Default | Purpose |
|----------|---------|---------|
| `MAX_ROOMS` | 20 | Cap concurrent rooms |
| `ROOM_IDLE_MS` | 900000 (15m) | Drop unused rooms |
| `RELAY_HOST` | 127.0.0.1 | Address returned to clients |
| `RELAY_PORT_BASE` | 23456 | First port in pool |

Monitor VPS egress in your provider dashboard; pause new rooms if usage exceeds ~80% of your cap.

## API

- `POST /v1/rooms` — create room → `{ code, host, port }`
- `GET /v1/rooms/SF4-XXXX` — resolve join target
- `GET /v1/rooms` — open room list (launcher browser)
- `POST /v1/queue/join` — quick match (pairs when 2+ waiting)
- `GET /v1/health` — status + room count
