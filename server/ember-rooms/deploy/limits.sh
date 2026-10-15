#!/usr/bin/env bash
# Prints what the running room supervisor uses now (GET /limits): max_rooms,
# the port range, the builds, how many rooms run and how many reloads it took.
# Run as: sudo bash ~/ember-rooms/limits.sh
set -euo pipefail

CONFIG=/etc/ember-rooms/config.json
SECRET=/etc/ember-rooms/supervisor.secret
if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo." >&2
    exit 1
fi
BIND=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("bind", "127.0.0.1:47830"))' "$CONFIG")
printf 'Authorization: Bearer %s\n' "$(tr -d '[:space:]' < "$SECRET")" |
    curl -fsS --max-time 5 -H @- "http://$BIND/limits" |
    python3 -c '
import json, sys
limits = json.load(sys.stdin)
print("max_rooms:   %d" % limits["max_rooms"])
print("rooms now:   %d" % limits["rooms"])
print("port_range:  %d-%d" % tuple(limits["port_range"]))
print("reloads:     %d" % limits["reloads"])
print("builds:      %s" % ", ".join(build[:8] for build in limits["builds"]))
'
