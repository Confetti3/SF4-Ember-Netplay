#!/usr/bin/env bash
# Restores one backup set made by backup.sh.
# Run as: sudo bash /usr/local/lib/ember-bridge/restore.sh <stamp>
# where <stamp> is the part of the file names such as 20261002T042000Z.
#
# The database, secrets and bridge.json are restored as one bundle. As the
# ember-bridge account, the script copies and checks all three into a staging
# folder, moves the current files (with the database's -wal and -shm) into a
# new recovery folder, and moves the staged files into place. If any step
# fails it puts the current files back. Root only runs systemctl, and the
# bridge starts again only from a consistent state: the restored bundle, or
# the files it had before.
set -euo pipefail

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }
stamp=${1:-}
[[ "$stamp" =~ ^[0-9]{8}T[0-9]{6}Z$ ]] || { echo "usage: restore.sh <stamp, for example 20261002T042000Z>" >&2; exit 2; }

# The timer first, so no new backup starts; then any backup already running
# (stop waits for it to exit) and the bridge, so nothing holds the database.
systemctl stop ember-bridge-backup.timer
systemctl stop ember-bridge-backup.service ember-bridge

RESTORE=$(cat <<'SH'
set -uo pipefail
umask 077
stamp=$1
STATE=/var/lib/ember-bridge
BACKUPS=/var/backups/ember-bridge
FILES="bridge.sqlite3 bridge-secrets.json bridge.json"
SIDECARS="bridge.sqlite3-wal bridge.sqlite3-shm"

# 1. Stage and check the whole bundle before touching the live files.
staging=$(mktemp -d "$STATE/.restore-XXXXXX") || exit 3
cp "$BACKUPS/bridge-$stamp.sqlite3" "$staging/bridge.sqlite3" &&
    cp "$BACKUPS/bridge-secrets-$stamp.json" "$staging/bridge-secrets.json" &&
    cp "$BACKUPS/bridge-config-$stamp.json" "$staging/bridge.json" || {
    echo "The backup set $stamp is incomplete." >&2; rm -rf "$staging"; exit 3; }
chmod 0600 "$staging"/*
python3 - "$staging" <<'PY' || { rm -rf "$staging"; exit 3; }
import json, sqlite3, sys
staging = sys.argv[1]
check = sqlite3.connect(f"file:{staging}/bridge.sqlite3?mode=ro", uri=True).execute("PRAGMA integrity_check").fetchone()[0]
if check != "ok":
    sys.exit(f"the backup database fails its integrity check: {check}")
for name in ("bridge-secrets.json", "bridge.json"):
    with open(f"{staging}/{name}") as file:
        json.load(file)
PY

# 2. Move the live files aside; undo on failure.
recovery=$(mktemp -d "$STATE/recovery-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX") || { rm -rf "$staging"; exit 3; }
moved=""
for name in $FILES $SIDECARS; do
    if [ -e "$STATE/$name" ] || [ -L "$STATE/$name" ]; then
        if mv "$STATE/$name" "$recovery/$name"; then
            moved="$moved $name"
        else
            for back in $moved; do mv "$recovery/$back" "$STATE/$back"; done
            echo "Could not move $name aside; the bridge files are unchanged." >&2
            rm -rf "$staging"; exit 1
        fi
    fi
done

# 3. Move the staged bundle in; on failure put the previous files back.
placed=""
for name in $FILES; do
    if mv "$staging/$name" "$STATE/$name"; then
        placed="$placed $name"
    else
        for back in $placed; do rm -f "$STATE/$back"; done
        failed=0
        for back in $moved; do mv "$recovery/$back" "$STATE/$back" || failed=1; done
        rm -rf "$staging"
        if [ "$failed" -ne 0 ]; then
            echo "Restore failed and the previous files could not all be put back; they are in $recovery." >&2
            exit 4
        fi
        echo "Restore failed; the previous files are back in place." >&2
        exit 1
    fi
done
rmdir "$staging" 2>/dev/null || rm -rf "$staging"
echo "Restored $stamp; the previous files are in $recovery."
SH
)

cd /
status=0
sudo -u ember-bridge bash -c "$RESTORE" restore "$stamp" || status=$?
case "$status" in
    0 | 1 | 3)
        # Restored, or left as it was: a consistent state to start from.
        systemctl start ember-bridge ember-bridge-backup.timer
        systemctl is-active ember-bridge || true
        ;;
    *)
        echo "Not starting the bridge. Put the files from the recovery folder back by hand, then: systemctl start ember-bridge ember-bridge-backup.timer" >&2
        ;;
esac
exit "$status"
