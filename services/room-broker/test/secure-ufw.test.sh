#!/usr/bin/env bash
# Unit-style checks for secure-ufw.sh validation (mocks ufw; never touches real firewall).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SCRIPT="$ROOT/services/room-broker/secure-ufw.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

MOCK_BIN="$TMP/bin"
mkdir -p "$MOCK_BIN"
UFW_LOG="$TMP/ufw.log"

cat > "$MOCK_BIN/ufw" <<'EOF'
#!/usr/bin/env bash
echo "ufw $*" >> "${UFW_LOG:?}"
exit 0
EOF
chmod +x "$MOCK_BIN/ufw"
export PATH="$MOCK_BIN:$PATH"
export UFW_LOG

run_ufw() {
  env -u RELAY_PORT_BASE -u GGPO_UDP_PORT_BASE -u MAX_ROOMS -u NAT_PROBE_PORT \
    ENV_FILE="$1" bash "$SCRIPT"
}

pass=0
fail=0

expect_fail() {
  local label="$1"
  local envfile="$2"
  if run_ufw "$envfile" >"$TMP/out" 2>"$TMP/err"; then
    echo "FAIL: $label (expected nonzero exit)"
    fail=$((fail + 1))
  else
    echo "PASS: $label"
    pass=$((pass + 1))
  fi
  # Validation failures must happen before reset.
  if grep -q "ufw --force reset" "$UFW_LOG" 2>/dev/null; then
    echo "FAIL: $label triggered ufw reset before validation failure"
    fail=$((fail + 1))
  fi
  : > "$UFW_LOG"
}

expect_ok_ranges() {
  local label="$1"
  local envfile="$2"
  local session="$3"
  local ggpo="$4"
  if ! run_ufw "$envfile" >"$TMP/out" 2>"$TMP/err"; then
    echo "FAIL: $label (exit nonzero)"
    cat "$TMP/err" || true
    fail=$((fail + 1))
    return
  fi
  if ! grep -q "Session relay TCP/UDP: ${session}" "$TMP/out"; then
    echo "FAIL: $label missing session range ${session}"
    cat "$TMP/out"
    fail=$((fail + 1))
    return
  fi
  if ! grep -q "GGPO relay UDP:        ${ggpo}" "$TMP/out"; then
    echo "FAIL: $label missing ggpo range ${ggpo}"
    cat "$TMP/out"
    fail=$((fail + 1))
    return
  fi
  if ! grep -q "ufw --force reset" "$UFW_LOG"; then
    echo "FAIL: $label did not call ufw reset after validation"
    fail=$((fail + 1))
    return
  fi
  echo "PASS: $label"
  pass=$((pass + 1))
  : > "$UFW_LOG"
}

cat > "$TMP/ok.env" <<'EOF'
RELAY_PORT_BASE=23456
GGPO_UDP_PORT_BASE=24456
MAX_ROOMS=50
NAT_PROBE_PORT=8790
EOF

cat > "$TMP/zero.env" <<'EOF'
MAX_ROOMS=0
RELAY_PORT_BASE=23456
GGPO_UDP_PORT_BASE=24456
EOF

cat > "$TMP/overlap.env" <<'EOF'
RELAY_PORT_BASE=23456
GGPO_UDP_PORT_BASE=23460
MAX_ROOMS=20
EOF

cat > "$TMP/overflow.env" <<'EOF'
RELAY_PORT_BASE=65530
GGPO_UDP_PORT_BASE=24456
MAX_ROOMS=20
EOF

expect_ok_ranges "default 50-room ranges" "$TMP/ok.env" "23456-23505" "24456-24505"
expect_fail "rejects MAX_ROOMS=0" "$TMP/zero.env"
expect_fail "rejects overlapping ranges" "$TMP/overlap.env"
expect_fail "rejects session overflow" "$TMP/overflow.env"

echo "secure-ufw tests: ${pass} passed, ${fail} failed"
if (( fail > 0 )); then
  exit 1
fi
