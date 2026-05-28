#!/usr/bin/env python3
"""Deploy SF4e relay dashboard to Linux VPS over SSH/SFTP."""

import json
import os
import secrets
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

import paramiko

_SCRIPT_DIR = Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))
from vps_ssh import connect_ssh

ROOT = Path(__file__).resolve().parents[1]
DASHBOARD_DIR = ROOT / "services" / "vps-relay" / "dashboard"
RELAY_DIR = ROOT / "services" / "vps-relay"
DASHBOARD_FILES = (
    "server.js",
    "package.json",
    ".env.example",
    "install.sh",
)


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


def write_credentials_file(creds: dict[str, str], domain: str) -> Path:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
    out = Path.cwd() / f"dashboard-credentials-{stamp}.txt"
    out.write_text(
        "\n".join(
            [
                "SF4e relay dashboard credentials",
                f"Generated (UTC): {datetime.now(timezone.utc).isoformat()}",
                f"URL: https://{domain}/login",
                f"Username: {creds['username']}",
                f"Password: {creds['password']}",
                "",
                "Store securely and delete this file when done.",
            ]
        ),
        encoding="utf-8",
    )
    try:
        os.chmod(out, 0o600)
    except OSError:
        pass
    return out


def generate_credentials() -> dict[str, str]:
    password = f"Sf4eRelay-{secrets.token_hex(4)}"
    node_script = """
const bcrypt = require('bcryptjs');
const crypto = require('crypto');
const password = process.argv[1];
Promise.all([
  bcrypt.hash(password, 12),
  Promise.resolve(crypto.randomBytes(32).toString('hex')),
]).then(([hash, secret]) => {
  process.stdout.write(JSON.stringify({ password, hash, secret }));
});
"""
    proc = subprocess.run(
        ["node", "-e", node_script, password],
        cwd=str(DASHBOARD_DIR),
        capture_output=True,
        text=True,
        check=True,
    )
    data = json.loads(proc.stdout.strip())
    return {
        "username": "admin",
        "password": data["password"],
        "password_hash": data["hash"],
        "session_secret": data["secret"],
    }


def main() -> int:
    host = os.environ.get("SF4E_VPS_HOST", "74.208.200.95")
    user = os.environ.get("SF4E_VPS_USER", "root")
    password = os.environ.get("SF4E_VPS_PASSWORD", "")
    if not password:
        print("Set SF4E_VPS_PASSWORD", file=sys.stderr)
        return 1

    remote_relay = "/opt/sf4e-relay"
    remote_dashboard = f"{remote_relay}/dashboard"
    creds = generate_credentials()

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
        mkdir_p(sftp, remote_dashboard)
        for name in DASHBOARD_FILES:
            local_path = DASHBOARD_DIR / name
            if not local_path.exists():
                print(f"Missing {local_path}", file=sys.stderr)
                return 1
            remote_path = f"{remote_dashboard}/{name}"
            print(f"Uploading dashboard/{name}...")
            sftp.put(str(local_path), remote_path)

        relay_manager = RELAY_DIR / "relay-manager.js"
        if relay_manager.exists():
            print("Uploading relay-manager.js...")
            sftp.put(str(relay_manager), f"{remote_relay}/relay-manager.js")
    finally:
        sftp.close()

    env_content = f"""DASHBOARD_PORT=8789
DASHBOARD_BIND=127.0.0.1
DASHBOARD_TRUST_PROXY=1
ADMIN_USERNAME={creds['username']}
ADMIN_PASSWORD_HASH={creds['password_hash']}
SESSION_SECRET={creds['session_secret']}
SESSION_TTL_SEC=28800
RELAY_MANAGER_URL=http://127.0.0.1:8788
BROKER_URL=http://127.0.0.1:8787
COOKIE_SECURE=1
"""

    commands = f"""set -e
sed -i 's/\\r$//' {remote_relay}/relay-manager.js {remote_dashboard}/install.sh {remote_dashboard}/server.js 2>/dev/null || true
chmod +x {remote_dashboard}/install.sh
cat > {remote_dashboard}/.env <<'ENVEOF'
{env_content}ENVEOF
systemctl restart sf4e-relay-manager
bash {remote_dashboard}/install.sh
curl -s http://127.0.0.1:8789/login | head -c 80 || true
echo
curl -s http://127.0.0.1:8788/v1/health
echo
"""

    print("Installing dashboard on VPS...")
    _, stdout, stderr = client.exec_command(commands, get_pty=True, timeout=600)
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

    domain = os.environ.get("SF4E_BROKER_DOMAIN", "74-208-200-95.nip.io")
    cred_file = write_credentials_file(creds, domain)
    print("\n=== Dashboard ready ===")
    print(f"URL:      https://{domain}/login")
    print(f"Username: {creds['username']}")
    print(f"Password: (see file) {cred_file.resolve()}")
    print("\nOpen the credentials file for the password; it is not printed to stdout.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
