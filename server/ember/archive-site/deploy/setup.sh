#!/usr/bin/env bash
# Installs the Ember replay archive on the VPS: a rustfs bucket on loopback,
# and the archive page, the listing proxy and the public bucket proxy in
# nginx. Run as:
#   sudo bash ~/ember-archive-staging/setup.sh [--replace-site] /path/to/rustfs
# Staging is the default: the archive under /archive/ on the staging bridge's
# own host, https://bridge-staging.embernetplay.link/archive/. Its nginx
# locations are a snippet the bridge's site includes (bridge.conf.in), so the
# bridge's setup.sh, with that include, must have run first. Production is a
# site of its own:
#   sudo HOST=archive.embernetplay.link PREFIX= WEBROOT=/var/www/ember-archive #        BRIDGE_PORT=47820 BUCKET=replays-live bash ~/ember-archive/setup.sh /path/to/rustfs
#   HOST         the host name                 (bridge-staging.embernetplay.link)
#   PREFIX       the path under it, or empty   (/archive; empty: a site of its own)
#   BRIDGE_NAME  the bridge whose site includes the snippet (ember-bridge-staging)
#   WEBROOT      where the page is installed   (/var/www/ember-archive-staging)
#   BRIDGE_PORT  the bridge loopback port      (47821, the staging bridge)
#   BUCKET       the bucket name               (replays)
#   S3_PORT      rustfs loopback port          (9400)
# Both on one server share one rustfs (and so one S3_PORT); give each its own
# BUCKET.
#
# Expects, next to this script: the templates and files of this folder,
# site/ (the page: index.html and assets/archive.css, assets/archive.js) and
# short-assets/ (ember-emblem.png and fonts/ from
# server/ember-short/deploy/site/assets). Run from the repository, it finds
# both there. The rustfs binary is the musl build of 1.0.4, from the rustfs
# releases. Running it again installs a newer binary, page or configuration;
# the credentials and the stored games stay.
#
# What it changes:
#   user rustfs-archive                              system account rustfs runs as
#   /usr/local/lib/rustfs-archive/rustfs             the binary (root-owned)
#   /var/lib/rustfs-archive/                         the bucket data (0700)
#   /etc/rustfs-archive/rustfs.env                   the bucket credentials, written on
#                                                    the first run (0600, root)
#   /etc/systemd/system/rustfs-archive.service       rustfs on 127.0.0.1:$S3_PORT, console off
#   /etc/nginx/conf.d/ember[_archive]_<site>.conf    request limits, the listing cache and the
#                                                    bridge upstream, named after the host
#   /var/lib/nginx/<zone>/                           that cache (owned by the nginx user, www-data)
#   /etc/nginx/snippets/ember-archive-headers.conf   HSTS, nosniff and no-referrer
#   /etc/nginx/snippets/$BRIDGE_NAME-archive.conf    under a prefix: the archive's locations
#   /etc/nginx/sites-available/$HOST (and its link)  a site of its own: its server block
#   $WEBROOT/                                        the page, its fonts and emblem
# It refuses to replace its own nginx file when that differs from the one it
# would write, unless --replace-site is given. A site of its own is always
# refused when another enabled nginx file serves the name; under a prefix the
# bridge's enabled site must include the snippet. It uses the
# embernetplay.link wildcard certificate the bridge sites use. The bucket and
# its policy come from bucket.sh. Nothing is opened in ufw: rustfs listens on
# loopback and nginx serves 80 and 443.
#
# The install is a transaction. Before anything moves, the previous binary,
# unit, nginx files, enabled link and page files are copied aside. Until the
# last check passes (rustfs answering on loopback, the bucket and its policy,
# nginx -t, the reload, and the archive page, the bucket refusal and the
# listing answering through nginx), any failure puts every one of them back
# and restarts the previous rustfs. A first install is removed instead. The
# bridge archive block and secret are not set here (README.md).
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"
HOST_GIVEN=${HOST:+1}
HOST=${HOST:-bridge-staging.embernetplay.link}
# PREFIX=/archive (the default) serves the archive inside the host's existing
# bridge site; PREFIX= (empty) gives it a server block of its own.
PREFIX=${PREFIX-/archive}
BRIDGE_NAME=${BRIDGE_NAME:-ember-bridge-staging}
WEBROOT=${WEBROOT:-/var/www/ember-archive-staging}
BRIDGE_PORT=${BRIDGE_PORT:-47821}
BUCKET=${BUCKET:-replays}
S3_PORT=${S3_PORT:-9400}
NAME=rustfs-archive
LIB=/usr/local/lib/$NAME
STATE=/var/lib/$NAME
ETC=/etc/$NAME
ENVFILE=$ETC/rustfs.env
UNIT=/etc/systemd/system/$NAME.service
CERT=/etc/ssl/certs/embernetplay.link.fullchain.pem
KEY=/etc/ssl/private/embernetplay.link.key

