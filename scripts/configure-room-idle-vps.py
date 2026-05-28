#!/usr/bin/env python3
"""Set tiered room idle timeouts on VPS broker .env and restart sf4e-broker."""

import os
import sys
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))
from vps_ssh import connect_ssh

LOBBY_MS = os.environ.get("SF4E_ROOM_LOBBY_IDLE_MS", "300000")
OCCUPIED_MS = os.environ.get("SF4E_ROOM_OCCUPIED_IDLE_MS", "1800000")
ENV_PATH = "/root/room-broker/.env"


def main() -> int:
    host = os.environ.get("SF4E_VPS_HOST", "74.208.200.95")
    user = os.environ.get("SF4E_VPS_USER", "root")
    password = os.environ.get("SF4E_VPS_PASSWORD", "")
    if not password:
        print("Set SF4E_VPS_PASSWORD", file=sys.stderr)
        return 1

    commands = f"""set -e
ENV="{ENV_PATH}"
touch "$ENV"
grep -q '^ROOM_LOBBY_IDLE_MS=' "$ENV" 2>/dev/null && sed -i 's/^ROOM_LOBBY_IDLE_MS=.*/ROOM_LOBBY_IDLE_MS={LOBBY_MS}/' "$ENV" || echo 'ROOM_LOBBY_IDLE_MS={LOBBY_MS}' >> "$ENV"
grep -q '^ROOM_OCCUPIED_IDLE_MS=' "$ENV" 2>/dev/null && sed -i 's/^ROOM_OCCUPIED_IDLE_MS=.*/ROOM_OCCUPIED_IDLE_MS={OCCUPIED_MS}/' "$ENV" || echo 'ROOM_OCCUPIED_IDLE_MS={OCCUPIED_MS}' >> "$ENV"
systemctl restart sf4e-broker.service
sleep 2
curl -s http://127.0.0.1:8787/v1/health
echo
"""
    print(f"Connecting to {user}@{host}...")
    client = connect_ssh(
        host, username=user, password=password, timeout=30, allow_agent=False, look_for_keys=False
    )
    _, stdout, stderr = client.exec_command(commands, get_pty=True, timeout=120)
    out = stdout.read().decode("utf-8", errors="replace")
    err = stderr.read().decode("utf-8", errors="replace")
    code = stdout.channel.recv_exit_status()
    if out.strip():
        print(out)
    if err.strip():
        print(err, file=sys.stderr)
    client.close()
    if code != 0:
        print(f"Remote failed (exit {code})", file=sys.stderr)
        return code
    print(f"Configured ROOM_LOBBY_IDLE_MS={LOBBY_MS} ROOM_OCCUPIED_IDLE_MS={OCCUPIED_MS}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
