# VPS capacity: 50-room default

Repository defaults now configure a single VPS broker for **up to 50 simultaneous rooms**.

This is a **configured ceiling**, not a claim that 50 concurrent matches have been load-tested.

## Default ranges

| Setting | Default |
|---------|---------|
| `MAX_ROOMS` | `50` |
| Session relay | `23456–23505` TCP/UDP |
| GGPO UDP relay | `24456–24505` UDP |
| Max active matches | 50 |
| Max active players | 100 (2 per room) |
| Max connected clients | 200 (2 players + 2 spectators per room) |

Base ports remain `RELAY_PORT_BASE=23456` and `GGPO_UDP_PORT_BASE=24456`. End ports are always derived as `base + MAX_ROOMS - 1`.

## Repository defaults vs live VPS `.env`

- New installs copy `services/room-broker/.env.example` → `/root/room-broker/.env` **only when `.env` does not already exist**.
- `scripts/deploy-broker-vps.py` and `install-vps.sh` **preserve** an existing live `.env`.
- An existing VPS that still has `MAX_ROOMS=20` will keep that value after a normal deploy until an operator edits `.env`.

## Live migration (existing VPS)

Service unit names in this repo:

- Broker: `sf4e-broker.service` (`/etc/systemd/system/sf4e-broker.service`)
- Relay manager: `sf4e-relay-manager.service`

**Warning:** Restarting these services interrupts active rooms. Perform the change when no important match is running. Back up `.env` first.

```bash
cd /root/room-broker
sudo cp .env ".env.backup-$(date +%Y%m%d-%H%M%S)"
sudo sed -i 's/^MAX_ROOMS=.*/MAX_ROOMS=50/' .env
# Ensure capacity-config.js was deployed with the broker (see deploy-broker-vps.py).
sudo bash secure-ufw.sh
sudo systemctl restart sf4e-broker
sudo systemctl restart sf4e-relay-manager
```

### Provider / cloud firewall (required separately)

`secure-ufw.sh` only updates **local UFW**. It cannot modify:

- VPS provider firewall panels (e.g. IONOS)
- Cloud security groups
- Hosting-provider network ACLs

Expand the same ranges there **before** relying on rooms above the old 20-room ceiling:

- Session: `23456–23505` TCP+UDP
- GGPO: `24456–24505` UDP
- NAT probe: `8790` UDP (unchanged)

### Verify

```bash
curl -s https://<broker-domain>/v1/health
# Expect maxRooms: 50, relayPortEnd: 23505, ggpoUdpPortEnd: 24505

sudo ufw status numbered
ss -lntup
systemctl status sf4e-broker
systemctl status sf4e-relay-manager

# From a workstation with repo checkout (no private credentials required):
python scripts/check-vps-capacity.py --max-rooms 50 \
  --health-url https://<broker-domain>/v1/health
```

## Rollback to 20 rooms

```bash
cd /root/room-broker
sudo cp .env ".env.backup-$(date +%Y%m%d-%H%M%S)"
sudo sed -i 's/^MAX_ROOMS=.*/MAX_ROOMS=20/' .env
sudo bash secure-ufw.sh
sudo systemctl restart sf4e-broker
sudo systemctl restart sf4e-relay-manager
curl -s https://<broker-domain>/v1/health   # expect maxRooms: 20
```

Optionally shrink provider firewall rules back to `23456–23475` / `24456–24475` after confirmation.

## Operator checks without creating rooms

```bash
python scripts/check-vps-capacity.py
python scripts/check-vps-capacity.py --max-rooms 50 --health-url https://<broker-domain>/v1/health
```

## Soft capacity warning

Optional env (dashboard / health only; does **not** block creates below `MAX_ROOMS`):

```env
ROOM_CAPACITY_WARNING_PERCENT=80
```

`/v1/health` includes `capacityWarning` when utilization crosses that percent. The operator dashboard colors room counts around 60% / 80% / 95%.
