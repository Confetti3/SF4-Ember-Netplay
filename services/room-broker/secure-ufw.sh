#!/usr/bin/env bash
# Lock down VPS firewall: HTTPS + game relay ports only; block broker/dashboard/manager HTTP.
# Idempotent — safe to re-run after changing .env port ranges.
#
# NOTE: This only configures local UFW. Provider/cloud firewalls (IONOS, AWS SG, etc.)
# must be expanded separately to match the computed session and GGPO ranges.
set -euo pipefail

BROKER_DIR="${BROKER_DIR:-/root/room-broker}"
ENV_FILE="${ENV_FILE:-$BROKER_DIR/.env}"

if [[ -f "$ENV_FILE" ]]; then
  # shellcheck disable=SC1090
  set -a
  source "$ENV_FILE"
  set +a
fi

RELAY_PORT_BASE="${RELAY_PORT_BASE:-23456}"
GGPO_UDP_PORT_BASE="${GGPO_UDP_PORT_BASE:-24456}"
MAX_ROOMS="${MAX_ROOMS:-50}"
NAT_PROBE_PORT="${NAT_PROBE_PORT:-8790}"

die() {
  echo "ERROR: $*" >&2
  exit 1
}

is_positive_int() {
  [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

is_port_int() {
  [[ "$1" =~ ^[1-9][0-9]*$ ]] && (( "$1" <= 65535 ))
}

if ! is_positive_int "$MAX_ROOMS"; then
  die "MAX_ROOMS must be an integer >= 1 (got '${MAX_ROOMS}')"
fi
if ! is_port_int "$RELAY_PORT_BASE"; then
  die "RELAY_PORT_BASE must be an integer 1-65535 (got '${RELAY_PORT_BASE}')"
fi
if ! is_port_int "$GGPO_UDP_PORT_BASE"; then
  die "GGPO_UDP_PORT_BASE must be an integer 1-65535 (got '${GGPO_UDP_PORT_BASE}')"
fi
if ! is_port_int "$NAT_PROBE_PORT"; then
  die "NAT_PROBE_PORT must be an integer 1-65535 (got '${NAT_PROBE_PORT}')"
fi

SESSION_PORT_END=$((RELAY_PORT_BASE + MAX_ROOMS - 1))
GGPO_PORT_END=$((GGPO_UDP_PORT_BASE + MAX_ROOMS - 1))

if (( SESSION_PORT_END > 65535 )); then
  die "Session relay range exceeds 65535: ${RELAY_PORT_BASE}-${SESSION_PORT_END} (MAX_ROOMS=${MAX_ROOMS})"
fi
if (( GGPO_PORT_END > 65535 )); then
  die "GGPO UDP relay range exceeds 65535: ${GGPO_UDP_PORT_BASE}-${GGPO_PORT_END} (MAX_ROOMS=${MAX_ROOMS})"
fi
if (( RELAY_PORT_BASE <= GGPO_PORT_END && GGPO_UDP_PORT_BASE <= SESSION_PORT_END )); then
  die "Session relay range ${RELAY_PORT_BASE}-${SESSION_PORT_END} overlaps GGPO UDP range ${GGPO_UDP_PORT_BASE}-${GGPO_PORT_END}"
fi

if ! command -v ufw >/dev/null 2>&1; then
  echo "ufw not installed — skipping firewall hardening."
  exit 0
fi

echo "Applying SF4e UFW rules:"
echo "  Session relay TCP/UDP: ${RELAY_PORT_BASE}-${SESSION_PORT_END}"
echo "  GGPO relay UDP:        ${GGPO_UDP_PORT_BASE}-${GGPO_PORT_END}"
echo "  NAT probe UDP:         ${NAT_PROBE_PORT}"
echo "NOTE: Expand the same ranges in any provider/cloud firewall separately."

ufw --force reset
ufw default deny incoming
ufw default allow outgoing

ufw allow OpenSSH 2>/dev/null || ufw allow 22/tcp comment 'SSH'
ufw allow 443/tcp comment 'HTTPS Caddy'
ufw allow 80/tcp comment 'HTTP ACME + redirect'

# Session relay (GNS) — TCP + UDP
ufw allow "${RELAY_PORT_BASE}:${SESSION_PORT_END}/tcp" comment 'SF4e session relay'
ufw allow "${RELAY_PORT_BASE}:${SESSION_PORT_END}/udp" comment 'SF4e session relay'

# GGPO UDP relay (Phase 1+)
ufw allow "${GGPO_UDP_PORT_BASE}:${GGPO_PORT_END}/udp" comment 'SF4e GGPO UDP relay'

# NAT discovery for connect-plan (UDP only)
ufw allow "${NAT_PROBE_PORT}/udp" comment 'SF4e NAT probe'

# Explicitly do NOT open: 8787 broker, 8788 relay-manager, 8789 dashboard (localhost + Caddy only)

if ufw --force enable; then
  ufw reload
fi

echo "UFW status:"
ufw status numbered
