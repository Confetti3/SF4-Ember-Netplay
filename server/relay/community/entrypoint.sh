#!/bin/bash
# Starts iroh-relay behind a reverse proxy that ends HTTPS for RELAY_HOSTNAME.
# The relay serves plain HTTP for the proxy and answers QUIC address discovery
# itself, with the certificate the proxy already holds for that name: found
# here under the mounted Let's Encrypt folder and re-read by the relay once a
# day, so renewals need no restart.
#
#   ember-relay          write the config and run the relay
#   ember-relay health   exit 0 when the relay answers /ping (health check)
set -euo pipefail

PORT=${RELAY_HTTP_PORT:-47895}
QUIC_PORT=${RELAY_QUIC_PORT:-7842}
CERT_ROOT=${RELAY_CERT_ROOT:-/etc/letsencrypt}
RUN_DIR=${RELAY_RUN_DIR:-/run/ember-relay}
BINARY=${RELAY_BINARY:-iroh-relay}

if [ "${1:-}" = health ]; then
    exec curl -fsS --max-time 4 -o /dev/null "http://127.0.0.1:$PORT/ping"
fi

fail() { echo "ember-relay: $*" >&2; exit 1; }

# A .env edited on Windows leaves a carriage return on each value.
RELAY_HOSTNAME=${RELAY_HOSTNAME:-}
RELAY_HOSTNAME=${RELAY_HOSTNAME%$'\r'}
RELAY_CERT_NAME=${RELAY_CERT_NAME:-}
RELAY_CERT_NAME=${RELAY_CERT_NAME%$'\r'}
[ -n "$RELAY_HOSTNAME" ] || fail "RELAY_HOSTNAME is not set (see .env.example)"
for port in "$PORT" "$QUIC_PORT"; do
    case $port in ''|*[!0-9]*) fail "not a port number: $port" ;; esac
done
[ -d "$CERT_ROOT/live" ] \
    || fail "no $CERT_ROOT/live: NPM_LETSENCRYPT_DIR must be the proxy's letsencrypt folder"

# Unexpired and issued for the relay's name (wildcards count).
covers() {
    openssl x509 -in "$1" -noout -checkend 0 >/dev/null 2>&1 \
        && openssl x509 -in "$1" -noout -checkhost "$RELAY_HOSTNAME" 2>/dev/null \
            | grep -q 'does match certificate'
}

if [ -n "$RELAY_CERT_NAME" ]; then
    cert_dir=$CERT_ROOT/live/$RELAY_CERT_NAME
    [ -f "$cert_dir/fullchain.pem" ] || fail "no certificate at live/$RELAY_CERT_NAME"
    covers "$cert_dir/fullchain.pem" \
        || fail "live/$RELAY_CERT_NAME is expired or not for $RELAY_HOSTNAME"
else
    # The proxy keeps one folder per certificate (npm-1, npm-2, ...). Of those
    # for this name, take the one that expires last.
    cert_dir=
    latest=0
    for candidate in "$CERT_ROOT"/live/*/; do
        chain=${candidate}fullchain.pem
        [ -f "$chain" ] && covers "$chain" || continue
        expires=$(date -d "$(openssl x509 -in "$chain" -noout -enddate | cut -d= -f2)" +%s)
        if [ "$expires" -gt "$latest" ]; then
            latest=$expires
            cert_dir=${candidate%/}
        fi
    done
    [ -n "$cert_dir" ] || fail "no valid certificate for $RELAY_HOSTNAME under $CERT_ROOT/live:" \
        "request one for that name in the proxy first"
fi
[ -r "$cert_dir/privkey.pem" ] || fail "cannot read $cert_dir/privkey.pem"
echo "ember-relay: $RELAY_HOSTNAME with the certificate in live/$(basename "$cert_dir")" >&2

mkdir -p "$RUN_DIR"
cat > "$RUN_DIR/relay.toml" <<EOF
enable_relay = true
# Plain HTTP, IPv4 only: the proxy is the only caller. Never forward this
# port from the internet.
http_bind_addr = "0.0.0.0:$PORT"
enable_quic_addr_discovery = true
enable_metrics = false

[tls]
cert_mode = "Reloading"
manual_cert_path = "$cert_dir/fullchain.pem"
manual_key_path = "$cert_dir/privkey.pem"
quic_bind_addr = "[::]:$QUIC_PORT"

# A relayed match sends a few KB/s per player; this only stops one client
# from filling the uplink.
[limits.client.rx]
bytes_per_second = ${RELAY_CLIENT_BYTES_PER_SECOND:-524288}
max_burst_bytes = ${RELAY_CLIENT_BURST_BYTES:-2097152}
EOF

# --dev only keeps TLS off the HTTP port; QUIC still uses the certificate.
exec "$BINARY" --dev --config-path "$RUN_DIR/relay.toml"
