#!/usr/bin/env bash
# Installs the Ember public room supervisor (loopback only, 127.0.0.1:47830).
# Run as: sudo bash ~/ember-rooms/setup.sh [--prune]
#
# Expects, next to this script (normally ~/ember-rooms of the user who ran
# sudo, put there by the operator): bin/ember-rooms (built as katie with
# `cargo build --release --locked -p ember-rooms`), ember-rooms.service,
# config.example.json, and optionally builds/<build_id>/ folders that each
# hold the Linux binaries sf4e-room-host and sf4-net of one game release.
#
# Running it again installs a newer supervisor binary, adds or updates the
# builds that are staged, and restarts the service. The secret and an existing
# config.json are never replaced; only config.json's "builds" entries change.
# Without --prune a build that is installed but no longer staged is kept;
# with --prune its config entry and installed files are removed. Rooms that
# already run keep their binaries until they end.
#
# What it changes:
#   user ember-rooms                          system account the supervisor runs as
#   /usr/local/lib/ember-rooms/               supervisor binary and builds/<build_id>/ (root-owned)
#   /etc/ember-rooms/config.json              0640 root:ember-rooms, created once from config.example.json
#   /etc/ember-rooms/supervisor.secret        0640 root:ember-rooms, created once with openssl
#   /var/lib/ember-rooms/                     state for the room hosts (0700)
#   /etc/systemd/system/ember-rooms.service   the service
#   ufw                                       UDP port_range, only if ufw is active
# Nothing is added to nginx: the supervisor is reached only by the bridge.
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"
# Commands run as ember-rooms would not be able to read katie's home.
cd /
LIB=/usr/local/lib/ember-rooms
BUILDS=$LIB/builds
ETC=/etc/ember-rooms
CONFIG=$ETC/config.json
SECRET=$ETC/supervisor.secret
STATE=/var/lib/ember-rooms
UNIT=/etc/systemd/system/ember-rooms.service
BUILD_ID_PATTERN='^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$'

PRUNE=0
case "${1:-}" in
    "") ;;
    --prune) PRUNE=1 ;;
    *) echo "usage: sudo bash setup.sh [--prune]" >&2; exit 2 ;;
esac

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo." >&2
    exit 1
fi
for file in bin/ember-rooms ember-rooms.service config.example.json; do
    if [ ! -f "$SRC/$file" ]; then
        echo "Missing $SRC/$file" >&2
        exit 1
    fi
done
for tool in python3 openssl curl; do
    command -v "$tool" >/dev/null 2>&1 || { echo "Missing $tool on this machine." >&2; exit 1; }
done

