#!/usr/bin/env python3
"""Deploy 50-room capacity files to the VPS and migrate live MAX_ROOMS.

Preserves unrelated .env keys. Backs up .env before changing MAX_ROOMS.
Does not modify provider/cloud firewalls (IONOS etc.).
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))
from vps_ssh import connect_ssh

ROOT = Path(__file__).resolve().parents[1]
MAX_ROOMS = os.environ.get("SF4E_TARGET_MAX_ROOMS", "50")

UPLOADS = [
    (ROOT / "services" / "room-broker" / "server.js", "/root/room-broker/server.js"),
    (ROOT / "services" / "room-broker" / "capacity-config.js", "/root/room-broker/capacity-config.js"),
    (ROOT / "services" / "room-broker" / "secure-ufw.sh", "/root/room-broker/secure-ufw.sh"),
    (ROOT / "services" / "room-broker" / ".env.example", "/root/room-broker/.env.example"),
    (ROOT / "services" / "room-broker" / "install-vps.sh", "/root/room-broker/install-vps.sh"),
    (
        ROOT / "services" / "room-broker" / "install-vps-relay.sh",
        "/root/room-broker/install-vps-relay.sh",
    ),
    (
        ROOT / "services" / "vps-relay" / "dashboard" / "server.js",
        "/opt/sf4e-relay/dashboard/server.js",
    ),
]


def safe_print(text: str) -> None:
    sys.stdout.buffer.write(text.encode("utf-8", errors="replace"))
    if not text.endswith("\n"):
        sys.stdout.buffer.write(b"\n")


def main() -> int:
    host = os.environ.get("SF4E_VPS_HOST", "74.208.200.95")
    user = os.environ.get("SF4E_VPS_USER", "root")
    password = os.environ.get("SF4E_VPS_PASSWORD", "")
    if not password:
        print("Set SF4E_VPS_PASSWORD", file=sys.stderr)
        return 1

    for local, _remote in UPLOADS:
        if not local.exists():
            print(f"Missing local file: {local}", file=sys.stderr)
            return 1

    print(f"Connecting to {user}@{host}...")
    client = connect_ssh(
        host,
        username=user,
        password=password,
        timeout=60,
        allow_agent=False,
        look_for_keys=False,
    )

    sftp = client.open_sftp()
    try:
        for local, remote in UPLOADS:
            print(f"Uploading {local.relative_to(ROOT)} -> {remote}")
            sftp.put(str(local), remote)
    finally:
        sftp.close()

    commands = f"""set -euo pipefail
cd /root/room-broker
sed -i 's/\\r$//' server.js capacity-config.js secure-ufw.sh install-vps.sh install-vps-relay.sh .env.example 2>/dev/null || true
chmod +x secure-ufw.sh install-vps.sh install-vps-relay.sh

# Backup and set capacity (do not rewrite the rest of .env).
cp -a .env ".env.backup-$(date +%Y%m%d-%H%M%S)"
if grep -q '^MAX_ROOMS=' .env; then
  sed -i 's/^MAX_ROOMS=.*/MAX_ROOMS={MAX_ROOMS}/' .env
else
  echo 'MAX_ROOMS={MAX_ROOMS}' >> .env
fi
if grep -q '^ROOM_CAPACITY_WARNING_PERCENT=' .env; then
  sed -i 's/^ROOM_CAPACITY_WARNING_PERCENT=.*/ROOM_CAPACITY_WARNING_PERCENT=80/' .env
else
  echo 'ROOM_CAPACITY_WARNING_PERCENT=80' >> .env
fi

echo '=== .env capacity lines ==='
grep -E '^(MAX_ROOMS|RELAY_PORT_BASE|GGPO_UDP_PORT_BASE|ROOM_CAPACITY_WARNING_PERCENT)=' .env || true

echo '=== applying UFW (local only; provider firewall is separate) ==='
bash ./secure-ufw.sh

echo '=== restarting services ==='
systemctl restart sf4e-broker
systemctl restart sf4e-relay-manager
systemctl restart sf4e-relay-dashboard 2>/dev/null || true
sleep 2
systemctl is-active sf4e-broker sf4e-relay-manager || true
systemctl is-active sf4e-relay-dashboard 2>/dev/null || true

echo '=== health ==='
curl -s http://127.0.0.1:8787/v1/health; echo
curl -s http://127.0.0.1:8788/v1/health; echo

echo '=== ufw numbered (relay ranges) ==='
ufw status numbered | grep -E '23456|24456|8790|Status' || ufw status numbered | head -n 40
"""
    print("Migrating MAX_ROOMS and applying UFW...")
    _, stdout, stderr = client.exec_command(commands, get_pty=True, timeout=300)
    out = stdout.read().decode("utf-8", errors="replace")
    err = stderr.read().decode("utf-8", errors="replace")
    code = stdout.channel.recv_exit_status()
    client.close()
    if out.strip():
        safe_print(out)
    if err.strip():
        safe_print(err)
    if code != 0:
        print(f"Remote migrate failed (exit {code})", file=sys.stderr)
        return code
    print(f"VPS capacity migrate complete (MAX_ROOMS={MAX_ROOMS}).")
    print("Remember: expand IONOS/cloud firewall to 23456-23505 and 24456-24505 separately.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
