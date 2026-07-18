#!/usr/bin/env bash
# Install relay manager + broker VPS relay mode on Linux VPS.
set -euo pipefail

NODE_DIR="/opt/sf4e-node"
BROKER_DIR="/root/room-broker"
RELAY_DIR="/opt/sf4e-relay"
RELAY_SERVICE="/etc/systemd/system/sf4e-relay-manager.service"
BROKER_SERVICE="/etc/systemd/system/sf4e-broker.service"

mkdir -p "$RELAY_DIR/bin" "$BROKER_DIR"

if [[ ! -f "$RELAY_DIR/relay-manager.js" ]]; then
  echo "Missing $RELAY_DIR/relay-manager.js"
  exit 1
fi

if [[ ! -x "$RELAY_DIR/bin/sf4e-session-relay" ]]; then
  echo "WARNING: $RELAY_DIR/bin/sf4e-session-relay missing — run build-linux.sh first."
fi

if [[ ! -x "$NODE_DIR/bin/node" ]]; then
  echo "Run room-broker install-vps.sh first (installs Node to $NODE_DIR)."
  exit 1
fi

PUBLIC_IP="${RELAY_HOST:-$(curl -fsSL https://api.ipify.org 2>/dev/null || echo 127.0.0.1)}"

cat > "$RELAY_SERVICE" <<EOF
[Unit]
Description=SF4 Netplay Launcher VPS relay manager
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=$RELAY_DIR
Environment=RELAY_MANAGER_PORT=8788
Environment=RELAY_MANAGER_BIND=127.0.0.1
Environment=SF4E_SESSION_RELAY_BIN=$RELAY_DIR/bin/sf4e-session-relay
Environment=LD_LIBRARY_PATH=$RELAY_DIR/lib
Environment=RELAY_IDENTITY=relay-vps
ExecStart=$NODE_DIR/bin/node $RELAY_DIR/relay-manager.js
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

if [[ -f "$BROKER_DIR/.env" ]]; then
  grep -q '^FORCE_VPS_RELAY=' "$BROKER_DIR/.env" || echo "FORCE_VPS_RELAY=1" >> "$BROKER_DIR/.env"
  grep -q '^RELAY_MANAGER_URL=' "$BROKER_DIR/.env" || echo "RELAY_MANAGER_URL=http://127.0.0.1:8788" >> "$BROKER_DIR/.env"
  sed -i "s|^RELAY_HOST=.*|RELAY_HOST=${PUBLIC_IP}|" "$BROKER_DIR/.env" || echo "RELAY_HOST=${PUBLIC_IP}" >> "$BROKER_DIR/.env"
fi

systemctl daemon-reload
systemctl enable sf4e-relay-manager.service
systemctl restart sf4e-relay-manager.service
systemctl restart sf4e-broker.service 2>/dev/null || true

if command -v ufw >/dev/null 2>&1; then
  ufw allow OpenSSH 2>/dev/null || ufw allow 22/tcp 2>/dev/null || true
  ufw allow 8787/tcp 2>/dev/null || true
  ufw allow 23456:23475/tcp 2>/dev/null || true
  ufw allow 23456:23475/udp 2>/dev/null || true
  if ufw status | grep -q "Status: active"; then
    ufw reload 2>/dev/null || true
  else
    ufw --force enable 2>/dev/null || true
  fi
fi

sleep 2
systemctl is-active sf4e-relay-manager.service
curl -s http://127.0.0.1:8788/v1/health
echo
curl -s http://127.0.0.1:8787/v1/health
echo
