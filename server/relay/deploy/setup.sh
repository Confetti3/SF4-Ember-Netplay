#!/usr/bin/env bash
# Installs or updates Ember's iroh relay at relay.embernetplay.link.
# Stage this folder as ~/ember-relay with the binary at bin/iroh-relay and its
# SHA-256 in bin/iroh-relay.sha256, then run: sudo bash ~/ember-relay/setup.sh
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"
HOST=relay.embernetplay.link
LIB=/usr/local/lib/ember-relay
ETC=/etc/ember-relay
SITE=/etc/nginx/sites-available/$HOST
CERT=/etc/ssl/certs/embernetplay.link.fullchain.pem
KEY=/etc/ssl/private/embernetplay.link.key
QUIC_PORT=7842

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo." >&2
    exit 1
fi
for file in "$CERT" "$KEY"; do
    [ -f "$file" ] || { echo "missing $file" >&2; exit 1; }
done

# The binary must be the one that was staged with it.
(cd "$SRC/bin" && sha256sum --check --quiet iroh-relay.sha256)
"$SRC/bin/iroh-relay" --version

install -d -o root -g root -m 0755 "$LIB" "$ETC"
install -o root -g root -m 0755 "$SRC/bin/iroh-relay" "$LIB/iroh-relay.new"
mv -f "$LIB/iroh-relay.new" "$LIB/iroh-relay"
install -o root -g root -m 0644 "$SRC/relay.toml" "$ETC/relay.toml"
install -o root -g root -m 0644 "$SRC/ember-relay.service" /etc/systemd/system/ember-relay.service

# nginx: keep the previous site to fall back to if the new one fails the test.
if [ -f "$SITE" ]; then cp -p "$SITE" "$SITE.prev"; fi
install -o root -g root -m 0644 "$SRC/$HOST" "$SITE"
ln -sf "$SITE" "/etc/nginx/sites-enabled/$HOST"
if ! nginx -t 2>/dev/null; then
    nginx -t || true
    if [ -f "$SITE.prev" ]; then mv -f "$SITE.prev" "$SITE"; else rm -f "$SITE" "/etc/nginx/sites-enabled/$HOST"; fi
    echo "nginx refused the relay site; the previous one is back." >&2
    exit 1
fi

# QUIC address discovery answers on UDP 7842 directly.
if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q '^Status: active'; then
    ufw allow "$QUIC_PORT/udp" comment 'ember-relay QUIC address discovery' >/dev/null
    ufw status | grep -qF "$QUIC_PORT/udp" || { echo "ufw: could not confirm UDP $QUIC_PORT" >&2; exit 1; }
    echo "ufw: UDP $QUIC_PORT allowed"
fi

systemctl daemon-reload
systemctl enable ember-relay.service >/dev/null
systemctl restart ember-relay.service
systemctl reload nginx

# Checks: the relay answers on its port, through nginx (HTTPS and the plain
# HTTP captive-portal path) and holds the QUIC port.
ok=1
for _ in $(seq 1 20); do
    curl -fsS -o /dev/null "http://127.0.0.1:47895/ping" 2>/dev/null && break
    sleep 0.5
done
curl -fsS -o /dev/null "http://127.0.0.1:47895/ping" || { echo "relay: no answer on 127.0.0.1:47895" >&2; ok=0; }
curl -fsS -o /dev/null --resolve "$HOST:443:127.0.0.1" "https://$HOST/ping" || { echo "nginx: no HTTPS answer for $HOST" >&2; ok=0; }
response=$(curl -fsS -o /dev/null -D - -H "X-Iroh-Challenge: ts_$HOST" --resolve "$HOST:80:127.0.0.1" "http://$HOST/generate_204" | tr -d '\r' | sed -n 's/^[Xx]-[Ii]roh-[Rr]esponse: //p')
[ "$response" = "response ts_$HOST" ] || { echo "nginx: captive-portal check answered '$response'" >&2; ok=0; }
ss -lun | grep -q ":$QUIC_PORT\b" || { echo "relay: nothing listens on UDP $QUIC_PORT" >&2; ok=0; }
systemctl --no-pager --lines=5 status ember-relay.service | head -n 12

if [ "$ok" -ne 1 ]; then
    echo "ember-relay: checks failed (journalctl -u ember-relay)" >&2
    exit 1
fi
echo "ember-relay: up at https://$HOST/ (QUIC on UDP $QUIC_PORT)."
echo "Remember: the IONOS Cloud Panel firewall must allow UDP $QUIC_PORT too."
