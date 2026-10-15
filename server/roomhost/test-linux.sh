#!/usr/bin/env bash
# Builds and runs the Linux helper client test with g++ alone, from the same
# source tree as build-linux.sh (see README.md).
#
#   test-linux.sh [<source root>]
#
# <source root> holds the src/ folder (default: two levels above this script).
# Set CXX or CXXFLAGS to override the compiler or its flags.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="${1:-$(cd "$HERE/../.." && pwd)}"
CXX="${CXX:-g++}"
OBJ="$(mktemp -d "${TMPDIR:-/tmp}/room-host-test.XXXXXX")"
trap 'rm -rf "$OBJ"' EXIT

# shellcheck disable=SC2086
"$CXX" -std=c++17 -O1 -g -pthread ${CXXFLAGS:-} -o "$OBJ/helper_client_posix_test" \
    "$ROOT/src/tests/helper_client_posix_test.cxx" "$ROOT/src/platform/HelperClientPosix.cxx"
# The test takes about 16 s (it waits out the client's 15 s deadline once).
timeout 60 "$OBJ/helper_client_posix_test"
