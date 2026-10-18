#!/usr/bin/env bash
# Installs or updates SF4 Ember Netplay on a Steam Deck (or any Linux Steam).
# After this, USF4's Play button starts Ember through the game's own Proton.
#
#   bash steamdeck-install.sh [--no-steam] [version]     # default: latest release
#
# It extracts the release to ~/Ember and sets USF4's Steam launch option to
# ~/Ember/steam-launch.sh %command%. Steam is closed while its config is
# edited and started again afterwards. --no-steam leaves Steam alone, for
# callers inside Steam (the Decky plugin) that set the launch option themselves.
set -euo pipefail

no_steam=
[ "${1:-}" = --no-steam ] && { no_steam=1; shift; }

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

# A fresh folder each time, as the player guide asks; settings live in the Proton prefix.
mkdir "$tmp/x"
python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$tmp/$zip" "$tmp/x"
src=$(find "$tmp/x" -name Launcher.exe -printf '%h' -quit)
[ -n "$src" ] || { echo "The release has no Launcher.exe."; exit 1; }
cat > "$src/steam-launch.sh" <<'EOF'
#!/bin/bash
# USF4 Steam launch option:  ~/Ember/steam-launch.sh %command%
# Runs Ember's Launcher.exe in the game's own Proton session instead of SSFIV.exe.
here=$(dirname "$(readlink -f "$0")")
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
chmod +x "$src/steam-launch.sh"
rm -rf "$DEST.old"
[ -e "$DEST" ] && mv "$DEST" "$DEST.old"
mv "$src" "$DEST"
rm -rf "$DEST.old"

if [ -n "$no_steam" ]; then
  echo "Installed Ember $tag"
  exit 0
fi

restart=
if systemctl --user -q is-active steam-launcher.service 2>/dev/null; then
  # Game Mode runs Steam as this service; restarting it any other way leaves gamescope.
  echo "Closing Steam to set the launch option..."
  systemctl --user stop steam-launcher.service
  restart=service
elif pgrep -x steam >/dev/null; then
  echo "Closing Steam to set the launch option..."
  steam -shutdown >/dev/null 2>&1 || true
  for _ in $(seq 60); do pgrep -x steam >/dev/null || break; sleep 1; done
  pgrep -x steam >/dev/null && { echo "Steam did not close; close it and run this again."; exit 1; }
  restart=desktop
fi

# test-steamdeck-install.sh runs this edit on fixtures.
set=0
python3 - "$STEAM" "$DEST/steam-launch.sh %command%" <<'EOF' || set=$?
import glob, os, re, sys
steam, option = sys.argv[1], sys.argv[2]
done = 0
# Steam keeps launch options per account, in its "apps" block keyed by app ID.
for f in glob.glob(os.path.join(steam, "userdata/*/config/localconfig.vdf")):
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
    done += 1
sys.exit(0 if done else "Could not set USF4's launch option: no Steam account found. Sign in to Steam once, then run this again.")
EOF

if [ "$restart" = service ]; then
  systemctl --user start steam-launcher.service
elif [ -n "$restart" ]; then
  systemd-run --user --collect steam >/dev/null 2>&1
fi
[ "$set" = 0 ] || exit 1
echo "Done. Press Play on Ultra Street Fighter IV in Steam."
