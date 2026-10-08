#!/usr/bin/env bash
# Only inert text fixtures in an isolated directory; never SSH or execute a binary.
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
fixture=$(mktemp -d)
trap 'rm -rf -- "$fixture"' EXIT
id=$(printf 'sidecar fixture' | sha256sum | awk '{print $1}')
host_hash=$(printf 'host fixture' | sha256sum | awk '{print $1}')
net_hash=$(printf 'helper fixture' | sha256sum | awk '{print $1}')
token=00000000000000000000000000000001
run_stage() { bash "$script_dir/stage-nightly-room-hosts.sh" "$root" "$id" "$host_hash" "$net_hash" "$token" "$1" "$fixture"; }
upload_pair() {
    run_stage prepare
    incoming="$fixture/$root/.incoming-$id-$token"
    printf 'host fixture' > "$incoming/sf4e-room-host"
    printf 'helper fixture' > "$incoming/sf4-net"
}
for root in ember-rooms ember-rooms-box; do
    final="$fixture/$root/builds/$id"
    upload_pair
    # Simulate interrupted/truncated scp: no incomplete build becomes visible.
    printf 'partial' > "$incoming/sf4-net"
    if run_stage commit; then echo 'Accepted truncated upload' >&2; exit 1; fi
    test ! -e "$final"
    upload_pair
    run_stage commit
    test ! -e "$incoming"
    test -x "$final/sf4e-room-host" && test -x "$final/sf4-net"
    upload_pair
    run_stage commit
    test ! -e "$incoming"
    # Existing build IDs, including partial pre-existing folders, are immutable.
    printf 'existing different bytes' > "$final/sf4-net"
    upload_pair
    if run_stage commit; then echo 'Accepted different existing pair' >&2; exit 1; fi
    [[ $(cat "$final/sf4-net") == 'existing different bytes' ]]
    rm -- "$final/sf4-net"
    upload_pair
    if run_stage commit; then echo 'Accepted incomplete existing pair' >&2; exit 1; fi
    test ! -e "$final/sf4-net"
    [[ $(cat "$final/sf4e-room-host") == 'host fixture' ]]
done
echo 'PASS atomic staging on both roots: truncated upload, complete pair, identical retry, conflicting and partial existing identities'
