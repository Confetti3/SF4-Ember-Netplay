#!/usr/bin/env bash
# Install SF4 Netplay Launcher room broker on a generic Linux VPS (Ubuntu/Debian).
# Usage (on the VPS, after copying server.js and .env.example to ~/room-broker/):
#   bash install-vps.sh

set -euo pipefail

NODE_DIR="/opt/sf4e-node"
BROKER_DIR="/root/room-broker"
SERVICE_PATH="/etc/systemd/system/sf4e-broker.service"

mkdir -p "$BROKER_DIR"

if [[ ! -f "$BROKER_DIR/server.js" ]]; then
  echo "Missing $BROKER_DIR/server.js — copy services/room-broker/server.js first."
  exit 1
fi

if [[ ! -f "$BROKER_DIR/.env" ]]; then
  cp -f "$BROKER_DIR/.env.example" "$BROKER_DIR/.env" 2>/dev/null || true
fi

if [[ -f "$BROKER_DIR/.env" ]]; then
  grep -q '^BROKER_BIND=' "$BROKER_DIR/.env" || echo "BROKER_BIND=127.0.0.1" >> "$BROKER_DIR/.env"
  grep -q '^BROKER_TRUST_PROXY=' "$BROKER_DIR/.env" || echo "BROKER_TRUST_PROXY=1" >> "$BROKER_DIR/.env"
fi

if [[ ! -x "$NODE_DIR/bin/node" ]]; then
  mkdir -p /tmp/sf4e-node
  cd /tmp/sf4e-node
  ARCH="$(uname -m)"
  case "$ARCH" in
    x86_64) NODE_PKG="node-v20.19.2-linux-x64.tar.xz" ;;
    aarch64) NODE_PKG="node-v20.19.2-linux-arm64.tar.xz" ;;
    *) echo "Unsupported arch: $ARCH"; exit 1 ;;
  esac
  curl -fsSL -o node.tar.xz "https://nodejs.org/dist/v20.19.2/$NODE_PKG"
  tar -xJf node.tar.xz
  rm -rf "$NODE_DIR"
  mv "${NODE_PKG%.tar.xz}" "$NODE_DIR"
fi

cat > "$SERVICE_PATH" <<EOF
[Unit]
Description=SF4 Netplay Launcher room broker
After=network-online.target sf4e-relay-manager.service
Wants=network-online.target sf4e-relay-manager.service

[Service]
Type=simple
WorkingDirectory=$BROKER_DIR
EnvironmentFile=-$BROKER_DIR/.env
ExecStart=$NODE_DIR/bin/node $BROKER_DIR/server.js
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

pkill -f "$BROKER_DIR/server.js" 2>/dev/null || true
sleep 1

systemctl daemon-reload
systemctl enable sf4e-broker.service
systemctl restart sf4e-broker.service

if command -v ufw >/dev/null 2>&1; then
  if [[ -x "$BROKER_DIR/secure-ufw.sh" ]]; then
    bash "$BROKER_DIR/secure-ufw.sh"
  else
    ufw allow OpenSSH 2>/dev/null || ufw allow 22/tcp 2>/dev/null || true
    ufw allow 443/tcp 2>/dev/null || true
    ufw allow 80/tcp 2>/dev/null || true
    ufw allow 23456:23475/tcp 2>/dev/null || true
    ufw allow 23456:23475/udp 2>/dev/null || true
    ufw allow 24456:24475/udp 2>/dev/null || true
    ufw allow 8790/udp 2>/dev/null || true
    if ufw status | grep -q "Status: active"; then
      ufw reload 2>/dev/null || true
    else
      ufw --force enable 2>/dev/null || true
    fi
  fi
fi

if [[ -x "$BROKER_DIR/install-vps-relay.sh" ]]; then
  bash "$BROKER_DIR/install-vps-relay.sh"
elif [[ -x "/opt/sf4e-relay/install-vps-relay.sh" ]]; then
  bash "/opt/sf4e-relay/install-vps-relay.sh"
else
  echo "WARNING: install-vps-relay.sh not found — VPS relay manager not installed."
fi

sleep 2
systemctl is-active sf4e-broker.service
curl -s http://127.0.0.1:8787/v1/health
echo
