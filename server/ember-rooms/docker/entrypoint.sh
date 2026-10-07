#!/bin/sh
# Starts the supervisor with the configuration the host provides, or one made
# from the environment for the image's build.
#
# With /etc/ember-rooms/config.json (bind-mounted, the same file a bare-metal
# install uses) the supervisor reads it as it is; it should name the image's
# build (EMBER_ROOMS_BUILD_ID) under "builds", as deploy/setup.sh writes it,
# with the binaries at /usr/local/lib/ember-rooms/builds/<build_id>/.
#
# Without it, config.json is written to the state directory from:
#   EMBER_ROOMS_BIND         default 127.0.0.1:47830
#   EMBER_ROOMS_SECRET_FILE  default /etc/ember-rooms/supervisor.secret
#   EMBER_ROOMS_MAX_ROOMS    default 10
#   EMBER_ROOMS_PORT_RANGE   default 45800-45899 (two UDP ports per room)
#   EMBER_ROOMS_EMPTY_CLOSE_SECS, EMBER_ROOMS_DRAIN_SECS  defaults 120, 600
# with the image's build as the only one. The secret file must exist and be
# readable by the container's user: uid 4783, with the host's ember-rooms
# group added (compose.yml, group_add).
set -eu
# EMBER_ROOMS_LIB: where the binaries are; only a test outside the image sets it.
LIB=${EMBER_ROOMS_LIB:-/usr/local/lib/ember-rooms}
CONFIG=${EMBER_ROOMS_CONFIG:-/etc/ember-rooms/config.json}
BUILD=${EMBER_ROOMS_BUILD_ID:?the image was built without a build id}
if [ -f "$CONFIG" ]; then
    if ! grep -q "\"$BUILD\"" "$CONFIG"; then
        echo "ember-rooms: $CONFIG does not list this image's build $BUILD; every room for it will be refused as unsupported_build" >&2
    fi
else
    CONFIG=${HOME:-/var/lib/ember-rooms}/config.json
    range=${EMBER_ROOMS_PORT_RANGE:-45800-45899}
    low=${range%-*}
    high=${range#*-}
    case "$low$high" in *[!0-9]*|"") echo "ember-rooms: EMBER_ROOMS_PORT_RANGE must be <low>-<high>" >&2; exit 2 ;; esac
    secret=${EMBER_ROOMS_SECRET_FILE:-/etc/ember-rooms/supervisor.secret}
    if [ ! -r "$secret" ]; then
        echo "ember-rooms: the secret file $secret is missing or not readable by uid $(id -u)" >&2
        exit 2
    fi
    cat > "$CONFIG" <<JSON
{
  "bind": "${EMBER_ROOMS_BIND:-127.0.0.1:47830}",
  "secret_file": "$secret",
  "max_rooms": ${EMBER_ROOMS_MAX_ROOMS:-10},
  "port_range": [$low, $high],
  "empty_close_secs": ${EMBER_ROOMS_EMPTY_CLOSE_SECS:-120},
  "drain_secs": ${EMBER_ROOMS_DRAIN_SECS:-600},
  "builds": {
    "$BUILD": {
      "room_host": "$LIB/builds/$BUILD/sf4e-room-host",
      "helper": "$LIB/builds/$BUILD/sf4-net"
    }
  }
}
JSON
    echo "ember-rooms: wrote $CONFIG for build $BUILD" >&2
fi
exec "$LIB/ember-rooms" "$CONFIG"
