#!/usr/bin/env bash
# Samples the server while the soak runs, and guards it.
#
#   soak-monitor.sh <out.csv> [interval-seconds]
#
# Every interval (default 30 s) one row is appended to <out.csv>: time, memory
# (MemTotal, MemAvailable, swap), load averages, resident memory and CPU of the
# soak's room hosts and of their sf4-net helpers (summed), of the live bridge,
# supervisor and short-link service (ember-bridge, ember-rooms, ember-short;
# found by name), the main interface's byte counters, and UDP socket counts.
# <out.csv>.rooms.csv has the same time with one row per room: that room's host
# and helper memory, CPU and open files. Only processes recorded by
# soak-hosts.sh under $SOAK_DIR (default /tmp/sf4-soak), and only while their
# record still names them (soak-lib.sh), count as the soak's, so neither the
# supervisor's own room hosts nor a process that reused a pid are mixed in. CPU is a percentage of one
# core over the interval. Needs no root.
#
# The watchdog checks every 5 s. If MemAvailable drops below SOAK_MIN_AVAIL_MB
# (250) or swap in use grows by more than SOAK_MAX_SWAP_GROWTH_MB (300) beyond
# what it was when the monitor started, it runs soak-stop.sh and writes the
# reason to <out.csv>.watchdog.log. SOAK_WATCHDOG=0 turns that off. The monitor
# exits once the soak's hosts are gone.
set -uo pipefail

if [ $# -lt 1 ]; then echo "usage: $0 <out.csv> [interval-seconds]" >&2; exit 2; fi
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=soak-lib.sh
. "$HERE/soak-lib.sh"
OUT="$1"
INTERVAL="${2:-30}"
RUN="${SOAK_DIR:-/tmp/sf4-soak}"
MIN_AVAIL_KB=$(( ${SOAK_MIN_AVAIL_MB:-250} * 1024 ))
MAX_SWAP_GROWTH_KB=$(( ${SOAK_MAX_SWAP_GROWTH_MB:-300} * 1024 ))
WATCHDOG="${SOAK_WATCHDOG:-1}"
CLK_TCK="$(getconf CLK_TCK)"
PAGE_KB=$(( $(getconf PAGESIZE) / 1024 ))
shopt -s nullglob

[ -f "$RUN/soak.env" ] || { echo "no $RUN/soak.env: run soak-hosts.sh first" >&2; exit 1; }
# shellcheck disable=SC1090
. "$RUN/soak.env"
LAST_PORT=$((FIRST_PORT + 2 * COUNT - 1))

say() { printf '%s %s\n' "$(date -u +%FT%TZ)" "$*"; }

meminfo() { # sets MEM_TOTAL MEM_AVAIL SWAP_TOTAL SWAP_FREE (kB)
    local key value _
    while read -r key value _; do
        case "$key" in
            MemTotal:) MEM_TOTAL=$value ;;
            MemAvailable:) MEM_AVAIL=$value ;;
            SwapTotal:) SWAP_TOTAL=$value ;;
            SwapFree:) SWAP_FREE=$value ;;
        esac
    done < /proc/meminfo
}

# The interface with the default route, else the first that is not lo.
IFACE="$(ip -o route get 1.1.1.1 2>/dev/null | sed -n 's/.* dev \([^ ]*\).*/\1/p' | head -n 1)"
if [ -z "$IFACE" ]; then IFACE="$(sed -n 's/^ *\([^:lo ]*\):.*/\1/p' /proc/net/dev | head -n 1)"; fi

