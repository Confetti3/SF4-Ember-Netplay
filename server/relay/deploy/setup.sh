#!/usr/bin/env bash
# Installs or updates Ember's iroh relay at relay.embernetplay.link.
# Stage this folder as ~/ember-relay with the binary at bin/iroh-relay and its
# SHA-256 in bin/iroh-relay.sha256, then run: sudo bash ~/ember-relay/setup.sh
#
# All or nothing: from the first change on, any failure (nginx refusing the
# site, the relay failing to start or a check failing) puts the previous
# binary, config, unit and site back and returns the service to the state it
# was in (enabled or not, running or not).
set -Eeuo pipefail  # -E: functions inherit the ERR trap

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

# What is installed now. A path that did not exist is recorded as absent so a
# rollback removes it again.
BACKUP=$(mktemp -d /root/ember-relay-backup.XXXXXX)
backup() { if [ -e "$1" ]; then cp -a "$1" "$BACKUP/$2"; else touch "$BACKUP/$2.absent"; fi; }
backup "$LIB/iroh-relay" binary
backup "$ETC/relay.toml" config
backup "$UNIT" unit
backup "$SITE" site
[ -L "$ENABLED" ] && touch "$BACKUP/site.enabled"
systemctl is-enabled --quiet ember-relay.service 2>/dev/null && touch "$BACKUP/service.enabled"
systemctl is-active --quiet ember-relay.service 2>/dev/null && touch "$BACKUP/service.active"

# Put a file in place by a staged copy and a rename, so a running binary is
# replaced rather than written to (a write fails with ETXTBSY).
place() { # place <source> <target> <mode>
    install -o root -g root -m "$3" "$1" "$2.new"
    mv -f "$2.new" "$2"
}
restore() { # restore <name> <target> <mode>
    if [ -e "$BACKUP/$1.absent" ]; then rm -f "$2"; else place "$BACKUP/$1" "$2" "$3"; fi
}

rollback() {
    trap - ERR
    set +e
    echo "ember-relay: rolling back to what was installed before" >&2
    journalctl -u ember-relay.service --no-pager -n 20 >&2
    systemctl stop ember-relay.service
    restore binary "$LIB/iroh-relay" 0755
    restore config "$ETC/relay.toml" 0644
    restore unit "$UNIT" 0644
    restore site "$SITE" 0644
    if [ -e "$BACKUP/site.enabled" ]; then ln -sf "$SITE" "$ENABLED"; else rm -f "$ENABLED"; fi
    systemctl daemon-reload
    if [ -e "$BACKUP/service.enabled" ]; then
        systemctl enable ember-relay.service >/dev/null
    else
        systemctl disable ember-relay.service >/dev/null 2>&1
    fi
    [ -e "$BACKUP/service.active" ] && systemctl start ember-relay.service
    if nginx -t 2>/dev/null; then systemctl reload nginx; else echo "nginx -t still fails after rollback" >&2; fi
    echo "ember-relay: the previous install is back (backup kept in $BACKUP)." >&2
    exit 1
}
trap rollback ERR

install -d -o root -g root -m 0755 "$LIB" "$ETC"
place "$SRC/bin/iroh-relay" "$LIB/iroh-relay" 0755
place "$SRC/relay.toml" "$ETC/relay.toml" 0644
place "$SRC/ember-relay.service" "$UNIT" 0644
place "$SRC/$HOST" "$SITE" 0644
ln -sf "$SITE" "$ENABLED"
nginx -t

# QUIC address discovery answers on UDP 7842 directly (the IONOS Cloud Panel
# firewall must allow it too).
if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q '^Status: active'; then
    ufw allow "$QUIC_PORT/udp" comment 'ember-relay QUIC address discovery' >/dev/null
    ufw status | grep -qF "$QUIC_PORT/udp"
    echo "ufw: UDP $QUIC_PORT allowed"
fi

systemctl daemon-reload
systemctl enable ember-relay.service >/dev/null
systemctl restart ember-relay.service
systemctl reload nginx

# Checks, each bounded. A full relay exchange and QUIC address discovery are
# checked from a client: cargo test --lib live_relays -- --ignored.
CURL=(curl -fsS --max-time 5 -o /dev/null)
for _ in $(seq 1 20); do
    "${CURL[@]}" "http://$HTTP/ping" 2>/dev/null && break
    sleep 0.5
done
"${CURL[@]}" "http://$HTTP/ping"
"${CURL[@]}" --resolve "$HOST:443:127.0.0.1" "https://$HOST/ping"

# A complete relay WebSocket upgrade through nginx: 101, the accept value for
# this key (RFC 6455's example) and a relay subprotocol back. curl stops
# waiting after 3 s on the open socket, which is expected.
headers=$(curl -sS --http1.1 --max-time 3 -o /dev/null -D - \
    -H 'Connection: Upgrade' -H 'Upgrade: websocket' -H 'Sec-WebSocket-Version: 13' \
    -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' \
    -H 'Sec-WebSocket-Protocol: iroh-relay-v2, iroh-relay-v1' \
    --resolve "$HOST:443:127.0.0.1" "https://$HOST/relay" 2>/dev/null | tr -d '\r' || true)
printf '%s\n' "$headers" | head -n 1 | grep -q '^HTTP/1.1 101 ' \
    || { echo "nginx: /relay upgrade answered: $(printf '%s\n' "$headers" | head -n 1)" >&2; false; }
printf '%s\n' "$headers" | grep -qi '^sec-websocket-accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=$' \
    || { echo "nginx: /relay upgrade has no matching Sec-WebSocket-Accept" >&2; false; }
printf '%s\n' "$headers" | grep -qiE '^sec-websocket-protocol: iroh-relay-v[12]$' \
    || { echo "nginx: /relay upgrade negotiated no relay subprotocol" >&2; false; }

response=$(curl -fsS --max-time 5 -o /dev/null -D - -H "X-Iroh-Challenge: ts_$HOST" \
    --resolve "$HOST:80:127.0.0.1" "http://$HOST/generate_204" | tr -d '\r' \
    | sed -n 's/^[Xx]-[Ii]roh-[Rr]esponse: //p')
[ "$response" = "response ts_$HOST" ] \
    || { echo "nginx: captive-portal check answered '$response'" >&2; false; }
ss -lun | grep -q ":$QUIC_PORT\b" \
    || { echo "relay: nothing listens on UDP $QUIC_PORT" >&2; false; }

trap - ERR
rm -rf "$BACKUP"
systemctl --no-pager --lines=0 status ember-relay.service | head -n 4
echo "ember-relay: up at https://$HOST/ (QUIC on UDP $QUIC_PORT)."
