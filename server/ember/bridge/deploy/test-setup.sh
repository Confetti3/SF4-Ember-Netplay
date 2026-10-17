#!/usr/bin/env bash
# Exercises setup.sh for the production and staging bridges in one mock
# server: no root, no systemd, no nginx, no network.
# Run as: bash test-setup.sh
#
# Every server path lives under a temporary folder (EMBER_BRIDGE_ROOT), and
# stand-ins for id, useradd, sudo, install, systemctl, nginx, curl and sleep
# come first on PATH. install drops the owner, sudo runs the command as the
# caller, and systemctl records which units were restarted. curl answers a
# well-known request the way the server would route it: through the enabled
# nginx site for its host, to the started unit whose bridge.json listens on
# that port. nginx -t fails while an enabled site contains BADNGINX. The
# staged ember-bridge binary is a script whose init writes a bridge.json.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export TMPDIR=$T/tmp
export EMBER_BRIDGE_ROOT=$T/root
export STUB=$T/stub
R=$EMBER_BRIDGE_ROOT
mkdir -p "$TMPDIR" "$T/stubs" "$STUB/up"
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
    is-active) [ -e "$STUB/up/$2" ] || { echo inactive; exit 3; }; echo active ;;
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
account() { grep -qx -- "$1" "$STUB/users"; }
# config_is <bridge.json> <key> <JSON value>
config_is() { python3 -c 'import json, sys; sys.exit(json.load(open(sys.argv[1])).get(sys.argv[2]) != json.loads(sys.argv[3]))' "$@"; }
lacks_key() { python3 -c 'import json, sys; sys.exit(sys.argv[2] in json.load(open(sys.argv[1])))' "$@"; }
connections_are() { python3 -c 'import json, sys; sys.exit(sorted(c["id"] for t in json.load(open(sys.argv[1]))["tenants"] for c in t["connections"]) != sys.argv[2:])' "$@"; }
bridge_id() { python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["bridge_id"])' "$1"; }
links_to() { [ "$(readlink "$1")" = "$2" ]; }

