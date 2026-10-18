#!/usr/bin/env bash
# Installs or updates SF4 Ember Netplay on a Steam Deck (or any Linux Steam).
#
#   bash steamdeck-install.sh [version]     # default: latest release
#
# It extracts the release into the game's Proton prefix as C:\Ember and adds an "SF4 Ember Netplay" tile to
# the Steam library. That tile starts USF4 through Steam with Ember; USF4's
# own tile still starts the plain game. Steam is closed while its config is
# edited and started again afterwards.
set -euo pipefail

REPO=Confetti3/SF4-Ember-Netplay
STEAM="$HOME/.local/share/Steam"
CURL=(curl -fL --connect-timeout 15 --retry 2)

[ -d "$STEAM/userdata" ] || { echo "Steam not found at $STEAM"; exit 1; }
# Inside the prefix, because Ember's updater refuses a folder Wine reports as
# behind a reparse point, which Z:\home is on a Steam Deck. The prefix sits in
# whichever library holds the game and exists once the game has run.
libs=$(echo "$STEAM"; sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"$/\1/p' "$STEAM/steamapps/libraryfolders.vdf" 2>/dev/null || true)
pfx=
while read -r lib; do
  [ -d "$lib/steamapps/compatdata/45760/pfx/drive_c" ] && { pfx="$lib/steamapps/compatdata/45760/pfx"; break; }
done <<<"$libs"
[ -n "$pfx" ] || { echo "Start Ultra Street Fighter IV once from Steam, quit it, then run this again."; exit 1; }
DEST="$pfx/drive_c/Ember"
# Wine shows the game's processes by their Windows path, such as Z:\...\SSFIV.exe.
if pgrep -f '^[A-Z]:\\.*\\(SSFIV|Launcher)\.exe' >/dev/null; then echo "Quit USF4 first."; exit 1; fi

tag=${1:-$("${CURL[@]}" -sSI "https://github.com/$REPO/releases/latest" | sed -n 's|^location:.*/tag/\(v[^[:space:]]*\).*|\1|ip')}
[ -n "$tag" ] || { echo "Could not find the latest release."; exit 1; }
tag=v${tag#v}
zip="sf4-ember-netplay-${tag#v}.zip"
echo "Installing Ember $tag"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
"${CURL[@]}" --progress-bar -o "$tmp/$zip" "https://github.com/$REPO/releases/download/$tag/$zip"
"${CURL[@]}" -sS -o "$tmp/$zip.sha256" "https://github.com/$REPO/releases/download/$tag/$zip.sha256"
(cd "$tmp" && sha256sum -c "$zip.sha256") || { echo "Download is corrupt."; exit 1; }
# The tile's scripts, Steam config edit and artwork; the tile works without artwork.
base=https://raw.githubusercontent.com/Confetti3/SF4-Ember-Netplay-Nightly/nightly
mkdir "$tmp/art"
for f in steam-launch.sh ember-play.sh steam_config.py; do
  "${CURL[@]}" -sS -o "$tmp/$f" "$base/scripts/steamdeck/$f"
done
for f in cover wide hero logo icon; do
  "${CURL[@]}" -sS -o "$tmp/art/$f.png" "$base/assets/steamdeck/$f.png" 2>/dev/null || rm -f "$tmp/art/$f.png"
done

# A fresh folder each time, as the player guide asks; settings live in the Proton prefix.
mkdir "$tmp/x"
python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$tmp/$zip" "$tmp/x"
src=$(find "$tmp/x" -name Launcher.exe -printf '%h' -quit)
[ -n "$src" ] || { echo "The release has no Launcher.exe."; exit 1; }
install -m 755 "$tmp/steam-launch.sh" "$tmp/ember-play.sh" "$src/"
rm -rf "$DEST"
mv "$src" "$DEST"

restart=
if systemctl --user -q is-active steam-launcher.service 2>/dev/null; then
  # Game Mode runs Steam as this service; restarting it any other way leaves gamescope.
  echo "Closing Steam to add Ember to the library..."
  systemctl --user stop steam-launcher.service
  restart=service
elif pgrep -x steam >/dev/null; then
  echo "Closing Steam to add Ember to the library..."
  steam -shutdown >/dev/null 2>&1 || true
  for _ in $(seq 60); do pgrep -x steam >/dev/null || break; sleep 1; done
  pgrep -x steam >/dev/null && { echo "Steam did not close; close it and run this again."; exit 1; }
  restart=desktop
fi

set=0
python3 "$tmp/steam_config.py" "$STEAM" "$DEST" "$tmp/art" || set=$?

if [ "$restart" = service ]; then
  systemctl --user start steam-launcher.service
elif [ -n "$restart" ]; then
  systemd-run --user --collect steam >/dev/null 2>&1
fi
[ "$set" = 0 ] || exit 1
echo "Done. Play SF4 Ember Netplay from your Steam library."
