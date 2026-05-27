#!/usr/bin/env bash
# Build sf4e-session-relay on Linux (VPS or CI). Requires vcpkg deps:
#   vcpkg install cli11 nlohmann-json spdlog gamenetworkingsockets
set -euo pipefail

RELAY_DIR="$(cd "$(dirname "$0")" && pwd)"
if [[ -d "${RELAY_DIR}/src/session" ]]; then
  ROOT="${RELAY_DIR}"
else
  ROOT="$(cd "${RELAY_DIR}/../.." && pwd)"
fi
BUILD_DIR="${RELAY_DIR}/build"
INSTALL_DIR="${RELAY_DIR}/bin"

if [[ -z "${VCPKG_ROOT:-}" ]]; then
  echo "Set VCPKG_ROOT to your vcpkg installation."
  exit 1
fi

mkdir -p "$BUILD_DIR" "$INSTALL_DIR"
cmake -S "${RELAY_DIR}" -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --config Release -j1
cmake --install "$BUILD_DIR" --prefix "${RELAY_DIR}"

GNS_LIB="${VCPKG_ROOT}/installed/x64-linux/lib/libGameNetworkingSockets.so"
if [[ -f "$GNS_LIB" ]]; then
  mkdir -p "${RELAY_DIR}/lib"
  cp -f "$GNS_LIB" "${RELAY_DIR}/lib/libGameNetworkingSockets.so"
  echo "Bundled: ${RELAY_DIR}/lib/libGameNetworkingSockets.so"
fi

echo "Built: ${RELAY_DIR}/bin/sf4e-session-relay"
echo "Built: ${RELAY_DIR}/bin/sf4e-ggpo-udp-relay"