# An operator folder: the deploy files and a stand-in binary of that version.
stage() {
    local dir=$T/$1
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
# run_setup <INSTANCE> <operator folder>
run_setup() {
    : > "$STUB/log"
    OUT=$T/out.txt
    INSTANCE=$1 bash "$2/setup.sh" > "$OUT" 2>&1
    RC=$?
}
# Everything an install of <name> owns, for comparing before and after.
snapshot() {
    local name=$1 site=$2 into=$T/snap-$3
    rm -rf "$into"
    mkdir -p "$into"
    cp -a "$R/usr/local/lib/$name" "$into/lib"
    cp -a "$R/var/lib/$name/bridge.json" "$into/bridge.json"
    cp -a "$R/etc/systemd/system/$name.service" "$R/etc/systemd/system/$name-backup.service" \
        "$R/etc/systemd/system/$name-backup.timer" "$into/"
    cp -a "$R/etc/nginx/sites-available/$site" "$into/site"
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
        [ "$(cat "$from/enabled")" = "$(readlink "$R/etc/nginx/sites-enabled/$site")" ]
}
# No systemctl call this run named one of production's units.
production_untouched_by_systemctl() {
    ! grep -E '^systemctl .* ember-bridge(\.service|-backup\.(service|timer))?( |$)' "$STUB/log"
}
# A staging unit is production's with only the names changed.
renamed_from() {
    sed -e 's/ember-bridge-staging/ember-bridge/g' -e '/^Environment=INSTANCE=staging$/d' \
        -e 's/ (staging)$//' "$2" | cmp -s - "$1"
}
# Two sites that differ only in host and port, comments aside.
same_site() {
    cmp -s <(grep -v '^#' "$1" | sed -e 's/bridge\.embernetplay\.link/bridge-staging.embernetplay.link/g' -e 's/47820/47821/g') \
        <(grep -v '^#' "$2")
}

P=ember-bridge
PSITE=bridge.embernetplay.link
PUNITS=$R/etc/systemd/system/$P
PSTATE=$R/var/lib/$P/bridge.json
S=ember-bridge-staging
SSITE=bridge-staging.embernetplay.link
SUNITS=$R/etc/systemd/system/$S
SSTATE=$R/var/lib/$S/bridge.json
stage production
stage staging

echo "1. production first install"
run_setup "" "$T/production"
check "exits 0" succeeded
check "account ember-bridge created" account "$P"
check "binary installed" same "$T/production/bin/ember-bridge" "$R/usr/local/lib/$P/ember-bridge"
check "backup.sh installed" same "$HERE/backup.sh" "$R/usr/local/lib/$P/backup.sh"
check "restore.sh installed" same "$HERE/restore.sh" "$R/usr/local/lib/$P/restore.sh"
check "state and backups owned by the account" logged "install -d -o $P -g $P -m 0700 $R/var/lib/$P $R/var/backups/$P"
check "init runs as the account" logged "sudo -u $P $R/usr/local/lib/$P/ember-bridge"
check "service unit as shipped" same "$HERE/ember-bridge.service" "$PUNITS.service"
check "backup service as shipped" same "$HERE/ember-bridge-backup.service" "$PUNITS-backup.service"
check "backup timer as shipped" same "$HERE/ember-bridge-backup.timer" "$PUNITS-backup.timer"
check "site as shipped" same "$HERE/nginx/$PSITE.conf" "$R/etc/nginx/sites-available/$PSITE"
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

echo "2. staging first install beside it"
run_setup staging "$T/staging"
check "exits 0" succeeded
check "account ember-bridge-staging created" account "$S"
check "own binary" same "$T/staging/bin/ember-bridge" "$R/usr/local/lib/$S/ember-bridge"
check "own backup.sh" same "$HERE/backup.sh" "$R/usr/local/lib/$S/backup.sh"
check "state and backups owned by its account" logged "install -d -o $S -g $S -m 0700 $R/var/lib/$S $R/var/backups/$S"
check "init runs as its account" logged "sudo -u $S $R/usr/local/lib/$S/ember-bridge"
check "service runs its binary on its config" has_line "$SUNITS.service" "ExecStart=/usr/local/lib/$S/ember-bridge serve /var/lib/$S/bridge.json"
check "service runs as its account" has_line "$SUNITS.service" "User=$S"
check "service group" has_line "$SUNITS.service" "Group=$S"
check "service state directory" has_line "$SUNITS.service" "StateDirectory=$S"
check "backup runs its backup.sh" has_line "$SUNITS-backup.service" "ExecStart=/usr/local/lib/$S/backup.sh"
check "backup tells backup.sh the instance" has_line "$SUNITS-backup.service" "Environment=INSTANCE=staging"
check "backup writes only its folders" has_line "$SUNITS-backup.service" "ReadWritePaths=/var/lib/$S /var/backups/$S"
check "backup follows its service" has_line "$SUNITS-backup.service" "After=$S.service"
check "timer named staging" has_line "$SUNITS-backup.timer" "Description=Daily Ember bridge backup (staging)"
check "service unit is production's renamed" renamed_from "$HERE/ember-bridge.service" "$SUNITS.service"
check "backup service is production's renamed" renamed_from "$HERE/ember-bridge-backup.service" "$SUNITS-backup.service"
check "backup timer is production's renamed" renamed_from "$HERE/ember-bridge-backup.timer" "$SUNITS-backup.timer"
check "no unit names production" bash -c '! grep -E "(=|lib/|backups/)ember-bridge([^-]|$)" "$@"' _ "$SUNITS".service "$SUNITS"-backup.*
check "origin" config_is "$SSTATE" origin '"https://bridge-staging.embernetplay.link"'
check "listen" config_is "$SSTATE" listen '"127.0.0.1:47821"'
check "display name" config_is "$SSTATE" display_name '"Ember bridge (staging)"'
check "no public rooms" lacks_key "$SSTATE" rooms
check "staging connections" connections_are "$SSTATE" blumint-partner-staging ember-test
check "development switches off" config_is "$SSTATE" allow_private_webhooks false
SID=$(bridge_id "$SSTATE")
check "its own bridge ID" test "$SID" != "$PID"
check "site as shipped" same "$HERE/nginx/$SSITE.conf" "$R/etc/nginx/sites-available/$SSITE"
check "site enabled" links_to "$R/etc/nginx/sites-enabled/$SSITE" "$R/etc/nginx/sites-available/$SSITE"
check "site is production's apart from host and port" same_site "$HERE/nginx/$PSITE.conf" "$HERE/nginx/$SSITE.conf"
check "units enabled" logged "systemctl enable $S $S-backup.timer"
check "answers on 47821" mentions "service: ok ($SID)"
check "answers through nginx" mentions "public: ok ($SID)"
check "production files unchanged" unchanged "$P" "$PSITE" production
check "production units never touched" production_untouched_by_systemctl
snapshot "$S" "$SSITE" staging

echo "3. a newer staging binary"
stage_binary "$T/staging" v2
run_setup staging "$T/staging"
check "exits 0" succeeded
check "binary replaced" same "$T/staging/bin/ember-bridge" "$R/usr/local/lib/$S/ember-bridge"
check "bridge ID kept" test "$(bridge_id "$SSTATE")" = "$SID"
check "account created once" test "$(grep -cx "$S" "$STUB/users")" = 1
check "production files unchanged" unchanged "$P" "$PSITE" production
check "production units never touched" production_untouched_by_systemctl
snapshot "$S" "$SSITE" staging

echo "4. a staging site nginx rejects"
cp "$T/staging/nginx/$SSITE.conf" "$T/staging-site"
echo "# BADNGINX" >> "$T/staging/nginx/$SSITE.conf"
run_setup staging "$T/staging"
check "fails" failed
check "says it restored nginx" mentions "restored the previous files"
check "staging site back byte for byte" same "$T/snap-staging/site" "$R/etc/nginx/sites-available/$SSITE"
check "staging still enabled" links_to "$R/etc/nginx/sites-enabled/$SSITE" "$R/etc/nginx/sites-available/$SSITE"
check "nothing restarted" bash -c '! grep -q "^systemctl restart" "$1"' _ "$STUB/log"
check "production files unchanged" unchanged "$P" "$PSITE" production
cp "$T/staging-site" "$T/staging/nginx/$SSITE.conf"

echo "5. a tenants file and a site that disagree are refused before any change"
cp "$T/staging/tenants.staging.json" "$T/staging-tenants"
sed -i 's/127.0.0.1:47821/127.0.0.1:47822/' "$T/staging/tenants.staging.json"
run_setup staging "$T/staging"
check "another port fails" failed
check "names the port" mentions "proxy_pass http://127.0.0.1:47822 only"
check "nothing ran" nothing_logged
cp "$T/staging-tenants" "$T/staging/tenants.staging.json"
cp "$HERE/nginx/$PSITE.conf" "$T/staging/nginx/$SSITE.conf"
run_setup staging "$T/staging"
check "production's site under staging's name fails" failed
check "names the host" mentions "server_name $SSITE"
check "nothing ran" nothing_logged
cp "$T/staging-site" "$T/staging/nginx/$SSITE.conf"
check "staging files unchanged" unchanged "$S" "$SSITE" staging

echo "6. instance names"
run_setup Staging "$T/staging"
check "an upper-case name fails" test "$RC" = 2
check "nothing ran" nothing_logged
run_setup qa "$T/staging"
check "an instance with no tenants file fails" failed
check "names the file" mentions "Missing $T/staging/tenants.qa.json"
check "nothing ran" nothing_logged

echo "7. production again after staging"
stage_binary "$T/production" v2
run_setup "" "$T/production"
check "exits 0" succeeded
check "binary replaced" same "$T/production/bin/ember-bridge" "$R/usr/local/lib/$P/ember-bridge"
check "bridge ID kept" test "$(bridge_id "$PSTATE")" = "$PID"
check "service unit still as shipped" same "$HERE/ember-bridge.service" "$PUNITS.service"
check "backup service still as shipped" same "$HERE/ember-bridge-backup.service" "$PUNITS-backup.service"
check "site still as shipped" same "$HERE/nginx/$PSITE.conf" "$R/etc/nginx/sites-available/$PSITE"
check "staging files unchanged" unchanged "$S" "$SSITE" staging
check "staging units never touched" bash -c '! grep -q "ember-bridge-staging" "$1"' _ "$STUB/log"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
