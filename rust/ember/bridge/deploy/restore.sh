#!/usr/bin/env bash
# Restores one backup set made by backup.sh.
# Run as: sudo bash /usr/local/lib/ember-bridge/restore.sh <stamp>
# where <stamp> is the part of the file names such as 20261002T042000Z.
#
# Stops the bridge and the backup timer, moves the current database with its
# -wal and -shm files, secrets and bridge.json into a recovery folder (never
# deleted), checks the backup copy, installs the three files, and starts the
# bridge again.
set -euo pipefail

STATE=/var/lib/ember-bridge
BACKUPS=/var/backups/ember-bridge

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }
stamp=${1:-}
[[ "$stamp" =~ ^[0-9]{8}T[0-9]{6}Z$ ]] || { echo "usage: restore.sh <stamp, for example 20261002T042000Z>" >&2; exit 2; }
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

systemctl stop ember-bridge-backup.timer ember-bridge
recovery="$STATE/recovery-$(date -u +%Y%m%dT%H%M%SZ)"
install -d -o ember-bridge -g ember-bridge -m 0700 "$recovery"
for name in bridge.sqlite3 bridge.sqlite3-wal bridge.sqlite3-shm bridge-secrets.json bridge.json; do
    if [ -e "$STATE/$name" ]; then mv "$STATE/$name" "$recovery/$name"; fi
done
install -o ember-bridge -g ember-bridge -m 0600 "$database" "$STATE/bridge.sqlite3"
install -o ember-bridge -g ember-bridge -m 0600 "$secrets" "$STATE/bridge-secrets.json"
install -o ember-bridge -g ember-bridge -m 0600 "$config" "$STATE/bridge.json"

systemctl start ember-bridge ember-bridge-backup.timer
echo "Restored $stamp; the previous files are in $recovery."
systemctl is-active ember-bridge
