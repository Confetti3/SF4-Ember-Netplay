#!/usr/bin/env bash
# Assembles the folder upgrade.sh runs from (docs/design/SFV_BRIDGE.md), on
# this machine, from this checkout. Copying it to the VPS is a separate,
# deliberate step:
#
#   bash server/sfv-bridge/stage.sh <linux ember-bridge> <out folder>
#       [<linux sfve-room-host> <linux sf4-net> <SFV's data/pick-space.json>]
#   scp -r <out folder> vps:ember-sfv-bridge
#
# <linux ember-bridge> is the library line's Linux bridge: the build SF4's
# production bridge runs now (a13ded4c, in ~/ember-sfv-rooms/ember-bridge on
# the VPS), or a newer one built in WSL with
# `cargo build --release --locked -p ember-bridge` in server/ember.
# The folder holds:
#   upgrade.sh          the phases
#   ember-bridge        the binary ember-bridge-sfv runs
#   bridge-deploy/      the bridge's deploy files (setup.sh, instance.sh, units,
#                       nginx templates, tenants.sfv.json), copied into
#                       ~/ember-bridge-sfv by the bridge phase
#   ember-short/        the site's setup.sh, nginx and site with SFV's feeds on
#                       SFV's bridge, copied into ~/ember-short by the site phase
#   room-build/         with the room-host arguments: sfve-room-host, sf4-net and
#                       BUILD_ID (pick-space.json's "build"), for upgrade.sh rooms.
#                       Build both in WSL (Linux): sf4-net with
#                       `cargo build --profile server --locked` in the library's
#                       rust/sf4-net, and sfve-room-host with sfv-ember's
#                       `scripts/build-room-host-linux.sh <library root>`, the
#                       library and sfv-ember commits the SFV release ships
#                       (beta.8: b8/lib-integrate and b8/integrate).
#   SHA256SUMS          every file above; upgrade.sh refuses a folder that differs
set -euo pipefail
[ $# -eq 2 ] || [ $# -eq 5 ] || { sed -n '2,30p' "$0"; exit 2; }
BIN=$1
OUT=$2
REPO=$(cd "$(dirname "$0")/../.." && pwd)
[ -f "$BIN" ] || { echo "No bridge binary at $BIN." >&2; exit 1; }
head -c 4 "$BIN" | grep -q $'\x7fELF' || { echo "$BIN is not a Linux binary." >&2; exit 1; }
[ ! -e "$OUT" ] || { echo "$OUT exists; give a new folder." >&2; exit 1; }

mkdir -p "$OUT/bridge-deploy/nginx" "$OUT/ember-short"
cp "$REPO/server/sfv-bridge/upgrade.sh" "$OUT/"
cp "$BIN" "$OUT/ember-bridge"
D=$REPO/server/ember/bridge/deploy
cp "$D"/setup.sh "$D"/instance.sh "$D"/backup.sh "$D"/restore.sh "$D"/set-integration-secret.sh \
    "$D"/ember-bridge.service.in "$D"/ember-bridge-backup.service.in "$D"/ember-bridge-backup.timer.in \
    "$D"/tenants.sfv.json "$OUT/bridge-deploy/"
cp "$D"/nginx/bridge.conf.in "$D"/nginx/ember-bridge-zone.conf "$OUT/bridge-deploy/nginx/"
cp -r "$REPO/server/ember-short/deploy/setup.sh" "$REPO/server/ember-short/deploy/nginx" \
    "$REPO/server/ember-short/deploy/site" "$OUT/ember-short/"
if [ $# -eq 5 ]; then
    for f in "$3" "$4"; do
        [ -f "$f" ] && head -c 4 "$f" | grep -q $'\x7fELF' || { echo "$f is not a Linux binary." >&2; exit 1; }
    done
    BUILD=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["build"])' "$5" 2>/dev/null ||
        python -c 'import json, sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["build"])' "$5")
    [[ "$BUILD" =~ ^sfv-ember/[0-9]+/[0-9]+\.[0-9]+/[0-9a-f]{16}/layout-[0-9a-f]{16}$ ]] ||
        { echo "$5 names no SFV room build: $BUILD" >&2; exit 1; }
    mkdir -p "$OUT/room-build"
    cp "$3" "$OUT/room-build/sfve-room-host"
    cp "$4" "$OUT/room-build/sf4-net"
    printf '%s\n' "$BUILD" > "$OUT/room-build/BUILD_ID"
    chmod 0755 "$OUT/room-build/sfve-room-host" "$OUT/room-build/sf4-net"
    echo "Room build $BUILD staged."
fi
chmod 0755 "$OUT/upgrade.sh" "$OUT/ember-bridge" "$OUT/bridge-deploy"/*.sh "$OUT/ember-short/setup.sh"
(cd "$OUT" && find . -type f ! -name SHA256SUMS | sed 's#^\./##' | LC_ALL=C sort | xargs sha256sum > SHA256SUMS)
echo "Staged $(wc -l < "$OUT/SHA256SUMS") files in $OUT (bridge $(sha256sum "$BIN" | cut -c1-8)), from $(git -C "$REPO" rev-parse --short HEAD)."
