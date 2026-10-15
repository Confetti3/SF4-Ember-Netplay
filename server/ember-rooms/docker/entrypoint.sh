#!/bin/sh
# Starts the supervisor for the image's build.
#
# The settings come from /etc/ember-rooms/config.json when the host has one
# (bind-mounted, the same file a bare-metal install uses), or else from:
#   EMBER_ROOMS_BIND         default 127.0.0.1:47830
#   EMBER_ROOMS_SECRET_FILE  default /etc/ember-rooms/supervisor.secret
#   EMBER_ROOMS_MAX_ROOMS    default 10
#   EMBER_ROOMS_PORT_RANGE   default 45800-45899 (two UDP ports per room)
#   EMBER_ROOMS_EMPTY_CLOSE_SECS, EMBER_ROOMS_DRAIN_SECS  defaults 120, 600
# Either way the image decides the builds: an image holds one build, so the
# configuration the supervisor reads, written to the state directory, names
# that build and its binaries and nothing else. A new image is therefore a
# new build with no edit to the host's file.
#
# The secret file must be readable by the container's user: uid 4783, with
# the host's ember-rooms group added (compose.yml, group_add).
set -eu
# EMBER_ROOMS_LIB: where the binaries are; only a test outside the image sets it.
LIB=${EMBER_ROOMS_LIB:-/usr/local/lib/ember-rooms}
SETTINGS=${EMBER_ROOMS_CONFIG:-/etc/ember-rooms/config.json}
BUILD=${EMBER_ROOMS_BUILD_ID:?the image was built without a build id}
STATE=${HOME:-/var/lib/ember-rooms}
CONFIG=$STATE/config.json

for binary in sf4e-room-host sf4-net; do
    if [ ! -x "$LIB/builds/$BUILD/$binary" ]; then
        echo "ember-rooms: $LIB/builds/$BUILD/$binary is missing from the image" >&2
        exit 2
    fi
done

if [ -f "$SETTINGS" ]; then
    source=$SETTINGS
else
    range=${EMBER_ROOMS_PORT_RANGE:-45800-45899}
    low=${range%-*}
    high=${range#*-}
    case "$low$high" in *[!0-9]*|"") echo "ember-rooms: EMBER_ROOMS_PORT_RANGE must be <low>-<high>" >&2; exit 2 ;; esac
    secret=${EMBER_ROOMS_SECRET_FILE:-/etc/ember-rooms/supervisor.secret}
    if [ ! -r "$secret" ]; then
        echo "ember-rooms: the secret file $secret is missing or not readable by uid $(id -u)" >&2
        exit 2
    fi
    source=$STATE/settings.json
    jq -n \
        --arg bind "${EMBER_ROOMS_BIND:-127.0.0.1:47830}" \
        --arg secret "$secret" \
        --argjson max_rooms "${EMBER_ROOMS_MAX_ROOMS:-10}" \
        --argjson low "$low" --argjson high "$high" \
        --argjson empty_close "${EMBER_ROOMS_EMPTY_CLOSE_SECS:-120}" \
        --argjson drain "${EMBER_ROOMS_DRAIN_SECS:-600}" \
        '{bind: $bind, secret_file: $secret, max_rooms: $max_rooms, port_range: [$low, $high],
          empty_close_secs: $empty_close, drain_secs: $drain}' > "$source"
fi

# A settings file that is not a JSON object stops here, not in the supervisor.
jq -e --arg build "$BUILD" --arg lib "$LIB" '
    if type != "object" then error("not a JSON object") else . end
    | .builds = {($build): {room_host: "\($lib)/builds/\($build)/sf4e-room-host",
                            helper: "\($lib)/builds/\($build)/sf4-net"}}' \
    "$source" > "$CONFIG.tmp"
mv "$CONFIG.tmp" "$CONFIG"
echo "ember-rooms: settings from $source, build $BUILD" >&2
exec "$LIB/ember-rooms" "$CONFIG"