declare -A PREV_TICKS
PREV_UP=0
# Memory kB and CPU ticks of a pid: sets P_RSS P_TICKS (0 when it is gone).
# The memory is the proportional set size (Pss in smaps_rollup): the pages of
# the binary that every room host or helper shares count once in all, not once
# per process as the resident set does, so the figure is what a room costs. A
# kernel without smaps_rollup falls back to the resident set.
pstat() {
    P_RSS=0; P_TICKS=0
    local line rest fields
    { read -r line < "/proc/$1/stat"; } 2>/dev/null || return 1
    rest="${line##*) }"
    # shellcheck disable=SC2206
    fields=($rest)
    P_TICKS=$(( ${fields[11]} + ${fields[12]} ))
    local pss
    pss=$(sed -n 's/^Pss:[[:space:]]*\([0-9][0-9]*\) kB.*/\1/p' "/proc/$1/smaps_rollup" 2>/dev/null) || pss=""
    if [ -n "$pss" ]; then
        P_RSS=$pss
    else
        local pages
        { read -r _ pages _ < "/proc/$1/statm"; } 2>/dev/null || return 1
        P_RSS=$(( pages * PAGE_KB ))
    fi
}
# CPU percent (two decimals, as an integer of hundredths) since the last call for this pid.
pcpu() { # pid ticks dt_cs
    local prev="${PREV_TICKS[$1]:-$2}"
    PREV_TICKS[$1]=$2
    if [ "$3" -le 0 ]; then P_CPU=0; return; fi
    P_CPU=$(( ($2 - prev) * 1000000 / (CLK_TCK * $3) ))
}
fmt() { printf '%d.%02d' $(( $1 / 100 )) $(( $1 % 100 )); }

alive_hosts() {
    local n=0 record
    for record in "$RUN"/room-*/host.proc; do soak_owns "$record" && n=$((n + 1)); done
    echo "$n"
}

HEADER="time_utc,epoch,mem_total_kb,mem_avail_kb,swap_free_kb,swap_used_kb,load1,load5,load15,soak_hosts_alive,soak_host_rss_kb,soak_host_cpu_pct,soak_helper_rss_kb,soak_helper_cpu_pct,soak_open_fds,bridge_rss_kb,bridge_cpu_pct,rooms_rss_kb,rooms_cpu_pct,short_rss_kb,short_cpu_pct,iface,net_rx_bytes,net_tx_bytes,udp_sockets_total,udp_sockets_soak_ports"
ROOMS_HEADER="time_utc,room,host_pid,host_rss_kb,host_cpu_pct,helper_pid,helper_rss_kb,helper_cpu_pct,open_fds"
[ -s "$OUT" ] || echo "$HEADER" > "$OUT"
[ -s "$OUT.rooms.csv" ] || echo "$ROOMS_HEADER" > "$OUT.rooms.csv"

meminfo
BASE_SWAP_USED=$((SWAP_TOTAL - SWAP_FREE))
say "monitor: ${COUNT} room(s), interface ${IFACE:-none}, row every ${INTERVAL}s, watchdog ${WATCHDOG} (avail < $((MIN_AVAIL_KB / 1024)) MB, swap growth > $((MAX_SWAP_GROWTH_KB / 1024)) MB over ${BASE_SWAP_USED} kB)"

sum_named() { # name -> sets N_RSS N_CPU (hundredths), summing every process of that name
    N_RSS=0; N_CPU=0
    local pid
    for pid in $(pgrep -x "$1" 2>/dev/null); do
        pstat "$pid" || continue
        pcpu "$pid" "$P_TICKS" "$DT_CS"
        N_RSS=$((N_RSS + P_RSS)); N_CPU=$((N_CPU + P_CPU))
    done
}

