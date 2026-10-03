# sf4e-room-host on Linux

`sf4e-room-host` is the authority of one public room (docs/design/PUBLIC_ROOMS.md).
The ember-rooms supervisor starts one per room on the server, with the helper
`sf4-net` as its child. This file is the operator recipe for producing the
Linux pair the supervisor needs for one client release:

```
~/ember-rooms/builds/<build_id>/sf4e-room-host
~/ember-rooms/builds/<build_id>/sf4-net
```

which `rust/ember-rooms/deploy/setup.sh` then installs (see
`rust/ember-rooms/deploy/README.md`). Nothing here needs sudo.

## How the Linux host differs

The program is the same on both platforms except for the helper link
(`RoomHostHelper.hxx`): Windows starts `sf4-net` on a named pipe in a job
(`RoomHostHelperWindows.cxx`); Linux starts `sf4-net --stdio --bind-port <port>
--coordination-port <coordination_port>` with its stdin and stdout piped
(`RoomHostHelperPosix.cxx`, `src/platform/HelperClientPosix.cxx`). The frames on
the pipes are the Windows pipe's frames, and the helper stops when its stdin
closes, so it cannot outlive the room host. The checkpoint digests
(`src/common/CoordinationBytes.hxx`) use Windows CNG on Windows and the session
layer's portable SHA-256 elsewhere; both give the standard SHA-256 (checked
against the `abc` and two-block test vectors on both platforms).

## The build id

A room host admits only clients of its own build. The client presents
`sf4e::sidecarHash`: the lowercase SHA-256 hex of the `Sidecar.dll` it runs
(`GetHash` in `src/sf4e/sf4e.cxx`). The same value is in the release's build
receipt: `scripts/build-current.ps1` writes `build-provenance.json` with a
`binaries` list of `{ path, sha256 }` for every staged `.exe` and `.dll`, and
`scripts/package-team.ps1` copies that file into the package. Take the `sha256`
of the `Sidecar.dll` entry and lowercase it (Get-FileHash writes uppercase):

```powershell
((Get-Content build\current\build-provenance.json | ConvertFrom-Json).binaries |
    Where-Object path -eq 'Sidecar.dll').sha256.ToLowerInvariant()
```

`build\current` is the `buildDirectory` named in `build-target.json`; the same
file is in the package next to the binaries. (Checked against the v1.0.0
package's receipt: the entry's `path` is `Sidecar.dll`.) That string is
`<build_id>`: the folder name under `builds/` and the key in the supervisor's
`config.json`. The test fixture (`src/tests/public_room_host_test.cxx`) uses the
literal build id `public-room-host-test` instead, so a host built for the
fixture is configured with that.

## Build machine

A Linux box of the same distribution release as the server (the room host links
the C++ runtime statically and spdlog and fmt header-only, so it needs only the
server's glibc; `sf4-net` is a Rust binary with the same glibc requirement).
Debian or Ubuntu packages: `g++` (C++17), `nlohmann-json3-dev`, `libspdlog-dev`,
`libfmt-dev` (Debian's spdlog is built over the external fmt; both -dev
packages ship the `-inl.h` headers the header-only build compiles), and the
Rust toolchain pinned by `rust/sf4-net/rust-toolchain.toml` (rustup). Installing
the packages needs sudo on that machine; the server itself needs nothing
installed. Here that machine is the WSL Ubuntu on the build PC; the server has
no compiler load and too little memory to build the helper.

## Steps

All from the release commit, so the room host and the helper are the sources
the client was built from.

1. Archive the sources (`git archive` takes committed content only, so commit
   first; `-c core.autocrlf=false` keeps the shell script's line endings LF on
   a Windows checkout):

   ```
   git -c core.autocrlf=false archive --format=tar.gz -o room-host-src.tgz <commit> src rust
   ```

2. On the build machine, unpack and build the room host:

   ```
   mkdir room-host && tar -xzf room-host-src.tgz -C room-host && cd room-host
   bash src/roomhost/build-linux.sh . ./sf4e-room-host
   ```

   `build-linux.sh [<source root>] [<output>]` compiles the same source list as
   the `sf4e-room-host` CMake target (the room host files, the session server,
   the Iroh room, the room model and the common pieces they use) with
   `g++ -std=c++17 -O2`, in parallel, and links the C++ runtime statically.
   About 25 s on 20 cores. `ldd ./sf4e-room-host` should list only libc.

3. Build the helper from the same tree (its path dependencies `rust/ember` and
   `rust/ember-short` are in the archive):

   ```
   (cd rust/sf4-net && cargo build --release --locked)
   ```

   The binary is `rust/sf4-net/target/release/sf4-net` (or under
   `$CARGO_TARGET_DIR`). About 1.5 min on 20 cores from a warm registry.

4. Stage the pair on the server under the build id and install it:

   ```
   ssh vps "mkdir -p ~/ember-rooms/builds/<build_id>"
   scp ./sf4e-room-host rust/sf4-net/target/release/sf4-net vps:~/ember-rooms/builds/<build_id>/
   ssh -t vps "sudo bash ~/ember-rooms/setup.sh"
   ```

   setup.sh checks both files are ELF binaries, copies them to
   `/usr/local/lib/ember-rooms/builds/<build_id>/`, adds the `builds` entry to
   `/etc/ember-rooms/config.json` and restarts the supervisor (which drains
   first; see the deploy README).

## Checking a host by hand

The supervisor's child protocol is one JSON line on stdin and status lines on
stdout (`rust/ember-rooms/README.md`, "Child protocol"). To run a host without
the supervisor, start it with stdin and stdout piped, write the configuration
line, keep stdin open, and read `hosted` (about 1 s after start on the VPS). Its
`invitation` is what a client joins with; closing stdin closes the room, and the
host exits 0 within about half a second with its helper gone. The fixture's
`--remote <invitation>` mode joins such a host from Windows with two clients
(the host must be configured with the fixture's room id, bridge id, build id,
ticket key and kid, and creator; the fixture's header lists them).
