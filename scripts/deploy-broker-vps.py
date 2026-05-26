#!/usr/bin/env python3
"""Deploy room broker to a Linux VPS over SSH/SFTP. Password via SF4E_VPS_PASSWORD env var."""

import os
import sys
from pathlib import Path

import paramiko

ROOT = Path(__file__).resolve().parents[1]
BROKER_DIR = ROOT / "services" / "room-broker"
FILES = ("server.js", ".env.example", "install-vps.sh")


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

    remote_dir = "/root/room-broker"
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    print(f"Connecting to {user}@{host}...")
    client.connect(host, username=user, password=password, timeout=30, allow_agent=False, look_for_keys=False)

    sftp = client.open_sftp()
    try:
        try:
            sftp.mkdir(remote_dir)
        except OSError:
            pass
        for name in FILES:
            local_path = BROKER_DIR / name
            remote_path = f"{remote_dir}/{name}"
            print(f"Uploading {name}...")
            sftp.put(str(local_path), remote_path)
    finally:
        sftp.close()

    commands = f"""set -e
cd {remote_dir}
cp -f .env.example .env
sed -i 's/\\r$//' server.js install-vps.sh .env .env.example 2>/dev/null || true
chmod +x install-vps.sh
bash install-vps.sh
"""
    print("Running install...")
    _, stdout, stderr = client.exec_command(commands, get_pty=True, timeout=300)
    out = stdout.read().decode("utf-8", errors="replace")
    err = stderr.read().decode("utf-8", errors="replace")
    code = stdout.channel.recv_exit_status()
    if out.strip():
        safe_print(out)
    if err.strip():
        safe_print(err)
    client.close()
    if code != 0:
        print(f"Remote install failed (exit {code})", file=sys.stderr)
        return code
    print("Broker deploy complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