# Check every staged build before anything is changed, so a half-copied
# folder or a Windows binary stops the run instead of reaching the config.
is_elf() { [ "$(head -c 4 "$1" | tail -c 3)" = ELF ]; }
build_ids=()
if [ -d "$SRC/builds" ]; then
    for dir in "$SRC"/builds/*/; do
        [ -d "$dir" ] || continue
        id=$(basename "$dir")
        if [[ ! "$id" =~ $BUILD_ID_PATTERN ]]; then
            echo "builds/$id: the folder name must be 1 to 128 letters, digits, dots, dashes or underscores." >&2
            exit 1
        fi
        for file in sf4e-room-host sf4-net; do
            if [ ! -f "$dir$file" ]; then
                echo "builds/$id: missing $file." >&2
                exit 1
            fi
            if ! is_elf "$dir$file"; then
                echo "builds/$id/$file is not a Linux (ELF) binary." >&2
                exit 1
            fi
        done
        build_ids+=("$id")
    done
fi

if ! id ember-rooms >/dev/null 2>&1; then
    useradd --system --home-dir "$STATE" --no-create-home --shell /usr/sbin/nologin ember-rooms
    echo "user: created ember-rooms"
else
    echo "user: ember-rooms exists"
fi
getent group ember-rooms >/dev/null || { echo "Group ember-rooms is missing." >&2; exit 1; }

install -d -o root -g root -m 0755 "$LIB" "$BUILDS"
install -d -o root -g ember-rooms -m 0750 "$ETC"
install -d -o ember-rooms -g ember-rooms -m 0700 "$STATE"
# New files are renamed into place: a running binary cannot be overwritten
# (text file busy), and a renamed one leaves running rooms on the old copy.
install -o root -g root -m 0755 "$SRC/bin/ember-rooms" "$LIB/ember-rooms.new"
mv -f "$LIB/ember-rooms.new" "$LIB/ember-rooms"
echo "binary: $LIB/ember-rooms installed"

# The secret is created once. A kept secret is only checked and re-owned.
if [ ! -s "$SECRET" ]; then
    tmp=$(mktemp "$ETC/.secret.XXXXXX")
    openssl rand -hex 32 > "$tmp"
    chown root:ember-rooms "$tmp"
    chmod 0640 "$tmp"
    mv -f "$tmp" "$SECRET"
    echo "secret: created $SECRET"
else
    chown root:ember-rooms "$SECRET"
    chmod 0640 "$SECRET"
    echo "secret: kept $SECRET"
fi
if [ "$(tr -d '[:space:]' < "$SECRET" | wc -c)" -lt 32 ]; then
    echo "$SECRET holds fewer than 32 characters; the supervisor would refuse it." >&2
    exit 1
fi

# Keep a copy of config.json (or note that it was absent) so a failed start
# puts it back as it was.
backup=$(mktemp -d)
had_config=0
if [ -f "$CONFIG" ]; then
    cp -a "$CONFIG" "$backup/config.json"
    had_config=1
    echo "config: kept $CONFIG"
else
    install -o root -g ember-rooms -m 0640 "$SRC/config.example.json" "$CONFIG"
    echo "config: created $CONFIG from config.example.json"
fi
restore_config() {
    if [ "$had_config" -eq 1 ]; then
        cat "$backup/config.json" > "$CONFIG"
    else
        rm -f "$CONFIG"
    fi
}

# Copy the staged builds into place.
for id in ${build_ids[@]+"${build_ids[@]}"}; do
    install -d -o root -g root -m 0755 "$BUILDS/$id"
    for file in sf4e-room-host sf4-net; do
        install -o root -g root -m 0755 "$SRC/builds/$id/$file" "$BUILDS/$id/$file.new"
        mv -f "$BUILDS/$id/$file.new" "$BUILDS/$id/$file"
    done
    echo "build $id: installed under $BUILDS/$id"
done

# Make config.json's builds match: add or update staged builds, keep every
# other key, and with --prune drop builds that are no longer staged.
SYNC=$(cat <<'PY'
import grp, json, os, sys, tempfile
path, lib, prune = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
ids = [line for line in sys.stdin.read().split("\n") if line]
with open(path) as file:
    config = json.load(file)
builds = config.setdefault("builds", {})
changed = False
for build in ids:
    entry = {
        "room_host": f"{lib}/builds/{build}/sf4e-room-host",
        "helper": f"{lib}/builds/{build}/sf4-net",
    }
    if builds.get(build) != entry:
        print(f"config: build {build} {'updated' if build in builds else 'added'}")
        builds[build] = entry
        changed = True
for build in [name for name in builds if name not in ids]:
    if prune:
        print(f"config: build {build} removed")
        del builds[build]
        changed = True
    else:
        print(f"config: build {build} is not staged; kept (run setup.sh --prune to remove it)")
if changed:
    handle, temporary = tempfile.mkstemp(dir=os.path.dirname(path), prefix=".config.json.")
    with os.fdopen(handle, "w") as file:
        json.dump(config, file, indent=2)
        file.write("\n")
        os.fchmod(file.fileno(), 0o640)
        os.fchown(file.fileno(), 0, grp.getgrnam("ember-rooms").gr_gid)
    os.replace(temporary, path)
else:
    print("config: builds unchanged")
PY
)
if ! printf '%s\n' ${build_ids[@]+"${build_ids[@]}"} | python3 -c "$SYNC" "$CONFIG" "$LIB" "$PRUNE"; then
    restore_config
    echo "Could not update the builds in $CONFIG; restored the previous file." >&2
    exit 1
fi
# Builds dropped from the config by --prune keep their files until the restart
# and health checks below have passed: a rollback to the previous config must
# find every binary it names.
pruned=()
if [ "$PRUNE" -eq 1 ] && [ -d "$BUILDS" ]; then
    for dir in "$BUILDS"/*/; do
        [ -d "$dir" ] || continue
        id=$(basename "$dir")
        keep=0
        for staged in ${build_ids[@]+"${build_ids[@]}"}; do
            [ "$staged" = "$id" ] && keep=1
        done
        [ "$keep" -eq 0 ] && pruned+=("$id")
    done
fi

# Read back what the supervisor will use. The secret file must be the one
# this script manages, because connect-bridge.sh stores that file's value.
SETTINGS=$(python3 - "$CONFIG" <<'PY'
import json, sys
config = json.load(open(sys.argv[1]))
low, high = config.get("port_range", [45800, 45899])
print(config.get("bind", "127.0.0.1:47830"), low, high, config.get("max_rooms", 8),
      config.get("secret_file", ""), len(config.get("builds", {})))
PY
) || { restore_config; echo "$CONFIG is not valid JSON; restored the previous file." >&2; exit 1; }
read -r BIND LOW HIGH MAX_ROOMS SECRET_FILE BUILD_COUNT <<< "$SETTINGS"
if [ "$SECRET_FILE" != "$SECRET" ]; then
    restore_config
    echo "$CONFIG names secret_file $SECRET_FILE; this deployment uses $SECRET. Fix it and run again." >&2
    exit 1
