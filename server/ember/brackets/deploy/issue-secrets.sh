#!/usr/bin/env bash
# Makes the two secrets a connector shares with its bridge, and leaves them
# where setup.sh installs them from. Run as:
#   sudo bash ~/ember-brackets-staging/issue-secrets.sh
# for the connector in the folder it is in (instance.sh), against the bridge
# its site file names ({"bridge": "ember-bridge-staging"}), which must be
# installed with the connector's connection in its tenants file:
#   1. the connection's provider credential
#      (ember-bridge credential <bridge.json> provider <connection> <label>),
#      written to secrets/<bridge.credential_file>;
#   2. a new result secret (ember-bridge result-secret), stored on the bridge
#      with its set-integration-secret.sh result <connection>, which restarts
#      the bridge, and written to secrets/<results.secret_file>.
# Neither is printed. A file already in secrets/ is kept (the result secret is
# stored on the bridge again from it), and nothing is issued for a secret the
# connector already has installed, so running it twice issues nothing new.
# Then run setup.sh.
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"
cd /
[ -f "$SRC/instance.sh" ] || { echo "Missing $SRC/instance.sh" >&2; exit 1; }
. "$SRC/instance.sh"
brackets_instance "$SRC" || exit 2
[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }
CONFIG=brackets${INSTANCE:+.$INSTANCE}.json
SITEFILE=site${INSTANCE:+.$INSTANCE}.json
for file in "$CONFIG" "$SITEFILE"; do
    [ -f "$SRC/$file" ] || { echo "Missing $SRC/$file" >&2; exit 1; }
done

FACTS=$(cat <<'PY'
import json, re, sys
config = json.load(open(sys.argv[1]))
site = json.load(open(sys.argv[2]))
values = [
    (config["bridge"]["connection_id"], r"[a-z][a-z0-9-]{0,63}"),
    (site["bridge"], r"ember-bridge(-[a-z][a-z0-9]{0,15})?"),
    (config["bridge"]["credential_file"], r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}"),
    (config["results"]["secret_file"], r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}"),
]
for text, pattern in values:
    if not re.fullmatch(pattern, str(text)):
        sys.exit(f"{text!r} in {sys.argv[1]} or {sys.argv[2]} is not usable here")
for text, _ in values:
    print(text)
PY
)
mapfile -t FACT < <(python3 -c "$FACTS" "$SRC/$CONFIG" "$SRC/$SITEFILE")
if [ "${#FACT[@]}" -ne 4 ]; then
    echo "Could not use $CONFIG and $SITEFILE." >&2
    exit 1
fi
CONNECTION=${FACT[0]}
BRIDGE=${FACT[1]}
CREDENTIAL=${FACT[2]}
RESULT=${FACT[3]}

BLIB=$ROOT/usr/local/lib/$BRIDGE
BSTATE=$ROOT/var/lib/$BRIDGE
BOPS=$OPERATOR_HOME/$BRIDGE
DROP=$SRC/secrets
for file in "$BLIB/ember-bridge" "$BSTATE/bridge.json" "$BOPS/set-integration-secret.sh"; do
    [ -e "$file" ] || { echo "Missing $file: install the bridge $BRIDGE first (its setup.sh)." >&2; exit 1; }
done
[ -d "$DROP" ] || install -d -o "${SUDO_USER:-root}" -m 0700 "$DROP"
if [ -L "$DROP" ] || [ "$(stat -c %a "$DROP")" != 700 ]; then
    echo "$DROP must be a real folder with mode 0700." >&2
    exit 1
fi
umask 077

if [ -s "$DROP/$CREDENTIAL" ] || [ -s "$STATE/$CREDENTIAL" ]; then
    echo "provider credential: already there, kept"
else
    # The token is on standard output only; the notice on standard error is dropped.
    if ! sudo -u "$BRIDGE" "$BLIB/ember-bridge" credential "$BSTATE/bridge.json" provider "$CONNECTION" "ember-brackets" \
        > "$DROP/$CREDENTIAL.partial" 2>/dev/null || [ ! -s "$DROP/$CREDENTIAL.partial" ]; then
        rm -f "$DROP/$CREDENTIAL.partial"
        echo "The bridge did not issue a credential for $CONNECTION. Is it in the bridge's tenants file, and did the bridge load it?" >&2
        exit 1
    fi
    mv -f "$DROP/$CREDENTIAL.partial" "$DROP/$CREDENTIAL"
    echo "provider credential: issued for $CONNECTION"
fi

if [ -s "$STATE/$RESULT" ] && [ ! -s "$DROP/$RESULT" ]; then
    echo "result secret: already there, kept"
else
    if [ ! -s "$DROP/$RESULT" ]; then
        "$BLIB/ember-bridge" result-secret > "$DROP/$RESULT.partial" 2>/dev/null || true
        if [ ! -s "$DROP/$RESULT.partial" ]; then
            rm -f "$DROP/$RESULT.partial"
            echo "ember-bridge result-secret gave nothing." >&2
            exit 1
        fi
        mv -f "$DROP/$RESULT.partial" "$DROP/$RESULT"
    fi
    # set-integration-secret.sh reads one line, so the secret goes with a newline.
    if ! { tr -d '\r\n' < "$DROP/$RESULT"; echo; } | bash "$BOPS/set-integration-secret.sh" result "$CONNECTION" > /dev/null 2>&1; then
        echo "Storing the result secret on $BRIDGE failed; $DROP/$RESULT is kept, run this again." >&2
        exit 1
    fi
    echo "result secret: stored on $BRIDGE for $CONNECTION"
fi
chmod 0600 "$DROP/$CREDENTIAL" "$DROP/$RESULT"
echo "Now run: sudo bash $SRC/setup.sh"
