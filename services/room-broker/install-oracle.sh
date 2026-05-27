#!/usr/bin/env bash
set -euo pipefail

NODE_DIR="$HOME/node"
BROKER_DIR="$HOME/room-broker"
LOG_DIR="$HOME/logs"
SERVICE_DIR="$HOME/.config/systemd/user"

mkdir -p "$BROKER_DIR" "$LOG_DIR" "$SERVICE_DIR"

if ! swapon --show | grep -q /swapfile; then
  if [[ ! -f /swapfile ]]; then
    sudo fallocate -l 2G /swapfile || sudo dd if=/dev/zero of=/swapfile bs=1M count=2048
    sudo chmod 600 /swapfile
    sudo mkswap /swapfile
    grep -q '/swapfile' /etc/fstab || echo '/swapfile none swap sw 0 0' | sudo tee -a /etc/fstab
  fi
  sudo swapon /swapfile || true
fi

if [[ ! -x "$NODE_DIR/bin/node" ]]; then
  mkdir -p "$HOME/tmp"
  cd "$HOME/tmp"
  curl -fsSL -o node.tar.xz https://nodejs.org/dist/v20.19.2/node-v20.19.2-linux-x64.tar.xz
  tar -xJf node.tar.xz
  rm -rf "$NODE_DIR"
  mv node-v20.19.2-linux-x64 "$NODE_DIR"
fi

cp -f "$BROKER_DIR/.env.example" "$BROKER_DIR/.env"

cat > "$SERVICE_DIR/sf4e-broker.service" <<EOF
[Unit]
Description=SF4 Netplay Launcher room broker
After=network.target

[Service]
Type=simple
WorkingDirectory=$BROKER_DIR
EnvironmentFile=$BROKER_DIR/.env
ExecStart=$NODE_DIR/bin/node $BROKER_DIR/server.js
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
EOF

pkill -f "$BROKER_DIR/server.js" 2>/dev/null || true
sleep 1

systemctl --user daemon-reload
systemctl --user enable sf4e-broker.service
systemctl --user restart sf4e-broker.service
loginctl enable-linger opc 2>/dev/null || true

sudo firewall-cmd --permanent --add-port=8787/tcp 2>/dev/null || true
sudo firewall-cmd --reload 2>/dev/null || true

sleep 2
systemctl --user is-active sf4e-broker.service
curl -s http://127.0.0.1:8787/v1/health
echo
