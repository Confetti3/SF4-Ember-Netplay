# Which connector a deploy script works on, from the folder it runs in rather
# than from the environment, so a forgotten or dropped variable cannot turn a
# staging command into a production one. Sourced by setup.sh, issue-secrets.sh
# and remove.sh, which call `brackets_instance <folder>` from an operator
# folder directly in the home of whoever runs them (through sudo, the
# invoking user's):
#   ember-brackets            production, such as ~/ember-brackets
#   ember-brackets-<instance> that instance, such as ~/ember-brackets-staging
# INSTANCE, when set (even to nothing), must name the same connector.
#
# Sets INSTANCE (empty for production), NAME, the folder name, which is also
# the account and the unit and folder names, ROOT, LIB and STATE. ROOT is
# empty on the server; test-setup.sh sets EMBER_BRACKETS_TEST=1 and an
# EMBER_BRACKETS_ROOT inside TMPDIR to move every path there.
brackets_instance() {
    local dir=$1 folder name parent
    folder=$(basename "$dir")
    case "$folder" in
        ember-brackets) name="" ;;
        ember-brackets-*)
            name=${folder#ember-brackets-}
            # A name has no hyphen, so one instance's units never take
            # another's names.
            if [[ ! "$name" =~ ^[a-z][a-z0-9]{0,15}$ ]]; then
                echo "$folder is not a connector folder: an instance is a short lowercase name." >&2
                return 2
            fi
            ;;
        *)
            echo "$dir is not a connector folder: run from ember-brackets (production) or ember-brackets-<instance>." >&2
            return 2
            ;;
    esac
    if [ -n "${INSTANCE+set}" ] && [ "$INSTANCE" != "$name" ]; then
        echo "INSTANCE=$INSTANCE, but $dir is the ${name:-production} connector's folder; nothing was changed." >&2
        return 2
    fi

    ROOT=""
    if [ -n "${EMBER_BRACKETS_ROOT:-}" ]; then
        if [ "${EMBER_BRACKETS_TEST:-}" != 1 ] || [[ "$EMBER_BRACKETS_ROOT" != "${TMPDIR:-/tmp}"/* ]]; then
            echo "EMBER_BRACKETS_ROOT is only for test-setup.sh, with EMBER_BRACKETS_TEST=1 and a folder in TMPDIR." >&2
            return 2
        fi
        ROOT=$EMBER_BRACKETS_ROOT
    fi

    # A copy nested in another folder is never taken for a connector.
    if [ -n "${SUDO_USER:-}" ]; then
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
    OPERATOR_HOME=$parent
    LIB=$ROOT/usr/local/lib/$NAME
    STATE=$ROOT/var/lib/$NAME
}
