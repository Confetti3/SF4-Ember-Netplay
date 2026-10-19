#!/usr/bin/env bash
# Installs one Ember bracket connector (ember-brackets), picked by the folder
# this script is in (instance.sh). Run as one of:
#   sudo bash ~/ember-brackets/setup.sh
#       production: brackets.json and site.json
#   sudo bash ~/ember-brackets-staging/setup.sh
#       staging: brackets.staging.json and site.staging.json
#
# Expects, next to this script: bin/ember-brackets, ember-brackets.service.in,
# nginx/brackets.conf.in, nginx/ember-brackets-zone.conf, instance.sh, the
# instance's connector configuration (brackets*.json, see README.md
# "Configure") and its site file (site*.json):
#   { "host": "brackets.embernetplay.link", "bridge": "ember-bridge" }
# the public host of its results URL and the installed bridge it connects to
# (issue-secrets.sh). The configuration's results.listen and results.path
# become the nginx site: the host serves that one path over HTTPS and answers
# 404 to everything else.
#
# Secrets are never in the configuration. Put each one a configuration names
# (bridge.credential_file, results.secret_file, results.previous_secret_file
# and every tournament's credential.file) as a file of that name in secrets/
# next to this script, a folder only the operator can open (mode 0700).
# setup.sh moves each into the state folder, owned by the service account with
# mode 0600, and removes the original. issue-secrets.sh makes the two that
# the bridge issues.
#
# Running it again installs a newer binary and applies the configuration; the
# database and the secrets stay. If a secret the configuration names is still
# missing, everything else is installed and the service is not started: the
# output names the missing files, and running setup.sh again after adding
# them starts it.
#
# What it changes, for production (an instance such as staging puts
# -<instance> after ember-brackets in every name and path, and has its own site):
#   user ember-brackets                          system account the connector runs as
#   /usr/local/lib/ember-brackets/               binary (root-owned)
#   /var/lib/ember-brackets/                     brackets.json, secrets, database (0700)
#   /etc/systemd/system/ember-brackets.service
#   /etc/nginx/conf.d/ember-brackets.conf        request zones, shared by every instance
#                                                (they must be the same file)
#   /etc/nginx/sites-available/<host> (and its sites-enabled link)
# Nothing is opened in ufw: the connector listens on loopback, nginx already
# serves 80 and 443. test-setup.sh moves every path above under a test folder
# (see instance.sh) and stands in for the commands that need root.
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"
# Commands run as the service account would not be able to read the operator's home.
cd /
[ -f "$SRC/instance.sh" ] || { echo "Missing $SRC/instance.sh" >&2; exit 1; }
. "$SRC/instance.sh"
brackets_instance "$SRC" || exit 2
CONFIG=brackets${INSTANCE:+.$INSTANCE}.json
SITEFILE=site${INSTANCE:+.$INSTANCE}.json
UNITS=$ROOT/etc/systemd/system
ZONE=$ROOT/etc/nginx/conf.d/ember-brackets.conf
CERT=$ROOT/etc/ssl/certs/embernetplay.link.fullchain.pem
KEY=$ROOT/etc/ssl/private/embernetplay.link.key

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo." >&2
    exit 1
fi
for file in bin/ember-brackets ember-brackets.service.in nginx/brackets.conf.in \
    nginx/ember-brackets-zone.conf "$CONFIG" "$SITEFILE"; do
    if [ ! -f "$SRC/$file" ]; then
        echo "Missing $SRC/$file" >&2
        exit 1
    fi
done
for file in "$CERT" "$KEY"; do
    if [ ! -f "$file" ]; then
        echo "Missing $file (the embernetplay.link wildcard certificate)." >&2
        exit 1
    fi
done
# The zone file is shared by every instance, so a different one is not replaced.
if [ -e "$ZONE" ] && ! cmp -s "$SRC/nginx/ember-brackets-zone.conf" "$ZONE"; then
    echo "$ZONE differs from $SRC/nginx/ember-brackets-zone.conf. Every connector shares it;" >&2
    echo "copy the installed one into this folder, or remove it if no connector is installed." >&2
    exit 1
fi

