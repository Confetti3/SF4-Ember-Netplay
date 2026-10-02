#!/usr/bin/env bash
# Installs the Ember tournament bridge at https://bridge.embernetplay.link.
# Run as: sudo bash ~/ember-bridge/setup.sh
#
# Expects, next to this script: bin/ember-bridge (built as katie with
# `cargo build --release --locked -p ember-bridge` in ~/ember-bridge/src/rust/ember),
# the unit files, backup.sh, tenants.json and nginx/. Running it again installs a
# newer binary and applies tenants.json; the bridge ID, secrets and database stay.
#
# What it changes:
#   user ember-bridge                                  system account the bridge runs as
#   /usr/local/lib/ember-bridge/                       binary and backup script (root-owned)
#   /var/lib/ember-bridge/                             bridge.json, bridge-secrets.json, database (0700)
#   /var/backups/ember-bridge/                         daily backups, kept 14 days (0700)
#   /etc/systemd/system/ember-bridge*.{service,timer}  the service and the backup timer
#   /etc/nginx/conf.d/ember-bridge.conf                request rate zones
#   /etc/nginx/sites-available/bridge.embernetplay.link (and its sites-enabled link)
# Nothing is opened in ufw: the bridge listens on loopback, nginx already serves 80 and 443.
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"
HOST=bridge.embernetplay.link
LIB=/usr/local/lib/ember-bridge
STATE=/var/lib/ember-bridge
BACKUPS=/var/backups/ember-bridge
SITE=/etc/nginx/sites-available/$HOST
ENABLED=/etc/nginx/sites-enabled/$HOST
ZONE=/etc/nginx/conf.d/ember-bridge.conf

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo." >&2
    exit 1
fi
for file in bin/ember-bridge ember-bridge.service ember-bridge-backup.service \
    ember-bridge-backup.timer backup.sh tenants.json \
    nginx/ember-bridge-zone.conf nginx/$HOST.conf; do
    if [ ! -f "$SRC/$file" ]; then
        echo "Missing $SRC/$file" >&2
        exit 1
    fi
done
for file in /etc/ssl/certs/embernetplay.link.fullchain.pem /etc/ssl/private/embernetplay.link.key; do
    if [ ! -f "$file" ]; then
        echo "Missing $file (the embernetplay.link wildcard certificate)." >&2
        exit 1
    fi
done

if ! id ember-bridge >/dev/null 2>&1; then
    useradd --system --home-dir "$STATE" --no-create-home --shell /usr/sbin/nologin ember-bridge
fi

install -d -o root -g root -m 0755 "$LIB"
install -o root -g root -m 0755 "$SRC/bin/ember-bridge" "$LIB/ember-bridge.new"
mv -f "$LIB/ember-bridge.new" "$LIB/ember-bridge"
install -o root -g root -m 0755 "$SRC/backup.sh" "$LIB/backup.sh"
install -d -o ember-bridge -g ember-bridge -m 0700 "$STATE" "$BACKUPS"

# First install: a fresh bridge ID and secrets, then the deployed settings.
if [ ! -f "$STATE/bridge.json" ]; then
    sudo -u ember-bridge "$LIB/ember-bridge" init "$STATE"
fi
# Apply tenants.json and the deployed settings, keeping bridge_id and paths.
# Development switches are always off here. Runs as root because katie's
# home, where tenants.json is, is not readable by the service account.
python3 - "$STATE/bridge.json" "$SRC/tenants.json" <<'PY'
import json, os, sys
path, deployed = sys.argv[1], json.load(open(sys.argv[2]))
config = json.load(open(path))
config.update(deployed)
config["allow_loopback_http"] = False
config["allow_private_webhooks"] = False
config["mock_browser"] = False
temporary = path + ".new"
with open(temporary, "w") as file:
    json.dump(config, file, indent=2)
    file.write("\n")
os.chown(temporary, os.stat(path).st_uid, os.stat(path).st_gid)
os.chmod(temporary, 0o600)
os.replace(temporary, path)
PY

install -o root -g root -m 0644 "$SRC/ember-bridge.service" /etc/systemd/system/ember-bridge.service
install -o root -g root -m 0644 "$SRC/ember-bridge-backup.service" /etc/systemd/system/ember-bridge-backup.service
install -o root -g root -m 0644 "$SRC/ember-bridge-backup.timer" /etc/systemd/system/ember-bridge-backup.timer

# nginx: keep copies so a failed check puts everything back as it was.
backup=$(mktemp -d)
[ -f "$ZONE" ] && cp -a "$ZONE" "$backup/zone"
[ -f "$SITE" ] && cp -a "$SITE" "$backup/site"
restore() {
    if [ -f "$backup/zone" ]; then cp -a "$backup/zone" "$ZONE"; else rm -f "$ZONE"; fi
    if [ -f "$backup/site" ]; then cp -a "$backup/site" "$SITE"; else rm -f "$SITE" "$ENABLED"; fi
}
install -o root -g root -m 0644 "$SRC/nginx/ember-bridge-zone.conf" "$ZONE"
install -o root -g root -m 0644 "$SRC/nginx/$HOST.conf" "$SITE"
ln -sfn "$SITE" "$ENABLED"
if ! nginx -t; then
    echo "nginx rejected the new configuration; restored the previous files." >&2
    restore
    nginx -t || true
    exit 1
fi

systemctl daemon-reload
systemctl enable ember-bridge ember-bridge-backup.timer
systemctl restart ember-bridge
systemctl start ember-bridge-backup.timer
systemctl reload nginx || systemctl restart nginx
rm -rf "$backup"

echo
echo "=== ember-bridge ==="
systemctl is-active ember-bridge
for attempt in 1 2 3 4 5; do
    if curl -fsS http://127.0.0.1:47820/.well-known/ember-bridge.json >/dev/null 2>&1; then break; fi
    sleep 1
done
echo "service: $(curl -fsS http://127.0.0.1:47820/.well-known/ember-bridge.json || echo 'no answer')"
echo "public:  $(curl -fsS --resolve $HOST:443:127.0.0.1 https://$HOST/.well-known/ember-bridge.json || echo 'no answer')"
echo "=== done ==="
