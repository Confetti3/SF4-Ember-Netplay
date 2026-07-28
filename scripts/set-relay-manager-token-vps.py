#!/usr/bin/env python3
"""Set RELAY_MANAGER_TOKEN on the VPS from local env (never prints the secret)."""

from __future__ import annotations

import os
import sys
from pathlib import Path

import paramiko

_SCRIPT_DIR = Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))
from vps_ssh import connect_ssh


def main() -> int:
    token = os.environ.get("RELAY_MANAGER_TOKEN", "").strip()
    if not token:
        print("Set RELAY_MANAGER_TOKEN in the environment (or scripts/.vps-env.ps1)", file=sys.stderr)
        return 1
    password = os.environ.get("SF4E_VPS_PASSWORD", "")
    if not password:
        print("Set SF4E_VPS_PASSWORD", file=sys.stderr)
        return 1

    host = os.environ.get("SF4E_VPS_HOST", "74.208.200.95")
    user = os.environ.get("SF4E_VPS_USER", "root")
    client = connect_ssh(
        host, username=user, password=password, timeout=30, allow_agent=False, look_for_keys=False
    )

    # Write token via SFTP so we never embed it in a shell command line.
    sftp = client.open_sftp()
    try:
        env_path = "/root/room-broker/.env"
        try:
            with sftp.open(env_path, "r") as f:
                lines = f.read().decode("utf-8", errors="replace").splitlines()
        except OSError:
            lines = []
        out_lines = [ln for ln in lines if not ln.startswith("RELAY_MANAGER_TOKEN=")]
        out_lines.append(f"RELAY_MANAGER_TOKEN={token}")
        with sftp.open(env_path, "w") as f:
            f.write("\n".join(out_lines) + "\n")
    finally:
        sftp.close()

    cmd = """set -e
bash /opt/sf4e-relay/install-vps-relay.sh
systemctl daemon-reload
systemctl restart sf4e-relay-manager.service
systemctl restart sf4e-broker.service
sleep 2
echo --- services ---
systemctl is-active sf4e-broker.service sf4e-relay-manager.service
echo --- relay health ---
curl -s http://127.0.0.1:8788/v1/health
echo
echo --- broker health ---
curl -s http://127.0.0.1:8787/v1/health
echo
echo --- session binary ---
ls -la /opt/sf4e-relay/bin/sf4e-session-relay 2>/dev/null || echo MISSING_SESSION_RELAY
echo --- token_line_present ---
grep -c '^RELAY_MANAGER_TOKEN=.' /root/room-broker/.env || true
echo --- envfiles ---
systemctl show sf4e-relay-manager.service -p EnvironmentFiles --value || true
"""
    _, stdout, stderr = client.exec_command(cmd, get_pty=True, timeout=180)
    out = stdout.read().decode("utf-8", errors="replace")
    err = stderr.read().decode("utf-8", errors="replace")
    code = stdout.channel.recv_exit_status()
    if out.strip():
        sys.stdout.buffer.write(out.encode("utf-8", errors="replace"))
        if not out.endswith("\n"):
            sys.stdout.buffer.write(b"\n")
    if err.strip():
        sys.stderr.buffer.write(err.encode("utf-8", errors="replace"))
    client.close()
    if code != 0:
        print(f"Remote command failed (exit {code})", file=sys.stderr)
        return code
    print("RELAY_MANAGER_TOKEN applied on VPS (value kept local-only).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
