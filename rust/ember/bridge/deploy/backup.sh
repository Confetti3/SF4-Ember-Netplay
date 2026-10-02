#!/usr/bin/env bash
# Online copy of the bridge database (safe while the bridge runs, WAL
# included) plus its secrets file, kept for 14 days. Without the secrets file
# the database's credential hashes and sealed webhook secrets are useless, so
# both are needed to restore.
set -euo pipefail

STATE=/var/lib/ember-bridge
DEST=/var/backups/ember-bridge
stamp=$(date -u +%Y%m%dT%H%M%SZ)

python3 - "$STATE/bridge.sqlite3" "$DEST/bridge-$stamp.sqlite3" <<'PY'
import sqlite3, sys
source = sqlite3.connect(f"file:{sys.argv[1]}?mode=ro", uri=True)
target = sqlite3.connect(sys.argv[2])
source.backup(target)
check = target.execute("PRAGMA integrity_check").fetchone()[0]
target.close()
source.close()
if check != "ok":
    sys.exit(f"backup failed its integrity check: {check}")
PY
cp "$STATE/bridge-secrets.json" "$DEST/bridge-secrets-$stamp.json"

find "$DEST" -maxdepth 1 -type f \( -name 'bridge-*.sqlite3' -o -name 'bridge-secrets-*.json' \) -mtime +14 -delete
echo "backed up to $DEST/bridge-$stamp.sqlite3"
