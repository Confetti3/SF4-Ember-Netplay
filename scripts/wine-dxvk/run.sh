#!/usr/bin/env bash
# Wine + DXVK compatibility check for an Ember release package, without SF4.
# See docs/validation/2026-09-24-wine-dxvk-check.md for what this does and
# does not establish.
#
# Requirements (Ubuntu 24.04): WineHQ wine-stable (11.x), mingw-w64, Xvfb,
# imagemagick, mesa-vulkan-drivers (amd64 + i386), a DXVK release tarball.
#
#   scripts/wine-dxvk/run.sh <extracted-ember-package-dir> <dxvk-x.y.z-dir> <work-dir>
set -euo pipefail

PACKAGE=$(realpath "$1")
DXVK=$(realpath "$2")
WORK=$(realpath -m "$3")
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$WORK"

export WINEPREFIX="$WORK/pfx" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
export DISPLAY="${DISPLAY:-:99}"
if ! xdpyinfo >/dev/null 2>&1; then (Xvfb "$DISPLAY" -screen 0 1280x720x24 >/dev/null 2>&1 &); sleep 1; fi

if [ ! -d "$WINEPREFIX" ]; then
	wineboot -i && wineserver -w
	cp "$DXVK"/x32/*.dll "$WINEPREFIX/drive_c/windows/syswow64/"
	cp "$DXVK"/x64/*.dll "$WINEPREFIX/drive_c/windows/system32/"
	for d in d3d8 d3d9 d3d10core d3d11 dxgi; do
		wine reg add 'HKCU\Software\Wine\DllOverrides' /v "$d" /d native /f >/dev/null
	done
fi
LOGS="$WINEPREFIX/drive_c/users/$(id -un)/AppData/Roaming/sf4e/logs"

echo "== Win32 timer, Winsock and frame-limiter probe"
i686-w64-mingw32-g++-posix -O2 -static "$HERE/probe.cpp" -o "$WORK/probe.exe" -lws2_32 -ld3d9 -lwinmm
(cd "$WORK" && DXVK_LOG_LEVEL=none wine probe.exe | tr -d '\r')

echo "== Launcher recovery screen (no game)"
(cd "$PACKAGE" && DXVK_LOG_LEVEL=none timeout 30 wine Launcher.exe >/dev/null 2>&1 &)
sleep 20; import -window root "$WORK/launcher-recovery.png"; wineserver -k || true; sleep 2

echo "== Launcher injecting Sidecar.dll into a stand-in SSFIV.exe"
# A 32-bit image with no-ops at every offset Sidecar hooks. It proves
# injection, payload delivery, hook installation and helper startup only.
mkdir -p "$WORK/fakegame"
i686-w64-mingw32-gcc-posix -O1 -nostdlib -e _start@0 -Wl,--image-base,0x400000 \
	-Wl,--disable-dynamicbase "$HERE/sled.s" "$HERE/stub.c" -o "$WORK/fakegame/SSFIV.exe" -lkernel32
# Sidecar also patches bytes in the game image, which SF4 keeps writable.
python3 - "$WORK/fakegame/SSFIV.exe" <<'EOF'
import struct, sys
d = bytearray(open(sys.argv[1], 'rb').read())
pe = struct.unpack_from('<I', d, 0x3c)[0]
sections = pe + 24 + struct.unpack_from('<H', d, pe + 20)[0]
for i in range(struct.unpack_from('<H', d, pe + 6)[0]):
	o = sections + 40 * i
	if d[o:o + 8].rstrip(b'\0') == b'.text':
		struct.pack_into('<I', d, o + 36, struct.unpack_from('<I', d, o + 36)[0] | 0x80000000)
open(sys.argv[1], 'wb').write(d)
EOF
rm -f "${LOGS:?}"/sidecar_bootstrap.log
STEAM_APP_PATH="$(winepath -w "$WORK/fakegame")" DXVK_LOG_LEVEL=none \
	bash -c "cd '$PACKAGE' && timeout 60 wine Launcher.exe >/dev/null 2>&1" &
sleep 8
ps -eo args | grep -E 'sf4-net\.exe|ember-discord\.exe|SSFIV\.exe' | grep -v grep || true
wait || true
cat "$LOGS/sidecar_bootstrap.log"
grep -E 'CreateSF4Process|Sidecar|helper' "$LOGS/launcher.log" | tail -4
# A hook past the end of sled.s cannot attach, so the commit fails. That
# means the sled must grow to cover the new highest hook offset.
if ! grep -q 'Sidecar install committed' "$LOGS/sidecar_bootstrap.log"; then
	echo "Sidecar hooks did not install on the stand-in; check that sled.s covers every hook offset" >&2
	exit 1
fi
