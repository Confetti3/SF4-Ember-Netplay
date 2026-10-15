#!/usr/bin/env bash
set -euo pipefail
export PATH="$HOME/.cargo/bin:$PATH"
build_root="$HOME/ember-nightly-build/$1"
export CARGO_TARGET_DIR="$HOME/ember-nightly-build/cargo-target"
mkdir -p "$build_root"
tar -xzf "$2" -C "$build_root"
cd "$build_root"
bash server/roomhost/build-linux.sh . ./sf4e-room-host
(cd rust/sf4-net && cargo build --profile server --locked)
dependencies=$(ldd ./sf4e-room-host)
printf '%s\n' "$dependencies"
while IFS= read -r library; do
    if [[ ! "$library" =~ ^(linux-vdso\.so\.[0-9]+|/[^[:space:]]*/ld-linux[^/[:space:]]*\.so(\.[0-9]+)?|lib(c|m|pthread|rt|dl|resolv|util|anl)\.so\.[0-9]+)$ ]]; then
        echo "Unexpected room-host dependency: $library (only libc-family libraries allowed)" >&2
        exit 1
    fi
done < <(printf '%s\n' "$dependencies" | awk 'NF { print $1 }')
test -x ./sf4e-room-host
test -x "$CARGO_TARGET_DIR/server/sf4-net"
cp ./sf4e-room-host "$CARGO_TARGET_DIR/server/sf4-net" "$3/"
