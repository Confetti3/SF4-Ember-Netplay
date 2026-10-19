#!/usr/bin/env bash
# Exercises setup.sh, backup.sh, restore.sh and set-integration-secret.sh for
# the production and staging bridges in one mock server: no root, no
# systemd, no nginx, no network.
# Run as: bash test-setup.sh
#
# Every server path lives under a temporary folder (EMBER_BRIDGE_ROOT inside
# TMPDIR, with EMBER_BRIDGE_TEST=1), operator folders sit in HOME, and
# stand-ins for id, useradd, sudo, install, systemctl, nginx, curl and sleep
# come first on PATH. install drops the owner, sudo runs the command as the
# caller, and systemctl records which units were restarted. curl answers a
# well-known request the way the server would route it: through the enabled
# nginx site for its host, to the started unit whose bridge.json listens on
# that port. nginx -t fails while an enabled site contains BADNGINX. The
# staged ember-bridge binary is a script whose init writes a bridge.json.
# test/production/ holds production's units and site as installed before the
# templates; production's render must stay byte for byte the same.
set -u
unset INSTANCE SUDO_USER

HERE="$(cd "$(dirname "$0")" && pwd)"
GOLDEN=$HERE/test/production
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export TMPDIR=$T/tmp
export EMBER_BRIDGE_ROOT=$TMPDIR/root EMBER_BRIDGE_TEST=1
export STUB=$T/stub
R=$EMBER_BRIDGE_ROOT
export HOME=$T/ops
OPS=$HOME
mkdir -p "$TMPDIR" "$T/stubs" "$STUB/up" "$OPS"
touch "$STUB/users" "$STUB/log"
for dir in etc/systemd/system etc/nginx/conf.d etc/nginx/sites-available etc/nginx/sites-enabled \
    etc/ssl/certs etc/ssl/private usr/local/lib var/lib var/backups; do
    mkdir -p "$R/$dir"
done
touch "$R/etc/ssl/certs/embernetplay.link.fullchain.pem" "$R/etc/ssl/private/embernetplay.link.key"

cat > "$T/stubs/id" <<'EOF'
#!/usr/bin/env bash
[ "$1" = -u ] && { echo 0; exit 0; }
grep -qx -- "$1" "$STUB/users"
EOF
cat > "$T/stubs/useradd" <<'EOF'
#!/usr/bin/env bash
echo "useradd $*" >> "$STUB/log"
for name in "$@"; do :; done
echo "$name" >> "$STUB/users"
EOF
cat > "$T/stubs/sudo" <<'EOF'
#!/usr/bin/env bash
echo "sudo $1 $2 $3" >> "$STUB/log"
[ "$1" = -u ] && grep -qx -- "$2" "$STUB/users" || { echo "sudo: no account $2" >&2; exit 1; }
shift 2
exec "$@"
EOF
cat > "$T/stubs/install" <<'EOF'
#!/usr/bin/env bash
echo "install $*" >> "$STUB/log"
args=()
while [ $# -gt 0 ]; do
    case "$1" in
        -o | -g) shift 2 ;;
        *) args+=("$1"); shift ;;
    esac
done
exec /usr/bin/install "${args[@]}"
EOF
cat > "$T/stubs/systemctl" <<'EOF'
#!/usr/bin/env bash
echo "systemctl $*" >> "$STUB/log"
case "$1" in
    restart) touch "$STUB/up/$2" ;;
    is-active)
        [ "$2" = --quiet ] && shift
        [ -e "$STUB/up/$2" ] || { echo inactive; exit 3; }
        echo active
        ;;
