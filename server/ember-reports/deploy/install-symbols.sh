#!/usr/bin/env bash
# Installs one release's Breakpad symbols into the ember-reports symbol store.
# The publishers' Symbols step (scripts/SymbolPublication.ps1) sends the copy
# kept in the release's symbol archive over ssh and runs it as katie, a member
# of ember-symbols (setup.sh), in three phases:
#   bash -s -- <stable|nightly> <label> prepare
#       makes an empty upload folder inside the store and prints its path
#   bash -s -- <stable|nightly> <label> commit
#       checks every <debug file>/<DEBUGID> folder of the upload, then moves
#       each one into the store, records the release's list in
#       ~/symbols-manifests/<channel>-<label>.txt and deletes the symbols of
#       Nightly releases older than 45 days that no other release lists
#   bash -s -- <stable|nightly> <label> cleanup
#       removes the upload folder
# The upload sits in the store's own filesystem, so each folder arrives by a
# rename and the service never reads a partly copied .sym. A folder already in
# the store is the same symbols (the DEBUGID names the PDB) and is kept. The
# service mounts the store read-only and never prunes it; Stable symbols stay.
# A commit holds a lock on the store itself for everything that relates its
# folders to the manifests (the checks for folders already there, the moves,
# the manifest write and the prune), so a Stable commit that keeps a shared
# folder and a Nightly prune that would delete it can never interleave.
set -euo pipefail
umask 002
[[ $# == 3 ]] || { echo "usage: install-symbols.sh <stable|nightly> <label> <prepare|commit|cleanup>" >&2; exit 2; }
channel=$1 label=$2 phase=$3
[[ $channel == stable || $channel == nightly ]] || { echo "Unknown channel: $channel" >&2; exit 2; }
[[ $label =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]] || { echo "Not a release label: $label" >&2; exit 2; }
store=${EMBER_SYMBOL_STORE:-/var/lib/ember-reports/symbols}
manifests=${EMBER_SYMBOL_MANIFESTS:-$HOME/symbols-manifests}
nightly_days=45
incoming=$store/.incoming-$channel-$label
# <debug file>/<DEBUGID>. The debug file starts with a letter or digit, so no
# entry, uploaded or in a manifest edited by hand, can name . or .. or an
# upload folder.
module_pattern='^[A-Za-z0-9][A-Za-z0-9_.+-]*/[0-9A-Fa-f]+$'

prune_nightly() {
    local manifest module
    local -A expired=() kept=()
    while IFS= read -r -d '' manifest; do expired[$manifest]=1; done \
        < <(find "$manifests" -maxdepth 1 -type f -name 'nightly-*.txt' -mtime "+$nightly_days" -print0)
    (( ${#expired[@]} )) || return 0
    for manifest in "$manifests"/*.txt; do
        [[ -n ${expired[$manifest]:-} ]] && continue
        while IFS= read -r module; do kept[$module]=1; done < "$manifest"
    done
    for manifest in "${!expired[@]}"; do
        while IFS= read -r module; do
            if [[ $module =~ $module_pattern && -z ${kept[$module]:-} ]]; then
                rm -rf -- "${store:?}/$module"
                rmdir --ignore-fail-on-non-empty -- "$store/${module%/*}" 2>/dev/null || true
            fi
        done < "$manifest"
        rm -f -- "$manifest"
        echo "Pruned Nightly symbols listed by ${manifest##*/}"
    done
}

case $phase in
prepare)
    [[ -d $store && -w $store ]] || { echo "Cannot write $store; is $(id -un) in ember-symbols? Reconnect after setup.sh adds it." >&2; exit 3; }
    rm -rf -- "$incoming"
    mkdir -- "$incoming"
    printf '%s\n' "$incoming"
    ;;
commit)
    [[ -d $incoming ]] || { echo "No upload at $incoming." >&2; exit 3; }
    # scp keeps the uploader's modes; the service reads through the group.
    chmod -R g+rX,o-rwx -- "$incoming"
    # Every entry is checked before anything moves: a bad upload installs nothing.
    modules=()
    shopt -s nullglob
    for directory in "$incoming"/*/*/; do
        directory=${directory%/}
        module=${directory#"$incoming"/}
        [[ $module =~ $module_pattern ]] || { echo "Unexpected upload entry: $module" >&2; exit 4; }
        files=("$directory"/*.sym)
        (( ${#files[@]} == 1 )) && [[ -f ${files[0]} && -s ${files[0]} ]] || { echo "No symbol file in $module." >&2; exit 4; }
        modules+=("$module")
    done
    shopt -u nullglob
    (( ${#modules[@]} )) || { echo "The upload holds no symbols." >&2; exit 4; }
    exec 9<"$store"
    flock -w 600 9 || { echo "Another symbol install held $store for 10 minutes." >&2; exit 5; }
    for module in "${modules[@]}"; do
        if [[ ! -e $store/$module ]]; then
            mkdir -p -- "$store/${module%/*}"
            mv -T -- "$incoming/$module" "$store/$module"
        fi
    done
    mkdir -p -- "$manifests"
    printf '%s\n' "${modules[@]}" > "$manifests/.$channel-$label.txt.partial"
    mv -f -- "$manifests/.$channel-$label.txt.partial" "$manifests/$channel-$label.txt"
    echo "Installed ${#modules[@]} symbol folders for $channel $label."
    prune_nightly
    ;;
cleanup)
    rm -rf -- "$incoming"
    ;;
*)
    echo "Unknown phase: $phase" >&2; exit 2
    ;;
esac
