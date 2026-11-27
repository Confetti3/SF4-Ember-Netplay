#!/usr/bin/env bash
# Installs the Ember short-link service behind nginx on embernetplay.link.
# Run as: sudo bash ~/ember-short/setup.sh
#
# Expects, next to this script: bin/ember-short (built as katie with
# `cargo build --release` in ~/ember-short/src), ember-short.service,
# nginx/ and site/. site/ is the built site, not the repository's source
# folder: `python3 scripts/site/make.py` writes it as build/site, with a
# MANIFEST that lists every file in it. Running this again installs a newer
# binary or site.
#
# The page's match links (/m), public room links (/r) and getting-started
# steps (/start) need the Ember release that opens them and has Ember ID.
# Install a page with them once that release is the latest public one; until
# then they are for testers with the test build.
#
# What it changes:
#   /usr/local/lib/ember-short/ember-short      the service binary (root-owned)
#   /etc/systemd/system/ember-short.service     runs it as a transient user on 127.0.0.1:47810
#   /etc/nginx/conf.d/ember-short.conf          request rate zones for /s/, /rooms.json,
#                                               /rankings.json, /points.json, /account/ and /link/, the
#                                               five-second cache of the room list, the fifteen-second
#                                               cache of the class board and the thirty-second cache of
#                                               the points boards
#   /var/lib/nginx/ember-rooms/, ember-rankings/, ember-points/  those caches (owned by nginx's www-data)
#   /etc/nginx/snippets/ember-short.conf        /s/ proxy, the /j, /m, /r and /start page, the landing,
#                                               rooms, rankings and 404 pages, the guides (/docs) and
#                                               the frame data (/frame-data), the points, own card and
#                                               player card pages (/points, /me, /players), the
#                                               /rooms.json, /rankings.json, /points.json and /account/
#                                               proxies to the Ember bridge
#                                               (127.0.0.1:47820), the /link/ proxies to it and
#                                               to the staging bridge (127.0.0.1:47821), the
#                                               /rooms-sfv.json, /rankings-sfv.json and /link-sfv/
#                                               proxies to SFV Ember's bridge (127.0.0.1:47822) and
#                                               /assets/, without access logs
#   /etc/nginx/snippets/ember-short-headers.conf  HSTS, nosniff and no-referrer, which the server
#                                               and each of those locations include
#   /etc/nginx/sites-available/embernetplay.link  one include line in the 443 server
#   /var/www/embernetplay.link/open.html         the page room, match and public room links open
#   /var/www/embernetplay.link/index.html, rooms.html, rankings.html, points.html, me.html,
#                                               players.html, 404.html, favicon.ico, assets/
#                                               the landing page, the public rooms page, the
#                                               rankings, points and card pages, the 404 page and
#                                               their scripts, styles, fonts and images
#   /var/www/embernetplay.link/docs/, frame-data/  the guides with their pictures, and the frame
#                                               data pages
#   /usr/local/lib/ember-short/site.manifest     the files of the site this script installed, so the
#                                               next run can remove the ones a newer site dropped
# index.html replaces the placeholder ~/ember-web/setup.sh wrote, which that
# script leaves alone once a page is there. Files this script installed under
# earlier names are removed after nginx accepts the new configuration.
# Nothing is opened in ufw: the service listens on loopback only.
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"
DOMAIN=embernetplay.link
SITE=/etc/nginx/sites-available/$DOMAIN
ROOT=/var/www/$DOMAIN
LIB=/usr/local/lib/ember-short
INCLUDE="    include snippets/ember-short.conf;"

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo." >&2
    exit 1
fi
# The site's files, relative to site/ here and to $ROOT on the server: every
# line of the built site's MANIFEST. The pages nginx's locations name must be
# among them, so a folder that is not a built site is refused before anything
# changes. PAGES are the HTML files and ASSETS everything else.
INSTALLED=$LIB/site.manifest
if [ ! -f "$SRC/site/MANIFEST" ]; then
    echo "Missing $SRC/site/MANIFEST: site/ must be the built site (python3 scripts/site/make.py writes build/site)." >&2
    exit 1
