#!/usr/bin/env bash
# Installs one Ember tournament bridge, picked by the folder this script is in
# (instance.sh). Run as one of:
#   sudo bash ~/ember-bridge/setup.sh
#       production, https://bridge.embernetplay.link (tenants.json)
#   sudo bash ~/ember-bridge-staging/setup.sh
#       staging, https://bridge-staging.embernetplay.link (tenants.staging.json)
#
# Expects, next to this script: bin/ember-bridge, the unit and site templates
# (*.in), instance.sh, backup.sh, restore.sh, the instance's tenants file and
# nginx/ember-bridge-zone.conf. Production's binary is built as katie with
# `cargo build --release --locked -p ember-bridge` in ~/ember-bridge/src/server/ember;
# staging's is a newer build, made in WSL and copied to ~/ember-bridge-staging/bin/.
# The tenants file sets the instance's origin and listen address, and so its
# site's host and port. Running it again installs a newer binary and applies
# the tenants file; the bridge ID, secrets and database stay.
#
# What it changes, for production (an instance such as staging puts
# -<instance> after ember-bridge in every name and path, and has its own site):
#   user ember-bridge                                  system account the bridge runs as
#   /usr/local/lib/ember-bridge/                       binary, backup and restore scripts (root-owned)
#   /var/lib/ember-bridge/                             bridge.json, bridge-secrets.json, database (0700)
#                                                      and integrations.json, which only
#                                                      set-integration-secret.sh writes: Discord
#                                                      sign-in and BluMint results start once
#                                                      their secrets are stored there
#   /var/backups/ember-bridge/                         daily backups, kept 14 days (0700)
#   /etc/systemd/system/ember-bridge*.{service,timer}  the service and the backup timer
#   /etc/nginx/conf.d/ember-bridge.conf                request rate zones, shared by every instance
#                                                      (an address's budget counts both bridges);
#                                                      another instance installs it only when absent,
#                                                      and refuses to replace a different one
#   /etc/nginx/sites-available/bridge.embernetplay.link (and its sites-enabled link);
#                                                      the site also defines its own BluMint callback
#                                                      request zones, named after the instance
# Nothing is opened in ufw: the bridge listens on loopback, nginx already serves 80 and 443.
#
# Nothing but the nginx zone and site is moved until nginx -t accepts them; a
# failed check puts those back and leaves the binary, bridge.json and units as
# they were. If the new bridge then does not start or answer, the previous
# binary, scripts, bridge.json, units, site and zone are put back and the
# previous bridge is restarted. Before changing anything it refuses a folder
# that is not directly in the operator's home, a tenants file whose origin or
# listen address another installed bridge has, and one the new binary would
# not start with (ember-bridge apply-tenants). test-setup.sh moves every
# path above under a test folder (see instance.sh) and stands in for the
# commands that need root.
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"
# Commands run as the service account below would not be able to read katie's home.
cd /
[ -f "$SRC/instance.sh" ] || { echo "Missing $SRC/instance.sh" >&2; exit 1; }
. "$SRC/instance.sh"
bridge_instance "$SRC" operator || exit 2
TENANTS=tenants${INSTANCE:+.$INSTANCE}.json
UNITS=$ROOT/etc/systemd/system
ZONE=$ROOT/etc/nginx/conf.d/ember-bridge.conf
# nginx zone names are global, so the zones a site defines for itself are named
# after its instance.
ZONE_PREFIX=${NAME//-/_}
CERT=$ROOT/etc/ssl/certs/embernetplay.link.fullchain.pem
KEY=$ROOT/etc/ssl/private/embernetplay.link.key

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo." >&2
    exit 1
fi
for file in bin/ember-bridge ember-bridge.service.in ember-bridge-backup.service.in \
    ember-bridge-backup.timer.in backup.sh restore.sh "$TENANTS" \
    nginx/ember-bridge-zone.conf nginx/bridge.conf.in; do
    if [ ! -f "$SRC/$file" ]; then
        echo "Missing $SRC/$file" >&2
        exit 1
    fi
done
for file in "$CERT" "$KEY"; do
    if [ ! -f "$file" ]; then
        echo "Missing $file (the embernetplay.link wildcard certificate)." >&2
        exit 1
    fi
done
# The zone file holds production's request limits too, so only production's
# setup replaces it.
if [ -n "$INSTANCE" ] && [ -e "$ZONE" ] && ! cmp -s "$SRC/nginx/ember-bridge-zone.conf" "$ZONE"; then
    echo "$ZONE differs from $SRC/nginx/ember-bridge-zone.conf. It sets production's request limits as well;" >&2
    echo "install it with production's setup.sh first, or copy the installed one into this folder." >&2
    exit 1
fi
# The instance's host and port, from the origin and listen its tenants file
# also writes into bridge.json below.
ADDRESS=$(cat <<'PY'
import json, re, sys
deployed = json.load(open(sys.argv[1]))
origin = re.fullmatch(r"https://([a-z0-9.-]+)", str(deployed.get("origin")))
listen = re.fullmatch(r"127\.0\.0\.1:([1-9][0-9]{0,4})", str(deployed.get("listen")))
if not origin or not listen:
    sys.exit(f"{sys.argv[1]} needs origin https://<host> and listen 127.0.0.1:<port>")
print(origin[1], listen[1])
PY
)
ADDRESS=$(python3 -c "$ADDRESS" "$SRC/$TENANTS")
HOST=${ADDRESS% *}
PORT=${ADDRESS#* }
SITE=$ROOT/etc/nginx/sites-available/$HOST
ENABLED=$ROOT/etc/nginx/sites-enabled/$HOST
# Another installed bridge with this origin or listen address would lose its
# site or its port to this one.
CLAIMED=$(cat <<'PY'
import glob, json, os, sys
own, origin, listen, pattern = sys.argv[1:]
for path in sorted(glob.glob(pattern)):
    if os.path.dirname(path) == os.path.dirname(own):
        continue
    try:
        other = json.load(open(path))
    except (OSError, ValueError) as error:
        sys.exit(f"Cannot read {path} to check it against this bridge: {error}")
    for key, mine in (("origin", origin), ("listen", listen)):
        if other.get(key) == mine:
            sys.exit(f"{path} already has {key} {mine}; give this bridge its own in its tenants file.")
PY
)
python3 -c "$CLAIMED" "$STATE/bridge.json" "https://$HOST" "127.0.0.1:$PORT" "$ROOT/var/lib/ember-bridge*/bridge.json"

if ! id "$NAME" >/dev/null 2>&1; then
    useradd --system --home-dir "$STATE" --no-create-home --shell /usr/sbin/nologin "$NAME"
fi

install -d -o root -g root -m 0755 "$LIB"
install -d -o "$NAME" -g "$NAME" -m 0700 "$STATE" "$BACKUPS"

# Stage everything as <path>.new, check it, and only then move it all into
# place, so a failure before that leaves the running bridge as it was. NEW
# names what the last step moves; the site and zone are moved earlier. The
# trap removes what was staged and the backup on every way out, and puts the
# site and zone back if the script dies while they are moved but unchecked.
backup=$(mktemp -d)
NEW=("$LIB/ember-bridge" "$LIB/instance.sh" "$LIB/backup.sh" "$LIB/restore.sh" "$STATE/bridge.json"
    "$UNITS/$NAME.service" "$UNITS/$NAME-backup.service" "$UNITS/$NAME-backup.timer")
swapped=
cleanup() {
    local path
    for path in "${NEW[@]}" "$SITE" "$ZONE"; do rm -f "$path.new"; done
    [ -z "$swapped" ] || restore
    rm -rf "$backup"
}
trap cleanup EXIT
stage_install() {
    install -o root -g root -m "$1" "$2" "$3.new"
}
stage_install 0755 "$SRC/bin/ember-bridge" "$LIB/ember-bridge"
stage_install 0644 "$SRC/instance.sh" "$LIB/instance.sh"
stage_install 0755 "$SRC/backup.sh" "$LIB/backup.sh"
stage_install 0755 "$SRC/restore.sh" "$LIB/restore.sh"

# First install: a fresh bridge ID and secrets, then the deployed settings.
# The new binary makes them; if a later step fails they stay, and the next run
# finds them and goes on.
if [ ! -f "$STATE/bridge.json" ]; then
    sudo -u "$NAME" "$LIB/ember-bridge.new" init "$STATE"
fi
# Apply the tenants file and the deployed settings, keeping bridge_id and
# paths, with the new binary: it turns the development switches off, refuses
# other database or secrets names than backup.sh and restore.sh work on, and
# refuses a configuration it would not start with, before anything moves.
# Root only opens the tenants file (katie's home is closed to the service
# account) and hands it over on stdin; everything inside the service-owned
# folder runs as the service account. The result is written as
# bridge.json.new and replaces bridge.json with the rest below.
sudo -u "$NAME" "$LIB/ember-bridge.new" apply-tenants "$STATE/bridge.json" "$STATE/bridge.json.new" < "$SRC/$TENANTS"

# Every instance's units and site come from one template each: @NAME@ is the
# account, unit and folder name, @HOST@ and @PORT@ come from the tenants
# file, @ZONE@ prefixes the request zones the site defines, and @LABEL@
# names an instance other than production in descriptions.
LABEL=${INSTANCE:+ ($INSTANCE)}
render() {
    sed -e "s/@NAME@/$NAME/g" -e "s/@HOST@/$HOST/g" -e "s/@PORT@/$PORT/g" \
        -e "s/@LABEL@/$LABEL/g" -e "s/@ZONE@/$ZONE_PREFIX/g" "$1" > "$2.new"
    chmod 0644 "$2.new"
}
for unit in .service -backup.service -backup.timer; do
    render "$SRC/ember-bridge$unit.in" "$UNITS/$NAME$unit"
done
render "$SRC/nginx/bridge.conf.in" "$SITE"
stage_install 0644 "$SRC/nginx/ember-bridge-zone.conf" "$ZONE"

# nginx reads the installed files, so the zone and the site go in first, with
# copies of the zone, the site and the enabled entry (or a note that each was
# absent) so a failed check puts everything back as it was. Nothing else has
# been moved yet.
[ -e "$ZONE" ] && cp -a "$ZONE" "$backup/zone"
[ -e "$SITE" ] && cp -a "$SITE" "$backup/site"
[ -e "$ENABLED" ] || [ -L "$ENABLED" ] && cp -a "$ENABLED" "$backup/enabled"
restore() {
    if [ -e "$backup/zone" ]; then cp -a "$backup/zone" "$ZONE"; else rm -f "$ZONE"; fi
    if [ -e "$backup/site" ]; then cp -a "$backup/site" "$SITE"; else rm -f "$SITE"; fi
    rm -f "$ENABLED"
    if [ -e "$backup/enabled" ] || [ -L "$backup/enabled" ]; then cp -a "$backup/enabled" "$ENABLED"; fi
}
swapped=1
mv -f "$ZONE.new" "$ZONE"
mv -f "$SITE.new" "$SITE"
ln -sfn "$SITE" "$ENABLED"
if ! nginx -t; then
    restore
    swapped=
    if nginx -t; then
        echo "nginx rejected the new configuration; restored the previous files. The binary, bridge.json and units were not replaced." >&2
    else
        echo "nginx rejected the new configuration, and the restored files fail too; fix nginx before reloading it." >&2
    fi
    exit 1
fi
swapped=

# The bridge must answer with its own origin, directly and through nginx.
check() {
    local where=$1; shift
    local body
    for attempt in 1 2 3 4 5 6 7 8 9 10; do
        if body=$(curl -fsS --max-time 5 "$@" 2>/dev/null) &&
            python3 -c 'import json, sys; d = json.loads(sys.argv[1]); sys.exit(d.get("origin") != sys.argv[2] or d.get("bridge_id") != sys.argv[3])'                 "$body" "https://$HOST" "$BRIDGE_ID"; then
            echo "$where: ok ($BRIDGE_ID)"
            return 0
        fi
        sleep 1
    done
    echo "$where: no valid answer" >&2
    return 1
}
start_bridge() {
    systemctl restart "$NAME" &&
        BRIDGE_ID=$(sudo -u "$NAME" python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["bridge_id"])' "$STATE/bridge.json") &&
        check service "http://127.0.0.1:$PORT/.well-known/ember-bridge.json"
}
# Put back what was there before. A file goes back by rename, because the new
# binary may still be running.
put_back() {
    local path
    for path in "${NEW[@]}"; do
        if [ -e "$backup/old$path" ]; then
            cp -a "$backup/old$path" "$path.new" && mv -f "$path.new" "$path" || return 1
        else
            rm -f "$path"
        fi
    done
    restore
    systemctl daemon-reload
    systemctl reset-failed "$NAME" || :
    systemctl restart "$NAME"
}

# nginx accepts it: keep a copy of what the rest replaces, move it into place
# and start the new bridge.
mkdir "$backup/old"
for path in "${NEW[@]}"; do
    [ ! -e "$path" ] || cp -a --parents "$path" "$backup/old"
done
for path in "${NEW[@]}"; do
    mv -f "$path.new" "$path"
done
systemctl daemon-reload
systemctl enable "$NAME" "$NAME-backup.timer"
if ! start_bridge; then
    if put_back; then
        echo "The new bridge did not start or answer; put back the previous binary, bridge.json, units, site and zone, and restarted it." >&2
    else
        echo "The new bridge did not start or answer, and putting the previous files back and restarting $NAME failed. Check the files and systemctl status $NAME." >&2
    fi
    exit 1
fi
systemctl start "$NAME-backup.timer"
systemctl reload nginx || systemctl restart nginx

echo
echo "=== $NAME ==="
systemctl is-active "$NAME"
check public --resolve "$HOST:443:127.0.0.1" "https://$HOST/.well-known/ember-bridge.json"
echo "=== done ==="
