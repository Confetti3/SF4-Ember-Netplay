#!/usr/bin/env bash
# Exercises setup.sh in a mock deployment: no root, no systemd, no network.
# Run as: bash test-setup.sh
#
# Every server path lives under a temporary folder (EMBER_ROOMS_ROOT), and
# stand-ins for systemctl, curl, ufw, sleep and systemd-analyze come first on
# PATH. The fake systemctl "starts" a supervisor depending on a marker inside
# the installed binary: BADSTART makes the restart fail, BADHEALTH makes the
# restart succeed but leaves /health unanswered. LIMITS marks a supervisor
# that reloads and answers GET /limits with the config.json it last read;
# BADRELOAD makes it ignore a reload.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export TMPDIR=$T/tmp
mkdir -p "$TMPDIR" "$T/stubs"

cat > "$T/stubs/systemctl" <<'EOF'
#!/usr/bin/env bash
echo "systemctl $*" >> "$STUB_LOG"
bin=$EMBER_ROOMS_ROOT/usr/local/lib/ember-rooms/ember-rooms
config=$EMBER_ROOMS_ROOT/etc/ember-rooms/config.json
# What the running supervisor read: its binary's markers, the config and a
# reload count.
loaded() {
    python3 - "$config" "$STUB_STATE.loaded" "$1" "$2" <<'PY'
import json, os, sys
config, out, markers, reloads = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
c = json.load(open(config)) if os.path.exists(config) else {}
# A reload keeps the settings only a restart changes.
started = json.load(open(out)) if reloads and os.path.exists(out) else c
json.dump({"markers": markers, "max_rooms": c.get("max_rooms", 8),
           "port_range": c.get("port_range", [45800, 45899]),
           "builds": sorted(c.get("builds", {})), "rooms": 0, "reloads": reloads,
           "bind": started.get("bind", "127.0.0.1:47830"), "secret_file": started.get("secret_file", ""),
           "empty_close_secs": started.get("empty_close_secs", 120),
           "drain_secs": started.get("drain_secs", 600)}, open(out, "w"))
PY
}
case "$1" in
    restart)
        [ "${STUB_NO_RECOVER:-0}" = 1 ] && { echo down > "$STUB_STATE"; exit 0; }
        grep -q BADSTART "$bin" 2>/dev/null && { echo down > "$STUB_STATE"; exit 1; }
        if grep -q BADHEALTH "$bin" 2>/dev/null; then echo down > "$STUB_STATE"; else echo up > "$STUB_STATE"; fi
        loaded "$(grep -o 'LIMITS\|BADRELOAD' "$bin" 2>/dev/null | tr '\n' ' ')" 0
        ;;
    reload)
        [ "$(cat "$STUB_STATE" 2>/dev/null)" = up ] || exit 1
        markers=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["markers"])' "$STUB_STATE.loaded")
        case "$markers" in *BADRELOAD*) exit 0 ;; esac
        count=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["reloads"])' "$STUB_STATE.loaded")
        loaded "$markers" $((count + 1))
        ;;
    stop) echo down > "$STUB_STATE" ;;
    is-active)
        if [ "$(cat "$STUB_STATE" 2>/dev/null)" = up ]; then
            [ "${2:-}" = --quiet ] || echo active
            exit 0
        fi
        [ "${2:-}" = --quiet ] || echo inactive
        exit 3
        ;;
esac
exit 0
EOF
cat > "$T/stubs/curl" <<'EOF'
#!/usr/bin/env bash
cat > /dev/null
for last in "$@"; do :; done
[ "$(cat "$STUB_STATE" 2>/dev/null)" = up ] || exit 7
case "$last" in
    */health) echo ok ;;
    */rooms) echo '[]' ;;
    */limits)
        grep -q LIMITS "$STUB_STATE.loaded" 2>/dev/null || exit 22
        python3 -c 'import json, sys; l = json.load(open(sys.argv[1])); l.pop("markers"); print(json.dumps(l))' "$STUB_STATE.loaded"
        ;;