REPLACE_SITE=
BINARY=
for arg in "$@"; do
    case "$arg" in
        --replace-site) REPLACE_SITE=1 ;;
        -*) echo "Unknown option $arg." >&2; exit 2 ;;
        *) [ -z "$BINARY" ] || { echo "One rustfs binary only." >&2; exit 2; }; BINARY=$arg ;;
    esac
done
[ -n "$BINARY" ] || { echo "usage: sudo bash setup.sh [--replace-site] /path/to/rustfs" >&2; exit 2; }
if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo." >&2
    exit 1
fi
[[ "$HOST" =~ ^[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+$ ]] || { echo "HOST $HOST is not a host name." >&2; exit 2; }
[[ "$WEBROOT" =~ ^/var/www/[A-Za-z0-9._-]+$ ]] || { echo "WEBROOT must be a folder directly in /var/www." >&2; exit 2; }
[[ "$BRIDGE_PORT" =~ ^[1-9][0-9]{0,4}$ && "$S3_PORT" =~ ^[1-9][0-9]{0,4}$ ]] || { echo "BRIDGE_PORT and S3_PORT are port numbers." >&2; exit 2; }
[[ "$BUCKET" =~ ^[a-z0-9][a-z0-9-]{2,62}$ ]] || { echo "BUCKET is 3 to 63 lowercase letters, digits and hyphens." >&2; exit 2; }
[ -f "$BINARY" ] || { echo "Missing $BINARY" >&2; exit 1; }
[[ -z "$PREFIX" || "$PREFIX" =~ ^/[a-z0-9][a-z0-9-]*$ ]] || { echo "PREFIX is empty or a single path segment such as /archive." >&2; exit 2; }
[[ "$BRIDGE_NAME" =~ ^ember-bridge(-[a-z][a-z0-9]{0,15})?$ ]] || { echo "BRIDGE_NAME is ember-bridge or ember-bridge-<instance>." >&2; exit 2; }
if [ -z "$PREFIX" ] && [ -z "$HOST_GIVEN" ]; then
    echo "A site of its own (PREFIX empty) needs HOST, such as HOST=archive.embernetplay.link." >&2
    exit 2
fi
# The cache folder, the zone names and the conf.d file are named after the
# host; under a prefix they say so, since the host is the bridge's.
SITENAME=$(printf '%s' "${HOST%%.*}" | tr -c 'a-z0-9' '_')
if [ -n "$PREFIX" ]; then ZONE=ember_archive_$SITENAME; else ZONE=ember_$SITENAME; fi
ZONEFILE=/etc/nginx/conf.d/$ZONE.conf
SITE=/etc/nginx/sites-available/$HOST
ENABLED=/etc/nginx/sites-enabled/$HOST
# Under a prefix the archive's locations are a snippet the bridge's own site
# includes (bridge.conf.in); with a site of its own there is a server block.
SNIPPET=/etc/nginx/snippets/$BRIDGE_NAME-archive.conf
INCLUDE_LINE="include /etc/nginx/snippets/$BRIDGE_NAME-archive*.conf;"
HEADERS=/etc/nginx/snippets/ember-archive-headers.conf
BASE=https://$HOST$PREFIX

# The page files, relative to site/ here and to $WEBROOT on the server, and
# the ones that come from the short-link site assets.
if [ ! -d "$SRC/site" ] && [ -d "$SRC/../site" ]; then SITEDIR="$SRC/../site"; else SITEDIR="$SRC/site"; fi
if [ -d "$SRC/short-assets" ]; then SHORT="$SRC/short-assets"; else SHORT="$SRC/../../../ember-short/deploy/site/assets"; fi
PAGE="index.html assets/archive.css assets/archive.js"
BORROWED="ember-emblem.png faces.webp flags.png flags.json flags-LICENSE.txt fonts/inter-latin-400.woff2 fonts/inter-latin-600.woff2 fonts/OFL.txt"
for file in rustfs-archive.service.in bucket.sh bucket-policy.json.in nginx/archive-zone.conf.in \
    nginx/archive.conf.in nginx/archive-locations.conf.in nginx/ember-archive-headers.conf; do
    [ -f "$SRC/$file" ] || { echo "Missing $SRC/$file" >&2; exit 1; }
done
for file in $PAGE; do
    [ -f "$SITEDIR/$file" ] || { echo "Missing $SITEDIR/$file" >&2; exit 1; }
done
for file in $BORROWED; do
    [ -f "$SHORT/$file" ] || { echo "Missing $SHORT/$file (from server/ember-short/deploy/site/assets)." >&2; exit 1; }
done
for file in "$CERT" "$KEY"; do
    [ -f "$file" ] || { echo "Missing $file (the embernetplay.link wildcard certificate)." >&2; exit 1; }
done
command -v nginx >/dev/null || { echo "nginx is not installed." >&2; exit 1; }
"$BINARY" --version >/dev/null 2>&1 || { echo "$BINARY does not run here (the musl build of rustfs is expected)." >&2; exit 1; }
echo "rustfs: $("$BINARY" --version 2>&1 | head -1)"

# Everything rendered from a template: @HOST@, @WEBROOT@, @BRIDGE_PORT@,
# @S3_PORT@, @BUCKET@, @ZONE@ and @PREFIX@ (and @REDIRECT@, the redirect from
# the bare prefix to its page, which a prefix needs and a host root does not).
if [ -n "$PREFIX" ]; then REDIRECT="location = $PREFIX { return 301 $PREFIX/; }"; else REDIRECT=; fi
render() {
    sed -e "s/@HOST@/$HOST/g" -e "s|@WEBROOT@|$WEBROOT|g" -e "s/@BRIDGE_PORT@/$BRIDGE_PORT/g" \
        -e "s/@S3_PORT@/$S3_PORT/g" -e "s/@BUCKET@/$BUCKET/g" -e "s/@ZONE@/$ZONE/g" \
        -e "s|@PREFIX@|$PREFIX|g" -e "s|@REDIRECT@|$REDIRECT|" "$1" > "$2"
}

work=$(mktemp -d)
backup=$(mktemp -d)
render "$SRC/rustfs-archive.service.in" "$work/unit"
render "$SRC/nginx/archive-zone.conf.in" "$work/zone"
render "$SRC/nginx/archive-locations.conf.in" "$work/locations"
render "$SRC/bucket-policy.json.in" "$work/policy.json"
if [ -n "$PREFIX" ]; then
    # The bridge's site includes this file; it is the only nginx file of the
    # archive's that names the host's paths.
    { echo "# The replay archive under $PREFIX/ on $HOST (archive-site/deploy/setup.sh)."; cat "$work/locations"; } > "$work/nginx-file"
    NGINX_FILE=$SNIPPET
else
    render "$SRC/nginx/archive.conf.in" "$work/site"
    sed -e "/@LOCATIONS@/{r $work/locations" -e 'd}' "$work/site" > "$work/nginx-file"
    NGINX_FILE=$SITE
fi

# An nginx file that is already there and differs is the operator's: replaced
# only with --replace-site.
if [ -e "$NGINX_FILE" ] && ! cmp -s "$work/nginx-file" "$NGINX_FILE" && [ -z "$REPLACE_SITE" ]; then
    echo "$NGINX_FILE exists and differs from the one this script writes. Look at it, then run again with --replace-site." >&2
    exit 1
fi
conf=$(nginx -T 2>/dev/null) || { echo "nginx -T fails; fix the nginx configuration first." >&2; exit 1; }
if [ -n "$PREFIX" ]; then
    # The host is the bridge's: its enabled site must already include the
    # snippet, or nothing here would be served.
    if ! grep -qF "$INCLUDE_LINE" "$ENABLED" 2>/dev/null; then
        echo "$ENABLED does not contain: $INCLUDE_LINE" >&2
        echo "Run the bridge's setup.sh first (a bridge.conf.in that has that line), then this script again." >&2
        exit 1
    fi
else
    if [ -e "$ENABLED" ] || [ -L "$ENABLED" ]; then
        if [ "$(readlink -f "$ENABLED")" != "$SITE" ] && [ -z "$REPLACE_SITE" ]; then
            echo "$ENABLED is not a link to $SITE. Look at it, then run again with --replace-site." >&2
            exit 1
        fi
    fi
    # Who names the host is read from nginx's own view of the whole
    # configuration (nginx -T prints every included file under a
    # "# configuration file" line), so a server_name split over lines, or in
    # any included file, is found. Only this script's own site file is left
    # out. A prefix install shares the bridge's host and skips this.
    OWNERS=$(cat <<'PY'
import re, sys
host, *own = sys.argv[1:]
text = sys.stdin.read()
found = []
parts = re.split(r"^# configuration file (.*):$", text, flags=re.M)
for path, body in zip(parts[1::2], parts[2::2]):
    if path in own:
        continue
    body = re.sub(r"#[^\n]*", "", body)
    for names in re.findall(r"\bserver_name\s+([^;]*);", body):
        if host in [name.strip("\"'").lower() for name in names.split()]:
            found.append(path)
            break
print("\n".join(found))
PY
)
    others=$(printf '%s\n' "$conf" | python3 -c "$OWNERS" "$HOST" "$ENABLED" "$SITE" "$ZONEFILE")
    if [ -n "$others" ]; then
        echo "Another nginx file already serves $HOST:" >&2
        echo "$others" >&2
        echo "Disable or remove it yourself; this script does not touch other sites." >&2
        exit 1
    fi
fi

if ! id "$NAME" >/dev/null 2>&1; then
    useradd --system --home-dir "$STATE" --no-create-home --shell /usr/sbin/nologin "$NAME"
fi
install -d -o root -g root -m 0755 "$LIB"
install -d -o "$NAME" -g "$NAME" -m 0700 "$STATE" "$STATE/data"
install -d -o root -g root -m 0700 "$ETC"
# Credentials: made once, kept after. The secret key is the bridge
# archive_secret_key (README.md); this script never prints it.
if [ ! -f "$ENVFILE" ]; then
    (
        umask 0077
        printf 'RUSTFS_ACCESS_KEY=emberarchive\nRUSTFS_SECRET_KEY=%s\n' "$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')" > "$ENVFILE.new"
    )
    mv -f "$ENVFILE.new" "$ENVFILE"
    echo "Wrote $ENVFILE (new credentials)."
fi
chown root:root "$ENVFILE"
chmod 0600 "$ENVFILE"

# Everything the install replaces, in the order it is put back: the binary,
# the unit, the nginx files and enabled link, and the page. TRACKED_NEW pairs
# each target with the file that replaces it.
install -d -o root -g root -m 0755 "$WEBROOT/assets" "$WEBROOT/assets/fonts"
TRACKED=("$LIB/rustfs" "$UNIT" "$HEADERS" "$ZONEFILE" "$NGINX_FILE")
[ -n "$PREFIX" ] || TRACKED+=("$ENABLED")
declare -A SOURCE=(["$LIB/rustfs"]="$BINARY" ["$UNIT"]="$work/unit" ["$HEADERS"]="$SRC/nginx/ember-archive-headers.conf"
    ["$ZONEFILE"]="$work/zone" ["$NGINX_FILE"]="$work/nginx-file")
for file in $BORROWED; do TRACKED+=("$WEBROOT/assets/$file"); SOURCE["$WEBROOT/assets/$file"]="$SHORT/$file"; done
for file in $(printf '%s\n' $PAGE | sort -r); do TRACKED+=("$WEBROOT/$file"); SOURCE["$WEBROOT/$file"]="$SITEDIR/$file"; done
# The page's files go in with each address it loads them by naming the file's
# own hash (?v=), so a browser that kept older files (assets are cached for an
# hour) fetches the new ones with the new page instead of mixing the two. The
# borrowed images and flag index first, then the stylesheet and script that
# name them, then the page that names those.
version() { sha256sum "$1" | cut -c1-12; }
mkdir -p "$work/site/assets"
sed -e "s|url(faces.webp)|url(faces.webp?v=$(version "$SHORT/faces.webp"))|" \
    -e "s|url(flags.png)|url(flags.png?v=$(version "$SHORT/flags.png"))|" \
    "$SITEDIR/assets/archive.css" > "$work/site/assets/archive.css"
sed -e "s|'assets/flags.json'|'assets/flags.json?v=$(version "$SHORT/flags.json")'|" \
    "$SITEDIR/assets/archive.js" > "$work/site/assets/archive.js"
sed -e "s|href=\"assets/archive.css\"|href=\"assets/archive.css?v=$(version "$work/site/assets/archive.css")\"|" \
    -e "s|src=\"assets/archive.js\"|src=\"assets/archive.js?v=$(version "$work/site/assets/archive.js")\"|" \
    "$SITEDIR/index.html" > "$work/site/index.html"
for file in $PAGE; do
    cmp -s "$SITEDIR/$file" "$work/site/$file" && { echo "$file names no versioned asset; the page and setup.sh disagree." >&2; exit 1; }
    SOURCE["$WEBROOT/$file"]="$work/site/$file"
done
# A random marker per install, served as install.txt. The final check asks the
# host for this exact marker, which an older install on the name cannot have.
INSTALL_MARKER=$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')
printf '%s\n' "$INSTALL_MARKER" > "$work/install.txt"
TRACKED+=("$WEBROOT/install.txt"); SOURCE["$WEBROOT/install.txt"]="$work/install.txt"

# The deployment is a transaction. The predecessor of every tracked path is
# copied to $backup/old (or noted absent) before anything moves. From then
# until the last check passes, any way out puts them all back and restarts the
# previous rustfs; an exit before that touches nothing.
had_rustfs=
[ ! -e "$LIB/rustfs" ] || had_rustfs=1
mkdir "$backup/old"
for path in "${TRACKED[@]}"; do
    if [ -e "$path" ] || [ -L "$path" ]; then cp -a --parents "$path" "$backup/old"; fi
done
armed=
cleanup() {
    local status=$? path keep=
    if [ -n "$armed" ] && ! put_back; then keep=1; fi
    for path in "${TRACKED[@]}" "$ENVFILE"; do rm -f "$path.new"; done
    rm -rf "$work"
    if [ -z "$keep" ]; then
        rm -rf "$backup"
        return
    fi
    # The predecessor is the only copy left; it stays until the operator has
    # put it back.
    {
        echo "The previous files are kept in $backup/old (each path mirrors its place on the server,"
        echo "for example $backup/old$LIB/rustfs). Copy them back over the installed ones, then:"
        echo "  systemctl daemon-reload; systemctl restart $NAME; nginx -t && systemctl reload nginx"
    } >&2
    exit "$((status == 0 ? 1 : status))"
}
trap cleanup EXIT
# Rustfs answers on loopback (not 503, which it sends while it starts) while its unit is active. A new binary that
# starts and then dies leaves the unit not active, so this does not pass on
# the previous process either: the restart stopped it. READY_TRIES (seconds)
# is for tests.
READY_TRIES=${READY_TRIES:-30}
wait_ready() {
    local attempt code
    for attempt in $(seq 1 "$READY_TRIES"); do
        if systemctl is-active --quiet "$NAME"; then
            code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 "http://127.0.0.1:$S3_PORT/" || :)
            if [ "$code" != 000 ] && [ "$code" != 503 ]; then return 0; fi
        fi
        sleep 1
    done
    return 1
}
# Put back what was there before; a file goes back by rename, because the
# running binary may be the new one. Paths with no predecessor are removed.
put_back() {
    set +e
    local path failed=
    armed=
    echo "The install failed; putting the previous files back." >&2
    for path in "${TRACKED[@]}"; do
        if [ -e "$backup/old$path" ] || [ -L "$backup/old$path" ]; then
            cp -a "$backup/old$path" "$path.new" && mv -f "$path.new" "$path" || failed=1
        else
            rm -f "$path"
        fi
    done
    systemctl daemon-reload
    if [ -n "$had_rustfs" ]; then
        systemctl reset-failed "$NAME" || :
        systemctl restart "$NAME" || failed=1
        wait_ready || failed=1
    else
        systemctl stop "$NAME" || :
        systemctl disable "$NAME" >/dev/null 2>&1 || :
    fi
    if nginx -t; then systemctl reload nginx || failed=1; else failed=1; fi
    if [ -n "$failed" ]; then
        echo "Putting the previous files back did not finish cleanly; the installation may be half changed. Check systemctl status $NAME and nginx -t." >&2
        return 1
    elif [ -n "$had_rustfs" ]; then
        echo "Put back the previous rustfs, unit, nginx files and page; the previous rustfs is running again." >&2
    else
        echo "Removed what this run installed (there was no previous install)." >&2
    fi
    return 0
}

