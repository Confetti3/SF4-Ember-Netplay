#!/bin/bash
# USF4's Steam launch option: "<prefix>/drive_c/Ember/steam-launch.sh" %command%
# The SF4 Ember Netplay tile adds -ember to the game's command; then this runs
# Ember's Launcher.exe in the game's own Proton session instead of SSFIV.exe.
# Without it, USF4's own tile starts the plain game.
[[ " $* " == *" -ember "* ]] || exec "$@"
here=$(dirname "$(readlink -f "$0")")
game="${STEAM_COMPAT_INSTALL_PATH:-$HOME/.local/share/Steam/steamapps/common/Super Street Fighter IV - Arcade Edition}"
# Proton's prefix has no Steam library list, so tell Launcher where the game is.
export STEAM_APP_PATH="Z:${game//\//\\}"
args=()
for a in "$@"; do
  [ "$a" = -ember ] && continue
  [[ $a == *SSFIV.exe ]] && a="$here/Launcher.exe"
  args+=("$a")
done
exec "${args[@]}"
