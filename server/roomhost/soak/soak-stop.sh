#!/usr/bin/env bash
# Ends the soak's room hosts: closes each host's stdin (the order to close the
# room and exit), waits for them, then ends stragglers.
#
#   soak-stop.sh [grace-seconds]
#
# Default grace is 15 s. Safe to run twice. Only processes recorded under
# $SOAK_DIR (default /tmp/sf4-soak) are touched, and only while their record
# still names them (soak-lib.sh): a pid the system has since given to another
# process is left alone. The live bridge and the supervisor's own room hosts
# are never signalled. The records of processes that are gone are removed.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=soak-lib.sh
. "$HERE/soak-lib.sh"
RUN="${SOAK_DIR:-/tmp/sf4-soak}"
GRACE="${1:-15}"
shopt -s nullglob
say() { printf '%s soak-stop: %s\n' "$(date -u +%FT%TZ)" "$*" | tee -a "$RUN/stop.log"; }

mkdir -p "$RUN"
# True when the record names a running soak process (SOAK_PID). A record whose
# pid now belongs to another process is reported and left for the cleanup.
owned() {
    soak_owns "$1" && return 0
    local pid _
    { read -r pid _ < "$1"; } 2>/dev/null || return 1
    if [[ $pid =~ ^[0-9]+$ ]] && [ -e "/proc/$pid" ]; then
        say "record $1: pid $pid is another process now; not signalling it"
    fi
    return 1
}

hosts=()
for dir in "$RUN"/room-*; do
    owned "$dir/host.proc" && hosts+=("$dir/host.proc")
    # Ending the writer closes the FIFO: the host reads end of input.
    owned "$dir/holder.proc" && kill "$SOAK_PID" 2>/dev/null
done
say "closed stdin of ${#hosts[@]} host(s); waiting up to ${GRACE}s"

deadline=$((SECONDS + GRACE))
while [ $SECONDS -lt $deadline ]; do
    left=0
    for record in "${hosts[@]}"; do soak_owns "$record" && left=$((left + 1)); done
    [ "$left" -eq 0 ] && break
    sleep 0.5
done

stragglers=0
for record in "${hosts[@]}"; do
    if soak_owns "$record"; then
        stragglers=$((stragglers + 1))
        say "host $SOAK_PID still running; sending TERM to it and its helper"
        children="$(pgrep -P "$SOAK_PID" || true)"
        # shellcheck disable=SC2086
        kill -TERM $children "$SOAK_PID" 2>/dev/null
    fi
done
if [ "$stragglers" -gt 0 ]; then
    sleep 3
    for record in "${hosts[@]}"; do
        if soak_owns "$record"; then
            say "host $SOAK_PID did not exit; killing it"
            # shellcheck disable=SC2046
            kill -KILL $(pgrep -P "$SOAK_PID" || true) "$SOAK_PID" 2>/dev/null
        fi
    done
fi
# The log filters end with their host's stdout; end any left over.
for dir in "$RUN"/room-*; do
    owned "$dir/filter.proc" && kill "$SOAK_PID" 2>/dev/null
done
sleep 0.5

# Remove the records of processes that are gone. One that survived KILL keeps
# its record, so another run can retry. Bare pid files from older scripts were
# never signalled; they go too.
kept=0
for record in "$RUN"/room-*/*.proc; do
    if soak_owns "$record"; then kept=$((kept + 1)); else rm -f "$record"; fi
done
rm -f "$RUN"/room-*/*.pid
say "done: ${#hosts[@]} host(s), $stragglers needed more than a closed stdin, $kept record(s) still running"
exit 0
