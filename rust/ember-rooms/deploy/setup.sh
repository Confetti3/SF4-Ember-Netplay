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
# Running it again installs a newer supervisor binary, adds the builds that
# are staged, and restarts the service. The secret and an existing
# config.json are never replaced; only config.json's "builds" entries change.
# Without --prune a build that is installed but no longer staged is kept;
# with --prune its config entry and installed files are removed. Rooms that
# already run keep their binaries until they end.
#
# A build id names one exact pair of binaries. A build that is already
# installed is left alone when the staged files are identical, and the run
# stops before changing anything when they differ: running rooms use those
# files, so they are never overwritten.
#
# Before it replaces anything the script copies the current supervisor binary,
# unit and config.json into one temporary bundle. If the install, the restart
# or the health check fails, the whole bundle is put back, builds added by this
# run are removed, and the previous service is restarted and checked again.
# The script then exits with an error. On a first install there is nothing to
# restore, so the service is stopped and what this run added is removed.
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
#
# EMBER_ROOMS_ROOT moves every path above under that folder and skips the root
# check, the user account and file ownership. test-setup.sh uses it with
# stand-ins for systemctl and curl; leave it unset on the server.
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"
# Commands run as ember-rooms would not be able to read katie's home.
cd /
ROOT=${EMBER_ROOMS_ROOT:-}
LIB=$ROOT/usr/local/lib/ember-rooms
BUILDS=$LIB/builds
ETC=$ROOT/etc/ember-rooms
CONFIG=$ETC/config.json
SECRET=$ETC/supervisor.secret
STATE=$ROOT/var/lib/ember-rooms
UNIT=$ROOT/etc/systemd/system/ember-rooms.service
BUILD_ID_PATTERN='^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$'

# install and chown with an owner, except under EMBER_ROOMS_ROOT.
inst() {
    local owner=$1 group=$2
    shift 2
    if [ -n "$ROOT" ]; then install "$@"; else install -o "$owner" -g "$group" "$@"; fi
}
own() {
    [ -n "$ROOT" ] || chown "$1:$2" "$3"
}

PRUNE=0
case "${1:-}" in
    "") ;;
    --prune) PRUNE=1 ;;
    *) echo "usage: sudo bash setup.sh [--prune]" >&2; exit 2 ;;
esac

if [ -z "$ROOT" ] && [ "$(id -u)" -ne 0 ]; then
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
# folder, a Windows binary or different bytes under an installed build id stop
# the run instead of reaching the config or the disk.
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
            if [ -e "$BUILDS/$id" ] && ! cmp -s "$dir$file" "$BUILDS/$id/$file"; then
                echo "builds/$id/$file differs from the installed $BUILDS/$id/$file." >&2
                echo "A build id names one exact pair of binaries and installed builds are never overwritten," >&2
                echo "because running rooms use them. Stage the new binaries under a new build id." >&2
                exit 1
            fi
        done
        build_ids+=("$id")
    done
fi

if [ -n "$ROOT" ]; then
    echo "user: skipped under EMBER_ROOMS_ROOT"
elif ! id ember-rooms >/dev/null 2>&1; then
    useradd --system --home-dir "$STATE" --no-create-home --shell /usr/sbin/nologin ember-rooms
    echo "user: created ember-rooms"
else
    echo "user: ember-rooms exists"
fi
if [ -z "$ROOT" ]; then
    getent group ember-rooms >/dev/null || { echo "Group ember-rooms is missing." >&2; exit 1; }
fi

inst root root -d -m 0755 "$LIB" "$BUILDS"
inst root ember-rooms -d -m 0750 "$ETC"
inst ember-rooms ember-rooms -d -m 0700 "$STATE"

# The secret is created once. A kept secret is only checked and re-owned.
if [ ! -s "$SECRET" ]; then
    tmp=$(mktemp "$ETC/.secret.XXXXXX")
    openssl rand -hex 32 > "$tmp"
    own root ember-rooms "$tmp"
    chmod 0640 "$tmp"
    mv -f "$tmp" "$SECRET"
    echo "secret: created $SECRET"
else
    own root ember-rooms "$SECRET"
    chmod 0640 "$SECRET"
    echo "secret: kept $SECRET"
