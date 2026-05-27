#!/usr/bin/env python3
"""Deploy room broker + VPS relay manager to Linux VPS over SSH/SFTP."""

import os
import sys
from pathlib import Path

import paramiko

ROOT = Path(__file__).resolve().parents[1]
BROKER_DIR = ROOT / "services" / "room-broker"
RELAY_DIR = ROOT / "services" / "vps-relay"
BROKER_FILES = (
    "server.js",
    ".env.example",
    "install-vps.sh",
    "install-vps-relay.sh",
    "install-caddy.sh",
    "secure-ufw.sh",
    "Caddyfile.example",
)
RELAY_FILES = ("relay-manager.js", "install-vps-relay.sh", "build-linux.sh")


def safe_print(text: str) -> None:
    sys.stdout.buffer.write(text.encode("utf-8", errors="replace"))
    if not text.endswith("\n"):
        sys.stdout.buffer.write(b"\n")


def mkdir_p(sftp: paramiko.SFTPClient, remote_dir: str) -> None:
    parts = remote_dir.rstrip("/").split("/")
    path = ""
    for part in parts:
        if not part:
            continue
        path = f"{path}/{part}" if path else f"/{part}"
        try:
            sftp.mkdir(path)
        except OSError:
            pass


def upload_tree(sftp: paramiko.SFTPClient, local_dir: Path, remote_dir: str) -> None:
    mkdir_p(sftp, remote_dir)
    for item in local_dir.iterdir():
        remote_path = f"{remote_dir}/{item.name}"
        if item.is_dir():
            upload_tree(sftp, item, remote_path)
        else:
            print(f"Uploading {item.relative_to(ROOT)}...")
            sftp.put(str(item), remote_path)


