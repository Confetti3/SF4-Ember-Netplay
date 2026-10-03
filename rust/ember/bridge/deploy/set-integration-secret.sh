#!/usr/bin/env bash
# Stores a secret the bridge needs to call another service, then restarts it.
# Run as one of:
#   sudo bash ~/ember-bridge/set-integration-secret.sh discord
#       Discord's client secret (Developer Portal, the Ember application, OAuth2).
#   sudo bash ~/ember-bridge/set-integration-secret.sh rooms
#       The shared secret of the room supervisor's loopback API (public rooms).
#   sudo bash ~/ember-bridge/set-integration-secret.sh blumint <connection-id>
#       The API key BluMint issued for that connection, such as blumint-partner-staging.
#
# The secret is pasted at a hidden prompt, so it never appears on the screen,
# in the shell history or on a command line. It is written to
# /var/lib/ember-bridge/integrations.json (0600, owned by ember-bridge), which
# backups leave out and a restore leaves in place. Pasting nothing removes it.
set -euo pipefail

STATE=/var/lib/ember-bridge
FILE=$STATE/integrations.json

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }
case "${1:-}" in
    discord) what="Discord client secret"; key=discord ;;
    rooms) what="room supervisor secret"; key=rooms ;;
    blumint)
        [[ "${2:-}" =~ ^[a-z][a-z0-9-]{0,63}$ ]] || { echo "usage: set-integration-secret.sh blumint <connection-id>" >&2; exit 2; }
        what="BluMint API key for $2"; key="api_keys/$2" ;;
    *) echo "usage: set-integration-secret.sh discord | rooms | blumint <connection-id>" >&2; exit 2 ;;
esac
[ -d "$STATE" ] || { echo "Install the bridge first (setup.sh)." >&2; exit 1; }

read -r -s -p "Paste the $what, then press Enter: " secret
echo

# Root hands the secret over on stdin; the service account writes its own file.
WRITE=$(cat <<'PY'
import json, os, sys, tempfile
path, key = sys.argv[1], sys.argv[2]
secret = sys.stdin.read().strip()
data = {"format": "ember-bridge-integrations", "version": 1}
if os.path.exists(path):
    with open(path) as file:
        data = json.load(file)
if key == "discord":
    data.pop("discord_client_secret", None)
    if secret:
        data["discord_client_secret"] = secret
elif key == "rooms":
    data.pop("rooms_supervisor_secret", None)
    if secret:
        data["rooms_supervisor_secret"] = secret
else:
    keys = data.setdefault("api_keys", {})
    keys.pop(key.split("/", 1)[1], None)
    if secret:
        keys[key.split("/", 1)[1]] = secret
os.umask(0o077)
handle, temporary = tempfile.mkstemp(dir=os.path.dirname(path), prefix=".integrations.json.")
with os.fdopen(handle, "w") as file:
    json.dump(data, file, indent=2)
    file.write("\n")
os.replace(temporary, path)
PY
)
printf '%s' "$secret" | sudo -u ember-bridge python3 -c "$WRITE" "$FILE" "$key"
unset secret

systemctl restart ember-bridge
sleep 2
if systemctl is-active --quiet ember-bridge; then
    echo "Stored. The bridge restarted."
else
    echo "Stored, but the bridge did not start; see: sudo journalctl -u ember-bridge -n 30" >&2
    exit 1
fi
