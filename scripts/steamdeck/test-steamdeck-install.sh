#!/usr/bin/env bash
# Runs steamdeck-install.sh's launch option edit on small localconfig.vdf fixtures.
set -euo pipefail
here=$(dirname "$(readlink -f "$0")")
edit=$(sed -n '/^python3 - "$STEAM"/,/^EOF$/p' "$here/steamdeck-install.sh" | sed '1d;$d')
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
option='/home/deck/Ember/steam-launch.sh %command%'

account() { mkdir -p "$tmp/$1/userdata/1/config"; printf '%s' "$2" > "$tmp/$1/userdata/1/config/localconfig.vdf"; }
run() { python3 -c "$edit" "$tmp/$1" "$option"; cat "$tmp/$1/userdata/1/config/localconfig.vdf"; }
apps=$'"UserLocalConfigStore"\n{\n\t"Software"\n\t{\n\t\t"Valve"\n\t\t{\n\t\t\t"Steam"\n\t\t\t{\n\t\t\t\t"apps"\n\t\t\t\t{\n'
tail=$'\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n'
want=$'\t\t\t\t\t\t"LaunchOptions"\t\t"'"$option"'"'

# Played before: the option goes into the game's block once, even on a rerun.
account played "$apps"$'\t\t\t\t\t"45760"\n\t\t\t\t\t{\n\t\t\t\t\t\t"LastPlayed"\t\t"1"\n\t\t\t\t\t}\n'"$tail"
run played >/dev/null
out=$(run played)
[ "$(grep -c LaunchOptions <<<"$out")" = 1 ] && grep -qF "$want" <<<"$out"

# An older option, with an escaped quote, is replaced.
account old "$apps"$'\t\t\t\t\t"45760"\n\t\t\t\t\t{\n\t\t\t\t\t\t"LaunchOptions"\t\t"PROTON_LOG=1 \\"x\\" %command%"\n\t\t\t\t\t}\n'"$tail"
out=$(run old)
[ "$(grep -c LaunchOptions <<<"$out")" = 1 ] && grep -qF "$want" <<<"$out"

# Never played: a block for the game is added, and the braces still balance.
account new "$apps"$'\t\t\t\t\t"7"\n\t\t\t\t\t{\n\t\t\t\t\t}\n'"$tail"
out=$(run new)
grep -qF "$want" <<<"$out" && [ "$(tr -cd '{' <<<"$out" | wc -c)" = "$(tr -cd '}' <<<"$out" | wc -c)" ]

# No account at all fails instead of reporting success.
mkdir -p "$tmp/none/userdata"
! python3 -c "$edit" "$tmp/none" "$option" 2>/dev/null
echo "steamdeck-install launch option edit: ok"