esac
EOF
printf '#!/bin/sh\nexit 0\n' > "$T/stubs/sleep"
printf '#!/bin/sh\nexit 0\n' > "$T/stubs/systemd-analyze"
printf '#!/bin/sh\necho "Status: inactive"\n' > "$T/stubs/ufw"
chmod +x "$T"/stubs/*
export PATH="$T/stubs:$PATH"

pass=0
fail=0
check() {
    local label=$1
    shift
    if "$@"; then
        pass=$((pass + 1))
        echo "  ok   $label"
    else
        fail=$((fail + 1))
        echo "  FAIL $label"
    fi
}
same() { cmp -s "$1" "$2"; }
absent() { [ ! -e "$1" ]; }
mentions() { grep -q -- "$1" "$OUT"; }
failed() { [ "$RC" -ne 0 ]; }
succeeded() { [ "$RC" -eq 0 ]; }

# A fresh server root with a staging folder next to its own copy of setup.sh.
new_env() {
    export EMBER_ROOMS_ROOT=$T/$1/root
    export STUB_STATE=$T/$1/state
    export STUB_LOG=$T/$1/systemctl.log
    STAGE=$T/$1/stage
    mkdir -p "$STAGE/bin" "$EMBER_ROOMS_ROOT"
    cp "$HERE/setup.sh" "$HERE/ember-rooms.service" "$STAGE/"
    sed "s#/etc/ember-rooms/supervisor.secret#$EMBER_ROOMS_ROOT/etc/ember-rooms/supervisor.secret#" \
        "$HERE/config.example.json" > "$STAGE/config.example.json"
    LIB=$EMBER_ROOMS_ROOT/usr/local/lib/ember-rooms
    CONFIG=$EMBER_ROOMS_ROOT/etc/ember-rooms/config.json
    UNIT=$EMBER_ROOMS_ROOT/etc/systemd/system/ember-rooms.service
    : > "$STUB_LOG"
    OUT=$T/$1/out.txt
}
stage_supervisor() { printf '\177ELF supervisor %s\n' "$1" > "$STAGE/bin/ember-rooms"; }
stage_build() {
    mkdir -p "$STAGE/builds/$1"
    printf '\177ELF host %s\n' "$2" > "$STAGE/builds/$1/sf4e-room-host"
    printf '\177ELF helper %s\n' "$2" > "$STAGE/builds/$1/sf4-net"
}
run_setup() {
    mkdir -p "$EMBER_ROOMS_ROOT/etc/systemd/system"
    bash "$STAGE/setup.sh" "$@" > "$OUT" 2>&1
    RC=$?
}
snapshot() {
    mkdir -p "$T/$1"
    cp -a "$LIB/ember-rooms" "$T/$1/exec"
    cp -a "$UNIT" "$T/$1/unit"
    cp -a "$CONFIG" "$T/$1/config"
}
service_up() { [ "$(cat "$STUB_STATE" 2>/dev/null)" = up ]; }
config_lacks() { ! config_lists "$1"; }
service_down() { ! service_up; }
config_lists() { python3 -c 'import json, sys; sys.exit(0 if sys.argv[2] in json.load(open(sys.argv[1]))["builds"] else 1)' "$CONFIG" "$1"; }

echo "1. first install"
new_env a
stage_supervisor v1
stage_build aaa111 one
run_setup
check "exits 0" succeeded
check "supervisor installed" same "$STAGE/bin/ember-rooms" "$LIB/ember-rooms"
check "unit installed" same "$STAGE/ember-rooms.service" "$UNIT"
check "build installed" same "$STAGE/builds/aaa111/sf4-net" "$LIB/builds/aaa111/sf4-net"
check "config lists the build" config_lists aaa111
check "service is up" service_up
snapshot snap1

echo "2. second run with nothing new"
build_stamp=$(stat -c %Y "$LIB/builds/aaa111/sf4-net")
run_setup
check "exits 0" succeeded
check "supervisor unchanged" same "$T/snap1/exec" "$LIB/ember-rooms"
check "unit unchanged" same "$T/snap1/unit" "$UNIT"
check "config unchanged" same "$T/snap1/config" "$CONFIG"
check "build file untouched" test "$build_stamp" = "$(stat -c %Y "$LIB/builds/aaa111/sf4-net")"
check "says the build is unchanged" mentions "already installed, unchanged"

echo "3. different bytes under an installed build id"
stage_supervisor v2
printf '\177ELF helper changed\n' > "$STAGE/builds/aaa111/sf4-net"
run_setup
check "refused" failed
check "message names the build" mentions "builds/aaa111/sf4-net differs"
check "installed build kept" test "$(cat "$LIB/builds/aaa111/sf4-net")" != "$(cat "$STAGE/builds/aaa111/sf4-net")"
check "supervisor not replaced" same "$T/snap1/exec" "$LIB/ember-rooms"
check "service never restarted" test "$(grep -c restart "$STUB_LOG")" = 2
stage_build aaa111 one

echo "4. new supervisor answers nothing on /health"
stage_supervisor "v2 BADHEALTH"
stage_build bbb222 two
{ cat "$HERE/ember-rooms.service"; echo "# changed"; } > "$STAGE/ember-rooms.service"
run_setup
check "fails" failed
check "says the previous installation was restored" mentions "Restored the previous supervisor binary, unit and config.json"
check "supervisor back byte for byte" same "$T/snap1/exec" "$LIB/ember-rooms"
check "unit back byte for byte" same "$T/snap1/unit" "$UNIT"
check "config back byte for byte" same "$T/snap1/config" "$CONFIG"
check "half-added build removed" absent "$LIB/builds/bbb222"
check "existing build intact" same "$STAGE/builds/aaa111/sf4-net" "$LIB/builds/aaa111/sf4-net"
check "no temporary build folders left" test -z "$(ls -A "$LIB/builds" | grep '^\.new\.')"
check "previous service is up again" service_up
check "no leftover .restore or .new files" test -z "$(find "$EMBER_ROOMS_ROOT" -name '*.restore' -o -name '*.new')"

echo "5. new supervisor fails to restart"
stage_supervisor "v2 BADSTART"
run_setup
check "fails" failed
check "supervisor back byte for byte" same "$T/snap1/exec" "$LIB/ember-rooms"
check "unit back byte for byte" same "$T/snap1/unit" "$UNIT"
check "config back byte for byte" same "$T/snap1/config" "$CONFIG"
check "half-added build removed" absent "$LIB/builds/bbb222"
check "previous service is up again" service_up

echo "6. --prune that fails keeps the pruned build"
rm -rf "$STAGE/builds/aaa111"
stage_supervisor "v2 BADHEALTH"
run_setup --prune
check "fails" failed
check "pruned build files still there" test -f "$LIB/builds/aaa111/sf4-net"
check "config back byte for byte" same "$T/snap1/config" "$CONFIG"
stage_build aaa111 one

echo "7. restoring fails as well"
STUB_NO_RECOVER=1 run_setup
check "fails" failed
check "says restoring failed" mentions "RESTORING FAILED"
check "files are still put back" same "$T/snap1/exec" "$LIB/ember-rooms"
check "service is down" service_down
check "bundle kept for the operator" test -n "$(ls "$TMPDIR")"
rm -rf "${TMPDIR:?}"/*
unset STUB_NO_RECOVER

echo "8. a good upgrade after the failures"
stage_supervisor v3
run_setup
check "exits 0" succeeded
check "supervisor replaced" same "$STAGE/bin/ember-rooms" "$LIB/ember-rooms"
check "unit replaced" same "$STAGE/ember-rooms.service" "$UNIT"
check "new build installed" same "$STAGE/builds/bbb222/sf4-net" "$LIB/builds/bbb222/sf4-net"
check "config lists the first build" config_lists aaa111
check "config lists the new build" config_lists bbb222
check "service is up" service_up
check "bundle folder removed" test -z "$(ls "$TMPDIR")"

echo "9. --prune that succeeds removes the build"
rm -rf "$STAGE/builds/aaa111"
run_setup --prune
check "exits 0" succeeded
check "pruned build files removed" absent "$LIB/builds/aaa111"
check "config no longer lists it" config_lacks aaa111

echo "10. --max-rooms sets the room limit"
config_max() { python3 -c 'import json, sys; sys.exit(0 if json.load(open(sys.argv[1]))["max_rooms"] == int(sys.argv[2]) else 1)' "$CONFIG" "$1"; }
run_setup --max-rooms 31
check "exits 0" succeeded
check "max_rooms is 31" config_max 31
check "says what changed" mentions "max_rooms 10 -> 31"
check "warns that MemoryMax is too small" mentions "needs about 3100M of memory"
check "restarts a supervisor that cannot reload" mentions "does not answer GET /limits"
check "config lists the new build" config_lists bbb222
snapshot snap10

echo "11. --max-rooms past the port range is refused before any change"
restarts=$(grep -c restart "$STUB_LOG")
run_setup --max-rooms 51
check "fails" failed
check "names the port range" mentions "needs 102 ports but port_range 45800-45899 has 100"
check "config unchanged" same "$T/snap10/config" "$CONFIG"
check "service never restarted" test "$(grep -c restart "$STUB_LOG")" = "$restarts"
check "service still up" service_up
run_setup --max-rooms 0
check "zero refused" failed
check "usage error, nothing touched" same "$T/snap10/config" "$CONFIG"
run_setup --max-rooms
check "missing value refused" failed

echo "12. a failure before the service is touched leaves it alone"
restarts=$(grep -c restart "$STUB_LOG")
python3 - "$CONFIG" <<'PY'
import json, sys
config = json.load(open(sys.argv[1]))
config["secret_file"] = "/somewhere/else"
json.dump(config, open(sys.argv[1], "w"))
PY
cp -a "$CONFIG" "$T/wrong-secret-config"
run_setup
check "fails" failed
check "names the secret file" mentions "names secret_file /somewhere/else"
check "service never restarted" test "$(grep -c restart "$STUB_LOG")" = "$restarts"
check "says no room was touched" mentions "neither reloaded nor restarted"
check "config back byte for byte" same "$T/wrong-secret-config" "$CONFIG"
cp -a "$T/snap10/config" "$CONFIG"

echo "13. first install that fails leaves nothing behind"
new_env b
stage_supervisor "v1 BADHEALTH"
stage_build ccc333 three
run_setup
check "fails" failed
check "says there is nothing to restore" mentions "first install, so there is nothing to restore"
check "no supervisor" absent "$LIB/ember-rooms"
check "no unit" absent "$UNIT"
check "no config" absent "$CONFIG"
check "no build" absent "$LIB/builds/ccc333"
check "service stopped" service_down

echo "14. a supervisor that reloads takes a new limit without a restart"
new_env c
limits_say() { python3 -c 'import json, sys; l = json.load(open(sys.argv[1])); sys.exit(0 if (l["max_rooms"], l["builds"], l["reloads"]) == (int(sys.argv[2]), sys.argv[3].split(","), int(sys.argv[4])) else 1)' "$STUB_STATE.loaded" "$@"; }
restarts_are() { test "$(grep -c 'systemctl restart' "$STUB_LOG")" = "$1"; }
reloads_are() { test "$(grep -c 'systemctl reload' "$STUB_LOG")" = "$1"; }
stage_supervisor "v1 LIMITS"
stage_build ddd444 four
run_setup
check "first install exits 0" succeeded
check "first install restarts" restarts_are 1
run_setup --max-rooms 30
check "exits 0" succeeded
check "says it reloads" mentions "apply: reload (rooms keep running)"
check "never restarted again" restarts_are 1
check "reloaded once" reloads_are 1
check "max_rooms is 30" config_max 30
check "the supervisor runs with 30" limits_say 30 ddd444 1
check "reports what runs" mentions "reload: the supervisor runs with max_rooms 30"
snapshot snap14

echo "15. a new build is added by a reload"
stage_build eee555 five
run_setup
check "exits 0" succeeded
check "still no restart" restarts_are 1
check "the supervisor lists both builds" limits_say 30 ddd444,eee555 2

echo "16. new resource limits in the unit are set on the running service"
sed 's/^MemoryMax=.*/MemoryMax=3100M/' "$HERE/ember-rooms.service" > "$STAGE/ember-rooms.service"
run_setup
check "exits 0" succeeded
check "still no restart" restarts_are 1
check "set on the running service" grep -q "set-property --runtime ember-rooms MemoryMax=3100M" "$STUB_LOG"
check "unit installed" same "$STAGE/ember-rooms.service" "$UNIT"

