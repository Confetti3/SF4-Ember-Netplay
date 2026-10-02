#!/usr/bin/env bash
# Restores one backup set made by backup.sh.
# Run as: sudo bash /usr/local/lib/ember-bridge/restore.sh <stamp>
# where <stamp> is the part of the file names such as 20261002T042000Z.
#
# Stops the bridge and backups, then, as the ember-bridge account, checks the
# backup copy, moves the current database with its -wal and -shm files,
# secrets and bridge.json into a new recovery folder (never deleted), and
# installs the three backup files. Root only runs systemctl: every file
# operation happens as the service account, inside folders it owns.
set -euo pipefail

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }
stamp=${1:-}
[[ "$stamp" =~ ^[0-9]{8}T[0-9]{6}Z$ ]] || { echo "usage: restore.sh <stamp, for example 20261002T042000Z>" >&2; exit 2; }

# The timer first, so no new backup starts; then any backup already running
# (stop waits for it to exit) and the bridge, so nothing holds the database.
systemctl stop ember-bridge-backup.timer
systemctl stop ember-bridge-backup.service ember-bridge

RESTORE=$(cat <<'SH'
set -euo pipefail
umask 077
stamp=$1
STATE=/var/lib/ember-bridge
BACKUPS=/var/backups/ember-bridge
database="$BACKUPS/bridge-$stamp.sqlite3"
secrets="$BACKUPS/bridge-secrets-$stamp.json"
config="$BACKUPS/bridge-config-$stamp.json"
for file in "$database" "$secrets" "$config"; do
    [ -f "$file" ] || { echo "Missing $file" >&2; exit 1; }
done
python3 - "$database" <<'PY'
import sqlite3, sys
check = sqlite3.connect(f"file:{sys.argv[1]}?mode=ro", uri=True).execute("PRAGMA integrity_check").fetchone()[0]
if check != "ok":
    sys.exit(f"the backup fails its integrity check: {check}")
PY
recovery=$(mktemp -d "$STATE/recovery-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")
for name in bridge.sqlite3 bridge.sqlite3-wal bridge.sqlite3-shm bridge-secrets.json bridge.json; do
    if [ -e "$STATE/$name" ] || [ -L "$STATE/$name" ]; then mv "$STATE/$name" "$recovery/$name"; fi
done
cp "$database" "$STATE/bridge.sqlite3"
cp "$secrets" "$STATE/bridge-secrets.json"
cp "$config" "$STATE/bridge.json"
chmod 0600 "$STATE/bridge.sqlite3" "$STATE/bridge-secrets.json" "$STATE/bridge.json"
echo "Restored $stamp; the previous files are in $recovery."
SH
)

status=0
cd /
sudo -u ember-bridge bash -c "$RESTORE" restore "$stamp" || status=$?
systemctl start ember-bridge ember-bridge-backup.timer
systemctl is-active ember-bridge
exit "$status"
