#!/usr/bin/env bash
# Installs or updates Ember's iroh relay at relay.embernetplay.link.
# Stage this folder as ~/ember-relay with the binary at bin/iroh-relay and its
# SHA-256 in bin/iroh-relay.sha256, then run: sudo bash ~/ember-relay/setup.sh
#
# All or nothing: the binary, config, unit and nginx site that were installed
# before are kept, and put back (and the old relay restarted) if nginx refuses
# the new site or the relay fails its checks.
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"
HOST=relay.embernetplay.link
LIB=/usr/local/lib/ember-relay
ETC=/etc/ember-relay
UNIT=/etc/systemd/system/ember-relay.service
SITE=/etc/nginx/sites-available/$HOST
ENABLED=/etc/nginx/sites-enabled/$HOST
CERT=/etc/ssl/certs/embernetplay.link.fullchain.pem
KEY=/etc/ssl/private/embernetplay.link.key
QUIC_PORT=7842
HTTP=127.0.0.1:47895

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo." >&2
    exit 1
fi
for file in "$CERT" "$KEY"; do
    [ -f "$file" ] || { echo "missing $file" >&2; exit 1; }
done

# The binary must be the one that was staged with it, and must run here.
(cd "$SRC/bin" && sha256sum --check --quiet iroh-relay.sha256)
"$SRC/bin/iroh-relay" --version

# Keep what is installed now. A path that did not exist is recorded as absent
# so a rollback removes it again.
BACKUP=$(mktemp -d /root/ember-relay-backup.XXXXXX)
backup() { if [ -e "$1" ]; then cp -a "$1" "$BACKUP/$2"; else touch "$BACKUP/$2.absent"; fi; }
backup "$LIB/iroh-relay" binary
backup "$ETC/relay.toml" config
backup "$UNIT" unit
backup "$SITE" site
if [ -L "$ENABLED" ]; then touch "$BACKUP/enabled"; fi
was_active=0
systemctl is-active --quiet ember-relay.service && was_active=1

restore_one() {
    if [ -e "$BACKUP/$1.absent" ]; then rm -f "$2"; else cp -a "$BACKUP/$1" "$2"; fi
}
rollback() {
    echo "ember-relay: rolling back to what was installed before" >&2
    restore_one binary "$LIB/iroh-relay"
    restore_one config "$ETC/relay.toml"
    restore_one unit "$UNIT"
    restore_one site "$SITE"
    if [ -e "$BACKUP/enabled" ]; then ln -sf "$SITE" "$ENABLED"; else rm -f "$ENABLED"; fi
    systemctl daemon-reload
    if [ "$was_active" -eq 1 ]; then
        systemctl restart ember-relay.service || true
    else
        systemctl disable --now ember-relay.service >/dev/null 2>&1 || true
    fi
    if nginx -t 2>/dev/null; then systemctl reload nginx; else echo "nginx -t still fails after rollback" >&2; fi
}

install -d -o root -g root -m 0755 "$LIB" "$ETC"
install -o root -g root -m 0755 "$SRC/bin/iroh-relay" "$LIB/iroh-relay.new"
mv -f "$LIB/iroh-relay.new" "$LIB/iroh-relay"
install -o root -g root -m 0644 "$SRC/relay.toml" "$ETC/relay.toml"
install -o root -g root -m 0644 "$SRC/ember-relay.service" "$UNIT"
install -o root -g root -m 0644 "$SRC/$HOST" "$SITE"
ln -sf "$SITE" "$ENABLED"

if ! nginx -t 2>/dev/null; then
    nginx -t || true
    rollback
    echo "nginx refused the relay site." >&2
    exit 1
fi

# QUIC address discovery answers on UDP 7842 directly (the IONOS Cloud Panel
# firewall must allow it too).
if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q '^Status: active'; then
    ufw allow "$QUIC_PORT/udp" comment 'ember-relay QUIC address discovery' >/dev/null
    ufw status | grep -qF "$QUIC_PORT/udp" || { echo "ufw: could not confirm UDP $QUIC_PORT" >&2; rollback; exit 1; }
    echo "ufw: UDP $QUIC_PORT allowed"
fi

systemctl daemon-reload
systemctl enable ember-relay.service >/dev/null
systemctl restart ember-relay.service
systemctl reload nginx

# Checks, each bounded: the relay answers on its port, through nginx (HTTPS,
# the WebSocket upgrade at /relay, and the plain HTTP captive-portal path),
# and holds the QUIC port. A full relay exchange and QUIC address discovery
# are checked from a client: cargo test --lib live_relays -- --ignored.
CURL=(curl -fsS --max-time 5 -o /dev/null)
ok=1
for _ in $(seq 1 20); do
    "${CURL[@]}" "http://$HTTP/ping" 2>/dev/null && break
    sleep 0.5
done
"${CURL[@]}" "http://$HTTP/ping" || { echo "relay: no answer on $HTTP" >&2; ok=0; }
"${CURL[@]}" --resolve "$HOST:443:127.0.0.1" "https://$HOST/ping" || { echo "nginx: no HTTPS answer for $HOST" >&2; ok=0; }
# A WebSocket upgrade without a relay handshake: the relay must answer it
# (anything but nginx's 404/502/503), proving nginx passes the upgrade on.
upgrade=$(curl -sS --max-time 5 -o /dev/null -w '%{http_code}' --http1.1 \
    -H 'Connection: Upgrade' -H 'Upgrade: websocket' -H 'Sec-WebSocket-Version: 13' \
    -H 'Sec-WebSocket-Key: ZW1iZXItcmVsYXktY2hlY2s=' \
    --resolve "$HOST:443:127.0.0.1" "https://$HOST/relay" || true)
case "$upgrade" in
    404|502|503|504|000) echo "nginx: /relay upgrade answered $upgrade" >&2; ok=0 ;;
    *) echo "relay: /relay upgrade answered $upgrade" ;;
esac
response=$(curl -fsS --max-time 5 -o /dev/null -D - -H "X-Iroh-Challenge: ts_$HOST" \
    --resolve "$HOST:80:127.0.0.1" "http://$HOST/generate_204" | tr -d '\r' \
    | sed -n 's/^[Xx]-[Ii]roh-[Rr]esponse: //p' || true)
[ "$response" = "response ts_$HOST" ] || { echo "nginx: captive-portal check answered '$response'" >&2; ok=0; }
ss -lun | grep -q ":$QUIC_PORT\b" || { echo "relay: nothing listens on UDP $QUIC_PORT" >&2; ok=0; }

if [ "$ok" -ne 1 ]; then
    journalctl -u ember-relay.service --no-pager -n 20 >&2 || true
    rollback
    echo "ember-relay: checks failed; the previous install is back." >&2
    exit 1
fi
rm -rf "$BACKUP"
systemctl --no-pager --lines=0 status ember-relay.service | head -n 4
echo "ember-relay: up at https://$HOST/ (QUIC on UDP $QUIC_PORT)."
