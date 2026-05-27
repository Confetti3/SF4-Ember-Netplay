#!/usr/bin/env bash
# Lock down VPS firewall: HTTPS + game relay ports only; block broker/dashboard/manager HTTP.
# Idempotent — safe to re-run after changing .env port ranges.
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
MAX_ROOMS="${MAX_ROOMS:-20}"
NAT_PROBE_PORT="${NAT_PROBE_PORT:-8790}"

SESSION_PORT_END=$((RELAY_PORT_BASE + MAX_ROOMS - 1))
GGPO_PORT_END=$((GGPO_UDP_PORT_BASE + MAX_ROOMS - 1))

if ! command -v ufw >/dev/null 2>&1; then
  echo "ufw not installed — skipping firewall hardening."
  exit 0
fi

echo "Applying SF4e UFW rules (session ${RELAY_PORT_BASE}-${SESSION_PORT_END}, ggpo udp ${GGPO_UDP_PORT_BASE}-${GGPO_PORT_END})..."

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
