#!/usr/bin/env python3
"""Build sf4e-session-relay on VPS (long-running, detached on server)."""

import argparse
import os
import sys
import time
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))
from vps_ssh import connect_ssh

LOG_PATH = "/tmp/sf4e-relay-build.log"
PID_PATH = "/tmp/sf4e-relay-build.pid"

STATUS_CMD = f"""(
  test -x /opt/sf4e-relay/bin/sf4e-session-relay && ls -la /opt/sf4e-relay/bin/sf4e-session-relay || echo 'binary: missing'
  if [ -f {PID_PATH} ] && kill -0 $(cat {PID_PATH}) 2>/dev/null; then echo "build: running pid=$(cat {PID_PATH})"; else echo 'build: not running'; fi
  echo '--- log tail ---'
  tail -8 {LOG_PATH} 2>/dev/null || echo 'log: (none)'
)"""


def connect(retries: int = 5) -> paramiko.SSHClient:
    password = os.environ.get("SF4E_VPS_PASSWORD", "")
    if not password:
        print("Set SF4E_VPS_PASSWORD", file=sys.stderr)
        raise SystemExit(1)

    host = os.environ.get("SF4E_VPS_HOST", "74.208.200.95")
    user = os.environ.get("SF4E_VPS_USER", "root")

    last_err = None
    for attempt in range(retries):
        try:
            client = connect_ssh(
                host,
                username=user,
                password=password,
                timeout=120,
                banner_timeout=120,
                auth_timeout=120,
                allow_agent=False,
                look_for_keys=False,
            )
            transport = client.get_transport()
            if transport:
                transport.set_keepalive(30)
            return client
        except Exception as exc:  # noqa: BLE001
            last_err = exc
            client.close()
            if attempt + 1 < retries:
                wait = 15 * (attempt + 1)
                print(f"SSH connect failed ({exc}); retry in {wait}s...", file=sys.stderr)
                time.sleep(wait)
    raise SystemExit(f"SSH connect failed after {retries} attempts: {last_err}")


def run(client: paramiko.SSHClient, cmd: str, timeout: int = 180) -> tuple[int, str]:
    _, stdout, _ = client.exec_command(cmd, timeout=timeout)
    out = stdout.read().decode("utf-8", errors="replace")
    code = stdout.channel.recv_exit_status()
    return code, out


def fetch_status() -> tuple[bool, bool, str]:
    """Returns (binary_ready, build_running, output_text)."""
    client = connect()
    try:
        _, out = run(client, STATUS_CMD, timeout=180)
    finally:
        client.close()

    binary_ready = "sf4e-session-relay" in out and "binary: missing" not in out.splitlines()[0]
    build_running = "build: running pid=" in out
    return binary_ready, build_running, out.rstrip()


def show_status() -> int:
    try:
        binary_ready, build_running, out = fetch_status()
    except SystemExit as exc:
        print(exc, file=sys.stderr)
        return 1

    print(out)
    if binary_ready:
        print("\nSTATUS: SUCCESS - binary ready")
        return 0
    if build_running:
        print("\nSTATUS: BUILDING")
        return 0
    print("\nSTATUS: NOT RUNNING (binary missing)")
    return 1


def watch_status(interval_sec: int) -> int:
    print(f"Watching VPS relay build every {interval_sec}s (Ctrl+C to stop)...")
    while True:
        ts = time.strftime("%Y-%m-%d %H:%M:%S")
        print(f"\n=== {ts} ===")
        try:
            binary_ready, build_running, out = fetch_status()
            print(out)
            if binary_ready:
                print("\nDONE: sf4e-session-relay built successfully.")
                return 0
            if not build_running:
                print("\nFAILED: build stopped but binary is still missing.")
                print(f"Inspect on VPS: tail -50 {LOG_PATH}")
                return 1
            print("\nStill building...")
        except SystemExit as exc:
            print(f"Status check failed: {exc}", file=sys.stderr)
        time.sleep(interval_sec)


def start_build() -> int:
    client = connect()
    try:
        _, out = run(
            client,
            "ps aux | grep -E '[v]cpkg install|[b]uild-linux.sh|[s]f4e-relay-build.sh' || true",
            timeout=60,
        )
        if out.strip():
            print("Build already in progress on VPS:")
            print(out.rstrip())
            print("Check progress: python scripts/vps-build-relay.py --watch")
            return 0

        build_script = f"""#!/bin/bash
set -euo pipefail
exec > {LOG_PATH} 2>&1
echo "[$(date -Is)] sf4e relay build started"
if ! swapon --show | grep -q .; then
  if [ ! -f /swapfile ]; then
    echo "[$(date -Is)] creating 2G swap (low RAM VPS)"
    fallocate -l 2G /swapfile || dd if=/dev/zero of=/swapfile bs=1M count=2048
    chmod 600 /swapfile
    mkswap /swapfile
  fi
  swapon /swapfile || true
  grep -q '^/swapfile' /etc/fstab || echo '/swapfile none swap sw 0 0' >> /etc/fstab
fi
free -h
export VCPKG_ROOT=/opt/vcpkg
export VCPKG_MAX_CONCURRENCY=1
if [ ! -x /opt/vcpkg/vcpkg ]; then
  echo "vcpkg missing at /opt/vcpkg"
  exit 1
fi
/opt/vcpkg/vcpkg install cli11 nlohmann-json spdlog gamenetworkingsockets --triplet x64-linux
cd /opt/sf4e-relay
sed -i 's/\\r$//' build-linux.sh
chmod +x build-linux.sh
export VCPKG_ROOT=/opt/vcpkg
export VCPKG_MAX_CONCURRENCY=1
bash build-linux.sh
chmod +x /opt/sf4e-relay/bin/sf4e-session-relay
systemctl restart sf4e-relay-manager
echo "[$(date -Is)] build complete"
ls -la /opt/sf4e-relay/bin/sf4e-session-relay
curl -s http://127.0.0.1:8788/v1/health || true
"""

        remote_script = "/tmp/sf4e-relay-build.sh"
        sftp = client.open_sftp()
        try:
            with sftp.file(remote_script, "w") as f:
                f.write(build_script)
        finally:
            sftp.close()

        launch = (
            f"chmod +x {remote_script}; "
            f"if [ -f {PID_PATH} ] && kill -0 $(cat {PID_PATH}) 2>/dev/null; then "
            f"echo 'Build already running (pid='$(cat {PID_PATH})')'; "
            f"else nohup {remote_script} >/dev/null 2>&1 & echo $! > {PID_PATH}; "
            f"echo 'Started background build pid='$(cat {PID_PATH}); fi"
        )
        _, out = run(client, launch, timeout=60)
        print(out.rstrip())
        print(f"Watch progress: python scripts/vps-build-relay.py --watch")
        return 0
    finally:
        client.close()


def main() -> int:
    parser = argparse.ArgumentParser(description="Build sf4e-session-relay on VPS")
    parser.add_argument("--status", action="store_true", help="Show build/binary status once")
    parser.add_argument("--watch", action="store_true", help="Poll until build succeeds or fails")
    parser.add_argument("--interval", type=int, default=90, help="Watch poll interval seconds")
    args = parser.parse_args()

    if args.watch:
        return watch_status(max(30, args.interval))
    if args.status:
        return show_status()
    return start_build()


if __name__ == "__main__":
    raise SystemExit(main())
