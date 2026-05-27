#!/usr/bin/env bash
# Install SF4e relay dashboard as a systemd service on Linux VPS.
set -euo pipefail

NODE_DIR="${NODE_DIR:-/opt/sf4e-node}"
RELAY_DIR="${RELAY_DIR:-/opt/sf4e-relay}"
DASHBOARD_DIR="${DASHBOARD_DIR:-$RELAY_DIR/dashboard}"
DASHBOARD_SERVICE="/etc/systemd/system/sf4e-relay-dashboard.service"
ENV_FILE="$DASHBOARD_DIR/.env"

if [[ ! -x "$NODE_DIR/bin/node" ]]; then
  echo "Run room-broker install-vps.sh first (installs Node to $NODE_DIR)."
  exit 1
fi

if [[ ! -f "$DASHBOARD_DIR/server.js" ]]; then
  echo "Missing $DASHBOARD_DIR/server.js"
  exit 1
fi

if [[ ! -f "$ENV_FILE" ]]; then
  if [[ -f "$DASHBOARD_DIR/.env.example" ]]; then
    cp "$DASHBOARD_DIR/.env.example" "$ENV_FILE"
    echo "Created $ENV_FILE — set ADMIN_PASSWORD_HASH and SESSION_SECRET before starting."
  else
    echo "Missing $ENV_FILE and .env.example"
    exit 1
  fi
fi

if ! grep -q '^ADMIN_PASSWORD_HASH=.\+' "$ENV_FILE"; then
  echo "Set ADMIN_PASSWORD_HASH in $ENV_FILE"
  echo '  node -e "require('\''bcryptjs'\'').hash('\''yourpassword'\'',12).then(console.log)"'
  exit 1
fi

if ! grep -q '^SESSION_SECRET=.\{32,\}' "$ENV_FILE"; then
  echo "Set SESSION_SECRET (32+ chars) in $ENV_FILE"
  echo "  node -e \"console.log(require('crypto').randomBytes(32).toString('hex'))\""
  exit 1
fi

pushd "$DASHBOARD_DIR" >/dev/null
export PATH="$NODE_DIR/bin:$PATH"
"$NODE_DIR/bin/npm" install --omit=dev
popd >/dev/null

cat > "$DASHBOARD_SERVICE" <<EOF
[Unit]
Description=SF4e VPS relay dashboard
After=network.target sf4e-relay-manager.service
Wants=sf4e-relay-manager.service

[Service]
Type=simple
WorkingDirectory=$DASHBOARD_DIR
EnvironmentFile=$ENV_FILE
ExecStart=$NODE_DIR/bin/node $DASHBOARD_DIR/server.js
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable sf4e-relay-dashboard.service
systemctl restart sf4e-relay-dashboard.service

sleep 2
systemctl is-active sf4e-relay-dashboard.service
DASHBOARD_PORT="$(grep -E '^DASHBOARD_PORT=' "$ENV_FILE" | cut -d= -f2- || echo 8789)"
DASHBOARD_BIND="$(grep -E '^DASHBOARD_BIND=' "$ENV_FILE" | cut -d= -f2- || echo 127.0.0.1)"
echo "Dashboard installed on ${DASHBOARD_BIND}:${DASHBOARD_PORT}."
echo "With Caddy: https://<your-domain>/login"