# What the configuration and the site file say, checked before anything
# changes. Line 1 is the port, line 2 the results path, line 3 the host, line
# 4 the bridge, then the name of every secret file.
FACTS=$(cat <<'PY'
import json, re, sys
config = json.load(open(sys.argv[1]))
site = json.load(open(sys.argv[2]))
def fail(text):
    sys.exit(text)
listen = re.fullmatch(r"127\.0\.0\.1:([1-9][0-9]{0,4})", str(config["results"].get("listen")))
if not listen or int(listen[1]) > 65535:
    fail(f"{sys.argv[1]}: results.listen is 127.0.0.1:<port>")
path = config["results"].get("path", "/results")
if not re.fullmatch(r"/[A-Za-z0-9_./-]{1,100}", path) or "//" in path or ".." in path.split("/") or path.endswith("/"):
    fail(f"{sys.argv[1]}: results.path is a plain path such as /ember/results (letters, digits, _ . - and /)")
host = site.get("host")
if not isinstance(host, str) or not re.fullmatch(r"[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+", host):
    fail(f"{sys.argv[2]}: host is a lowercase DNS name")
bridge = site.get("bridge")
if not isinstance(bridge, str) or not re.fullmatch(r"ember-bridge(-[a-z][a-z0-9]{0,15})?", bridge):
    fail(f"{sys.argv[2]}: bridge is an installed bridge, such as ember-bridge-staging")
names = [config["bridge"]["credential_file"], config["results"]["secret_file"]]
if config["results"].get("previous_secret_file"):
    names.append(config["results"]["previous_secret_file"])
names += [t["credential"]["file"] for t in config["tournaments"]]
for name in names + [config["database"]]:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", str(name)):
        fail(f"{sys.argv[1]}: file {name!r} is a plain name in the state folder (no folder, no leading dot)")
print(listen[1])
print(path)
print(host)
print(bridge)
for name in dict.fromkeys(names):
    print(name)
PY
)
mapfile -t FACT < <(python3 -c "$FACTS" "$SRC/$CONFIG" "$SRC/$SITEFILE")
if [ "${#FACT[@]}" -lt 5 ]; then
    echo "Could not use $CONFIG and $SITEFILE; nothing was changed." >&2
    exit 1
fi
PORT=${FACT[0]}
RESULTS_PATH=${FACT[1]}
HOST=${FACT[2]}
SECRETS=("${FACT[@]:4}")
SITE=$ROOT/etc/nginx/sites-available/$HOST
ENABLED=$ROOT/etc/nginx/sites-enabled/$HOST

# Another connector with this port would lose it to this one; a site this
# connector did not write is somebody else's host.
for other in "$ROOT"/var/lib/ember-brackets*/brackets.json; do
    [ -f "$other" ] || continue
    [ "$(dirname "$other")" = "$STATE" ] && continue
    if python3 -c 'import json, sys; sys.exit(json.load(open(sys.argv[1]))["results"]["listen"] != sys.argv[2])' "$other" "127.0.0.1:$PORT"; then
        echo "$other already listens on 127.0.0.1:$PORT; give this connector its own port." >&2
        exit 1
    fi
done
if [ -e "$SITE" ] && ! head -n 1 "$SITE" | grep -qxF "# ember-brackets: $NAME"; then
    echo "$SITE exists and is not this connector's site; nothing was changed." >&2
    exit 1
fi

# The secrets folder must be closed to everyone but its owner, or what is in
# it could be swapped before it is installed.
DROP=$SRC/secrets
if [ -d "$DROP" ]; then
    if [ -L "$DROP" ] || [ "$(stat -c %a "$DROP")" != 700 ]; then
        echo "$DROP must be a real folder with mode 0700 (chmod 700 $DROP); nothing was changed." >&2
        exit 1
    fi
    for name in "${SECRETS[@]}"; do
        if [ -e "$DROP/$name" ] && { [ -L "$DROP/$name" ] || [ ! -f "$DROP/$name" ] || [ ! -s "$DROP/$name" ]; }; then
            echo "$DROP/$name must be a regular file with something in it; nothing was changed." >&2
            exit 1
        fi
    done
fi

if ! id "$NAME" >/dev/null 2>&1; then
    useradd --system --home-dir "$STATE" --no-create-home --shell /usr/sbin/nologin "$NAME"
fi

install -d -o root -g root -m 0755 "$LIB"
install -o root -g root -m 0755 "$SRC/bin/ember-brackets" "$LIB/ember-brackets.new"
mv -f "$LIB/ember-brackets.new" "$LIB/ember-brackets"
install -o root -g root -m 0644 "$SRC/instance.sh" "$LIB/instance.sh"
install -d -o "$NAME" -g "$NAME" -m 0700 "$STATE"
install -o "$NAME" -g "$NAME" -m 0600 "$SRC/$CONFIG" "$STATE/brackets.json.new"
mv -f "$STATE/brackets.json.new" "$STATE/brackets.json"

