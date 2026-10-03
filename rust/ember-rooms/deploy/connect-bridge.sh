#!/usr/bin/env bash
# Connects the Ember bridge to the room supervisor on this machine.
# Run as: sudo bash ~/ember-rooms/connect-bridge.sh [--skip-bridge-check]
#
# Needs the supervisor running (setup.sh) and a bridge build that supports
# public rooms already installed (~/ember-bridge/setup.sh). It does two things
# and restarts the bridge once:
#   1. adds "rooms": { "supervisor_url": ... } to /var/lib/ember-bridge/bridge.json
#   2. stores the supervisor's secret as rooms_supervisor_secret in the bridge's
#      integrations.json, through ~/ember-bridge/set-integration-secret.sh rooms
#      (the secret is piped in from the secret file, never typed or printed)
# The daily-backup service runs first, so bridge.json and the database are
# copied before anything changes. Running it again when both are already in
# place changes nothing and does not restart the bridge. A later
# ~/ember-bridge/setup.sh keeps the "rooms" key: it only overwrites the keys
# that tenants.json names.
#
# If the bridge does not come back, bridge.json is put back as it was and the
# bridge is restarted. The stored secret is left: without the config key it is
# ignored.
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"
cd /
STATE=/var/lib/ember-bridge
BRIDGE_CONFIG=$STATE/bridge.json
INTEGRATIONS=$STATE/integrations.json
BRIDGE_BIN=/usr/local/lib/ember-bridge/ember-bridge
ROOMS_CONFIG=/etc/ember-rooms/config.json
BRIDGE_DEPLOY="$(dirname "$SRC")/ember-bridge"

CHECK_BRIDGE=1
case "${1:-}" in
    "") ;;
    --skip-bridge-check) CHECK_BRIDGE=0 ;;
    *) echo "usage: sudo bash connect-bridge.sh [--skip-bridge-check]" >&2; exit 2 ;;
esac

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo." >&2
    exit 1
fi
for file in "$BRIDGE_CONFIG" "$BRIDGE_BIN" "$ROOMS_CONFIG" "$BRIDGE_DEPLOY/set-integration-secret.sh"; do
    if [ ! -f "$file" ]; then
        echo "Missing $file" >&2
        exit 1
    fi
done
# The bridge refuses a config key it does not know, so an older build would
# not start. The key's name is a string inside a build that supports rooms;
# --skip-bridge-check passes over this test if it ever gives a wrong answer.
if [ "$CHECK_BRIDGE" -eq 1 ] && ! grep -aq supervisor_url "$BRIDGE_BIN"; then
    echo "The installed bridge does not support public rooms yet. Deploy the newer bridge first:" >&2
    echo "  sudo bash ~/ember-bridge/setup.sh" >&2
    exit 1
fi

# What the supervisor listens on and where its secret is, from its own config.
read -r BIND SECRET < <(python3 - "$ROOMS_CONFIG" <<'PY'
import json, sys
config = json.load(open(sys.argv[1]))
print(config.get("bind", "127.0.0.1:47830"), config.get("secret_file", ""))
PY
)
URL="http://$BIND"
if [ ! -s "$SECRET" ]; then
    echo "Missing the supervisor secret $SECRET (run ~/ember-rooms/setup.sh)." >&2
    exit 1
fi
secret_value() { tr -d '[:space:]' < "$SECRET"; }
auth_curl() {
    printf 'Authorization: Bearer %s\n' "$(secret_value)" | curl -fsS --max-time 5 -H @- "$@"
}
if [ "$(curl -fsS --max-time 5 "$URL/health" 2>/dev/null || true)" != "ok" ]; then
    echo "The supervisor does not answer at $URL/health; start it first (sudo systemctl status ember-rooms)." >&2
    exit 1
fi
if ! auth_curl "$URL/rooms" >/dev/null 2>&1; then
    echo "The supervisor refused its own secret from $SECRET; check ember-rooms in the journal." >&2
    exit 1
fi

# Already connected? Compare as the service account, which owns both files.
CHECK=$(cat <<'PY'
import json, sys
secret = sys.stdin.read().strip()
url = sys.argv[3]
with open(sys.argv[1]) as file:
    config = json.load(file)
try:
    with open(sys.argv[2]) as file:
        stored = json.load(file).get("rooms_supervisor_secret")
except FileNotFoundError:
    stored = None
sys.exit(0 if config.get("rooms", {}).get("supervisor_url") == url and stored == secret else 1)
PY
)
if secret_value | sudo -u ember-bridge python3 -c "$CHECK" "$BRIDGE_CONFIG" "$INTEGRATIONS" "$URL"; then
    echo "bridge: already connected to $URL"
    systemctl is-active ember-bridge
    exit 0
fi

# Copy the database, secrets and bridge.json first; the backup service is a
# oneshot, so this returns when the copy is done.
systemctl start ember-bridge-backup.service
echo "backup: done"

# Keep bridge.json so a bridge that fails to start gets it back.
backup=$(mktemp -d)
cp -a "$BRIDGE_CONFIG" "$backup/bridge.json"
rollback() {
    cat "$backup/bridge.json" > "$BRIDGE_CONFIG"
    systemctl restart ember-bridge || true
    echo "Put bridge.json back as it was and restarted the bridge. See: sudo journalctl -u ember-bridge -n 30" >&2
}

EDIT=$(cat <<'PY'
import json, os, sys, tempfile
path, url = sys.argv[1], sys.argv[2]
with open(path) as file:
    config = json.load(file)
if not config.get("integration_secrets"):
    sys.exit("bridge.json has no integration_secrets; the bridge needs it for the supervisor secret")
config["rooms"] = {"supervisor_url": url}
handle, temporary = tempfile.mkstemp(dir=os.path.dirname(path), prefix=".bridge.json.")
with os.fdopen(handle, "w") as file:
    json.dump(config, file, indent=2)
    file.write("\n")
os.replace(temporary, path)
PY
)
sudo -u ember-bridge python3 -c "$EDIT" "$BRIDGE_CONFIG" "$URL"
echo "config: rooms.supervisor_url = $URL"

# set-integration-secret.sh reads the secret from stdin when it is not a
# terminal (the prompt is skipped), stores it, and restarts the bridge. The
# added newline is what its read needs.
if ! { secret_value; echo; } | bash "$BRIDGE_DEPLOY/set-integration-secret.sh" rooms; then
    rollback
    exit 1
fi

# The bridge must answer on its own origin.
LISTEN=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("listen", "127.0.0.1:47820"))' "$BRIDGE_CONFIG")
ok=0
for attempt in 1 2 3 4 5 6 7 8 9 10; do
    if curl -fsS --max-time 3 "http://$LISTEN/.well-known/ember-bridge.json" >/dev/null 2>&1; then
        ok=1
        break
    fi
    sleep 1
done
if [ "$ok" -ne 1 ]; then
    echo "The bridge does not answer at http://$LISTEN after the change." >&2
    rollback
    exit 1
fi
rm -rf "$backup"

echo
echo "=== bridge ==="
systemctl is-active ember-bridge
echo "bridge: answers on http://$LISTEN and uses the supervisor at $URL"
echo "=== done ==="
