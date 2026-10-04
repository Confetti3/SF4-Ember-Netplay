#!/usr/bin/env bash
# Exercises setup.sh in a mock deployment: no root, no systemd, no network.
# Run as: bash test-setup.sh
#
# Every server path lives under a temporary folder (EMBER_ROOMS_ROOT), and
# stand-ins for systemctl, curl, ufw, sleep and systemd-analyze come first on
# PATH. The fake systemctl "starts" a supervisor depending on a marker inside
# the installed binary: BADSTART makes the restart fail, BADHEALTH makes the
# restart succeed but leaves /health unanswered.
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
case "$1" in
    restart)
        [ "${STUB_NO_RECOVER:-0}" = 1 ] && { echo down > "$STUB_STATE"; exit 0; }
        grep -q BADSTART "$bin" 2>/dev/null && { echo down > "$STUB_STATE"; exit 1; }
        if grep -q BADHEALTH "$bin" 2>/dev/null; then echo down > "$STUB_STATE"; else echo up > "$STUB_STATE"; fi
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

echo "10. first install that fails leaves nothing behind"
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

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
