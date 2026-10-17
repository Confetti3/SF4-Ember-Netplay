#!/usr/bin/env bash
# Installs or updates SF4 Ember Netplay into the Proton prefix of Ultra
# Street Fighter IV on Linux. Run it again to update. See docs/guides/LINUX.md.
#
#   scripts/install-linux.sh [version]      e.g. 1.1.2; default: latest release
#
# Environment: STEAM_ROOT (Steam install, autodetected), EMBER_DIR (folder name
# inside drive_c, default Ember), EMBER_NAME (Steam entry name), EMBER_PROTON
# (compatibility tool id, default proton_experimental; proton_10 for a numbered one),
# EMBER_ENTRY=game (the USFIV entry starts Ember; no separate shortcut), EMBER_ART
# (picture for the Steam entry; default the package's menu background; set empty to
# keep the pictures already in Steam), EMBER_LOGO, EMBER_ICON (optional pictures).
set -euo pipefail

REPO=Confetti3/SF4-Ember-Netplay
APPID=45760
EMBER_DIR=${EMBER_DIR:-Ember}

for tool in curl sha256sum; do command -v "$tool" >/dev/null || { echo "need $tool" >&2; exit 1; }; done
if command -v unzip >/dev/null; then unzip_to() { unzip -q "$1" -d "$2"; }
elif command -v python3 >/dev/null; then unzip_to() { python3 -I -m zipfile -e "$1" "$2"; }
else echo "need unzip or python3" >&2; exit 1; fi

# 1. Steam root: native, Flatpak or Snap.
if [ -z "${STEAM_ROOT:-}" ]; then
	for d in ~/.steam/steam ~/.local/share/Steam ~/.var/app/com.valvesoftware.Steam/.local/share/Steam ~/snap/steam/common/.local/share/Steam; do
		[ -f "$d/steamapps/libraryfolders.vdf" ] && STEAM_ROOT=$(realpath "$d") && break
	done
fi
[ -f "${STEAM_ROOT:-}/steamapps/libraryfolders.vdf" ] || { echo "Steam not found; set STEAM_ROOT=/path/to/Steam" >&2; exit 1; }

# 2. The library holding the game, then its Proton prefix.
LIBRARY=
while IFS= read -r path; do
	[ -f "$path/steamapps/appmanifest_$APPID.acf" ] && LIBRARY=$path && break
done < <({ echo "$STEAM_ROOT"; grep -oP '"path"\s+"\K[^"]+' "$STEAM_ROOT/steamapps/libraryfolders.vdf" | sed 's#\\\\#\\#g'; })
[ -n "$LIBRARY" ] || { echo "Ultra Street Fighter IV ($APPID) is not installed in any Steam library" >&2; exit 1; }
COMPAT=$LIBRARY/steamapps/compatdata/$APPID
DRIVE_C=$COMPAT/pfx/drive_c
[ -d "$DRIVE_C" ] || { echo "No Proton prefix at $COMPAT. Start the game once from Steam with Proton, then run this again." >&2; exit 1; }

# 3. Release zip and checksum from GitHub.
VERSION=${1:-}
if [ -z "$VERSION" ]; then
	VERSION=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" | grep -oP '"tag_name":\s*"v\K[^"]+')
	[ -n "$VERSION" ] || { echo "could not read the latest release from GitHub" >&2; exit 1; }
fi
ZIP=sf4-ember-netplay-$VERSION.zip
URL=https://github.com/$REPO/releases/download/v$VERSION
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
echo "Downloading $ZIP"
curl -fL --progress-bar -o "$WORK/$ZIP" "$URL/$ZIP"
curl -fsSL -o "$WORK/$ZIP.sha256" "$URL/$ZIP.sha256"
(cd "$WORK" && tr -d "\r" < "$ZIP.sha256" | sha256sum -c --quiet)

# 4. Extract. The zip holds one top-level folder; its contents become $EMBER_DIR.
mkdir -p "$WORK/x" && unzip_to "$WORK/$ZIP" "$WORK/x"
SRC=$WORK/x; [ "$(ls -A "$SRC" | wc -l)" = 1 ] && [ -d "$SRC/$(ls -A "$SRC")" ] && SRC=$SRC/$(ls -A "$SRC")
TARGET=$DRIVE_C/$EMBER_DIR
[ -d "$TARGET" ] && echo "Updating $TARGET" || echo "Installing to $TARGET"
mkdir -p "$TARGET" && cp -a "$SRC"/. "$TARGET"/
# preflight.cmd is a no-op under Wine (PowerShell stub), so verify MANIFEST.txt here.
(cd "$TARGET" && sed '1s/^\xEF\xBB\xBF//; s#\\#/#g; s/\r$//' MANIFEST.txt | sha256sum -c --quiet) || { echo "package verification failed" >&2; exit 1; }

echo
echo "Ember $VERSION is in $TARGET"
echo
# Steam entry, written while Steam is closed; otherwise the manual steps. EMBER_ENTRY=game
# makes the game's own USFIV entry start Ember instead of adding a shortcut.
HERE=$(dirname "$(realpath "$0")")
SHORTCUT=$HERE/steam-shortcut.py
[ -f "$SHORTCUT" ] || { SHORTCUT=$WORK/steam-shortcut.py; curl -fsSL -o "$SHORTCUT" "https://raw.githubusercontent.com/$REPO/release/scripts/steam-shortcut.py" || SHORTCUT=; }
if [ -n "$SHORTCUT" ] && command -v python3 >/dev/null && ! pgrep -x steam >/dev/null; then
	if [ "${EMBER_ENTRY:-shortcut}" = game ]; then
		python3 -I "$SHORTCUT" --steam "$STEAM_ROOT" --exe "$TARGET/Launcher.exe" --game "$APPID" && { echo "Start Steam and play Ultra Street Fighter IV; it starts Ember now. Clear its launch options to undo."; exit 0; }
	else
		python3 -I "$SHORTCUT" --steam "$STEAM_ROOT" --exe "$TARGET/Launcher.exe" --name "${EMBER_NAME:-Ember}" \
			--launch-options "STEAM_COMPAT_DATA_PATH=\"$COMPAT\" %command%" --compat "${EMBER_PROTON:-proton_experimental}" \
			--art "${EMBER_ART-$TARGET/assets/brand/ember-menu-background.png}" --logo "${EMBER_LOGO-}" --icon "${EMBER_ICON-}" \
			&& { echo "Start Steam and play the ${EMBER_NAME:-Ember} entry; Ember and USFIV start together."; exit 0; }
	fi
fi
echo "Steam entry not written (Steam running, or python3 missing). Close Steam and run this again, or by hand:"
echo "  1. Steam > Add a Non-Steam Game > Browse > $TARGET/Launcher.exe"
echo "  2. Properties > Launch options:"
echo "       STEAM_COMPAT_DATA_PATH=\"$COMPAT\" %command%"
echo "  3. Properties > Compatibility > force Proton Experimental (or your usual Proton)."
echo "Play that entry; Ember and USFIV start together."
