#!/bin/bash
# Starts iroh-relay behind a reverse proxy that ends HTTPS for RELAY_HOSTNAME.
# The relay serves plain HTTP for the proxy and answers QUIC address discovery
# itself, with the certificate the proxy already holds for that name: the one
# Let's Encrypt folder RELAY_CERT_NAME, mounted read-only, which the relay
# re-reads once a day, so renewals need no restart.
#
#   ember-relay          write the config and run the relay
#   ember-relay health   exit 0 when the relay answers /ping (health check)
set -euo pipefail

PORT=${RELAY_HTTP_PORT:-47895}
QUIC_PORT=${RELAY_QUIC_PORT:-7842}
CERT_ROOT=${RELAY_CERT_ROOT:-/etc/letsencrypt}
RUN_DIR=${RELAY_RUN_DIR:-/run/ember-relay}
BINARY=${RELAY_BINARY:-iroh-relay}
RATE=${RELAY_CLIENT_BYTES_PER_SECOND:-524288}
BURST=${RELAY_CLIENT_BURST_BYTES:-2097152}

fail() { echo "ember-relay: $*" >&2; exit 1; }
number() { # number <what> <value> <max>
    case $2 in '' | *[!0-9]* | 0*) fail "$1 must be a whole number from 1 to $3, not '$2'" ;; esac
    [ "${#2}" -le 10 ] && [ "$2" -le "$3" ] || fail "$1 must be a whole number from 1 to $3, not '$2'"
}
number RELAY_HTTP_PORT "$PORT" 65535
number RELAY_QUIC_PORT "$QUIC_PORT" 65535

if [ "${1:-}" = health ]; then
    exec curl -fsS --max-time 4 -o /dev/null "http://127.0.0.1:$PORT/ping"
fi

number RELAY_CLIENT_BYTES_PER_SECOND "$RATE" 4294967295
number RELAY_CLIENT_BURST_BYTES "$BURST" 4294967295

# A .env edited on Windows leaves a carriage return on each value.
RELAY_HOSTNAME=${RELAY_HOSTNAME:-}
RELAY_HOSTNAME=${RELAY_HOSTNAME%$'\r'}
RELAY_CERT_NAME=${RELAY_CERT_NAME:-}
RELAY_CERT_NAME=${RELAY_CERT_NAME%$'\r'}
[ -n "$RELAY_HOSTNAME" ] || fail "RELAY_HOSTNAME is not set (see .env.example)"
[ -n "$RELAY_CERT_NAME" ] || fail "RELAY_CERT_NAME is not set (see .env.example)"
# The name becomes part of two paths in the relay's config.
case $RELAY_CERT_NAME in
    *[!A-Za-z0-9._-]* | .*) fail "RELAY_CERT_NAME is a folder name such as npm-12, not '$RELAY_CERT_NAME'" ;;
esac

cert=$CERT_ROOT/live/$RELAY_CERT_NAME/fullchain.pem
key=$CERT_ROOT/live/$RELAY_CERT_NAME/privkey.pem
[ -r "$cert" ] || fail "cannot read live/$RELAY_CERT_NAME/fullchain.pem" \
    "(wrong RELAY_CERT_NAME or NPM_LETSENCRYPT_DIR, or RELAY_USER cannot read it)"
[ -r "$key" ] || fail "cannot read live/$RELAY_CERT_NAME/privkey.pem (set RELAY_USER to the owner of that file)"
openssl x509 -in "$cert" -noout -checkend 0 >/dev/null 2>&1 \
    || fail "the certificate in live/$RELAY_CERT_NAME has expired"
openssl x509 -in "$cert" -noout -checkhost "$RELAY_HOSTNAME" 2>/dev/null \
    | grep -q 'does match certificate' \
    || fail "the certificate in live/$RELAY_CERT_NAME is not for $RELAY_HOSTNAME"
[ "$(openssl x509 -in "$cert" -noout -pubkey 2>/dev/null | openssl pkey -pubin -outform DER 2>/dev/null | sha256sum)" \
    = "$(openssl pkey -in "$key" -pubout -outform DER 2>/dev/null | sha256sum)" ] \
    || fail "live/$RELAY_CERT_NAME/privkey.pem is not the key of that certificate"
echo "ember-relay: $RELAY_HOSTNAME with the certificate in live/$RELAY_CERT_NAME" >&2

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
manual_cert_path = "$cert"
manual_key_path = "$key"
quic_bind_addr = "[::]:$QUIC_PORT"

# A relayed match sends a few KB/s per player; this only stops one client
# from filling the uplink.
[limits.client.rx]
bytes_per_second = $RATE
max_burst_bytes = $BURST
EOF

# --dev only keeps TLS off the HTTP port; QUIC still uses the certificate.
exec "$BINARY" --dev --config-path "$RUN_DIR/relay.toml"
