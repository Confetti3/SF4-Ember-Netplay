#!/usr/bin/env bash
# Run through SSH stdin; every argument is validated before using any path.
set -euo pipefail
[[ "$1" =~ ^ember-rooms(-box)?$ && "$2" =~ ^[0-9a-f]{64}$ &&
   "$3" =~ ^[0-9a-f]{64}$ && "$4" =~ ^[0-9a-f]{64}$ && "$5" =~ ^[0-9a-f]{32}$ ]]
# The optional seventh argument lets local tests use an isolated directory.
# SSH callers pass six arguments and always stage beneath the login home.
root="${7:-$HOME}/$1"
incoming="$root/.incoming-$2-$5"
final="$root/builds/$2"
verify_pair() {
    test -f "$1/sf4e-room-host" && test -f "$1/sf4-net"
    [[ $(sha256sum "$1/sf4e-room-host" | awk '{print $1}') == "$2" &&
       $(sha256sum "$1/sf4-net" | awk '{print $1}') == "$3" ]]
}
# Pass hashes explicitly: function positional arguments have their own scope.
case "$6" in
    cleanup)
        # Only the validated build/token path owned by this upload; idempotent.
        rm -rf -- "$incoming"
        ;;
    prepare)
        mkdir -p "$root"
        mkdir "$incoming"
        ;;
    commit)
        trap 'rm -rf -- "$incoming"' EXIT
        verify_pair "$incoming" "$3" "$4"
        chmod +x "$incoming/sf4e-room-host" "$incoming/sf4-net"
        mkdir -p "$root/builds"
        # A rename on one filesystem is atomic; never permit mv's copy fallback.
        [[ $(stat -c %d "$incoming") == $(stat -c %d "$root/builds") ]]
        if [[ ! -e "$final" && ! -L "$final" ]]; then
            mv -T -n -- "$incoming" "$final"
        fi
        # Also handles a concurrent winner. Existing bytes are never overwritten.
        verify_pair "$final" "$3" "$4"
        ;;
    *) exit 1 ;;
esac
