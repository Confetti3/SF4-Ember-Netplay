#!/usr/bin/env python3
"""Compile sf4e-ggpo-udp-relay standalone on VPS (no SessionRelay/Dimps)."""
import os
import sys
from pathlib import Path

import paramiko

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "relay" / "ggpo_udp_relay.cxx"
REMOTE_SRC = "/tmp/ggpo_udp_relay.cxx"
REMOTE_BIN = "/opt/sf4e-relay/bin/sf4e-ggpo-udp-relay"


def safe_print(text: str) -> None:
    sys.stdout.buffer.write(text.encode("utf-8", errors="replace"))
    if not text.endswith("\n"):
        sys.stdout.buffer.write(b"\n")


def main() -> int:
    password = os.environ.get("SF4E_VPS_PASSWORD", "")
    if not password:
        print("Set SF4E_VPS_PASSWORD", file=sys.stderr)
        return 1

    host = os.environ.get("SF4E_VPS_HOST", "74.208.200.95")
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(host, username="root", password=password, timeout=60, allow_agent=False, look_for_keys=False)

    sftp = client.open_sftp()
    try:
        sftp.put(str(SRC), REMOTE_SRC)
    finally:
        sftp.close()

    commands = f"""set -e
VCPKG=/opt/vcpkg/installed/x64-linux
g++ -std=c++17 -O2 -pipe \\
  -I"$VCPKG/include" \\
  {REMOTE_SRC} \\
  -o {REMOTE_BIN} \\
  -L"$VCPKG/lib" -Wl,-rpath,"$VCPKG/lib" \\
  -lCLI11 -lspdlog -lfmt -pthread
chmod +x {REMOTE_BIN}
ls -la {REMOTE_BIN}
systemctl restart sf4e-relay-manager
sleep 2
curl -s http://127.0.0.1:8788/v1/health; echo
"""
    _, stdout, stderr = client.exec_command(commands, get_pty=True, timeout=300)
    out = stdout.read().decode("utf-8", errors="replace")
    err = stderr.read().decode("utf-8", errors="replace")
    code = stdout.channel.recv_exit_status()
    client.close()
    if out.strip():
        safe_print(out)
    if err.strip():
        safe_print(err)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