sample() {
    # "baseline" only sets the CPU counters: it writes nothing.
    local out="$OUT" rooms_out="$OUT.rooms.csv"
    if [ "${1:-}" = baseline ]; then out=/dev/null; rooms_out=/dev/null; fi
    local up _ now_cs
    read -r up _ < /proc/uptime
    now_cs=$(( 10#${up%.*} * 100 + 10#${up#*.} ))
    DT_CS=$(( PREV_UP ? now_cs - PREV_UP : 0 ))
    PREV_UP=$now_cs
    local stamp epoch
    stamp="$(date -u +%FT%TZ)"; epoch="$(date -u +%s)"
    meminfo
    local l1 l5 l15
    read -r l1 l5 l15 _ < /proc/loadavg

    local host_rss=0 host_cpu=0 help_rss=0 help_cpu=0 fds=0 alive=0 dir number hostpid helper hrss hcpu hfds
    for dir in "$RUN"/room-*; do
        number="${dir##*/room-}"
        soak_owns "$dir/host.proc" || continue
        hostpid="$SOAK_PID"
        pstat "$hostpid" || continue
        pcpu "$hostpid" "$P_TICKS" "$DT_CS"
        alive=$((alive + 1))
        host_rss=$((host_rss + P_RSS)); host_cpu=$((host_cpu + P_CPU))
        local r_host_rss=$P_RSS r_host_cpu=$P_CPU
        hfds=$(ls "/proc/$hostpid/fd" 2>/dev/null | wc -l)
        hrss=0; hcpu=0; helper=""
        for helper in $(pgrep -P "$hostpid" 2>/dev/null); do
            pstat "$helper" || continue
            pcpu "$helper" "$P_TICKS" "$DT_CS"
            hrss=$((hrss + P_RSS)); hcpu=$((hcpu + P_CPU))
            hfds=$((hfds + $(ls "/proc/$helper/fd" 2>/dev/null | wc -l)))
            break
        done
        help_rss=$((help_rss + hrss)); help_cpu=$((help_cpu + hcpu)); fds=$((fds + hfds))
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$stamp" "$number" "$hostpid" "$r_host_rss" "$(fmt "$r_host_cpu")" "${helper:-0}" "$hrss" "$(fmt "$hcpu")" "$hfds" >> "$rooms_out"
    done

    sum_named ember-bridge; local b_rss=$N_RSS b_cpu=$N_CPU
    sum_named ember-rooms; local r_rss=$N_RSS r_cpu=$N_CPU
    sum_named ember-short; local s_rss=$N_RSS s_cpu=$N_CPU

    local rx=0 tx=0 line
    if [ -n "$IFACE" ]; then
        line="$(grep "^ *$IFACE:" /proc/net/dev | head -n 1)"
        # shellcheck disable=SC2086
        set -- ${line#*:}
        rx=${1:-0}; tx=${9:-0}
    fi
    local udp_total=0 udp_soak=0 local_addr port
    for file in /proc/net/udp /proc/net/udp6; do
        [ -r "$file" ] || continue
        while read -r _ local_addr _; do
            [ "$local_addr" = "local_address" ] && continue
            udp_total=$((udp_total + 1))
            port=$((16#${local_addr##*:}))
            if [ "$port" -ge "$FIRST_PORT" ] && [ "$port" -le "$LAST_PORT" ]; then udp_soak=$((udp_soak + 1)); fi
        done < "$file"
    done

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$stamp" "$epoch" "$MEM_TOTAL" "$MEM_AVAIL" "$SWAP_FREE" "$((SWAP_TOTAL - SWAP_FREE))" "$l1" "$l5" "$l15" \
        "$alive" "$host_rss" "$(fmt "$host_cpu")" "$help_rss" "$(fmt "$help_cpu")" "$fds" \
        "$b_rss" "$(fmt "$b_cpu")" "$r_rss" "$(fmt "$r_cpu")" "$s_rss" "$(fmt "$s_cpu")" \
        "${IFACE:-none}" "$rx" "$tx" "$udp_total" "$udp_soak" >> "$out"
}

TRIPPED=0
watchdog() {
    [ "$WATCHDOG" = "1" ] && [ "$TRIPPED" = "0" ] || return 0
    meminfo
    local swap_used=$((SWAP_TOTAL - SWAP_FREE)) reason=""
    if [ "$MEM_AVAIL" -lt "$MIN_AVAIL_KB" ]; then
        reason="MemAvailable is $((MEM_AVAIL / 1024)) MB, below $((MIN_AVAIL_KB / 1024)) MB"
    elif [ $((swap_used - BASE_SWAP_USED)) -gt "$MAX_SWAP_GROWTH_KB" ]; then
        reason="swap in use grew by $(( (swap_used - BASE_SWAP_USED) / 1024 )) MB, past $((MAX_SWAP_GROWTH_KB / 1024)) MB"
    fi
    [ -n "$reason" ] || return 0
    TRIPPED=1
    say "WATCHDOG: $reason; stopping the soak hosts" | tee -a "$OUT.watchdog.log"
    sample   # one last row at the moment it tripped
    "$HERE/soak-stop.sh" | tee -a "$OUT.watchdog.log"
}

sample baseline
elapsed=0
GONE=0
while true; do
    sleep 5
    elapsed=$((elapsed + 5))
    watchdog
    if [ "$elapsed" -ge "$INTERVAL" ]; then
        elapsed=0
        sample
    fi
    if [ "$(alive_hosts)" -eq 0 ]; then
        GONE=$((GONE + 1))
        if [ "$GONE" -ge 2 ]; then say "no soak host is running; monitor done"; exit 0; fi
    else
        GONE=0
    fi
done
