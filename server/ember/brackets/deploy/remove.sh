#!/usr/bin/env bash
# Removes one installed connector, picked by the folder this script is in
# (instance.sh):
#   sudo bash ~/ember-brackets-staging/remove.sh
#   sudo bash ~/ember-brackets-staging/remove.sh --purge
# It stops and disables the service and takes away its unit, its nginx site
# and its program folder. The state folder (brackets.json, secrets and the
# database of what it played) and the account stay, so installing it again
# carries on; --purge deletes those too. The shared request zone
# /etc/nginx/conf.d/ember-brackets.conf stays, since another connector may use
# it. The bridge's side of the connection (its entry in the tenants file, the
# provider credential, the result secret) is not touched.
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"
cd /
[ -f "$SRC/instance.sh" ] || { echo "Missing $SRC/instance.sh" >&2; exit 1; }
. "$SRC/instance.sh"
brackets_instance "$SRC" || exit 2
[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }
PURGE=0
case "${1:-}" in
    "") ;;
    --purge) PURGE=1 ;;
    *) echo "usage: remove.sh [--purge]" >&2; exit 2 ;;
esac

UNIT=$ROOT/etc/systemd/system/$NAME.service
# The site this connector installed, found by the marker setup.sh writes.
SITES=$ROOT/etc/nginx/sites-available
site=""
for candidate in "$SITES"/*; do
    if [ -f "$candidate" ] && head -n 1 "$candidate" | grep -qxF "# ember-brackets: $NAME"; then
        site=$candidate
    fi
done

systemctl disable --now "$NAME" 2>/dev/null || true
rm -f "$UNIT"
if [ -n "$site" ]; then
    rm -f "$ROOT/etc/nginx/sites-enabled/$(basename "$site")" "$site"
fi
rm -rf "$LIB"
systemctl daemon-reload
if nginx -t; then
    systemctl reload nginx || systemctl restart nginx
else
    echo "nginx does not accept its configuration after the removal; fix it before reloading nginx." >&2
    exit 1
fi
if [ "$PURGE" -eq 1 ]; then
    rm -rf "$STATE"
    userdel "$NAME" 2>/dev/null || true
    echo "$NAME removed, with its state and account."
else
    echo "$NAME removed. Its state in $STATE and its account stay; remove.sh --purge deletes them."
fi
