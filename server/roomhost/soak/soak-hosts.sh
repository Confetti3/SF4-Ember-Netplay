#!/usr/bin/env bash
# Starts public room hosts by hand for the soak (README.md), without the
# ember-rooms supervisor. Run it as the normal user.
#
#   soak-hosts.sh <sf4e-room-host> <sf4-net> <count> <first-port>
#
# Host N (N = SOAK_FIRST_ROOM + index, default 1) gets the UDP ports
# <first-port> + 2*index (primary) and +1 (coordination), its own folder
# $SOAK_DIR/room-N (default /tmp/sf4-soak), and the room id that
# PublicRoomSoakTest mints its tickets for: the fixture's room id prefix and the
# room number as two hex digits. Every host is configured with the fixture's
# bridge id, build id, ticket key and kid and creator (the values printed by
# `PublicRoomSoakTest --print-config`), exactly like the --remote mode of
# PublicRoomHostTest.
#
# Each host's stdin is a FIFO held open by a sleeping writer; soak-stop.sh ends
# that writer, which is the order to close the room. Its stdout goes through a
# filter to room-N/stdout.log (timestamped, the invitation shortened) and the
# hosted invitation to room-N/invitation. The host, the writer and the filter
# are recorded in room-N/host.proc, holder.proc and filter.proc with their start
# time and executable (soak-lib.sh), so the other scripts never mistake a reused
# pid for one of them. When every host has reported
# `hosted`, the invitations are written, one per line and in room order, to
# $SOAK_DIR/invitations.txt (the same with `N=` in front in
# invitations-numbered.txt).
set -euo pipefail

# The stdout filter runs as this same script, so it can be started detached.
if [ "${1:-}" = "--log-filter" ]; then
    export TZ=UTC
    dir="$2"
    find='"invitation":"([^"]*)"'
    strip='^(.*"invitation":")[^"]*(".*)$'
    while IFS= read -r line; do
        if [[ $line == *'"type":"hosted"'* && $line =~ $find ]]; then
            printf '%s\n' "${BASH_REMATCH[1]}" > "$dir/invitation"
        fi
        if [[ $line =~ $strip ]]; then
            line="${BASH_REMATCH[1]}...${BASH_REMATCH[2]}"
        fi
        printf '%(%FT%TZ)T %s\n' -1 "${line:0:1500}"
    done
    exit 0
fi

