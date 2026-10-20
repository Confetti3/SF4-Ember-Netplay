#!/usr/bin/env bash
# Exercises setup.sh, issue-secrets.sh and remove.sh for a production and a
# staging connector in one mock server: no root, no systemd, no nginx, no
# network.
# Run as: bash test-setup.sh
#
# Every server path lives under a temporary folder (EMBER_BRACKETS_ROOT inside
# TMPDIR, with EMBER_BRACKETS_TEST=1), operator folders sit in HOME, and
# stand-ins for id, useradd, userdel, sudo, install, systemctl, nginx, curl,
# getent and sleep come first on PATH. install drops the owner, sudo runs the
# command as the caller, and systemctl records which units were restarted.
# curl answers the way the server would route a request: through the enabled
# nginx site for its host, to the started unit whose brackets.json listens on
# that port, where only the results path exists and refuses a GET. nginx -t
# fails while an enabled site contains BADNGINX. The staged ember-brackets
# binary is a placeholder; the bridge's ember-bridge is a script that issues
# a fixed credential and result secret.
set -u
unset INSTANCE SUDO_USER

HERE="$(cd "$(dirname "$0")" && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export TMPDIR=$T/tmp
export EMBER_BRACKETS_ROOT=$TMPDIR/root EMBER_BRACKETS_TEST=1
export STUB=$T/stub
R=$EMBER_BRACKETS_ROOT
export HOME=$T/ops
OPS=$HOME
mkdir -p "$TMPDIR" "$T/stubs" "$STUB/up" "$OPS"
touch "$STUB/users" "$STUB/log"
for dir in etc/systemd/system etc/nginx/conf.d etc/nginx/sites-available etc/nginx/sites-enabled \
    etc/ssl/certs etc/ssl/private usr/local/lib var/lib; do
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
cat > "$T/stubs/userdel" <<'EOF'
#!/usr/bin/env bash
echo "userdel $*" >> "$STUB/log"
grep -vx -- "$1" "$STUB/users" > "$STUB/users.new"; mv "$STUB/users.new" "$STUB/users"
EOF
cat > "$T/stubs/sudo" <<'EOF'
#!/usr/bin/env bash
echo "sudo $1 $2 $(basename "$3") $4 $6" >> "$STUB/log"
[ "$1" = -u ] || exit 1
shift 2
exec "$@"
EOF
cat > "$T/stubs/install" <<'EOF'
#!/usr/bin/env bash
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
    disable) rm -f "$STUB/up/${@: -1}" ;;
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
! grep -qs BADNGINX "$EMBER_BRACKETS_ROOT"/etc/nginx/sites-enabled/*
EOF
cat > "$T/stubs/curl" <<'EOF'
#!/usr/bin/env python3
import glob, json, os, re, sys
root, stub, url = os.environ["EMBER_BRACKETS_ROOT"], os.environ["STUB"], sys.argv[-1]
loopback = re.match(r"http://127\.0\.0\.1:(\d+)(/.*)$", url)
public = re.match(r"https://([^/]+)(/.*)$", url)
if loopback:
    port, path = loopback[1], loopback[2]
elif public:
    site = f"{root}/etc/nginx/sites-enabled/{public[1]}"
    if not os.path.exists(site):
        sys.exit(6)
    text = open(site).read()
    ports = set(re.findall(r"proxy_pass http://127\.0\.0\.1:(\d+);", text))
    if len(ports) != 1:
        sys.exit(7)
    port, path = ports.pop(), public[2]
    if path not in re.findall(r"location = (\S+) \{", text):
        print("404")
        sys.exit(0)
else:
    sys.exit(3)
for unit in glob.glob(f"{root}/etc/systemd/system/*.service"):
    name = os.path.basename(unit)[: -len(".service")]
    if os.path.exists(f"{stub}/up/{name}"):
        config = json.load(open(f"{root}/var/lib/{name}/brackets.json"))
        if config["results"]["listen"] == f"127.0.0.1:{port}":
            print("405" if path == config["results"]["path"] else "404")
            sys.exit(0)
sys.exit(7)
EOF
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
silent_about() { ! grep -qF -- "$1" "$OUT"; }
logged() { grep -qxF -- "$1" "$STUB/log"; }
not_logged() { ! grep -qxF -- "$1" "$STUB/log"; }
failed() { [ "$RC" -ne 0 ]; }
succeeded() { [ "$RC" -eq 0 ]; }
exited() { [ "$RC" -eq "$1" ]; }
absent() { [ ! -e "$1" ]; }
present() { [ -e "$1" ]; }
account() { grep -qx -- "$1" "$STUB/users"; }
mode_is() { [ "$(stat -c %a "$1")" = "$2" ]; }
has_text() { grep -qF -- "$2" "$1"; }
lacks_text() { ! grep -qF -- "$2" "$1"; }
first_line_is() { [ "$(head -n 1 "$1")" = "$2" ]; }
dir_empty() { [ -z "$(ls -A "$1")" ]; }

S=ember-brackets-staging
P=ember-brackets
BS=ember-bridge-staging
TOKEN="emk_token_for_the_test"

# An operator folder: the deploy files and a placeholder binary.
stage() {
    local dir=$OPS/$1
    mkdir -p "$dir/bin"
    cp -r "$HERE"/. "$dir"/
    printf '#!/bin/sh\nexit 0\n' > "$dir/bin/ember-brackets"
    chmod +x "$dir/bin/ember-brackets"
}
# A production connector's configuration: its own port, host and files.
production_files() {
    python3 - "$HERE" "$OPS/$P" "${1:-47880}" <<'PY'
import json, sys
here, dest, port = sys.argv[1:]
config = json.load(open(f"{here}/brackets.staging.json"))
config["results"]["listen"] = f"127.0.0.1:{port}"
config["bridge"]["connection_id"] = "challonge"
json.dump(config, open(f"{dest}/brackets.json", "w"))
json.dump({"host": "brackets.embernetplay.link", "bridge": "ember-bridge"}, open(f"{dest}/site.json", "w"))
PY
}
# run <script> [VAR=value ...] [-- arguments]: a fresh log, the output in OUT
# and the exit status in RC. INSTANCE is unset unless given.
run() {
    local script=$1 vars=()
    shift
    while [ $# -gt 0 ] && [ "$1" != -- ]; do vars+=("$1"); shift; done
    [ $# -gt 0 ] && shift
    : > "$STUB/log"
    OUT=$T/out.txt
    env "${vars[@]}" bash "$script" "$@" > "$OUT" 2>&1 < /dev/null
    RC=$?
}
SD=$OPS/$S
LIBS=$R/usr/local/lib/$S
STS=$R/var/lib/$S
SITE=$R/etc/nginx/sites-available/brackets-staging.embernetplay.link
ENABLED=$R/etc/nginx/sites-enabled/brackets-staging.embernetplay.link
ZONE=$R/etc/nginx/conf.d/ember-brackets.conf

echo "1. the staging bridge, as issue-secrets.sh finds it"
mkdir -p "$OPS/$BS" "$R/usr/local/lib/$BS" "$R/var/lib/$BS"
echo '{}' > "$R/var/lib/$BS/bridge.json"
cat > "$R/usr/local/lib/$BS/ember-bridge" <<EOF
#!/usr/bin/env bash
case "\$1" in
    credential) echo "$TOKEN"; echo "Store this credential now." >&2 ;;
    result-secret) echo "whsec_secret_for_the_test"; echo "Give this to the platform." >&2 ;;
    *) exit 1 ;;
esac
EOF
cat > "$OPS/$BS/set-integration-secret.sh" <<'EOF'
#!/usr/bin/env bash
echo "set-integration-secret $*" >> "$STUB/log"
read -r secret
printf '%s\n' "$secret" > "$STUB/stored-$1-$2"
EOF
chmod +x "$R/usr/local/lib/$BS/ember-bridge"
stage "$S"
check "the staging files are in the deploy folder" present "$SD/brackets.staging.json"

echo "2. setup.sh without secrets installs everything and starts nothing"
run "$SD/setup.sh" --
check "exits 0" succeeded
check "says not started" mentions "NOT STARTED"
check "names the missing files" mentions "provider.token result.secret challonge.key"
check "creates the account" account "$S"
check "installs the binary" present "$LIBS/ember-brackets"
check "installs the configuration as brackets.json" same "$SD/brackets.staging.json" "$STS/brackets.json"
check "the state folder is 0700" mode_is "$STS" 700
check "the configuration is 0600" mode_is "$STS/brackets.json" 600
check "installs the zone" same "$SD/nginx/ember-brackets-zone.conf" "$ZONE"
check "the unit names the account and the file" has_text "$R/etc/systemd/system/$S.service" "ExecStart=/usr/local/lib/$S/ember-brackets serve /var/lib/$S/brackets.json"
check "the unit says staging" has_text "$R/etc/systemd/system/$S.service" "Description=Ember bracket connector (staging)"
check "the site begins with the marker" first_line_is "$SITE" "# ember-brackets: $S"
check "the site serves the host" has_text "$SITE" "server_name brackets-staging.embernetplay.link;"
check "the site serves the one results path" has_text "$SITE" "location = /ember/results {"
check "the site proxies to the connector" has_text "$SITE" "proxy_pass http://127.0.0.1:47881;"
check "the site has no other proxy" bash -c '[ "$(grep -c proxy_pass "$1")" = 1 ]' _ "$SITE"
check "the site is enabled" bash -c '[ "$(readlink "$1")" = "$2" ]' _ "$ENABLED" "$SITE"
check "every other path answers 404" has_text "$SITE" "return 404;"
check "no placeholder is left" bash -c '! grep -q "@[A-Z_]*@" "$1" "$2"' _ "$SITE" "$R/etc/systemd/system/$S.service"
check "the service is not restarted" not_logged "systemctl restart $S"
check "nginx is reloaded" logged "systemctl reload nginx"
check "the unit is enabled" logged "systemctl enable $S"

echo "3. issue-secrets.sh issues the two secrets and prints neither"
run "$SD/issue-secrets.sh" SUDO_USER=katie --
check "exits 0" succeeded
check "the provider credential is in secrets/" has_text "$SD/secrets/provider.token" "$TOKEN"
check "the result secret is in secrets/" has_text "$SD/secrets/result.secret" "whsec_secret_for_the_test"
check "secrets/ is 0700" mode_is "$SD/secrets" 700
check "the files are 0600" bash -c '[ "$(stat -c %a "$1")" = 600 ] && [ "$(stat -c %a "$2")" = 600 ]' _ "$SD/secrets/provider.token" "$SD/secrets/result.secret"
check "the credential is issued as the bridge's account" logged "sudo -u $BS ember-bridge credential provider"
check "the result secret is stored on the bridge for the connection" logged "set-integration-secret result challonge-staging"
check "the bridge got the secret on one line" has_text "$STUB/stored-result-challonge-staging" "whsec_secret_for_the_test"
check "the credential is not in the output" silent_about "$TOKEN"
check "the result secret is not in the output" silent_about "whsec_secret_for_the_test"

echo "4. a second run issues nothing new"
cp "$SD/secrets/provider.token" "$T/token.before"
: > "$STUB/stored-result-challonge-staging"
run "$SD/issue-secrets.sh" SUDO_USER=katie --
check "exits 0" succeeded
check "keeps the credential" same "$SD/secrets/provider.token" "$T/token.before"
check "does not issue another" not_logged "sudo -u $BS ember-bridge credential provider"
check "stores the result secret again from its file" has_text "$STUB/stored-result-challonge-staging" "whsec_secret_for_the_test"

echo "5. setup.sh with the secrets in place starts the service"
umask 077
printf 'organizer-key\n' > "$SD/secrets/challonge.key"
umask 022
run "$SD/setup.sh" --
check "exits 0" succeeded
check "restarts the connector" logged "systemctl restart $S"
check "the connector answers through nginx" mentions "public: ok (https://brackets-staging.embernetplay.link/ember/results)"
check "other paths are 404" mentions "every other path: 404"
check "ends done" mentions "=== done ==="
check "the credential is installed" has_text "$STS/provider.token" "$TOKEN"
check "every secret is owned 0600" bash -c 'for f in provider.token result.secret challonge.key; do [ "$(stat -c %a "$1/$f")" = 600 ] || exit 1; done' _ "$STS"
check "secrets/ is empty afterwards" dir_empty "$SD/secrets"
check "no secret is in the output" silent_about "organizer-key"

echo "6. running it again changes nothing that matters"
echo "played sets" > "$STS/brackets.sqlite3"
cp "$STS/provider.token" "$T/token.kept"
cp "$SITE" "$T/site.before"
run "$SD/setup.sh" --
check "exits 0" succeeded
check "the site is unchanged" same "$T/site.before" "$SITE"
check "the database is untouched" has_text "$STS/brackets.sqlite3" "played sets"
check "the credential is untouched" same "$T/token.kept" "$STS/provider.token"
check "the connector restarts" logged "systemctl restart $S"
run "$SD/issue-secrets.sh" SUDO_USER=katie --
check "issue-secrets.sh keeps what is installed" mentions "provider credential: already there, kept"
check "and the result secret" mentions "result secret: already there, kept"

echo "7. a configuration or site that cannot be used changes nothing"
cp "$STS/brackets.json" "$T/config.before"
cp "$R/etc/systemd/system/$S.service" "$T/unit.before"
for path in '/ember results' '/../x' '/ember/' '//x' 'results' ''; do
    python3 - "$HERE/brackets.staging.json" "$SD/brackets.staging.json" "$path" <<'PY'
import json, sys
c = json.load(open(sys.argv[1]))
c["results"]["path"] = sys.argv[3]
json.dump(c, open(sys.argv[2], "w"))
PY
    run "$SD/setup.sh" --
    check "path '$path' is refused" failed
done
python3 - "$HERE/brackets.staging.json" "$SD/brackets.staging.json" <<'PY'
import json, sys
c = json.load(open(sys.argv[1]))
c["results"]["listen"] = "0.0.0.0:47881"
json.dump(c, open(sys.argv[2], "w"))
PY
run "$SD/setup.sh" --
check "a listen address off loopback is refused" failed
python3 - "$HERE/brackets.staging.json" "$SD/brackets.staging.json" <<'PY'
import json, sys
c = json.load(open(sys.argv[1]))
c["tournaments"][0]["credential"]["file"] = "../challonge.key"
json.dump(c, open(sys.argv[2], "w"))
PY
run "$SD/setup.sh" --
check "a secret file outside the state folder is refused" failed
cp "$HERE/brackets.staging.json" "$SD/brackets.staging.json"
printf '{"host": "Brackets.Example", "bridge": "ember-bridge-staging"}' > "$SD/site.staging.json"
run "$SD/setup.sh" --
check "a host that is not lowercase DNS is refused" failed
printf '{"host": "brackets-staging.embernetplay.link", "bridge": "somewhere-else"}' > "$SD/site.staging.json"
run "$SD/setup.sh" --
check "a bridge that is not a bridge folder is refused" failed
cp "$HERE/site.staging.json" "$SD/site.staging.json"
check "the configuration is unchanged" same "$T/config.before" "$STS/brackets.json"
check "the unit is unchanged" same "$T/unit.before" "$R/etc/systemd/system/$S.service"
check "the site is unchanged" same "$T/site.before" "$SITE"

echo "8. a secrets folder that others can open is refused"
mkdir -m 755 "$SD/secrets.open"
mv "$SD/secrets" "$T/secrets.kept"
mv "$SD/secrets.open" "$SD/secrets"
run "$SD/setup.sh" --
check "mode 0755 is refused" failed
check "says so" mentions "must be a real folder with mode 0700"
rmdir "$SD/secrets"
mv "$T/secrets.kept" "$SD/secrets"
printf 'x\n' > "$T/real"
ln -s "$T/real" "$SD/secrets/challonge.key"
run "$SD/setup.sh" --
check "a symlink among the secrets is refused" failed
rm -f "$SD/secrets/challonge.key"

echo "9. nginx rejecting the site puts the old files back"
cp "$HERE/nginx/brackets.conf.in" "$T/template.kept"
printf '# BADNGINX\n' >> "$SD/nginx/brackets.conf.in"
run "$SD/setup.sh" --
check "fails" failed
check "says nginx rejected it" mentions "nginx rejected the new configuration; restored the previous files."
check "the site is back" same "$T/site.before" "$SITE"
check "the zone is back" same "$HERE/nginx/ember-brackets-zone.conf" "$ZONE"
cp "$T/template.kept" "$SD/nginx/brackets.conf.in"

echo "10. a second connector gets its own account, site and port"
stage "$P"
production_files 47881
run "$OPS/$P/setup.sh" --
check "the port of another connector is refused" failed
check "says whose" mentions "already listens on 127.0.0.1:47881"
check "no account was made" bash -c '! grep -qx ember-brackets "$1"' _ "$STUB/users"
production_files 47880
mkdir -p "$R/etc/nginx/sites-available"
printf 'server { server_name brackets.embernetplay.link; }\n' > "$R/etc/nginx/sites-available/brackets.embernetplay.link"
run "$OPS/$P/setup.sh" --
check "a site that is not its own is refused" failed
rm -f "$R/etc/nginx/sites-available/brackets.embernetplay.link"
cp "$STS/brackets.json" "$T/staging.config.before"
cp "$SITE" "$T/staging.site.before"
run "$OPS/$P/setup.sh" --
check "production installs" succeeded
check "its account is ember-brackets" account "$P"
check "its unit has no label" has_text "$R/etc/systemd/system/$P.service" "Description=Ember bracket connector"
check "its description is not staging's" lacks_text "$R/etc/systemd/system/$P.service" "(staging)"
check "its site is its own host" has_text "$R/etc/nginx/sites-available/brackets.embernetplay.link" "server_name brackets.embernetplay.link;"
check "on its own port" has_text "$R/etc/nginx/sites-available/brackets.embernetplay.link" "proxy_pass http://127.0.0.1:47880;"
check "not started, its secrets are missing" mentions "NOT STARTED"
check "staging's configuration is untouched" same "$T/staging.config.before" "$STS/brackets.json"
check "staging's site is untouched" same "$T/staging.site.before" "$SITE"
check "production never restarted staging" not_logged "systemctl restart $S"

echo "11. the folder decides the connector"
run "$SD/setup.sh" INSTANCE= --
check "a disagreeing INSTANCE exits 2" exited 2
run "$SD/issue-secrets.sh" INSTANCE=other SUDO_USER=katie --
check "issue-secrets.sh with a disagreeing INSTANCE exits 2" exited 2
mkdir -p "$T/elsewhere"
cp -r "$SD" "$T/elsewhere/$S"
run "$T/elsewhere/$S/setup.sh" --
check "a folder outside the home exits 2" exited 2
mkdir -p "$OPS/ember-brackets-bad-name"
cp -r "$SD"/. "$OPS/ember-brackets-bad-name/"
run "$OPS/ember-brackets-bad-name/setup.sh" --
check "a name with a hyphen exits 2" exited 2
run "$SD/setup.sh" SUDO_USER=nobody --
check "an operator other than the home's owner exits 2" exited 2

echo "12. remove.sh keeps the state unless told otherwise"
run "$SD/remove.sh" --
check "exits 0" succeeded
check "the unit is gone" absent "$R/etc/systemd/system/$S.service"
check "the site and its link are gone" bash -c '[ ! -e "$1" ] && [ ! -L "$2" ]' _ "$SITE" "$ENABLED"
check "the program folder is gone" absent "$LIBS"
check "the state stays" present "$STS/brackets.sqlite3"
check "the account stays" account "$S"
check "the service was disabled" logged "systemctl disable --now $S"
check "production is untouched" present "$R/etc/systemd/system/$P.service"
check "the shared zone stays" present "$ZONE"
run "$SD/remove.sh" -- --purge
check "--purge exits 0" succeeded
check "--purge deletes the state" absent "$STS"
check "--purge removes the account" logged "userdel $S"
run "$SD/remove.sh" -- --everything
check "an unknown option exits 2" exited 2

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
