# Sourced by the soak scripts, not run. Every process the soak starts is
# recorded in a file of its own (room-N/host.proc, holder.proc, filter.proc) as
# one line:
#
#   <pid> <start time> <boot id> <executable>
#
# The start time is field 22 of /proc/<pid>/stat (clock ticks since boot) and
# the boot id is /proc/sys/kernel/random/boot_id, so a pid that the system has
# given to another process since, before or after a reboot, never matches. The
# scripts signal or count a recorded pid only when soak_owns says it is still
# the process that was recorded.

SOAK_BOOT_ID="$(cat /proc/sys/kernel/random/boot_id 2>/dev/null || echo unknown)"

# Sets SOAK_START to the start time of a pid; false when it is gone.
soak_start_time() {
    SOAK_START=""
    local line rest fields
    { read -r line < "/proc/$1/stat"; } 2>/dev/null || return 1
    # The command name is in parentheses and may hold spaces: split after it.
    rest="${line##*) }"
    # shellcheck disable=SC2206
    fields=($rest)
    SOAK_START="${fields[19]:-}"
    [ -n "$SOAK_START" ]
}

# Sets SOAK_EXE to the executable of a pid, without the " (deleted)" the kernel
# appends when the file was replaced while the process runs.
soak_exe() {
    SOAK_EXE=""
    local exe
    exe="$(readlink "/proc/$1/exe" 2>/dev/null)" || return 1
    SOAK_EXE="${exe% (deleted)}"
}

# soak_record <file> <pid> <executable>
# Records a process right after it was started. The executable is the one it
# will run: it starts as a shell that waits for its FIFOs and then execs, and
# an exec keeps the pid and the start time.
soak_record() {
    local start=0
    soak_start_time "$2" && start="$SOAK_START"
    printf '%s %s %s %s\n' "$2" "$start" "$SOAK_BOOT_ID" "$3" > "$1"
}

# soak_owns <file> [started]
# True when the record still names its process: the same pid, boot and start
# time, running the recorded executable. With `started` the executable is not
# compared (the process may not have reached its exec yet). Sets SOAK_PID.
soak_owns() {
    SOAK_PID=""
    local pid start boot exe
    { read -r pid start boot exe < "$1"; } 2>/dev/null || return 1
    [[ ${pid:-} =~ ^[0-9]+$ && ${start:-} =~ ^[0-9]+$ ]] || return 1
    [ "${boot:-}" = "$SOAK_BOOT_ID" ] || return 1
    soak_start_time "$pid" || return 1
    [ "$SOAK_START" = "$start" ] || return 1
    if [ "${2:-}" != started ]; then
        soak_exe "$pid" || return 1
        [ "$SOAK_EXE" = "$exe" ] || return 1
    fi
    SOAK_PID="$pid"
}

# soak_settle <file>: waits up to 10 s for a just started process to reach its
# exec, so that soak_owns holds. False when it ended or never got there.
soak_settle() {
    local _
    for _ in $(seq 1 100); do
        soak_owns "$1" && return 0
        soak_owns "$1" started || return 1
        sleep 0.1
    done
    return 1
}
