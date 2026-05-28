#!/usr/bin/env python3
"""Fix Caddy on VPS and finish TLS + dashboard update."""
import os
import sys
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))
from vps_ssh import connect_ssh

ROOT = Path(__file__).resolve().parents[1]
DOMAIN = os.environ.get("SF4E_BROKER_DOMAIN", "74-208-200-95.nip.io")


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
    client = connect_ssh(
        host, username="root", password=password, timeout=60, allow_agent=False, look_for_keys=False
    )

    sftp = client.open_sftp()
    try:
        local = ROOT / "services" / "room-broker" / "install-caddy.sh"
        sftp.put(str(local), "/root/room-broker/install-caddy.sh")
        for local, remote in [
            (ROOT / "services" / "vps-relay" / "dashboard" / "server.js", "/opt/sf4e-relay/dashboard/server.js"),
            (ROOT / "services" / "vps-relay" / "relay-manager.js", "/opt/sf4e-relay/relay-manager.js"),
            (ROOT / "services" / "room-broker" / "server.js", "/root/room-broker/server.js"),
        ]:
            print(f"Uploading {local.name}...")
            sftp.put(str(local), remote)
    finally:
        sftp.close()

    commands = f"""set -e
sed -i 's/\\r$//' /root/room-broker/install-caddy.sh
chmod +x /root/room-broker/install-caddy.sh
export SF4E_BROKER_DOMAIN={DOMAIN}
bash /root/room-broker/install-caddy.sh
grep -q '^DASHBOARD_BIND=' /opt/sf4e-relay/dashboard/.env && sed -i 's|^DASHBOARD_BIND=.*|DASHBOARD_BIND=127.0.0.1|' /opt/sf4e-relay/dashboard/.env || echo 'DASHBOARD_BIND=127.0.0.1' >> /opt/sf4e-relay/dashboard/.env
grep -q '^DASHBOARD_TRUST_PROXY=' /opt/sf4e-relay/dashboard/.env && sed -i 's|^DASHBOARD_TRUST_PROXY=.*|DASHBOARD_TRUST_PROXY=1|' /opt/sf4e-relay/dashboard/.env || echo 'DASHBOARD_TRUST_PROXY=1' >> /opt/sf4e-relay/dashboard/.env
grep -q '^COOKIE_SECURE=' /opt/sf4e-relay/dashboard/.env && sed -i 's|^COOKIE_SECURE=.*|COOKIE_SECURE=1|' /opt/sf4e-relay/dashboard/.env || echo 'COOKIE_SECURE=1' >> /opt/sf4e-relay/dashboard/.env
systemctl restart sf4e-relay-manager sf4e-broker sf4e-relay-dashboard
sleep 3
systemctl is-active caddy sf4e-broker sf4e-relay-manager sf4e-relay-dashboard
curl -s http://127.0.0.1:8787/v1/health; echo
curl -sk https://127.0.0.1/v1/health -H 'Host: {DOMAIN}'; echo
curl -skI https://127.0.0.1/login -H 'Host: {DOMAIN}' | head -5
ss -tlnp | grep -E '8787|8788|8789|443' || true
ls -la /opt/sf4e-relay/bin/
"""
    print("Finishing TLS setup...")
    _, stdout, stderr = client.exec_command(commands, get_pty=True, timeout=600)
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