esac
exit 0
EOF
cat > "$T/stubs/nginx" <<'EOF'
#!/usr/bin/env bash
echo "nginx $*" >> "$STUB/log"
! grep -qs BADNGINX "$EMBER_BRIDGE_ROOT"/etc/nginx/sites-enabled/*
EOF
cat > "$T/stubs/curl" <<'EOF'
#!/usr/bin/env python3
import glob, json, os, re, sys
root, stub, url = os.environ["EMBER_BRIDGE_ROOT"], os.environ["STUB"], sys.argv[-1]
loopback = re.match(r"http://127\.0\.0\.1:(\d+)/", url)
public = re.match(r"https://([^/]+)/", url)
if loopback:
    port = loopback[1]
elif public:
    site = f"{root}/etc/nginx/sites-enabled/{public[1]}"
    if not os.path.exists(site):
        sys.exit(6)
    ports = set(re.findall(r"proxy_pass http://127\.0\.0\.1:(\d+);", open(site).read()))
    if len(ports) != 1:
        sys.exit(7)
    port = ports.pop()
else:
    sys.exit(3)
for unit in glob.glob(f"{root}/etc/systemd/system/*.service"):
    name = os.path.basename(unit)[: -len(".service")]
    serve = re.search(r"^ExecStart=\S+ serve (\S+)$", open(unit).read(), re.M)
    if serve and os.path.exists(f"{stub}/up/{name}"):
        config = json.load(open(root + serve[1]))
        if config["listen"] == f"127.0.0.1:{port}":
            print(json.dumps({"origin": config["origin"], "bridge_id": config["bridge_id"]}))
            sys.exit(0)
sys.exit(7)
EOF
# getent knows one operator, katie, whose home holds the operator folders.
printf 'katie:x:1000:1000::%s:/bin/bash\n' "$HOME" > "$STUB/passwd"
cat > "$T/stubs/getent" <<'EOF'
#!/usr/bin/env bash
[ "$1" = passwd ] && grep "^$2:" "$STUB/passwd" || exit 2
EOF
printf '#!/bin/sh\nexit 0\n' > "$T/stubs/sleep"
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
mentions() { grep -qF -- "$1" "$OUT"; }
logged() { grep -qxF -- "$1" "$STUB/log"; }
nothing_logged() { [ ! -s "$STUB/log" ]; }
has_line() { grep -qxF -- "$2" "$1"; }
failed() { [ "$RC" -ne 0 ]; }
succeeded() { [ "$RC" -eq 0 ]; }
exited() { [ "$RC" -eq "$1" ]; }
absent() { [ ! -e "$1" ]; }
account() { grep -qx -- "$1" "$STUB/users"; }
# config_is <bridge.json> <key> <JSON value>
config_is() { python3 -c 'import json, sys; sys.exit(json.load(open(sys.argv[1])).get(sys.argv[2]) != json.loads(sys.argv[3]))' "$@"; }
lacks_key() { python3 -c 'import json, sys; sys.exit(sys.argv[2] in json.load(open(sys.argv[1])))' "$@"; }
connections_are() { python3 -c 'import json, sys; sys.exit(sorted(c["id"] for t in json.load(open(sys.argv[1]))["tenants"] for c in t["connections"]) != sys.argv[2:])' "$@"; }
bridge_id() { python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["bridge_id"])' "$1"; }
links_to() { [ "$(readlink "$1")" = "$2" ]; }

# An operator folder: the deploy files and a stand-in binary of that version.
stage() {
    local dir=$OPS/$1
    mkdir -p "$dir/bin"
    cp -r "$HERE"/. "$dir"/
    stage_binary "$dir" v1
}
stage_binary() {
    cat > "$1/bin/ember-bridge" <<EOF
#!/usr/bin/env bash
# stand-in ember-bridge $2
[ "\$1" = init ] || exit 1
python3 - "\$2" <<'PY'
import json, sys, uuid
json.dump({"bridge_id": f"brg_{uuid.uuid4()}", "display_name": "Local Ember bridge",
           "origin": "http://127.0.0.1:8787", "listen": "127.0.0.1:8787",
           "database": "bridge.sqlite3", "secrets": "bridge-secrets.json",
           "allow_loopback_http": True, "allow_private_webhooks": True, "mock_browser": True,
           "tenants": []}, open(f"{sys.argv[1]}/bridge.json", "w"))
open(f"{sys.argv[1]}/bridge-secrets.json", "w").write("{}")
PY
EOF
    chmod +x "$1/bin/ember-bridge"
}
# run <script> [VAR=value ...] [-- arguments]: a fresh log, the output in OUT
# and the exit status in RC. Without INSTANCE=..., INSTANCE is unset, as
# when it is forgotten or sudo drops it.
run() {
    local script=$1 vars=()
    shift
    while [ $# -gt 0 ] && [ "$1" != -- ]; do vars+=("$1"); shift; done
    [ $# -gt 0 ] && shift
    : > "$STUB/log"
    OUT=$T/out.txt
    env "${vars[@]}" bash "$script" "$@" > "$OUT" 2>&1 < "${INPUT:-/dev/null}"
    RC=$?
}
# Everything an install of <name> owns, and the shared zone, for comparing
# before and after.
snapshot() {
    local name=$1 site=$2 into=$T/snap-$3
    rm -rf "$into"
    mkdir -p "$into"
    cp -a "$R/usr/local/lib/$name" "$into/lib"
    cp -a "$R/var/lib/$name/bridge.json" "$into/bridge.json"
    cp -a "$R/etc/systemd/system/$name.service" "$R/etc/systemd/system/$name-backup.service" \
        "$R/etc/systemd/system/$name-backup.timer" "$into/"
    cp -a "$R/etc/nginx/sites-available/$site" "$into/site"
    cp -a "$R/etc/nginx/conf.d/ember-bridge.conf" "$into/zone"
    readlink "$R/etc/nginx/sites-enabled/$site" > "$into/enabled"
}
unchanged() {
    local name=$1 site=$2 from=$T/snap-$3
    diff -r "$from/lib" "$R/usr/local/lib/$name" > /dev/null &&
        same "$from/bridge.json" "$R/var/lib/$name/bridge.json" &&
        same "$from/$name.service" "$R/etc/systemd/system/$name.service" &&
        same "$from/$name-backup.service" "$R/etc/systemd/system/$name-backup.service" &&
        same "$from/$name-backup.timer" "$R/etc/systemd/system/$name-backup.timer" &&
        same "$from/site" "$R/etc/nginx/sites-available/$site" &&
        same "$from/zone" "$R/etc/nginx/conf.d/ember-bridge.conf" &&
        [ "$(cat "$from/enabled")" = "$(readlink "$R/etc/nginx/sites-enabled/$site")" ]
}
# No command this run named one of production's units.
production_untouched_by_systemctl() {
    ! grep -E '^systemctl .* ember-bridge(\.service|-backup\.(service|timer))?( |$)' "$STUB/log"
}
staging_untouched() { ! grep -q ember-bridge-staging "$STUB/log"; }
# A staging unit is production's with only the names changed.
renamed_from() {
    sed -e 's/ember-bridge-staging/ember-bridge/g' -e 's/ (staging)$//' "$2" | cmp -s - "$1"
}
# Nothing in a staging unit names production: every ember-bridge is
# ember-bridge-staging, apart from the binary's own name.
names_only_staging() {
    ! sed -e 's/ember-bridge-staging//g' -e 's#/ember-bridge serve #serve #' "$@" | grep -q ember-bridge
}
# The staging site is production's with staging's host and port.
staging_site_from() {
    sed -e 's/bridge\.embernetplay\.link/bridge-staging.embernetplay.link/g' -e 's/47820/47821/g' "$1" | cmp -s - "$2"
}
# A bridge database holding one row naming its bridge.
make_database() {
    python3 - "$1" "$2" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.execute("CREATE TABLE IF NOT EXISTS who (name TEXT)")
db.execute("DELETE FROM who")
db.execute("INSERT INTO who VALUES (?)", (sys.argv[2],))
db.commit()
PY
}
database_says() { [ "$(python3 -c 'import sqlite3, sys; print(sqlite3.connect(sys.argv[1]).execute("SELECT name FROM who").fetchone()[0])' "$1")" = "$2" ]; }
stamps() { ls "$1" | sed -n 's/^bridge-\([0-9T]*Z\)\.sqlite3$/\1/p'; }

P=ember-bridge
PSITE=bridge.embernetplay.link
PUNITS=$R/etc/systemd/system/$P
PSTATE=$R/var/lib/$P/bridge.json
PDIR=$OPS/$P
S=ember-bridge-staging
SSITE=bridge-staging.embernetplay.link
SUNITS=$R/etc/systemd/system/$S
SSTATE=$R/var/lib/$S/bridge.json
SDIR=$OPS/$S
stage "$P"
stage "$S"

echo "1. production first install"
run "$PDIR/setup.sh"
check "exits 0" succeeded
check "account ember-bridge created" account "$P"
check "binary installed" same "$PDIR/bin/ember-bridge" "$R/usr/local/lib/$P/ember-bridge"
check "instance.sh installed" same "$HERE/instance.sh" "$R/usr/local/lib/$P/instance.sh"
check "backup.sh installed" same "$HERE/backup.sh" "$R/usr/local/lib/$P/backup.sh"
check "restore.sh installed" same "$HERE/restore.sh" "$R/usr/local/lib/$P/restore.sh"
check "state and backups owned by the account" logged "install -d -o $P -g $P -m 0700 $R/var/lib/$P $R/var/backups/$P"
check "init runs as the account" logged "sudo -u $P $R/usr/local/lib/$P/ember-bridge"
check "service unit as before the templates" same "$GOLDEN/ember-bridge.service" "$PUNITS.service"
check "backup service as before" same "$GOLDEN/ember-bridge-backup.service" "$PUNITS-backup.service"
check "backup timer as before" same "$GOLDEN/ember-bridge-backup.timer" "$PUNITS-backup.timer"
check "site as before" same "$GOLDEN/$PSITE" "$R/etc/nginx/sites-available/$PSITE"
check "site enabled" links_to "$R/etc/nginx/sites-enabled/$PSITE" "$R/etc/nginx/sites-available/$PSITE"
check "zone as shipped" same "$HERE/nginx/ember-bridge-zone.conf" "$R/etc/nginx/conf.d/ember-bridge.conf"
check "origin" config_is "$PSTATE" origin '"https://bridge.embernetplay.link"'
check "listen" config_is "$PSTATE" listen '"127.0.0.1:47820"'
check "display name" config_is "$PSTATE" display_name '"Ember bridge"'
check "development switches off" config_is "$PSTATE" mock_browser false
check "units enabled" logged "systemctl enable $P $P-backup.timer"
check "service restarted" logged "systemctl restart $P"
PID=$(bridge_id "$PSTATE")
check "answers on 47820" mentions "service: ok ($PID)"
check "answers through nginx" mentions "public: ok ($PID)"
snapshot "$P" "$PSITE" production

echo "2. staging first install beside it, from its folder alone"
run "$SDIR/setup.sh"
check "exits 0" succeeded
check "account ember-bridge-staging created" account "$S"
check "own binary" same "$SDIR/bin/ember-bridge" "$R/usr/local/lib/$S/ember-bridge"
check "own backup.sh" same "$HERE/backup.sh" "$R/usr/local/lib/$S/backup.sh"
check "state and backups owned by its account" logged "install -d -o $S -g $S -m 0700 $R/var/lib/$S $R/var/backups/$S"
check "init runs as its account" logged "sudo -u $S $R/usr/local/lib/$S/ember-bridge"
check "service runs its binary on its config" has_line "$SUNITS.service" "ExecStart=/usr/local/lib/$S/ember-bridge serve /var/lib/$S/bridge.json"
check "service runs as its account" has_line "$SUNITS.service" "User=$S"
check "service group" has_line "$SUNITS.service" "Group=$S"
check "service state directory" has_line "$SUNITS.service" "StateDirectory=$S"
check "backup runs its backup.sh" has_line "$SUNITS-backup.service" "ExecStart=/usr/local/lib/$S/backup.sh"
check "backup writes only its folders" has_line "$SUNITS-backup.service" "ReadWritePaths=/var/lib/$S /var/backups/$S"
check "backup follows its service" has_line "$SUNITS-backup.service" "After=$S.service"
check "timer named staging" has_line "$SUNITS-backup.timer" "Description=Daily Ember bridge backup (staging)"
check "service unit is production's renamed" renamed_from "$GOLDEN/ember-bridge.service" "$SUNITS.service"
check "backup service is production's renamed" renamed_from "$GOLDEN/ember-bridge-backup.service" "$SUNITS-backup.service"
check "backup timer is production's renamed" renamed_from "$GOLDEN/ember-bridge-backup.timer" "$SUNITS-backup.timer"
check "no unit names production" names_only_staging "$SUNITS".service "$SUNITS"-backup.*
check "site is production's with its host and port" staging_site_from "$GOLDEN/$PSITE" "$R/etc/nginx/sites-available/$SSITE"
check "site enabled" links_to "$R/etc/nginx/sites-enabled/$SSITE" "$R/etc/nginx/sites-available/$SSITE"
check "origin" config_is "$SSTATE" origin '"https://bridge-staging.embernetplay.link"'
check "listen" config_is "$SSTATE" listen '"127.0.0.1:47821"'
check "display name" config_is "$SSTATE" display_name '"Ember bridge (staging)"'
check "no public rooms" lacks_key "$SSTATE" rooms
check "staging connections" connections_are "$SSTATE" blumint-partner-staging ember-test
check "development switches off" config_is "$SSTATE" allow_private_webhooks false
SID=$(bridge_id "$SSTATE")
check "its own bridge ID" test "$SID" != "$PID"
check "units enabled" logged "systemctl enable $S $S-backup.timer"
check "answers on 47821" mentions "service: ok ($SID)"
check "answers through nginx" mentions "public: ok ($SID)"
check "production files unchanged" unchanged "$P" "$PSITE" production
check "production units never touched" production_untouched_by_systemctl
snapshot "$S" "$SSITE" staging

echo "3. a newer staging binary, INSTANCE given and agreeing"
stage_binary "$SDIR" v2
run "$SDIR/setup.sh" INSTANCE=staging
check "exits 0" succeeded
check "binary replaced" same "$SDIR/bin/ember-bridge" "$R/usr/local/lib/$S/ember-bridge"
check "bridge ID kept" test "$(bridge_id "$SSTATE")" = "$SID"
check "account created once" test "$(grep -cx "$S" "$STUB/users")" = 1
check "production files unchanged" unchanged "$P" "$PSITE" production
check "production units never touched" production_untouched_by_systemctl
snapshot "$S" "$SSITE" staging

echo "4. INSTANCE that disagrees with the folder is refused before any change"
run "$SDIR/setup.sh" INSTANCE=
check "empty INSTANCE in staging's folder exits 2" exited 2
check "says which bridge the folder is" mentions "is the staging bridge's folder"
check "nothing ran" nothing_logged
run "$PDIR/setup.sh" INSTANCE=staging
check "INSTANCE=staging in production's folder exits 2" exited 2
check "nothing ran" nothing_logged
check "production files unchanged" unchanged "$P" "$PSITE" production
check "staging files unchanged" unchanged "$S" "$SSITE" staging

echo "5. folders that are not a bridge's are refused"
for folder in deploy ember-bridge- ember-bridge-backup ember-bridge-Staging ember-bridge-qa-old; do
    mkdir -p "$OPS/$folder"
    cp -r "$SDIR"/. "$OPS/$folder"/
    run "$OPS/$folder/setup.sh"
    check "$folder exits 2" exited 2
    check "$folder ran nothing" nothing_logged
done
mkdir -p "$OPS/ember-bridge-qa"
cp -r "$SDIR"/. "$OPS/ember-bridge-qa"/
run "$OPS/ember-bridge-qa/setup.sh"
check "an instance with no tenants file fails" failed
check "names the file" mentions "Missing $OPS/ember-bridge-qa/tenants.qa.json"
check "nothing ran" nothing_logged
mkdir -p "$SDIR/ember-bridge"
cp -r "$PDIR"/. "$SDIR/ember-bridge"/
run "$SDIR/ember-bridge/setup.sh"
check "a production folder nested in staging's exits 2" exited 2
check "says it is not in the home folder" mentions "is not directly in $OPS"
check "nothing ran" nothing_logged
run "$SDIR/ember-bridge/set-integration-secret.sh" -- discord
check "its set-integration-secret.sh exits 2" exited 2
check "nothing ran" nothing_logged
rm -rf "$SDIR/ember-bridge"
check "production files unchanged" unchanged "$P" "$PSITE" production

echo "5b. a tenants file claiming another bridge's address or port is refused"
cp "$SDIR/tenants.staging.json" "$T/staging-tenants"
sed -i 's#"https://bridge-staging.embernetplay.link"#"https://bridge.embernetplay.link"#' "$SDIR/tenants.staging.json"
run "$SDIR/setup.sh"
check "production's origin fails" failed
check "names the origin" mentions "already has origin https://bridge.embernetplay.link"
check "nothing ran" nothing_logged
cp "$T/staging-tenants" "$SDIR/tenants.staging.json"
sed -i 's#"127.0.0.1:47821"#"127.0.0.1:47820"#' "$SDIR/tenants.staging.json"
run "$SDIR/setup.sh"
check "production's port fails" failed
check "names the port" mentions "already has listen 127.0.0.1:47820"
check "nothing ran" nothing_logged
cp "$T/staging-tenants" "$SDIR/tenants.staging.json"
check "production files unchanged" unchanged "$P" "$PSITE" production
check "staging files unchanged" unchanged "$S" "$SSITE" staging

echo "5c. the test root is honoured only in a test"
run "$SDIR/setup.sh" EMBER_BRIDGE_TEST=
check "without EMBER_BRIDGE_TEST it exits 2" exited 2
check "says it is for the test" mentions "EMBER_BRIDGE_ROOT is only for test-setup.sh"
check "nothing ran" nothing_logged
run "$R/usr/local/lib/$S/backup.sh" EMBER_BRIDGE_TEST=
check "backup.sh without it exits 2" exited 2
run "$SDIR/setup.sh" "EMBER_BRIDGE_ROOT=$T/elsewhere"
check "a root outside TMPDIR exits 2" exited 2
check "nothing ran" nothing_logged

echo "6. a staging site nginx rejects"
cp "$SDIR/nginx/bridge.conf.in" "$T/site-template"
echo "# BADNGINX" >> "$SDIR/nginx/bridge.conf.in"
run "$SDIR/setup.sh"
check "fails" failed
check "says it restored nginx" mentions "restored the previous files"
check "staging site back byte for byte" same "$T/snap-staging/site" "$R/etc/nginx/sites-available/$SSITE"
check "staging still enabled" links_to "$R/etc/nginx/sites-enabled/$SSITE" "$R/etc/nginx/sites-available/$SSITE"
check "nothing restarted" bash -c '! grep -q "^systemctl restart" "$1"' _ "$STUB/log"
check "production files unchanged" unchanged "$P" "$PSITE" production
cp "$T/site-template" "$SDIR/nginx/bridge.conf.in"

echo "7. staging never replaces production's request limits"
cp "$SDIR/nginx/ember-bridge-zone.conf" "$T/zone"
sed -i 's/rate=20r\/s/rate=200r\/s/' "$SDIR/nginx/ember-bridge-zone.conf"
run "$SDIR/setup.sh"
check "a different zone file is refused" failed
check "says production's setup owns it" mentions "install it with production's setup.sh first"
check "nothing ran" nothing_logged
check "production files and zone unchanged" unchanged "$P" "$PSITE" production
cp "$T/zone" "$SDIR/nginx/ember-bridge-zone.conf"

echo "8. backups and restores stay with their own bridge"
make_database "$R/var/lib/$P/bridge.sqlite3" production
make_database "$R/var/lib/$S/bridge.sqlite3" staging
run "$R/usr/local/lib/$P/backup.sh"
check "production backup exits 0" succeeded
# A stamp only production has, whatever second the two backups ran in.
pstamp=$(stamps "$R/var/backups/$P")
for file in "$R/var/backups/$P"/*"$pstamp"*; do
    mv "$file" "${file/$pstamp/20200101T000000Z}"
done
run "$R/usr/local/lib/$S/backup.sh"
check "staging backup exits 0" succeeded
sstamp=$(stamps "$R/var/backups/$S")
check "staging backed up its own database" database_says "$R/var/backups/$S/bridge-$sstamp.sqlite3" staging
check "production's backups hold only production's set" test "$(stamps "$R/var/backups/$P")" = 20200101T000000Z
run "$R/usr/local/lib/$S/backup.sh" INSTANCE=
check "backup.sh with a disagreeing INSTANCE exits 2" exited 2
run "$R/usr/local/lib/$S/restore.sh" -- 20200101T000000Z
check "staging's restore.sh refuses production's stamp" exited 3
check "says nothing was stopped" mentions "ember-bridge-staging was not stopped"
check "nothing ran" nothing_logged
make_database "$R/var/lib/$S/bridge.sqlite3" "staging, later"
run "$R/usr/local/lib/$S/restore.sh" -- "$sstamp"
check "staging restores its own stamp" succeeded
check "its database is back" database_says "$R/var/lib/$S/bridge.sqlite3" staging
check "the newer one is kept aside" bash -c 'ls -d "$1"/recovery-* > /dev/null' _ "$R/var/lib/$S"
check "stops and starts only staging" logged "systemctl stop $S-backup.service $S"
check "production units never touched" production_untouched_by_systemctl
check "production's database unchanged" database_says "$R/var/lib/$P/bridge.sqlite3" production

echo "9. integration secrets go to the folder's bridge"
INPUT=$T/secret
printf 'staging-secret\n' > "$INPUT"
run "$SDIR/set-integration-secret.sh" -- discord
check "staging folder exits 0" succeeded
check "stored for staging" config_is "$R/var/lib/$S/integrations.json" discord_client_secret '"staging-secret"'
check "restarts staging" logged "systemctl restart $S"
check "production has no integrations.json" absent "$R/var/lib/$P/integrations.json"
check "production units never touched" production_untouched_by_systemctl
printf 'startgg-secret\n' > "$INPUT"
run "$SDIR/set-integration-secret.sh" -- site startgg
check "a site's exits 0" succeeded
check "a site's stored for staging" python3 -c 'import json, sys; sys.exit(json.load(open(sys.argv[1]))["site_client_secrets"] != {"startgg": "startgg-secret"})' "$R/var/lib/$S/integrations.json"
run "$SDIR/set-integration-secret.sh" -- site Start.gg
check "a site ID that cannot be one exits 2" exited 2
run "$SDIR/set-integration-secret.sh" -- site discord
check "discord as a site ID exits 2" exited 2
check "beside discord's" config_is "$R/var/lib/$S/integrations.json" discord_client_secret '"staging-secret"'
run "$SDIR/set-integration-secret.sh" INSTANCE= -- discord
check "a disagreeing INSTANCE exits 2" exited 2
check "nothing ran" nothing_logged
check "production still has none" absent "$R/var/lib/$P/integrations.json"
printf 'production-secret\n' > "$INPUT"
run "$PDIR/set-integration-secret.sh" -- blumint blumint-partner-staging
check "production folder exits 0" succeeded
check "stored for production" python3 -c 'import json, sys; sys.exit(json.load(open(sys.argv[1]))["api_keys"] != {"blumint-partner-staging": "production-secret"})' "$R/var/lib/$P/integrations.json"
check "staging's secret unchanged" config_is "$R/var/lib/$S/integrations.json" discord_client_secret '"staging-secret"'
check "staging untouched" staging_untouched
unset INPUT
snapshot "$S" "$SSITE" staging

echo "10. upgrading a production bridge.json shaped like the live one"
python3 - "$PSTATE" "$HERE/tenants.json" <<'PY'
import json, sys
config = json.load(open(sys.argv[1]))
config.update(json.load(open(sys.argv[2])))
config["display_name"] = "Ember bridge (staging)"
config["rooms"] = {"supervisor_url": "http://127.0.0.1:47830"}
config["match_expiry_hours"] = 24
json.dump(config, open(sys.argv[1], "w"), indent=2)
PY
cp "$PSTATE" "$T/live.json"
stage_binary "$PDIR" v2
run "$PDIR/setup.sh"
check "exits 0" succeeded
check "only display_name changed" python3 - "$T/live.json" "$PSTATE" <<'PY'
import json, sys
before, after = (json.load(open(path)) for path in sys.argv[1:])
changed = {key for key in before.keys() | after.keys() if before.get(key) != after.get(key)}
sys.exit(changed != {"display_name"} or after["display_name"] != "Ember bridge")
PY
check "connections kept" connections_are "$PSTATE" blumint-partner-staging blumint-staging ember-test
check "binary replaced" same "$PDIR/bin/ember-bridge" "$R/usr/local/lib/$P/ember-bridge"
check "bridge ID kept" test "$(bridge_id "$PSTATE")" = "$PID"
check "service unit still as before" same "$GOLDEN/ember-bridge.service" "$PUNITS.service"
check "backup service still as before" same "$GOLDEN/ember-bridge-backup.service" "$PUNITS-backup.service"
check "site still as before" same "$GOLDEN/$PSITE" "$R/etc/nginx/sites-available/$PSITE"
check "staging files unchanged" unchanged "$S" "$SSITE" staging
check "staging untouched" staging_untouched
snapshot "$P" "$PSITE" production

echo "11. under sudo the folder sits in the invoking user's home, not HOME"
run "$PDIR/setup.sh" SUDO_USER=katie HOME=/root
check "production setup exits 0" succeeded
check "production answers through nginx" mentions "public: ok ($PID)"
check "staging untouched" staging_untouched
run "$SDIR/setup.sh" SUDO_USER=katie HOME=/root
check "staging setup exits 0" succeeded
check "staging answers through nginx" mentions "public: ok ($SID)"
check "production units never touched" production_untouched_by_systemctl
INPUT=$T/secret
printf 'staging-secret-2\n' > "$INPUT"
run "$SDIR/set-integration-secret.sh" SUDO_USER=katie HOME=/root -- discord
check "staging secret exits 0" succeeded
check "stored for staging" config_is "$R/var/lib/$S/integrations.json" discord_client_secret '"staging-secret-2"'
check "production units never touched" production_untouched_by_systemctl
snapshot "$P" "$PSITE" production
snapshot "$S" "$SSITE" staging
run "$PDIR/setup.sh" SUDO_USER=nobody
check "an unknown SUDO_USER exits 2" exited 2
check "says the folder is not in a home" mentions "is not directly in"
check "nothing ran" nothing_logged
run "$SDIR/set-integration-secret.sh" SUDO_USER=nobody -- discord
check "its set-integration-secret.sh exits 2" exited 2
check "nothing ran" nothing_logged
check "staging secret unchanged" config_is "$R/var/lib/$S/integrations.json" discord_client_secret '"staging-secret-2"'
check "production files unchanged" unchanged "$P" "$PSITE" production
check "staging files unchanged" unchanged "$S" "$SSITE" staging
unset INPUT

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
