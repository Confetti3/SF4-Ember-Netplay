#!/usr/bin/env bash
# Installs the idle room closer (idle-close.py, every 30 seconds) from
# ~/ember-rooms and starts its timer. Run again after changing any of the
# three files. Does not touch the supervisor or any room.
# Run as: sudo bash ~/ember-rooms/install-idle-close.sh
# Remove: sudo systemctl disable --now ember-rooms-idle.timer
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo." >&2
    exit 1
fi
SRC=$(cd "$(dirname "$0")" && pwd)
for f in idle-close.py ember-rooms-idle.service ember-rooms-idle.timer; do
    [ -f "$SRC/$f" ] || { echo "Missing $SRC/$f" >&2; exit 1; }
done
python3 -c 'import ast, sys; ast.parse(open(sys.argv[1]).read())' "$SRC/idle-close.py"

install -o root -g root -m 0755 "$SRC/idle-close.py" /usr/local/lib/ember-rooms/idle-close.py
install -o root -g root -m 0644 "$SRC/ember-rooms-idle.service" /etc/systemd/system/ember-rooms-idle.service
install -o root -g root -m 0644 "$SRC/ember-rooms-idle.timer" /etc/systemd/system/ember-rooms-idle.timer
systemctl daemon-reload

# One run first, so a broken install shows here and not in the journal later.
if ! systemctl start ember-rooms-idle.service; then
    echo "The first run failed:" >&2
    journalctl -u ember-rooms-idle -n 20 --no-pager >&2
    exit 1
fi
systemctl enable --now ember-rooms-idle.timer
echo "Installed. Quiet rooms now:"
python3 /usr/local/lib/ember-rooms/idle-close.py --dry-run
echo "Closures are logged: sudo journalctl -u ember-rooms-idle"