# Stage every replacement beside its target. Nothing is renamed yet.
install -d -o www-data -g www-data -m 0700 "/var/lib/nginx/$ZONE"
for path in "${TRACKED[@]}"; do
    [ -n "${SOURCE[$path]:-}" ] || continue
    mode=0644; [ "$path" != "$LIB/rustfs" ] || mode=0755
    install -o root -g root -m "$mode" "${SOURCE[$path]}" "$path.new"
done

armed=1
# rustfs: the binary and the unit, and a restart only when one changed.
changed=
for path in "$LIB/rustfs" "$UNIT"; do
    if cmp -s "$path.new" "$path"; then rm -f "$path.new"; else mv -f "$path.new" "$path"; changed=1; fi
done
systemctl daemon-reload
systemctl enable "$NAME" >/dev/null
if [ -n "$changed" ] || ! systemctl is-active --quiet "$NAME"; then
    systemctl reset-failed "$NAME" || :
    systemctl restart "$NAME"
fi
if ! wait_ready; then
    echo "rustfs did not answer on 127.0.0.1:$S3_PORT. See: sudo journalctl -u $NAME -n 40" >&2
    exit 1
fi
bash "$SRC/bucket.sh" "$ENVFILE" "http://127.0.0.1:$S3_PORT" "$BUCKET" "$work/policy.json"