if [ $# -ne 4 ]; then
    echo "usage: $0 <sf4e-room-host> <sf4-net> <count> <first-port>" >&2
    exit 2
fi
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=soak-lib.sh
. "$HERE/soak-lib.sh"
HOST_BIN="$(readlink -f "$1")"
NET_BIN="$(readlink -f "$2")"
COUNT="$3"
FIRST_PORT="$4"
RUN="${SOAK_DIR:-/tmp/sf4-soak}"
FIRST_ROOM="${SOAK_FIRST_ROOM:-1}"
CAPACITY="${SOAK_CAPACITY:-16}"
# What the writer and the filter run once they exec (see soak_record).
SLEEP_BIN="$(readlink -f "$(command -v sleep)")"
BASH_BIN="$(readlink -f "$(command -v bash)")"

# The fixture's values (src/tests/public_room_support.hxx). `PublicRoomSoakTest
# --print-config <room_ticket.exe>` prints them from the client side.
BRIDGE_ID="brg_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11"
BUILD_ID="public-room-host-test"
ROOM_ID_PREFIX="5f1e0d3c2b4a69788796a5b4c3d2e1"
TICKET_KEY="0EqyMnQrtKs6E2i9RhXk5tAiSrcaAWuvhSCjMsl3hzc"   # room_ticket key 111...1
TICKET_KID="room-ticket-test-1"
CREATOR="emb1_qhi5twaf7xszmmzqmduqggd646w2pjk6npvewlik26j6fw3abfwq" # room_ticket ember-id aaa...a

[ -x "$HOST_BIN" ] || { echo "not executable: $HOST_BIN" >&2; exit 2; }
[ -x "$NET_BIN" ] || { echo "not executable: $NET_BIN" >&2; exit 2; }
[[ $COUNT =~ ^[0-9]+$ && $COUNT -ge 1 && $COUNT -le 200 ]] || { echo "count must be 1 to 200" >&2; exit 2; }
[[ $FIRST_PORT =~ ^[0-9]+$ && $((FIRST_PORT + 2 * COUNT)) -le 65535 ]] || { echo "first-port is out of range" >&2; exit 2; }
[[ $FIRST_ROOM =~ ^[0-9]+$ && $((FIRST_ROOM + COUNT - 1)) -le 255 ]] || { echo "rooms must be numbered 1 to 255" >&2; exit 2; }

# Refuse to run over hosts that are still up.
shopt -s nullglob
for record in "$RUN"/room-*/host.proc; do
    if soak_owns "$record"; then
        echo "the soak host in $record is still running; run $HERE/soak-stop.sh first" >&2
        exit 1
    fi
done
if command -v ss > /dev/null; then
    for ((offset = 0; offset < 2 * COUNT; offset++)); do
        if ss -lunH "sport = :$((FIRST_PORT + offset))" | grep -q .; then
            echo "UDP port $((FIRST_PORT + offset)) is already in use; pick another first-port" >&2
            exit 1
        fi
    done
fi
mkdir -p "$RUN"
rm -rf "$RUN"/room-* "$RUN/invitations.txt" "$RUN/invitations-numbered.txt"
printf 'FIRST_PORT=%s\nCOUNT=%s\nFIRST_ROOM=%s\n' "$FIRST_PORT" "$COUNT" "$FIRST_ROOM" > "$RUN/soak.env"

for ((index = 0; index < COUNT; index++)); do
    number=$((FIRST_ROOM + index))
    port=$((FIRST_PORT + 2 * index))
    dir="$RUN/room-$number"
    mkdir -p "$dir"
    mkfifo "$dir/in" "$dir/out"
    room_id="$(printf '%s%02x' "$ROOM_ID_PREFIX" "$number")"
    config="$(printf '{"room_id":"%s","name":"Soak room %d","capacity":%d,"build_id":"%s","creator":"%s","bridge_id":"%s","ticket_key":"%s","ticket_kid":"%s","helper":"%s","port":%d,"coordination_port":%d}' \
        "$room_id" "$number" "$CAPACITY" "$BUILD_ID" "$CREATOR" "$BRIDGE_ID" "$TICKET_KEY" "$TICKET_KID" "$NET_BIN" "$port" "$((port + 1))")"
    # Every process gets its own session, so a closed ssh connection does not end them.
    (cd "$dir" && exec setsid "$HOST_BIN" < in > out 2> stderr.log) &
    soak_record "$dir/host.proc" $! "$HOST_BIN"
    # The writer that holds stdin open: the configuration line, then a sleep.
    (printf '%s\n' "$config"; exec setsid sleep 2147483647) > "$dir/in" &
    soak_record "$dir/holder.proc" $! "$SLEEP_BIN"
    setsid bash "$0" --log-filter "$dir" < "$dir/out" >> "$dir/stdout.log" 2>&1 &
    soak_record "$dir/filter.proc" $! "$BASH_BIN"
    # All three meet at the FIFOs and exec; until then the records do not match.
    for role in host holder filter; do
        if ! soak_settle "$dir/$role.proc"; then
            echo "room $number: the $role did not start; see $dir/stdout.log and $dir/stderr.log" >&2
            "$HERE/soak-stop.sh" 5 >&2 || true
            exit 1
        fi
    done
    printf '%s %s %s\n' "$number" "$port" "$room_id" >> "$RUN/hosts.txt.new"
done
mv "$RUN/hosts.txt.new" "$RUN/hosts.txt"

# Each host reports `hosted` about a second after it starts.
for ((index = 0; index < COUNT; index++)); do
    number=$((FIRST_ROOM + index))
    dir="$RUN/room-$number"
    for _ in $(seq 1 120); do
        [ -s "$dir/invitation" ] && break
        soak_owns "$dir/host.proc" || break
        sleep 0.5
    done
    if [ ! -s "$dir/invitation" ]; then
        echo "room host $number did not report hosted; see $dir/stdout.log and $dir/stderr.log" >&2
        "$HERE/soak-stop.sh" 5 >&2 || true
        exit 1
    fi
done
: > "$RUN/invitations.txt"
: > "$RUN/invitations-numbered.txt"
for ((index = 0; index < COUNT; index++)); do
    number=$((FIRST_ROOM + index))
    cat "$RUN/room-$number/invitation" >> "$RUN/invitations.txt"
    printf '%s=%s\n' "$number" "$(cat "$RUN/room-$number/invitation")" >> "$RUN/invitations-numbered.txt"
done
echo "started $COUNT room host(s); invitations in $RUN/invitations.txt"
echo "ports $FIRST_PORT to $((FIRST_PORT + 2 * COUNT - 1)) (UDP), rooms $FIRST_ROOM to $((FIRST_ROOM + COUNT - 1))"
