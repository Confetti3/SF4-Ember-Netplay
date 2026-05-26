#!/usr/bin/env bash
# Build sf4e-session-relay in WSL (native Linux).
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
RELAY_DIR="${REPO}/services/vps-relay"
VCPKG_ROOT="${VCPKG_ROOT:-$HOME/vcpkg}"
LOG="${REPO}/services/vps-relay/wsl-build.log"

exec > >(tee -a "$LOG") 2>&1
echo "[$(date -Is)] WSL build started"

echo "Repo: ${REPO}"
echo "VCPKG_ROOT: ${VCPKG_ROOT}"

if ! command -v cmake >/dev/null 2>&1; then
  if sudo -n true 2>/dev/null; then
    echo "Installing build tools via apt..."
    sudo apt-get update -qq
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
      cmake g++ git curl zip unzip pkg-config autoconf automake libtool
  else
    echo "ERROR: cmake missing and sudo requires a password."
    echo "Run in WSL: sudo apt update && sudo apt install -y cmake g++ git curl zip unzip pkg-config autoconf automake libtool"
    exit 1
  fi
fi

if [[ ! -x "${VCPKG_ROOT}/vcpkg" ]]; then
  echo "Bootstrapping vcpkg at ${VCPKG_ROOT}..."
  git clone --depth 1 https://github.com/microsoft/vcpkg "${VCPKG_ROOT}"
  "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics
fi

export VCPKG_ROOT
export VCPKG_MAX_CONCURRENCY="${VCPKG_MAX_CONCURRENCY:-2}"

echo "Installing vcpkg packages (first run may take 15-30 min)..."
"${VCPKG_ROOT}/vcpkg" install --classic \
  cli11 nlohmann-json spdlog gamenetworkingsockets \
  --triplet x64-linux

sed -i 's/\r$//' "${RELAY_DIR}/build-linux.sh" 2>/dev/null || true
chmod +x "${RELAY_DIR}/build-linux.sh"
cd "${RELAY_DIR}"
bash build-linux.sh

ls -la "${RELAY_DIR}/bin/sf4e-session-relay"
file "${RELAY_DIR}/bin/sf4e-session-relay"
echo "[$(date -Is)] OK: ${RELAY_DIR}/bin/sf4e-session-relay"
