# Which bridge a deploy script works on, from the folder it runs in rather
# than from the environment, so a forgotten or dropped variable cannot turn a
# staging command into a production one. Sourced by setup.sh,
# set-integration-secret.sh (in the operator folder) and by backup.sh and
# restore.sh (in /usr/local/lib/<name>/), then called with that folder:
#   ember-bridge            production, such as ~/ember-bridge
#   ember-bridge-<instance> that instance, such as ~/ember-bridge-staging
# INSTANCE, when set (even to nothing), must name the same bridge.
# EMBER_BRIDGE_ROOT moves every path under a test folder (test-setup.sh).
#
# Sets INSTANCE (empty for production), NAME, the folder name, which is also
# the account and the unit and folder names, and LIB, STATE and BACKUPS.
bridge_instance() {
    local folder name
    folder=$(basename "$1")
    case "$folder" in
        ember-bridge) name="" ;;
        ember-bridge-*) name=${folder#ember-bridge-} ;;
        *)
            echo "$1 is not a bridge folder: run from ember-bridge (production) or ember-bridge-<instance>." >&2
            return 2
            ;;
    esac
    # A name has no hyphen, so one instance's units never take another's
    # names; backup would make NAME.service production's backup service.
    if [[ ! "$name" =~ ^([a-z][a-z0-9]{0,15})?$ ]] || [ "$name" = backup ]; then
        echo "$folder is not a bridge folder: an instance is a short lowercase name other than backup." >&2
        return 2
    fi
    if [ -n "${INSTANCE+set}" ] && [ "$INSTANCE" != "$name" ]; then
        echo "INSTANCE=$INSTANCE, but $1 is the ${name:-production} bridge's folder; nothing was changed." >&2
        return 2
    fi
    INSTANCE=$name
    NAME=$folder
    local root=${EMBER_BRIDGE_ROOT:-}
    LIB=$root/usr/local/lib/$NAME
    STATE=$root/var/lib/$NAME
    BACKUPS=$root/var/backups/$NAME
}
