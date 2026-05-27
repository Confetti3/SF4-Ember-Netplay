#!/usr/bin/env bash
# Quick WSL environment check for sf4e-session-relay build.
set -u

REPO="/mnt/c/Users/ptwar/Desktop/SF4e"
RELAY_DIR="${REPO}/services/vps-relay"
FAIL=0

pass() { echo "[OK]   $*"; }
fail() { echo "[FAIL] $*"; FAIL=1; }
warn() { echo "[WARN] $*"; }

echo "SF4 Netplay Launcher WSL build environment check"
echo "========================================="
echo "WSL: $(uname -sr)"
echo "Arch: $(uname -m)"
echo "User: $(id -un) (uid=$(id -u))"

for tool in g++ git curl; do
  if command -v "$tool" >/dev/null 2>&1; then
    pass "$tool -> $(command -v "$tool")"
  else
    fail "$tool not found"
  fi
done

if command -v cmake >/dev/null 2>&1; then
  pass "cmake -> $(cmake --version | head -1)"
else
  fail "cmake not found (required)"
fi

if [[ -f "${RELAY_DIR}/build-linux.sh" ]]; then
  pass "build-linux.sh present"
else
  fail "missing ${RELAY_DIR}/build-linux.sh"
fi

if [[ -f "${RELAY_DIR}/CMakeLists.txt" ]]; then
  pass "CMakeLists.txt present"
else
  fail "missing CMakeLists.txt"
fi

if [[ -x "${HOME}/vcpkg/vcpkg" ]]; then
  pass "Linux vcpkg -> ${HOME}/vcpkg/vcpkg"
elif [[ -x /mnt/c/Users/ptwar/vcpkg/vcpkg.exe ]]; then
  warn "Only Windows vcpkg.exe found; build script will bootstrap ~/vcpkg"
else
  warn "No vcpkg yet (build script will bootstrap ~/vcpkg)"
fi

if [[ -f "${REPO}/vcpkg.json" ]]; then
  pass "repo vcpkg.json present (relay build uses vcpkg --classic for Linux deps)"
fi

if sudo -n true 2>/dev/null; then
  pass "passwordless sudo available for apt install"
else
  warn "sudo needs a password — run once in WSL: sudo apt install cmake g++ git curl zip unzip pkg-config autoconf automake libtool"
fi

if [[ -x "${RELAY_DIR}/bin/sf4e-session-relay" ]]; then
  pass "existing binary: ${RELAY_DIR}/bin/sf4e-session-relay"
else
  warn "binary not built yet"
fi

echo "========================================="
if [[ $FAIL -eq 0 ]]; then
  echo "RESULT: READY (build can start)"
  exit 0
fi
echo "RESULT: NOT READY — fix failures above first"
exit 1
