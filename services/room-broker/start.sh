#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [[ -f .env ]]; then
  set -a
  # shellcheck disable=SC1091
  source .env
  set +a
fi
export PORT="${PORT:-8787}"
export RELAY_HOST="${RELAY_HOST:-127.0.0.1}"
export RELAY_PORT_BASE="${RELAY_PORT_BASE:-23456}"
export MAX_ROOMS="${MAX_ROOMS:-50}"
export ROOM_LOBBY_IDLE_MS="${ROOM_LOBBY_IDLE_MS:-300000}"
export ROOM_OCCUPIED_IDLE_MS="${ROOM_OCCUPIED_IDLE_MS:-0}"
export ROOM_IDLE_MS="${ROOM_IDLE_MS:-$ROOM_OCCUPIED_IDLE_MS}"
exec node server.js
