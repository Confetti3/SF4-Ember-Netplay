#!/usr/bin/env bash
# Runs steamdeck-install.sh's Steam config edit on small fixtures: USF4's
# launch option in localconfig.vdf and the library tile in shortcuts.vdf.
set -euo pipefail
here=$(dirname "$(readlink -f "$0")")
edit=$(sed -n '/^python3 - "\$STEAM"/,/^EOF$/p' "$here/steamdeck-install.sh" | sed '1d;$d')
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/art" && printf png > "$tmp/art/cover.png"
want=$'\t\t\t\t\t\t"LaunchOptions"\t\t"/home/deck/Ember/steam-launch.sh %command%"'

account() { mkdir -p "$tmp/$1/userdata/1/config"; printf '%s' "$2" > "$tmp/$1/userdata/1/config/localconfig.vdf"; }
run() { python3 -c "$edit" "$tmp/$1" /home/deck/Ember "$tmp/art"; cat "$tmp/$1/userdata/1/config/localconfig.vdf"; }
apps=$'"UserLocalConfigStore"\n{\n\t"Software"\n\t{\n\t\t"Valve"\n\t\t{\n\t\t\t"Steam"\n\t\t\t{\n\t\t\t\t"apps"\n\t\t\t\t{\n'
tail=$'\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n'

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

# The tile: one Ember shortcut after a rerun, another game's shortcut kept,
# the file still reads back, and the cover is in the grid folder.
config="$tmp/played/userdata/1/config"
python3 - "$config/shortcuts.vdf" <<'EOF'
import sys
other = b"\x02appid\x00\x01\x00\x00\x80\x01appname\x00Other\x00\x01Exe\x00\"/x\"\x00\x00tags\x00\x08\x08"
open(sys.argv[1], "wb").write(b"\x00shortcuts\x00\x000\x00" + other + b"\x08\x08")
EOF
run played >/dev/null
run played >/dev/null
names=$(python3 -c "$(sed -n '/^def read/,/^def write/p' <<<"$edit" | head -n -1)"'
import sys; print(*sorted(s["appname"] for s in read(open(sys.argv[1], "rb").read())[0]["shortcuts"].values()))' "$config/shortcuts.vdf")
[ "$names" = "Other SF4 Ember Netplay" ]
ls "$config"/grid/*p.png >/dev/null

# No account at all fails instead of reporting success.
mkdir -p "$tmp/none/userdata"
! python3 -c "$edit" "$tmp/none" /home/deck/Ember "$tmp/art" 2>/dev/null
echo "steamdeck-install Steam config edit: ok"
