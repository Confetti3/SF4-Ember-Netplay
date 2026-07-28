# SF4 Netplay Launcher room broker

Lightweight HTTP broker for casual netplay in an **experimental unofficial port**: short codes (`SF4-XXXX`), room caps, idle cleanup, connect-plan / NAT probe coordination, and quick match queue. Not production infrastructure — friends-only testing scale.

## Run locally

```bash
cd services/room-broker
npm init -y   # optional if no package.json
node server.js
```

Default: `http://127.0.0.1:8787`. Point the launcher at it (Advanced → broker URL) or set:

```text
SF4E_BROKER_URL=http://127.0.0.1:8787
SF4E_ALLOW_HTTP_BROKER=1
```

The launcher rejects plain HTTP broker URLs unless `SF4E_ALLOW_HTTP_BROKER=1` is set.

**Production VPS (TLS):** see [docs/VPS_TLS_SETUP.md](../../docs/VPS_TLS_SETUP.md). Clients use `https://your-domain` (Caddy terminates TLS; broker binds `127.0.0.1:8787`).

## Production path (VPS + FORCE_VPS_RELAY)

On the shared project VPS (or your own):

1. Run **relay-manager** + session / GGPO UDP relays (ports `23456–23505` and `24456–24505` for `MAX_ROOMS=50`).
2. Start the broker on localhost with VPS relay forced:

```bash
export RELAY_HOST=your.vps.ip
export RELAY_PORT_BASE=23456
export MAX_ROOMS=50
export ROOM_LOBBY_IDLE_MS=300000
export ROOM_OCCUPIED_IDLE_MS=0
export FORCE_VPS_RELAY=1
export BROKER_GGPO_TRANSPORT=auto
node server.js
```

3. Put Caddy in front for HTTPS on **443**; keep **8787** off the public internet.
4. Open TCP/UDP **23456–23505** and GGPO UDP **24456–24505** in both host firewall and provider cloud firewall.
5. Point testers at `SF4E_BROKER_URL=https://your-domain` (no port).

### Advanced: host-PC relay (Direct IP / local RelayHost)

When not using `FORCE_VPS_RELAY`, the broker can return room codes that point at the **host's public IP**. In that case:

1. Run **`RelayHost.exe`** (next to `Launcher.exe`) — launcher starts it automatically on **Start game**
2. Forward **TCP+UDP 23456** on the host router, or use launcher **Try UPnP**
3. Create room sends `relayHost` = your public IP to the broker

Prefer the VPS path for WAN friends testing.

## Budget controls (env)

| Variable | Default | Purpose |
|----------|---------|---------|
| `MAX_ROOMS` | 50 | Cap concurrent rooms (session `23456–23505`, GGPO `24456–24505`) |
| `ROOM_CAPACITY_WARNING_PERCENT` | 80 | Soft dashboard/health warning only |
| `ROOM_LOBBY_IDLE_MS` | 300000 (5m) | Drop host-only rooms (no joiner) |
| `ROOM_OCCUPIED_IDLE_MS` | 0 (disabled) | Optional idle timeout for rooms with a guest or match; `0` keeps them until explicit deletion or broker restart |
| `ROOM_IDLE_MS` | (legacy) | Alias for `ROOM_OCCUPIED_IDLE_MS` if occupied var unset |
| `RELAY_HOST` | 127.0.0.1 | Address returned to clients |
| `RELAY_PORT_BASE` | 23456 | First port in pool |
| `GGPO_UDP_PORT_BASE` | 24456 | First GGPO UDP relay port |
| `FORCE_VPS_RELAY` | (unset) | When set, rooms use VPS relays instead of host-PC IP |
| `BROKER_GGPO_TRANSPORT` | auto | `auto` / `udp` / `legacy` — connect-plan transport hint |

Monitor VPS egress in your provider dashboard; pause new rooms if usage exceeds ~80% of your cap.

Existing VPS installs keep their live `/root/room-broker/.env` across deploys. See [docs/VPS_CAPACITY_50.md](../../docs/VPS_CAPACITY_50.md) to raise capacity to 50 on a host that still has `MAX_ROOMS=20`.

## API

| Method | Path | Purpose |
|--------|------|---------|
| `GET` | `/v1/health` | Status, room count, `maxRooms`, relay/GGPO base+end ports, `forceVpsRelay` |
| `GET` | `/v1/matches` | Recent / active match metadata |
| `GET` | `/v1/rooms` | Open room list (launcher browser) |
| `POST` | `/v1/rooms` | Create room → code, host, ports, secrets |
| `GET` | `/v1/rooms/SF4-XXXX` | Resolve join target |
| `GET` | `/v1/rooms/SF4-XXXX/connect-plan` | Connect-plan for host/guest (transport + relay ports) |
| `POST` | `/v1/rooms/SF4-XXXX/register-endpoint` | Register observed GGPO endpoint (after NAT probe) |
| `GET` | `/v1/rooms/SF4-XXXX/health` | Per-room relay / GGPO health |
| `POST` | `/v1/rooms/SF4-XXXX/heartbeat` | Keepalive while launcher is open |
| `GET` | `/v1/rooms/SF4-XXXX/events` | Room event stream / polling |
| `POST` | `/v1/queue/join` | Quick match (pairs when 2+ waiting) |

Capacity migration: [docs/VPS_CAPACITY_50.md](../../docs/VPS_CAPACITY_50.md). TLS layout: [docs/VPS_TLS_SETUP.md](../../docs/VPS_TLS_SETUP.md).

## Tests

```bash
node --test services/room-broker/test/capacity.test.js
bash services/room-broker/test/secure-ufw.test.sh
python scripts/check-vps-capacity.py
```