fi
mapfile -t FILES < <(grep -v '^MANIFEST$' "$SRC/site/MANIFEST")
PAGES=()
ASSETS=()
for file in "${FILES[@]}"; do
    case "$file" in
        ''|/*|*..*|*[[:space:]]*) echo "MANIFEST names '$file', which is no file of the site." >&2; exit 1 ;;
        *.html) PAGES+=("$file") ;;
        *) ASSETS+=("$file") ;;
    esac
done
declare -A LISTED=()
for file in "${FILES[@]}"; do LISTED["$file"]=1; done
listed() {
    [[ -n ${LISTED[$1]-} ]]
}
for file in open.html index.html rooms.html rankings.html points.html me.html players.html 404.html docs.html frame-data.html favicon.ico assets/site.css; do
    if ! listed "$file"; then
        echo "The site's MANIFEST lacks $file: site/ must be the built site." >&2
        exit 1
    fi
done
for file in bin/ember-short ember-short.service nginx/ember-short-zone.conf \
    nginx/ember-short-locations.conf nginx/ember-short-headers.conf; do
    if [ ! -f "$SRC/$file" ]; then
        echo "Missing $SRC/$file" >&2
        exit 1
    fi
done
for file in "${FILES[@]}"; do
    if [ ! -f "$SRC/site/$file" ]; then
        echo "Missing $SRC/site/$file" >&2
        exit 1
    fi
done
if [ ! -f "$SITE" ]; then
    echo "Missing $SITE; run ~/ember-web/setup.sh first." >&2
    exit 1
fi

install -d -o root -g root -m 0755 "$LIB"
install -o root -g root -m 0755 "$SRC/bin/ember-short" "$LIB/ember-short.new"
mv -f "$LIB/ember-short.new" "$LIB/ember-short"
install -o root -g root -m 0644 "$SRC/ember-short.service" /etc/systemd/system/ember-short.service

# nginx: keep copies so a failed check puts everything back as it was.
backup=$(mktemp -d)
cp -a "$SITE" "$backup/site"
[ -f /etc/nginx/conf.d/ember-short.conf ] && cp -a /etc/nginx/conf.d/ember-short.conf "$backup/zone"
[ -f /etc/nginx/snippets/ember-short.conf ] && cp -a /etc/nginx/snippets/ember-short.conf "$backup/locations"
[ -f /etc/nginx/snippets/ember-short-headers.conf ] && cp -a /etc/nginx/snippets/ember-short-headers.conf "$backup/headers"
restore() {
    cp -a "$backup/site" "$SITE"
    if [ -f "$backup/zone" ]; then cp -a "$backup/zone" /etc/nginx/conf.d/ember-short.conf; else rm -f /etc/nginx/conf.d/ember-short.conf; fi
    if [ -f "$backup/locations" ]; then cp -a "$backup/locations" /etc/nginx/snippets/ember-short.conf; else rm -f /etc/nginx/snippets/ember-short.conf; fi
    if [ -f "$backup/headers" ]; then cp -a "$backup/headers" /etc/nginx/snippets/ember-short-headers.conf; else rm -f /etc/nginx/snippets/ember-short-headers.conf; fi
}

install -d -o www-data -g www-data -m 0700 /var/lib/nginx/ember-rooms /var/lib/nginx/ember-rankings /var/lib/nginx/ember-points
install -o root -g root -m 0644 "$SRC/nginx/ember-short-zone.conf" /etc/nginx/conf.d/ember-short.conf
install -o root -g root -m 0644 "$SRC/nginx/ember-short-locations.conf" /etc/nginx/snippets/ember-short.conf
install -o root -g root -m 0644 "$SRC/nginx/ember-short-headers.conf" /etc/nginx/snippets/ember-short-headers.conf

# One include line in the HTTPS server, before its catch-all location. The
# HTTP server only redirects and has no "location /", so the first match is
# the 443 block. An existing include is left alone.
if ! grep -q 'snippets/ember-short.conf' "$SITE"; then
    if ! grep -q '^[[:space:]]*location / {' "$SITE"; then
        echo "No 'location / {' in $SITE; add '$INCLUDE' to its 443 server by hand." >&2
        restore
        exit 1
    fi
    awk -v line="$INCLUDE" '!done && /^[[:space:]]*location \/ \{/ { print line; print ""; done = 1 } { print }' \
        "$backup/site" > "$backup/site.new"
    cat "$backup/site.new" > "$SITE"
fi

if ! nginx -t; then
    echo "nginx rejected the new configuration; restored the previous files." >&2
    restore
    nginx -t || true
    exit 1
fi

# The site's files only once nginx has accepted the routes they link to, so
# a rejected configuration leaves the pages as they were too. Every file is
# written beside its target first; if one cannot be, those copies go and the
# nginx files are put back. Then each is renamed over its target, assets first
# so a page never names one that is not there yet, and nginx never serves
# half a file.
unstage() {
    for file in "${ASSETS[@]}" "${PAGES[@]}"; do rm -f "$ROOT/$file.new"; done
}
for file in "${ASSETS[@]}" "${PAGES[@]}"; do
    if ! install -d -o root -g root -m 0755 "$(dirname "$ROOT/$file")" ||
        ! install -o root -g root -m 0644 "$SRC/site/$file" "$ROOT/$file.new"; then
        echo "Could not write $ROOT/$file; restored the previous nginx files." >&2
        unstage
        restore
        exit 1
    fi
done
for file in "${ASSETS[@]}" "${PAGES[@]}"; do
    mv -f "$ROOT/$file.new" "$ROOT/$file"
done
# Files the site before this one had and this one does not (a guide that was
# removed or renamed) go now that no installed page links to them.
if [ -f "$INSTALLED" ]; then
    while IFS= read -r file; do
        case "$file" in ''|/*|*..*) continue ;; esac
        listed "$file" || rm -f "$ROOT/$file"
    done < "$INSTALLED"
fi
printf '%s\n' "${FILES[@]}" > "$INSTALLED.new"
mv -f "$INSTALLED.new" "$INSTALLED"

systemctl daemon-reload
systemctl enable ember-short
systemctl restart ember-short
systemctl reload nginx || systemctl restart nginx
rm -rf "$backup"
# Files under their earlier names, removed only now: a failed run restores
# the earlier configuration, which still serves them. open.css became site.css.
rm -f "$ROOT/j.html" "$ROOT/assets/j.js" "$ROOT/assets/j.css" "$ROOT/assets/open.css"

echo
echo "=== ember-short ==="
systemctl is-active ember-short
for attempt in 1 2 3 4 5; do
    if curl -fsS http://127.0.0.1:47810/s/health >/dev/null 2>&1; then break; fi
    sleep 1
done
echo "service: $(curl -fsS http://127.0.0.1:47810/s/health || echo 'no answer')"
echo "public:  $(curl -fsS --resolve $DOMAIN:443:127.0.0.1 https://$DOMAIN/s/health || echo 'no answer')"
for page in '' rooms rooms.json rooms-sfv.json rooms.html rankings rankings.json rankings-sfv.json rankings.html points points.json 'points.json?fighter=20' 'points.json?fighter=44' points.html me me.html players account/me.json assets/points.js assets/rankings.js assets/game.js assets/sfv-ember-mark.png assets/og.jpg j j/K7QM-4XRT-9PZD v v/K7QM-4XRT-9PZD m r start open.html assets/site.css assets/fonts/inter-latin-400.woff2 \
    assets/ember-background.webp docs docs/ docs.html docs/install docs/install.html docs/search.json docs/no-such-guide frame-data frame-data/ryu assets/docs.css no-such-page; do
    echo "/$page:  $(curl -sS -o /dev/null -w '%{http_code} %{content_type}' --resolve $DOMAIN:443:127.0.0.1 https://$DOMAIN/$page)"
done
echo "=== done ==="