# The secrets from the folder, then a check that every one the configuration
# names is in the state folder.
for name in "${SECRETS[@]}"; do
    if [ -f "$DROP/$name" ]; then
        install -o "$NAME" -g "$NAME" -m 0600 "$DROP/$name" "$STATE/$name.new"
        mv -f "$STATE/$name.new" "$STATE/$name"
        rm -f "$DROP/$name"
    fi
done
MISSING=()
for name in "${SECRETS[@]}"; do
    [ -s "$STATE/$name" ] || MISSING+=("$name")
done

# The unit and the site come from templates: @NAME@ is the account, unit and
# folder name, @HOST@, @PORT@ and @RESULTS_PATH@ come from the files above,
# and @LABEL@ names an instance other than production in the description.
LABEL=${INSTANCE:+ ($INSTANCE)}
render() {
    sed -e "s|@NAME@|$NAME|g" -e "s|@HOST@|$HOST|g" -e "s|@PORT@|$PORT|g" \
        -e "s|@RESULTS_PATH@|$RESULTS_PATH|g" -e "s|@LABEL@|$LABEL|g" "$1" > "$2.new"
    chmod 0644 "$2.new"
    mv -f "$2.new" "$2"
}
render "$SRC/ember-brackets.service.in" "$UNITS/$NAME.service"

# nginx: keep copies of the zone, the site and the enabled entry (or note
# that each was absent) so a failed check puts everything back as it was.
backup=$(mktemp -d)
[ -e "$ZONE" ] && cp -a "$ZONE" "$backup/zone"
[ -e "$SITE" ] && cp -a "$SITE" "$backup/site"
[ -e "$ENABLED" ] || [ -L "$ENABLED" ] && cp -a "$ENABLED" "$backup/enabled"
restore() {
    if [ -e "$backup/zone" ]; then cp -a "$backup/zone" "$ZONE"; else rm -f "$ZONE"; fi
    if [ -e "$backup/site" ]; then cp -a "$backup/site" "$SITE"; else rm -f "$SITE"; fi
    rm -f "$ENABLED"
    if [ -e "$backup/enabled" ] || [ -L "$backup/enabled" ]; then cp -a "$backup/enabled" "$ENABLED"; fi
}
install -o root -g root -m 0644 "$SRC/nginx/ember-brackets-zone.conf" "$ZONE"
render "$SRC/nginx/brackets.conf.in" "$SITE"
ln -sfn "$SITE" "$ENABLED"
if ! nginx -t; then
    restore
    if nginx -t; then
        echo "nginx rejected the new configuration; restored the previous files." >&2
    else
        echo "nginx rejected the new configuration, and the restored files fail too; fix nginx before reloading it." >&2
    fi
    exit 1
fi

systemctl daemon-reload
systemctl enable "$NAME"
systemctl reload nginx || systemctl restart nginx
rm -rf "$backup"

echo
echo "=== $NAME ==="
if [ "${#MISSING[@]}" -gt 0 ]; then
    echo "NOT STARTED: these files are missing from $STATE: ${MISSING[*]}"
    echo "Put them in $DROP (mode 0700) and run this script again."
    echo "=== installed, not running ==="
    exit 0
fi
systemctl restart "$NAME"

# The connector must be running, and its results URL must answer through
# nginx: the connector itself refuses a GET with 405, so that answer shows
# the path reaches it; any other path is 404.
EXPECT=""
answers() {
    local attempt status=000
    for attempt in 1 2 3 4 5 6 7 8 9 10; do
        status=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$@" 2>/dev/null) || status=000
        if [ "$status" = "$EXPECT" ]; then
            return 0
        fi
        sleep 1
    done
    echo "got $status, wanted $EXPECT: $*" >&2
    return 1
}
sleep 1
systemctl is-active "$NAME"
EXPECT=405
answers "http://127.0.0.1:$PORT$RESULTS_PATH" || exit 1
echo "service: ok (127.0.0.1:$PORT$RESULTS_PATH)"
answers --resolve "$HOST:443:127.0.0.1" "https://$HOST$RESULTS_PATH" || exit 1
echo "public: ok (https://$HOST$RESULTS_PATH)"
EXPECT=404
answers --resolve "$HOST:443:127.0.0.1" "https://$HOST/" || exit 1
echo "every other path: 404"
echo "=== done ==="
