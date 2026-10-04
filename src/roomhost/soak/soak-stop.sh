#!/usr/bin/env bash
# Ends the soak's room hosts: closes each host's stdin (the order to close the
# room and exit), waits for them, then ends stragglers.
#
#   soak-stop.sh [grace-seconds]
#
# Default grace is 15 s. Safe to run twice. Only processes recorded under
# $SOAK_DIR (default /tmp/sf4-soak) are touched; the live bridge and the
# supervisor's own room hosts are not.
set -uo pipefail

RUN="${SOAK_DIR:-/tmp/sf4-soak}"
GRACE="${1:-15}"
shopt -s nullglob
say() { printf '%s soak-stop: %s\n' "$(date -u +%FT%TZ)" "$*" | tee -a "$RUN/stop.log"; }

alive() { [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null; }
pid_of() { [ -f "$1" ] && cat "$1" || true; }

mkdir -p "$RUN"
hosts=()
for dir in "$RUN"/room-*; do
    host="$(pid_of "$dir/host.pid")"
    holder="$(pid_of "$dir/holder.pid")"
    alive "$host" && hosts+=("$host")
    # Ending the writer closes the FIFO: the host reads end of input.
    alive "$holder" && kill "$holder" 2>/dev/null
done
say "closed stdin of ${#hosts[@]} host(s); waiting up to ${GRACE}s"

deadline=$((SECONDS + GRACE))
while [ $SECONDS -lt $deadline ]; do
    left=0
    for host in "${hosts[@]}"; do alive "$host" && left=$((left + 1)); done
    [ "$left" -eq 0 ] && break
    sleep 0.5
done

stragglers=0
for host in "${hosts[@]}"; do
    if alive "$host"; then
        stragglers=$((stragglers + 1))
        say "host $host still running; sending TERM to it and its helper"
        children="$(pgrep -P "$host" || true)"
        kill -TERM $children "$host" 2>/dev/null
    fi
done
if [ "$stragglers" -gt 0 ]; then
    sleep 3
    for host in "${hosts[@]}"; do
        if alive "$host"; then
            say "host $host did not exit; killing it"
            kill -KILL $(pgrep -P "$host" || true) "$host" 2>/dev/null
        fi
    done
fi
# The log filters end with their host's stdout; remove any left over.
for dir in "$RUN"/room-*; do
    filter="$(pid_of "$dir/filter.pid")"
    alive "$filter" && kill "$filter" 2>/dev/null
done
say "done: ${#hosts[@]} host(s), $stragglers needed more than a closed stdin"
exit 0