fi
echo "config: bind $BIND, ports $LOW-$HIGH, max_rooms $MAX_ROOMS, $BUILD_COUNT build(s)"
if [ "$BUILD_COUNT" -eq 0 ]; then
    echo "note: no builds yet, so every room request is refused as unsupported_build." >&2
    echo "      Stage one under ~/ember-rooms/builds/<build_id>/ and run this script again." >&2
fi
tasks=$(sed -n 's/^TasksMax=//p' "$SRC/ember-rooms.service" | head -n 1)
if [ -n "$tasks" ] && [ "$tasks" -lt $((MAX_ROOMS * 64)) ] 2>/dev/null; then
    echo "warning: max_rooms $MAX_ROOMS needs about $((MAX_ROOMS * 64)) tasks but the unit allows $tasks; raise TasksMax (and check MemoryMax) in the unit." >&2
fi

# Firewall: each room uses two UDP ports from the range.
if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q '^Status: active'; then
    ufw allow "$LOW:$HIGH/udp" comment 'ember-rooms room hosts' >/dev/null
    if ufw status | grep -qF "$LOW:$HIGH/udp"; then
        echo "ufw: UDP $LOW:$HIGH allowed"
    else
        echo "ufw: could not confirm the UDP $LOW:$HIGH rule in 'ufw status'." >&2
        restore_config
        exit 1
    fi
else
    echo "note: ufw is not active here, so no rule was added. Make sure UDP $LOW-$HIGH is open to the Internet" >&2
    echo "      (and in any provider firewall) or players cannot reach public rooms." >&2
fi

# The unit check is advisory: systemd-analyze may warn about things that
# still run, but a typo in a directive shows up here.
if command -v systemd-analyze >/dev/null 2>&1; then
    if ! verify=$(systemd-analyze verify "$SRC/ember-rooms.service" 2>&1); then
        echo "systemd-analyze verify reported:" >&2
        echo "$verify" >&2
    fi
fi
install -o root -g root -m 0644 "$SRC/ember-rooms.service" "$UNIT"
systemctl daemon-reload
systemctl enable ember-rooms >/dev/null 2>&1

# Ask the supervisor how many rooms run, to warn before a restart that drains.
auth_curl() {
    printf 'Authorization: Bearer %s\n' "$(tr -d '[:space:]' < "$SECRET")" | curl -fsS --max-time 5 -H @- "$@"
}
if systemctl is-active --quiet ember-rooms; then
    rooms=$(auth_curl "http://$BIND/rooms" 2>/dev/null | python3 -c 'import json, sys; print(len(json.load(sys.stdin)))' 2>/dev/null || echo "?")
    if [ "$rooms" != "0" ]; then
        echo "restart: $rooms room(s) are running; the supervisor drains first (no new rooms, up to the drain time in config.json, then the rest are closed)."
    fi
fi
if ! systemctl restart ember-rooms; then
    restore_config
    systemctl restart ember-rooms || true
    echo "ember-rooms did not restart; restored the previous config.json. See: sudo journalctl -u ember-rooms -n 30" >&2
    exit 1
fi

echo
echo "=== ember-rooms ==="
systemctl is-active ember-rooms || true
body=""
for attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    if body=$(curl -fsS --max-time 3 "http://$BIND/health" 2>/dev/null) && [ "$body" = "ok" ]; then
        break
    fi
    body=""
    sleep 1
done
if [ -z "$body" ]; then
    echo "health: no valid answer from http://$BIND/health" >&2
    restore_config
    systemctl restart ember-rooms || true
    echo "Restored the previous config.json. See: sudo journalctl -u ember-rooms -n 30" >&2
    exit 1
fi
echo "health: $body"
if listing=$(auth_curl "http://$BIND/rooms" 2>/dev/null); then
    echo "auth: the secret works ($(python3 -c 'import json, sys; print(len(json.loads(sys.argv[1])))' "$listing") rooms listed)"
else
    echo "auth: GET /rooms with the stored secret failed" >&2
    exit 1
fi
rm -rf "$backup"
for id in ${pruned[@]+"${pruned[@]}"}; do
    rm -rf "$BUILDS/$id"
    echo "build $id: files removed (rooms already running keep their copy until they end)"
done

echo
echo "Next, to connect the bridge (needs the bridge build that supports public rooms):"
echo "  ssh -t vps \"sudo bash ~/ember-rooms/connect-bridge.sh\""
echo "=== done ==="