fi
if [ "$(tr -d '[:space:]' < "$SECRET" | wc -c)" -lt 32 ]; then
    echo "$SECRET holds fewer than 32 characters; the supervisor would refuse it." >&2
    exit 1
fi

# Everything the failure path needs to put back. Nothing below this point is
# replaced before the bundle is complete.
backup=$(mktemp -d)
had_exec=0
had_unit=0
had_config=0
added_builds=()
armed=0
if [ -f "$LIB/ember-rooms" ]; then cp -a "$LIB/ember-rooms" "$backup/ember-rooms"; had_exec=1; fi
if [ -f "$UNIT" ]; then cp -a "$UNIT" "$backup/ember-rooms.service"; had_unit=1; fi
if [ -f "$CONFIG" ]; then cp -a "$CONFIG" "$backup/config.json"; had_config=1; fi

auth_curl() {
    printf 'Authorization: Bearer %s\n' "$(tr -d '[:space:]' < "$SECRET")" | curl -fsS --max-time 5 -H @- "$@"
}
# Succeeds once /health answers "ok", for up to 15 seconds.
wait_health() {
    local body="" attempt
    for attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
        if body=$(curl -fsS --max-time 3 "http://$BIND/health" 2>/dev/null) && [ "$body" = "ok" ]; then
            echo "health: $body"
            return 0
        fi
        body=""
        sleep 1
    done
    return 1
}
# Puts one bundled file back, or removes the new one if there was none.
put_back() {
    local name=$1 had=$2 dest=$3
    if [ "$had" -eq 1 ]; then
        cp -a "$backup/$name" "$dest.restore" && mv -f "$dest.restore" "$dest"
    else
        rm -f "$dest"
    fi
}
rollback() {
    set +e
    local ok=1 id
    echo "Install failed; restoring the previous installation." >&2
    if [ "$had_exec" -eq 0 ] || [ "$had_unit" -eq 0 ]; then
        systemctl stop ember-rooms >/dev/null 2>&1
        systemctl disable ember-rooms >/dev/null 2>&1
    fi
    put_back ember-rooms "$had_exec" "$LIB/ember-rooms" || ok=0
    put_back ember-rooms.service "$had_unit" "$UNIT" || ok=0
    put_back config.json "$had_config" "$CONFIG" || ok=0
    for id in ${added_builds[@]+"${added_builds[@]}"}; do
        rm -rf "$BUILDS/$id" || ok=0
    done
    systemctl daemon-reload || ok=0
    if [ "$had_exec" -eq 0 ] || [ "$had_unit" -eq 0 ]; then
        if [ "$ok" -eq 1 ]; then
            echo "This was a first install, so there is nothing to restore: the service is stopped and the files this run added are removed." >&2
        else
            echo "This was a first install and removing what it added failed too; the service is stopped. Check $LIB, $UNIT and $CONFIG." >&2
        fi
        return
    fi
    BIND=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("bind", "127.0.0.1:47830"))' "$CONFIG" 2>/dev/null || echo 127.0.0.1:47830)
    systemctl restart ember-rooms || ok=0
    if [ "$ok" -eq 1 ] && wait_health; then
        echo "Restored the previous supervisor binary, unit and config.json, and it answers on /health. See: sudo journalctl -u ember-rooms -n 30" >&2
    else
        echo "RESTORING FAILED: the previous installation is not answering. The bundle is kept in $backup. See: sudo journalctl -u ember-rooms -n 30" >&2
        backup_keep=1
    fi
}
backup_keep=0
on_exit() {
    local status=$?
    trap - EXIT
    if [ -d "$BUILDS" ]; then rm -rf "$BUILDS"/.new.*; fi
    if [ "$status" -ne 0 ] && [ "$armed" -eq 1 ]; then
        armed=0
        rollback
    fi
    if [ "$backup_keep" -eq 0 ]; then rm -rf "$backup"; fi
    exit "$status"
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
armed=1

# New files are renamed into place: a running binary cannot be overwritten
# (text file busy), and a renamed one leaves running rooms on the old copy.
inst root root -m 0755 "$SRC/bin/ember-rooms" "$LIB/ember-rooms.new"
mv -f "$LIB/ember-rooms.new" "$LIB/ember-rooms"
echo "binary: $LIB/ember-rooms installed"

if [ "$had_config" -eq 1 ]; then
    echo "config: kept $CONFIG"
else
    inst root ember-rooms -m 0640 "$SRC/config.example.json" "$CONFIG"
    echo "config: created $CONFIG from config.example.json"
fi

# Copy the staged builds into place. A new build is built in a temporary
# folder and renamed, so it appears whole or not at all. An installed build
# was checked above to be identical, so it is left as it is.
rm -rf "$BUILDS"/.new.*
for id in ${build_ids[@]+"${build_ids[@]}"}; do
    if [ -e "$BUILDS/$id" ]; then
        echo "build $id: already installed, unchanged"
        continue
    fi
    tmp=$(mktemp -d "$BUILDS/.new.XXXXXX")
    chmod 0755 "$tmp"
    for file in sf4e-room-host sf4-net; do
        inst root root -m 0755 "$SRC/builds/$id/$file" "$tmp/$file"
    done
    mv -T "$tmp" "$BUILDS/$id"
    added_builds+=("$id")
    echo "build $id: installed under $BUILDS/$id"
done

# Make config.json's builds match: add or update staged builds, keep every
# other key, and with --prune drop builds that are no longer staged.
SYNC=$(cat <<'PY'
import grp, json, os, sys, tempfile
path, lib, prune, owned = sys.argv[1], sys.argv[2], sys.argv[3] == "1", sys.argv[4] == "1"
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
        if owned:
            os.fchown(file.fileno(), 0, grp.getgrnam("ember-rooms").gr_gid)
    os.replace(temporary, path)
else:
    print("config: builds unchanged")
PY
)
OWNED=1
[ -z "$ROOT" ] || OWNED=0
if ! printf '%s\n' ${build_ids[@]+"${build_ids[@]}"} | python3 -c "$SYNC" "$CONFIG" "$LIB" "$PRUNE" "$OWNED"; then
    echo "Could not update the builds in $CONFIG." >&2
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
) || { echo "$CONFIG is not valid JSON." >&2; exit 1; }
read -r BIND LOW HIGH MAX_ROOMS SECRET_FILE BUILD_COUNT <<< "$SETTINGS"
if [ "$SECRET_FILE" != "$SECRET" ]; then
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
inst root root -m 0644 "$SRC/ember-rooms.service" "$UNIT.new"
mv -f "$UNIT.new" "$UNIT"
systemctl daemon-reload
systemctl enable ember-rooms >/dev/null 2>&1

