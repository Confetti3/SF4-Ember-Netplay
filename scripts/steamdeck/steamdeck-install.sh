#!/usr/bin/env bash
# Installs or updates SF4 Ember Netplay on a Steam Deck (or any Linux Steam).
#
#   bash steamdeck-install.sh [version]     # default: latest release
#
# It extracts the release to ~/Ember and adds an "SF4 Ember Netplay" tile to
# the Steam library. That tile starts USF4 through Steam with Ember; USF4's
# own tile still starts the plain game. Steam is closed while its config is
# edited and started again afterwards.
set -euo pipefail

REPO=Confetti3/SF4-Ember-Netplay
DEST="$HOME/Ember"
STEAM="$HOME/.local/share/Steam"
CURL=(curl -fL --connect-timeout 15 --retry 2)

[ -d "$STEAM/userdata" ] || { echo "Steam not found at $STEAM"; exit 1; }
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
# The tile's artwork; the tile works without it.
mkdir "$tmp/art"
for f in cover wide hero logo icon; do
  "${CURL[@]}" -sS -o "$tmp/art/$f.png" "${EMBER_ART_BASE:-https://raw.githubusercontent.com/Confetti3/SF4-Ember-Netplay-Nightly/nightly/assets/steamdeck}/$f.png" 2>/dev/null || rm -f "$tmp/art/$f.png"
done

# A fresh folder each time, as the player guide asks; settings live in the Proton prefix.
mkdir "$tmp/x"
python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$tmp/$zip" "$tmp/x"
src=$(find "$tmp/x" -name Launcher.exe -printf '%h' -quit)
[ -n "$src" ] || { echo "The release has no Launcher.exe."; exit 1; }
cat > "$src/steam-launch.sh" <<'EOF'
#!/bin/bash
# USF4 Steam launch option:  ~/Ember/steam-launch.sh %command%
# Starts the plain game, or Ember's Launcher.exe in the game's own Proton
# session when the SF4 Ember Netplay tile asked for it in the last minute.
here=$(dirname "$(readlink -f "$0")")
marker="$here/.play-ember"
if [ ! -f "$marker" ] || [ $(( $(date +%s) - $(stat -c %Y "$marker") )) -gt 60 ]; then
  rm -f "$marker"
  exec "$@"
fi
rm -f "$marker"
game="${STEAM_COMPAT_INSTALL_PATH:-$HOME/.local/share/Steam/steamapps/common/Super Street Fighter IV - Arcade Edition}"
# Proton's prefix has no Steam library list, so tell Launcher where the game is.
export STEAM_APP_PATH="Z:${game//\//\\}"
args=()
for a in "$@"; do
  [[ $a == *SSFIV.exe ]] && a="$here/Launcher.exe"
  args+=("$a")
done
exec "${args[@]}"
EOF
cat > "$src/ember-play.sh" <<'EOF'
#!/bin/bash
# The SF4 Ember Netplay tile: start USF4 through Steam, with Ember this once.
touch "$(dirname "$(readlink -f "$0")")/.play-ember"
exec steam steam://rungameid/45760
EOF
chmod +x "$src/steam-launch.sh" "$src/ember-play.sh"
rm -rf "$DEST.old"
[ -e "$DEST" ] && mv "$DEST" "$DEST.old"
mv "$src" "$DEST"
rm -rf "$DEST.old"

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

# test-steamdeck-install.sh runs this edit on fixtures.
set=0
python3 - "$STEAM" "$DEST" "$tmp/art" <<'EOF' || set=$?
import glob, os, re, shutil, sys, zlib
steam, dest, art = sys.argv[1:]
option = f"{dest}/steam-launch.sh %command%"
name, exe = "SF4 Ember Netplay", f'"{dest}/ember-play.sh"'
appid = zlib.crc32((exe + name).encode()) | 0x80000000  # how Steam numbers a shortcut


