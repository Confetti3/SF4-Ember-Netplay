# VPS TLS and port hardening

Production SF4e netplay exposes **HTTPS (443)** and **game relay UDP/TCP** only. The room broker, relay manager, and dashboard listen on **127.0.0.1** and are reached through **Caddy** with automatic Let's Encrypt certificates.

## Architecture

```
Internet
   │
   ├─ :443/tcp  ──► Caddy ──► 127.0.0.1:8787  (room broker /v1/*)
   │              └──► 127.0.0.1:8789  (dashboard)
   │
   ├─ :80/tcp   ──► Caddy (ACME + HTTP→HTTPS redirect)
   │
   ├─ :23456-23505/tcp+udp  ──► sf4e-session-relay (GNS)
   ├─ :24456-24505/udp      ──► sf4e-ggpo-udp-relay
   └─ :8790/udp             ──► NAT probe (connect-plan)

NOT public: 8787, 8788, 8789 (localhost only)
```

## Prerequisites

1. **Domain name** with DNS A record pointing at your VPS (e.g. `netplay.example.com` → `74.208.200.95`).
2. Broker and dashboard deployed (`scripts/deploy-broker-vps.py`, `scripts/deploy-dashboard-vps.py`).
3. Ports **80** and **443** reachable from the internet (for ACME and HTTPS).

> IP-only TLS (no domain) is not supported by Let's Encrypt. Use a domain or a self-signed cert for lab setups only.

## Quick install (on VPS)

After copying broker files to `/root/room-broker`:

```bash
export SF4E_BROKER_DOMAIN=netplay.example.com
# optional separate admin host:
# export SF4E_DASHBOARD_DOMAIN=admin.example.com
# optional ACME email:
# export SF4E_ACME_EMAIL=you@example.com

cd /root/room-broker
bash install-caddy.sh
```

This will:

- Install Caddy and write `/etc/caddy/Caddyfile`
- Set `BROKER_BIND=127.0.0.1`, `BROKER_TRUST_PROXY=1`, `BROKER_PUBLIC_URL=https://…`
- Set dashboard `DASHBOARD_BIND=127.0.0.1`, `COOKIE_SECURE=1`
- Run `secure-ufw.sh` (443, 80, relay ports, NAT probe only)

## Deploy from Windows (one shot)

**Persistent Windows credentials (recommended):**

```powershell
# One-time: copy scripts\.vps-env.ps1.example → scripts\.vps-env.ps1, edit password, then:
.\scripts\setup-vps-env.ps1 -FromSecretsFile
# Or interactive (password not stored in a file):
.\scripts\setup-vps-env.ps1
```

Restart Cursor/terminal after `setup-vps-env.ps1` so new User env vars are visible. For the current session only: `. .\scripts\load-vps-env.ps1`

```powershell
$env:SF4E_BROKER_DOMAIN = "netplay.example.com"
python scripts/deploy-broker-vps.py
python scripts/configure-room-idle-vps.py
python scripts/deploy-dashboard-vps.py
```

If `SF4E_BROKER_DOMAIN` is set, `deploy-broker-vps.py` runs `install-caddy.sh` after broker install.

## Launcher configuration

Set broker URL to your HTTPS domain (no port):

```
https://netplay.example.com
```

The launcher **rejects plain HTTP** for broker URLs unless you set:

```
SF4E_ALLOW_HTTP_BROKER=1
```

Use that only for local/dev; never in production.

For local broker testing:

```
SF4E_ALLOW_LOCAL_BROKER=1
SF4E_ALLOW_HTTP_BROKER=1
```

## Firewall rules (`secure-ufw.sh`)

| Port | Protocol | Service |
|------|----------|---------|
| 22 | tcp | SSH |
| 80 | tcp | ACME + redirect |
| 443 | tcp | HTTPS (Caddy) |
| 23456–23505 | tcp+udp | Session relay (`MAX_ROOMS=50` default) |
| 24456–24505 | udp | GGPO UDP relay |
| 8790 | udp | NAT probe |

**Blocked from internet:** 8787 (broker), 8788 (relay-manager), 8789 (dashboard).

Port ranges follow `RELAY_PORT_BASE`, `GGPO_UDP_PORT_BASE`, and `MAX_ROOMS` in `/root/room-broker/.env`.
`secure-ufw.sh` derives end ports dynamically and validates ranges **before** resetting UFW.

**Provider firewalls are separate:** expand the same ranges in IONOS / cloud security groups / hosting ACLs. UFW alone is not enough. See [VPS_CAPACITY_50.md](VPS_CAPACITY_50.md).

## Trust proxy

When behind Caddy, the broker and dashboard only trust `X-Forwarded-For` from **127.0.0.1**. Direct public exposure with `BROKER_TRUST_PROXY=1` would allow IP spoofing — always bind to localhost in production.

## Verification

```bash
curl -s https://netplay.example.com/v1/health
curl -sI https://netplay.example.com/login | grep -i strict-transport
ss -tlnp | grep -E '8787|8788|8789'   # should show 127.0.0.1 only
sudo ufw status numbered
```

From the launcher: create/join a room using `https://your-domain` as broker URL.

## Rollback / dev without TLS

In `/root/room-broker/.env`:

```
BROKER_BIND=0.0.0.0
BROKER_TRUST_PROXY=0
```

In dashboard `.env`:

```
DASHBOARD_BIND=0.0.0.0
COOKIE_SECURE=0
DASHBOARD_TRUST_PROXY=0
```

Open firewall temporarily (not recommended for production):

```bash
ufw allow 8787/tcp
ufw allow 8789/tcp
```

Set launcher `SF4E_ALLOW_HTTP_BROKER=1` while using `http://vps-ip:8787`.

## Upgrading an existing VPS (HTTP on :8787)

1. Create DNS A record for your domain **before** running `secure-ufw.sh`.
2. Deploy broker + dashboard, then run Caddy **before** or **with** the same deploy:

   ```powershell
   $env:SF4E_BROKER_DOMAIN = "netplay.example.com"
   python scripts/deploy-broker-vps.py
   ```

3. Update launcher broker URL to `https://netplay.example.com`.
4. Rebuild launcher (HTTPS required unless `SF4E_ALLOW_HTTP_BROKER=1`).

If you already ran `secure-ufw.sh` without Caddy, use SSH and run `bash install-caddy.sh` or temporarily `ufw allow 8787/tcp` until TLS works.

## Related

- [Caddyfile.example](../services/room-broker/Caddyfile.example)
- [SECURITY_REMEDIATION.md](SECURITY_REMEDIATION.md) — P2-3 (SEC-008)
