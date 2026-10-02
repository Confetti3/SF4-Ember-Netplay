#!/usr/bin/env bash
# Installs the Ember short-link service behind nginx on embernetplay.link.
# Run as: sudo bash ~/ember-short/setup.sh
#
# Expects, next to this script: bin/ember-short (built as katie with
# `cargo build --release` in ~/ember-short/src), ember-short.service,
# nginx/ and site/. Running it again installs a newer binary or page.
#
# What it changes:
#   /usr/local/lib/ember-short/ember-short      the service binary (root-owned)
#   /etc/systemd/system/ember-short.service     runs it as a transient user on 127.0.0.1:47810
#   /etc/nginx/conf.d/ember-short.conf          request rate zone for /s/
#   /etc/nginx/snippets/ember-short.conf        /s/ proxy and the /j and /m page, without access logs
#   /etc/nginx/sites-available/embernetplay.link  one include line in the 443 server
#   /var/www/embernetplay.link/open.html, assets/  the page room and match links open
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
for file in bin/ember-short ember-short.service nginx/ember-short-zone.conf \
    nginx/ember-short-locations.conf site/open.html site/assets/open.js site/assets/open.css; do
    if [ ! -f "$SRC/$file" ]; then
        echo "Missing $SRC/$file" >&2
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

install -d -o root -g root -m 0755 "$ROOT/assets"
install -o root -g root -m 0644 "$SRC/site/open.html" "$ROOT/open.html"
install -o root -g root -m 0644 "$SRC/site/assets/open.js" "$ROOT/assets/open.js"
install -o root -g root -m 0644 "$SRC/site/assets/open.css" "$ROOT/assets/open.css"

# nginx: keep copies so a failed check puts everything back as it was.
backup=$(mktemp -d)
cp -a "$SITE" "$backup/site"
[ -f /etc/nginx/conf.d/ember-short.conf ] && cp -a /etc/nginx/conf.d/ember-short.conf "$backup/zone"
[ -f /etc/nginx/snippets/ember-short.conf ] && cp -a /etc/nginx/snippets/ember-short.conf "$backup/locations"
restore() {
    cp -a "$backup/site" "$SITE"
    if [ -f "$backup/zone" ]; then cp -a "$backup/zone" /etc/nginx/conf.d/ember-short.conf; else rm -f /etc/nginx/conf.d/ember-short.conf; fi
    if [ -f "$backup/locations" ]; then cp -a "$backup/locations" /etc/nginx/snippets/ember-short.conf; else rm -f /etc/nginx/snippets/ember-short.conf; fi
}

install -o root -g root -m 0644 "$SRC/nginx/ember-short-zone.conf" /etc/nginx/conf.d/ember-short.conf
install -o root -g root -m 0644 "$SRC/nginx/ember-short-locations.conf" /etc/nginx/snippets/ember-short.conf

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

systemctl daemon-reload
systemctl enable ember-short
systemctl restart ember-short
systemctl reload nginx || systemctl restart nginx
rm -rf "$backup"
# The page under its earlier name, removed only now: a failed run restores
# the earlier configuration, which still serves it.
rm -f "$ROOT/j.html" "$ROOT/assets/j.js" "$ROOT/assets/j.css"

echo
echo "=== ember-short ==="
systemctl is-active ember-short
for attempt in 1 2 3 4 5; do
    if curl -fsS http://127.0.0.1:47810/s/health >/dev/null 2>&1; then break; fi
    sleep 1
done
echo "service: $(curl -fsS http://127.0.0.1:47810/s/health || echo 'no answer')"
echo "public:  $(curl -fsS --resolve $DOMAIN:443:127.0.0.1 https://$DOMAIN/s/health || echo 'no answer')"
for page in j j/K7QM-4XRT-9PZD m start; do
    echo "/$page:  $(curl -fsS -o /dev/null -w '%{http_code}' --resolve $DOMAIN:443:127.0.0.1 https://$DOMAIN/$page)"
done
echo "=== done ==="