def read(b, i=0):
    """Steam's binary VDF: 0 opens a map, 1 a string, 2 an int32, 8 ends a map."""
    d = {}
    while (t := b[i]) != 8:
        j = b.index(0, i + 1)
        k, i = b[i + 1:j].decode(errors="surrogateescape"), j + 1
        if t == 0:
            d[k], i = read(b, i)
        elif t == 1:
            j = b.index(0, i)
            d[k], i = b[i:j].decode(errors="surrogateescape"), j + 1
        elif t == 2:
            d[k], i = int.from_bytes(b[i:i + 4], "little", signed=True), i + 4
        else:
            raise ValueError(f"unknown VDF type {t}")
    return d, i + 1


def write(d):
    out = b""
    for k, v in d.items():
        k = k.encode(errors="surrogateescape") + b"\0"
        if isinstance(v, dict):
            out += b"\0" + k + write(v)
        elif isinstance(v, int):
            out += b"\2" + k + v.to_bytes(4, "little", signed=True)
        else:
            out += b"\1" + k + v.encode(errors="surrogateescape") + b"\0"
    return out + b"\x08"


done = 0
for config in glob.glob(os.path.join(steam, "userdata/*/config")):
    # USF4's launch option, in the account's "apps" block keyed by app ID.
    f = os.path.join(config, "localconfig.vdf")
    if not os.path.exists(f):
        continue
    t = open(f, encoding="utf-8").read()
    apps = re.search(r'\n(\t+)"apps"\n\t+\{', t)
    if not apps:
        print("No apps list in", f)
        continue
    ind = apps.group(1) + "\t"
    line = f'{ind}\t"LaunchOptions"\t\t"{option}"'
    game = re.compile(r'\n' + ind + r'"45760"\n' + ind + r'\{').search(t, apps.end())
    if game:
        end = t.index("\n" + ind + "}", game.end())
        body = t[game.end():end]
        body, n = re.subn(r'\n\t+"LaunchOptions"\t\t"(?:[^"\\\n]|\\.)*"', lambda _: "\n" + line, body)
        if not n:
            body = "\n" + line + body
        t = t[:game.end()] + body + t[end:]
    else:
        t = t[:apps.end()] + f'\n{ind}"45760"\n{ind}{{\n{line}\n{ind}}}' + t[apps.end():]
    open(f, "w", encoding="utf-8").write(t)

    # The library tile, replacing an earlier one, and its artwork.
    grid = os.path.join(config, "grid")
    os.makedirs(grid, exist_ok=True)
    for src, suffix in (("cover", "p"), ("wide", ""), ("hero", "_hero"), ("logo", "_logo"), ("icon", "_icon")):
        if os.path.exists(os.path.join(art, src + ".png")):
            shutil.copyfile(os.path.join(art, src + ".png"), os.path.join(grid, f"{appid}{suffix}.png"))
    f = os.path.join(config, "shortcuts.vdf")
    shortcuts = read(open(f, "rb").read())[0]["shortcuts"] if os.path.exists(f) else {}
    kept = [s for s in shortcuts.values() if name not in (s.get("appname"), s.get("AppName"))]
    kept.append({
        "appid": appid - 2**32, "appname": name, "Exe": exe, "StartDir": f'"{dest}/"',
        "icon": os.path.join(grid, f"{appid}_icon.png"), "ShortcutPath": "", "LaunchOptions": "",
        "IsHidden": 0, "AllowDesktopConfig": 1, "AllowOverlay": 1, "OpenVR": 0, "Devkit": 0,
        "DevkitGameID": "", "DevkitOverrideAppID": 0, "LastPlayTime": 0, "FlatpakAppID": "", "tags": {},
    })
    open(f, "wb").write(write({"shortcuts": {str(i): s for i, s in enumerate(kept)}}))
    done += 1
sys.exit(0 if done else "Could not add Ember to Steam: no Steam account found. Sign in to Steam once, then run this again.")
EOF

if [ "$restart" = service ]; then
  systemctl --user start steam-launcher.service
elif [ -n "$restart" ]; then
  systemd-run --user --collect steam >/dev/null 2>&1
fi
[ "$set" = 0 ] || exit 1
echo "Done. Play SF4 Ember Netplay from your Steam library."