# Ask the supervisor how many rooms run, to warn before a restart that drains.
if systemctl is-active --quiet ember-rooms; then
    rooms=$(auth_curl "http://$BIND/rooms" 2>/dev/null | python3 -c 'import json, sys; print(len(json.load(sys.stdin)))' 2>/dev/null || echo "?")
    if [ "$rooms" != "0" ]; then
        echo "restart: $rooms room(s) are running; the supervisor drains first (no new rooms, up to the drain time in config.json, then the rest are closed)."
    fi
fi
if ! systemctl restart ember-rooms; then
    echo "ember-rooms did not restart." >&2
    exit 1
fi

echo
echo "=== ember-rooms ==="
systemctl is-active ember-rooms || true
if ! wait_health; then
    echo "health: no valid answer from http://$BIND/health" >&2
    exit 1
fi
if listing=$(auth_curl "http://$BIND/rooms" 2>/dev/null); then
    echo "auth: the secret works ($(python3 -c 'import json, sys; print(len(json.loads(sys.argv[1])))' "$listing") rooms listed)"
else
    echo "auth: GET /rooms with the stored secret failed" >&2
    exit 1
fi
armed=0
for id in ${pruned[@]+"${pruned[@]}"}; do
    rm -rf "$BUILDS/$id"
    echo "build $id: files removed (rooms already running keep their copy until they end)"
done

echo
echo "Next, to connect the bridge (needs the bridge build that supports public rooms):"
echo "  ssh -t vps \"sudo bash ~/ember-rooms/connect-bridge.sh\""
echo "=== done ==="