echo "17. any other unit change restarts"
{ cat "$STAGE/ember-rooms.service"; echo "Nice=1"; } > "$STAGE/ember-rooms.service.new"
mv "$STAGE/ember-rooms.service.new" "$STAGE/ember-rooms.service"
run_setup
check "exits 0" succeeded
check "restarted" restarts_are 2
check "says why" mentions "the unit changed beyond its resource limits"
run_setup --restart
check "--restart restarts" restarts_are 3
check "says why" mentions "--restart was given"

echo "18. a reload the supervisor does not take is undone by another reload"
stage_supervisor "v2 LIMITS BADRELOAD"
run_setup
check "new binary restarts" restarts_are 4
snapshot snap18
restarts=$(grep -c 'systemctl restart' "$STUB_LOG")
run_setup --max-rooms 12
check "fails" failed
check "says it did not take" mentions "did not take the new config.json"
check "never restarted" restarts_are "$restarts"
check "config back byte for byte" same "$T/snap18/config" "$CONFIG"
check "service still up" service_up

echo "19. a restart-only setting forces a restart"
stage_supervisor "v3 LIMITS"
run_setup
python3 - "$CONFIG" <<'PY'
import json, sys
config = json.load(open(sys.argv[1]))
config["drain_secs"] = 300
json.dump(config, open(sys.argv[1], "w"))
PY
restarts=$(grep -c 'systemctl restart' "$STUB_LOG")
run_setup
check "exits 0" succeeded
check "restarted" restarts_are $((restarts + 1))
check "says why" mentions "a setting only a restart applies"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
