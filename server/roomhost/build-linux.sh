#!/usr/bin/env bash
# Builds sf4e-room-host on Linux with g++ alone (no CMake), from a source tree
# made with `git archive` (see README.md).
#
#   build-linux.sh [<source root>] [<output file>]
#
# <source root> holds the src/ folder (default: two levels above this script);
# <output file> defaults to ./sf4e-room-host. Needs g++ with C++17 and the
# distribution's nlohmann-json3-dev, libspdlog-dev and libfmt-dev (spdlog built
# against the external fmt, as Debian and Ubuntu ship it). Set CXX, CXXFLAGS or
# JOBS to override the compiler, its flags or the parallelism.
#
# The sources are the room host's own files, the session server, the Iroh room,
# the room model and the small common pieces they use: the same list as the
# sf4e-room-host target in CMakeLists.txt, with RoomHostHelperPosix.cxx and
# HelperClientPosix.cxx in place of the Windows helper files. No game library.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="${1:-$(cd "$HERE/../.." && pwd)}"
OUT="${2:-$PWD/sf4e-room-host}"
CXX="${CXX:-g++}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"

if [ ! -f "$ROOT/server/roomhost/main.cxx" ]; then
    echo "No server/roomhost/main.cxx under $ROOT" >&2
    exit 1
fi
for header in nlohmann/json.hpp spdlog/spdlog.h fmt/format.h; do
    if ! echo "#include <$header>" | "$CXX" -std=c++17 -DSPDLOG_FMT_EXTERNAL -x c++ -fsyntax-only - 2>/dev/null; then
        echo "Missing header <$header>: install nlohmann-json3-dev, libspdlog-dev and libfmt-dev." >&2
        exit 1
    fi
done

# spdlog and fmt are compiled in header-only (the -dev packages ship their
# -inl.h files, and Debian's spdlog is built over the external fmt) and the
# C++ runtime is linked statically, so the binary needs only glibc: the room
# host machine does not have the -dev packages, and its glibc is the same
# release as the build machine's (both Ubuntu 26.04 here).
for header in spdlog/spdlog-inl.h fmt/format-inl.h; do
    if ! echo "#include <$header>" | "$CXX" -std=c++17 -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY -x c++ -fsyntax-only - 2>/dev/null; then
        echo "Missing header <$header>: the header-only build needs libspdlog-dev and libfmt-dev." >&2
        exit 1
    fi
done
LIBS="-static-libstdc++ -static-libgcc -pthread"
FLAGS="-std=c++17 -O2 -pthread -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY ${CXXFLAGS:-}"

SOURCES="
server/roomhost/main.cxx
server/roomhost/RoomHost.cxx
server/roomhost/RoomHostStatus.cxx
server/roomhost/RoomHostHelperPosix.cxx
src/platform/HelperClientPosix.cxx
src/session/MatchAuthority.cxx
src/session/sf4e__SessionServer.cxx
src/session/sf4e__SessionServer__Handlers.cxx
src/session/sf4e__SessionServer__Recovery.cxx
src/session/sf4e__SessionServer__Rooms.cxx
src/session/sf4e__SessionServer__Tournament.cxx
src/session/sf4e__SessionProtocol.cxx
src/session/IrohRoom.cxx
src/session/IrohRoomAdapters.cxx
src/session/IrohRoomCheckpoint.cxx
src/session/IrohRoomPeers.cxx
src/session/IrohRoomPublic.cxx
src/session/IdentityEvents.cxx
src/session/TournamentAnswers.cxx
src/session/RoomModel.cxx
src/session/RoomModelActions.cxx
src/session/RoomModelJson.cxx
src/session/RoomModelTournament.cxx
src/session/RoomModelServerOwned.cxx
src/session/RoomDigest.cxx
src/common/FighterCatalog.cxx
src/common/StageCatalog.cxx
src/common/sf4e__RollbackDiagnostics.cxx
"

OBJ="$(mktemp -d "${TMPDIR:-/tmp}/room-host-build.XXXXXX")"
trap 'rm -rf "$OBJ"' EXIT
export ROOT OBJ CXX FLAGS
compile() {
    local source="$1" object="$OBJ/$(echo "$1" | tr / _).o"
    # shellcheck disable=SC2086
    "$CXX" $FLAGS -c "$ROOT/$source" -o "$object"
}
export -f compile
echo "Compiling $(echo "$SOURCES" | grep -c .) files with $JOBS jobs"
echo "$SOURCES" | grep . | xargs -P "$JOBS" -I{} bash -c 'compile "$@"' _ {}
# shellcheck disable=SC2086
"$CXX" $FLAGS -o "$OUT" "$OBJ"/*.o $LIBS
echo "Built $OUT"
