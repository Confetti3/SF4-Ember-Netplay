# Which bridge a deploy script works on, from the folder it runs in rather
# than from the environment, so a forgotten or dropped variable cannot turn a
# staging command into a production one. Sourced by setup.sh and
# set-integration-secret.sh, which call `bridge_instance <folder> operator`
# from an operator folder directly in the home of whoever runs them (through
# sudo, the invoking user's), and by backup.sh and restore.sh, which call
# `bridge_instance <folder> installed` from /usr/local/lib/<name>/:
#   ember-bridge            production, such as ~/ember-bridge
#   ember-bridge-<instance> that instance, such as ~/ember-bridge-staging
# INSTANCE, when set (even to nothing), must name the same bridge.
#
# Sets INSTANCE (empty for production), NAME, the folder name, which is also
# the account and the unit and folder names, ROOT, and LIB, STATE and
# BACKUPS. ROOT is empty on the server; test-setup.sh sets EMBER_BRIDGE_TEST=1
# and an EMBER_BRIDGE_ROOT inside TMPDIR to move every path there.
bridge_instance() {
    local dir=$1 where=$2 folder name parent
    folder=$(basename "$dir")
    case "$folder" in
        ember-bridge) name="" ;;
        ember-bridge-*)
            name=${folder#ember-bridge-}
            # A name has no hyphen, so one instance's units never take
            # another's names; backup would make NAME.service production's
            # backup service.
            if [[ ! "$name" =~ ^[a-z][a-z0-9]{0,15}$ ]] || [ "$name" = backup ]; then
                echo "$folder is not a bridge folder: an instance is a short lowercase name other than backup." >&2
                return 2
            fi
            ;;
        *)
            echo "$dir is not a bridge folder: run from ember-bridge (production) or ember-bridge-<instance>." >&2
            return 2
            ;;
    esac
    if [ -n "${INSTANCE+set}" ] && [ "$INSTANCE" != "$name" ]; then
        echo "INSTANCE=$INSTANCE, but $dir is the ${name:-production} bridge's folder; nothing was changed." >&2
        return 2
    fi

    ROOT=""
    if [ -n "${EMBER_BRIDGE_ROOT:-}" ]; then
        if [ "${EMBER_BRIDGE_TEST:-}" != 1 ] || [[ "$EMBER_BRIDGE_ROOT" != "${TMPDIR:-/tmp}"/* ]]; then
            echo "EMBER_BRIDGE_ROOT is only for test-setup.sh, with EMBER_BRIDGE_TEST=1 and a folder in TMPDIR." >&2
            return 2
        fi
        ROOT=$EMBER_BRIDGE_ROOT
    fi

    # A copy nested in another folder, such as ~/ember-bridge-staging/ember-bridge,
    # is never taken for a bridge.
    if [ "$where" = installed ]; then
        parent=$ROOT/usr/local/lib
    elif [ -n "${SUDO_USER:-}" ]; then
        parent=$(getent passwd "$SUDO_USER" | cut -d: -f6)
    else
        parent=$HOME
    fi
    if [ -z "$parent" ] || [ "$(cd "$(dirname "$dir")" && pwd -P)" != "$(cd "$parent" 2>/dev/null && pwd -P)" ]; then
        echo "$dir is not directly in $parent; run the copy that is." >&2
        return 2
    fi

    INSTANCE=$name
    NAME=$folder
    LIB=$ROOT/usr/local/lib/$NAME
    STATE=$ROOT/var/lib/$NAME
    BACKUPS=$ROOT/var/backups/$NAME
}
