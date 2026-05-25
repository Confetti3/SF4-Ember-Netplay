#!/usr/bin/env bash
set -euo pipefail

systemctl --user disable --now sf4e-broker.service 2>/dev/null || true
pkill -9 -f '/home/opc/room-broker/server.js' 2>/dev/null || true
sleep 2

sed -i '/sf4e-broker autostart/,+4d' "$HOME/.bashrc" 2>/dev/null || true
sed -i 's/\r$//' "$HOME/room-broker/.env" "$HOME/room-broker/server.js" 2>/dev/null || true

if ss -ltnp | grep -q ':8787'; then
  echo "Port 8787 still in use"
  ss -ltnp | grep 8787
  exit 1
fi

systemctl --user enable sf4e-broker.service
systemctl --user start sf4e-broker.service
sleep 3
systemctl --user is-active sf4e-broker.service
curl -s http://127.0.0.1:8787/v1/health
echo
