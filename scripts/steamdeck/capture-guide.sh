#!/usr/bin/env bash
# Captures the Steam Deck guide's screenshots by running the real install on
# a Deck in Desktop Mode over SSH. Reinstalls Ember there; does not start USF4.
#
#   scripts/steamdeck/capture-guide.sh deck@steamdeck
#
# Writes docs/images/steamdeck-*.png. A browser click and KDE's trust dialog
# cannot be pressed over SSH, so the script downloads the file and marks it
# trusted the way KDE's "Make executable" does after capturing each screen.
set -euo pipefail
deck=$1
images="$(dirname "$(readlink -f "$0")")/../../docs/images"
page=https://github.com/Confetti3/SF4-Ember-Netplay-Nightly/releases/tag/steamdeck
file=https://github.com/Confetti3/SF4-Ember-Netplay-Nightly/releases/download/steamdeck/install-ember.desktop

on() { ssh "$deck" "$@"; }
# Starts a program in the Deck's desktop session, where SSH has no display.
run() { on systemd-run --user --collect -q "$@" >/dev/null 2>&1 || true; }
shot() {
  sleep "${2:-4}"
  on 'rm -f /tmp/shot.png; systemd-run --user --collect --wait -q spectacle -b -n -f -o /tmp/shot.png >/dev/null 2>&1'
  scp -q "$deck:/tmp/shot.png" "$images/steamdeck-$1.png"
  echo "steamdeck-$1.png"
}

on pgrep -x plasmashell >/dev/null || { echo "Switch the Deck to Desktop Mode first."; exit 1; }
on 'rm -f ~/Downloads/install-ember.desktop ~/.cache/ember-install.sh'

run flatpak run org.mozilla.firefox --new-window "$page"
shot 1-download 12
on "curl -fsSL -o ~/Downloads/install-ember.desktop '$file'"
on pkill -f org.mozilla.firefox || true

# KWin keeps new windows behind the focused one, so clear the screen first.
on 'DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus qdbus6 org.kde.kglobalaccel /component/kwin invokeShortcut "Show Desktop"'
run dolphin --select /home/deck/Downloads/install-ember.desktop
shot 2-downloads

run kioclient exec /home/deck/Downloads/install-ember.desktop
shot 3-trust
on pkill -x kioclient || true
on pkill -x dolphin || true
on "sed -i '1i #!/usr/bin/env xdg-open' ~/Downloads/install-ember.desktop && chmod +x ~/Downloads/install-ember.desktop"

run kioclient exec /home/deck/Downloads/install-ember.desktop
# The install finishes within seconds of closing Steam, and the terminal
# stays in front until Steam's window comes back.
on 'for _ in $(seq 120); do pgrep -x steam >/dev/null || break; sleep 1; done'
shot 4-done 2
# Steam numbers the shortcut as the installer does: crc32 of its Exe and name.
id=$(python3 -c 'import zlib; print(zlib.crc32(b"\"/home/deck/Ember/ember-play.sh\"SF4 Ember Netplay") | 0x80000000)')
on 'for _ in $(seq 60); do pgrep -f steamwebhelper >/dev/null && break; sleep 1; done'
sleep 15
run steam "steam://nav/games/details/$id"
shot 5-library 8