# nginx: the new files in place (and the enabled link, with a site of its own), and a check.
for path in "$HEADERS" "$ZONEFILE" "$NGINX_FILE"; do mv -f "$path.new" "$path"; done
[ -n "$PREFIX" ] || ln -sfn "$SITE" "$ENABLED"
if ! nginx -t; then
    echo "nginx rejected the new configuration." >&2
    exit 1
fi

# The page, once nginx has accepted the site: fonts and emblem first, then the
# page files, so a page never names a file that is not there yet.
for path in "${TRACKED[@]}"; do
    case "$path" in "$WEBROOT"/*) mv -f "$path.new" "$path" ;; esac
done
systemctl reload nginx || systemctl restart nginx

# What answers on the name must be this install: install.txt holds this run's
# marker, then the page, the bucket proxy's refusal to list, and a listing
# that is JSON with games when it answers 200.
# CHECK_CURL_OPTS is for tests.
echo
echo "=== $HOST ==="
systemctl is-active "$NAME"
resolve=(--resolve "$HOST:443:127.0.0.1")
page_ok=
for attempt in 1 2 3 4 5; do
    served=$(curl -fsS --max-time 10 ${CHECK_CURL_OPTS:-} "${resolve[@]}" "$BASE/install.txt" 2>/dev/null || :)
    if [ "$served" = "$INSTALL_MARKER" ] &&
        curl -fsS --max-time 10 ${CHECK_CURL_OPTS:-} "${resolve[@]}" "$BASE/" 2>/dev/null | grep -q '<title>Ember replay archive</title>'; then
        page_ok=1; break
    fi
    sleep 1
done
if [ -z "$page_ok" ]; then
    echo "$BASE/ does not serve this install (its install.txt is not $INSTALL_MARKER); another site or an older install answers on the name." >&2
    exit 1
fi
echo "page: ok (install $INSTALL_MARKER)"
code=$(curl -sS -o /dev/null -w '%{http_code}' --max-time 10 ${CHECK_CURL_OPTS:-} "${resolve[@]}" "$BASE/$BUCKET/" || :)
if [ "$code" != 404 ]; then
    echo "bucket listing through nginx answered $code, expected 404." >&2
    exit 1
fi
echo "bucket listing through nginx: refused (404)"
# The listing through nginx must be the bridge's own answer: a bridge that
# lists games on loopback must list them here too (a wrong upstream path
# reads as 404 here); a bridge without its archive block yet answers 404 on
# both, which is fine until its archive is turned on.
listing=$(mktemp)
direct=$(curl -sS -o /dev/null -w '%{http_code}' --max-time 10 "http://127.0.0.1:$BRIDGE_PORT/v1/archive" || :)
code=$(curl -sS -o "$listing" -w '%{http_code}' --max-time 10 ${CHECK_CURL_OPTS:-} "${resolve[@]}" "$BASE/v1/archive" || :)
# Either probe that got no HTTP answer at all (curl's 000) fails, even if both did.
if [ "$direct" = 000 ] || [ "$code" = 000 ]; then
    rm -f "$listing"
    echo "The listing gave no answer (the bridge on loopback: $direct, $BASE/v1/archive: $code)." >&2
    exit 1
fi
if [ "$direct" = 200 ]; then
    if [ "$code" != 200 ] || ! grep -q '"games"' "$listing"; then
        rm -f "$listing"
        echo "The bridge lists games on loopback, but $BASE/v1/archive answered $code without them." >&2
        exit 1
    fi
    echo "listing: ok"
elif [ "$code" = "$direct" ]; then
    echo "listing answered $code, as the bridge does; it needs its archive block and secret (README.md)."
else
    rm -f "$listing"
    echo "The bridge answers $direct on loopback, but $BASE/v1/archive answered $code." >&2
    exit 1
fi
rm -f "$listing"
armed=
echo "=== done ==="