def main() -> int:
    host = os.environ.get("SF4E_VPS_HOST", "74.208.200.95")
    user = os.environ.get("SF4E_VPS_USER", "root")
    password = os.environ.get("SF4E_VPS_PASSWORD", "")
    if not password:
        print("Set SF4E_VPS_PASSWORD", file=sys.stderr)
        return 1

    remote_broker = "/root/room-broker"
    remote_relay = "/opt/sf4e-relay"
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    print(f"Connecting to {user}@{host}...")
    client.connect(host, username=user, password=password, timeout=30, allow_agent=False, look_for_keys=False)

    sftp = client.open_sftp()
    try:
        mkdir_p(sftp, remote_broker)
        mkdir_p(sftp, remote_relay)
        mkdir_p(sftp, f"{remote_relay}/bin")
        mkdir_p(sftp, f"{remote_relay}/lib")

        gns_lib = RELAY_DIR / "lib" / "libGameNetworkingSockets.so"
        if not gns_lib.exists():
            fallback = Path(
                os.environ.get(
                    "SF4E_GNS_LIB",
                    str(Path.home() / "vcpkg" / "installed" / "x64-linux" / "lib" / "libGameNetworkingSockets.so"),
                )
            )
            if fallback.exists():
                gns_lib = fallback
        if gns_lib.exists():
            print(f"Uploading {gns_lib.name}...")
            sftp.put(str(gns_lib), f"{remote_relay}/lib/libGameNetworkingSockets.so")
            sftp.chmod(f"{remote_relay}/lib/libGameNetworkingSockets.so", 0o755)
        else:
            print("WARNING: libGameNetworkingSockets.so not found — relay will not start on VPS.", file=sys.stderr)

        for name in BROKER_FILES:
            local_path = BROKER_DIR / name
            if not local_path.exists():
                local_path = RELAY_DIR / name
            if not local_path.exists():
                print(f"Skip missing {name}", file=sys.stderr)
                continue
            remote_path = f"{remote_broker}/{name}"
            print(f"Uploading {name}...")
            sftp.put(str(local_path), remote_path)

        for name in RELAY_FILES:
            local_path = RELAY_DIR / name
            if local_path.exists():
                print(f"Uploading vps-relay/{name}...")
                sftp.put(str(local_path), f"{remote_relay}/{name}")

        local_bin = RELAY_DIR / "bin" / "sf4e-session-relay"
        if local_bin.exists():
            print("Uploading sf4e-session-relay binary...")
            sftp.put(str(local_bin), f"{remote_relay}/bin/sf4e-session-relay")
            sftp.chmod(f"{remote_relay}/bin/sf4e-session-relay", 0o755)
        else:
            print("No local sf4e-session-relay binary — VPS will build from source if vcpkg is installed.")
            upload_tree(sftp, ROOT / "src" / "relay", f"{remote_relay}/src/relay")
            upload_tree(sftp, ROOT / "src" / "session", f"{remote_relay}/src/session")
            upload_tree(sftp, ROOT / "src" / "Dimps", f"{remote_relay}/src/Dimps")
            upload_tree(sftp, ROOT / "src" / "common", f"{remote_relay}/src/common")
            sftp.put(str(RELAY_DIR / "CMakeLists.txt"), f"{remote_relay}/CMakeLists.txt")

        ggpo_bin = RELAY_DIR / "bin" / "sf4e-ggpo-udp-relay"
        if ggpo_bin.exists():
            print("Uploading sf4e-ggpo-udp-relay binary...")
            sftp.put(str(ggpo_bin), f"{remote_relay}/bin/sf4e-ggpo-udp-relay")
            sftp.chmod(f"{remote_relay}/bin/sf4e-ggpo-udp-relay", 0o755)
    finally:
        sftp.close()

    build_relay = ""
    if not (RELAY_DIR / "bin" / "sf4e-session-relay").exists():
        build_relay = f"""
if [[ -n "${{VCPKG_ROOT:-}}" && -x "{remote_relay}/build-linux.sh" ]]; then
  cp -f "{remote_relay}/build-linux.sh" "{remote_relay}/build-linux.sh"
  sed -i 's/\\r$//' "{remote_relay}/build-linux.sh" || true
  chmod +x "{remote_relay}/build-linux.sh"
  cd "{remote_relay}" && bash build-linux.sh || echo "WARNING: sf4e-session-relay build failed"
  cp -f "{remote_relay}/bin/sf4e-session-relay" "{remote_relay}/bin/sf4e-session-relay" 2>/dev/null || true
fi
"""

    domain = os.environ.get("SF4E_BROKER_DOMAIN", "")
    dashboard_domain = os.environ.get("SF4E_DASHBOARD_DOMAIN", "")
    acme_email = os.environ.get("SF4E_ACME_EMAIL", "")
    caddy_step = ""
    if domain:
        caddy_step = f"""
export SF4E_BROKER_DOMAIN={domain}
export SF4E_DASHBOARD_DOMAIN={dashboard_domain}
export SF4E_ACME_EMAIL={acme_email}
bash install-caddy.sh
"""

    commands = f"""set -e
systemctl stop sf4e-relay-manager 2>/dev/null || true
sleep 2
cd {remote_broker}
if [[ ! -f .env ]]; then cp -f .env.example .env; fi
sed -i 's/\\r$//' server.js install-vps.sh install-vps-relay.sh install-caddy.sh secure-ufw.sh .env .env.example 2>/dev/null || true
chmod +x install-vps.sh install-vps-relay.sh install-caddy.sh secure-ufw.sh
cp -f install-vps-relay.sh {remote_relay}/install-vps-relay.sh 2>/dev/null || true
cp -f relay-manager.js {remote_relay}/relay-manager.js 2>/dev/null || true
chmod +x {remote_relay}/install-vps-relay.sh 2>/dev/null || true
{build_relay}
bash install-vps.sh
{caddy_step}
"""
    print("Running install...")
    _, stdout, stderr = client.exec_command(commands, get_pty=True, timeout=900)
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
    print("Broker + relay deploy complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
