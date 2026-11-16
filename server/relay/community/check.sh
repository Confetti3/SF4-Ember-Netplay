#!/usr/bin/env bash
# Checks an Ember relay from the outside, the way a player's helper reaches
# it. Run it from a line outside the relay's own network (a phone hotspot
# will do), on Linux or WSL:
#
#   bash check.sh relay-eu.embernetplay.link
#
# It needs curl, ip (iproute2), and OpenSSL 3.2 or later for the QUIC check. Everything is
# checked over IPv4, and again over IPv6 when the name has an IPv6 address
# and this line has IPv6. A relay that passes still gets one more check from
# Katie before a build uses it: the helper's own live test
# (server/relay/README.md).
set -uo pipefail

HOST=${1:?usage: bash check.sh <relay host name> [quic port]}
QUIC_PORT=${2:-7842}
failed=0
pass() { echo "ok    $*"; }
fail() { echo "FAIL  $*"; failed=1; }
note() { echo "note  $*"; }

# The three things the helper does over HTTP, for one address family.
http_checks() { # http_checks <4|6>
    local family=$1 via="IPv$1" code headers status portal portal_status portal_answer challenge

    # HTTPS with a certificate curl trusts for this name.
    code=$(curl "-$family" -sS --max-time 8 -o /dev/null -w '%{http_code}' "https://$HOST/ping" 2>&1)
    if [ "$code" = 200 ]; then
        pass "$via: https://$HOST/ping answers 200 with a valid certificate"
    else
        fail "$via: https://$HOST/ping: $code (proxy host missing, certificate not issued, or the relay is down)"
    fi

    # A complete relay WebSocket upgrade: 101, the accept value for this key
    # (RFC 6455's example) and a relay subprotocol. curl gives up after 4 s
    # on the open socket, which is expected.
    headers=$(curl "-$family" -sS --http1.1 --max-time 4 -o /dev/null -D - \
        -H 'Connection: Upgrade' -H 'Upgrade: websocket' -H 'Sec-WebSocket-Version: 13' \
        -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' \
        -H 'Sec-WebSocket-Protocol: iroh-relay-v2, iroh-relay-v1' \
        "https://$HOST/relay" 2>/dev/null | tr -d '\r')
    status=$(printf '%s\n' "$headers" | head -n 1)
    if ! printf '%s\n' "$status" | grep -q '^HTTP/1.1 101 '; then
        fail "$via: /relay did not upgrade: '${status:-no answer}' (proxy host missing, or Websockets Support is off)"
    elif ! printf '%s\n' "$headers" | grep -qi '^sec-websocket-accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=$'; then
        fail "$via: /relay upgraded without the matching Sec-WebSocket-Accept"
    elif ! printf '%s\n' "$headers" | grep -qiE '^sec-websocket-protocol: iroh-relay-v[12]$'; then
        fail "$via: /relay upgraded without a relay subprotocol"
    else
        pass "$via: /relay upgrades to a relay WebSocket through the proxy"
    fi

    # iroh's captive-portal check is plain HTTP on port 80: it must reach the
    # relay (204 and the challenge echoed), not a redirect to HTTPS.
    challenge="ts_$HOST"
    portal=$(curl "-$family" -sS --max-time 8 -o /dev/null -D - -H "X-Iroh-Challenge: $challenge" \
        "http://$HOST/generate_204" 2>/dev/null | tr -d '\r')
    portal_status=$(printf '%s\n' "$portal" | head -n 1)
    portal_answer=$(printf '%s\n' "$portal" | sed -n 's/^[Xx]-[Ii]roh-[Rr]esponse: //p')
    if printf '%s\n' "$portal_status" | grep -q ' 204' && [ "$portal_answer" = "response $challenge" ]; then
        pass "$via: http://$HOST/generate_204 reaches the relay"
    elif printf '%s\n' "$portal_status" | grep -qE ' 30[1278]'; then
        fail "$via: http://$HOST/generate_204 redirects: turn Force SSL off for the proxy host"
    else
        fail "$via: http://$HOST/generate_204: '${portal_status:-no answer}'"
    fi
}

# QUIC address discovery on UDP, with the same certificate. Prints nothing;
# succeeds when the handshake verifies.
quic_answers() { # quic_answers <4|6>
    local quic
    quic=$( (sleep 5) | timeout 12 openssl s_client "-$1" -quic -alpn /iroh-qad/0 \
        -connect "$HOST:$QUIC_PORT" -servername "$HOST" \
        -verify_return_error -verify_hostname "$HOST" 2>&1)
    printf '%s\n' "$quic" | grep -q '^Verification: OK' \
        && printf '%s\n' "$quic" | grep -q 'Protocol *: *QUICv1'
}

quic_client=yes
case $(uname -s) in
    MINGW* | MSYS* | CYGWIN*)
        note "QUIC not checked: OpenSSL's QUIC client does not work from Git Bash; use Linux or WSL"
        quic_client=
        failed=1
        ;;
    *)
        if ! openssl s_client -help 2>&1 | grep -q -- '-quic'; then
            note "QUIC not checked: this OpenSSL has no QUIC client (3.2 or later needed)"
            quic_client=
            failed=1
        fi
        ;;
esac

http_checks 4
if [ -n "$quic_client" ]; then
    if quic_answers 4; then
        pass "IPv4: QUIC address discovery answers on UDP $QUIC_PORT with a valid certificate"
    else
        fail "IPv4: no QUIC answer on UDP $QUIC_PORT (router forward or firewall for UDP $QUIC_PORT," \
            "or the relay could not load the certificate: docker logs ember-relay)"
    fi
fi

# IPv6: a player whose line has IPv6 reaches the name's IPv6 address first.
curl -6 -s --max-time 8 -o /dev/null "https://$HOST/ping" 2>/dev/null
case $? in
    6) note "IPv6: $HOST has no IPv6 address (fine)" ;;
    *)
        if ! command -v ip >/dev/null 2>&1; then
            # Not knowing is not passing: the name advertises IPv6.
            note "IPv6 not checked: no 'ip' command here (package iproute2) to tell whether this line has IPv6"
            failed=1
        elif ! ip -6 route get 2001:4860:4860::8888 >/dev/null 2>&1; then
            note "IPv6: $HOST has an IPv6 address, but this line has no IPv6 to check it with"
        else
            http_checks 6
            # Without it players still learn their IPv4 address from the relay.
            if [ -n "$quic_client" ]; then
                if quic_answers 6; then
                    pass "IPv6: QUIC address discovery answers on UDP $QUIC_PORT"
                else
                    note "IPv6: no QUIC answer on UDP $QUIC_PORT (allow it in the router's IPv6 firewall)"
                fi
            fi
        fi
        ;;
esac

if [ "$failed" = 0 ]; then
    echo "All checks passed for $HOST."
else
    echo "Not ready: see above."
fi
exit "$failed"
