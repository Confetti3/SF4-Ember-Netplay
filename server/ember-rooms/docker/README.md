# The room service as a container

One image holds the `ember-rooms` supervisor and the `sf4e-room-host` and
`sf4-net` pair of one client build. It replaces the staged binaries of
[deploy/README.md](../deploy/README.md) on a machine that runs Docker; the
bridge, the short-link service and nginx stay as they are, on systemd. What
the supervisor does is in [../README.md](../README.md), and what a room costs
in [docs/development/PUBLIC_ROOM_HOSTING.md](../../../docs/development/PUBLIC_ROOM_HOSTING.md).

Why a container: the room host must be built against the glibc of the machine
it runs on, so today it is built on a matching Linux and copied over by hand.
The image builds all three binaries in one place and pins them to the build
id in its tag, and a bigger or a second machine is a `docker compose up`.

## Build

From the repository root, with the client release's sidecar build hash (the
same value `deploy/setup.sh` takes as the folder name under `builds/`):

```
docker build --build-arg BUILD_ID=<hash> -t ember-rooms:<hash> -f server/ember-rooms/docker/Dockerfile .
```

Three stages: a Rust stage builds the supervisor with
`cargo build --release --locked` and the helper with the server profile
(`--profile server`: fat LTO, one codegen unit, no symbols; players' helpers
keep the release profile); a Debian stage builds the room host with
`server/roomhost/build-linux.sh` and the distribution's `nlohmann-json3-dev`,
`libspdlog-dev` and `libfmt-dev`; a slim Debian image of the same release
holds the three binaries, `tini` and a system user `ember-rooms` with uid
4783. `.dockerignore` at the repository root keeps the context to the code
these need.

A room host admits only clients of its own build, so one image is one build.
A new client release is a new image; run it beside the old one only if their
ports and bind addresses differ, otherwise replace it as below.

To move an image to the server without a registry:

```
docker save ember-rooms:<hash> | gzip | ssh vps 'gunzip | sudo docker load'
```

## Install on the server

Docker Engine with the compose plugin, then:

1. `/etc/ember-rooms/` as `deploy/setup.sh` leaves it: the directory 0750
   and `supervisor.secret` (32 or more characters) and `config.json` 0640,
   all owned by root and the host's `ember-rooms` group. The container's
   user (uid 4783) joins that group through `group_add` in `compose.yml`, so
   nothing is re-owned. On a machine that never had the bare install, make
   the same layout:

   ```
   sudo groupadd --system ember-rooms
   sudo install -d -o root -g ember-rooms -m 0750 /etc/ember-rooms
   openssl rand -hex 32 | sudo install -o root -g ember-rooms -m 0640 /dev/stdin /etc/ember-rooms/supervisor.secret
   ```

   `config.json` is optional. Without it the container writes one from its environment (see
   `entrypoint.sh`: bind, secret file, `max_rooms`, `port_range`, timings)
   naming the image's build as the only one. With it, the file is used as it
   is and must list the image's build under `builds` with the paths
   `/usr/local/lib/ember-rooms/builds/<hash>/sf4e-room-host` and `.../sf4-net`,
   which is what `deploy/setup.sh` writes.
2. `/etc/ember-rooms/docker/` with `compose.yml` from this folder and a
   `.env` next to it:

   ```
   BUILD_ID=<hash>
   EMBER_ROOMS_HOST_GID=<getent group ember-rooms | cut -d: -f3>
   EMBER_ROOMS_MAX_ROOMS=10
   EMBER_ROOMS_PORT_RANGE=45800-45899
   EMBER_ROOMS_MEMORY=2400m
   EMBER_ROOMS_TASKS=512
   ```

   Size the memory and task limits as `MemoryMax` and `TasksMax` in
   `deploy/ember-rooms.service`, whose comments give the per-room figures. A
   room past the memory limit is stopped mid-match, so raise the limit
   before the room count.
3. The UDP port range open to the Internet, in ufw and in the provider's
   firewall, as `deploy/setup.sh` does (`ufw allow 45800:45899/udp`).
4. Start it, and check it answers:

   ```
   cd /etc/ember-rooms/docker && docker compose up -d
   curl http://127.0.0.1:47830/health
   ```

5. For a reboot, `deploy/ember-rooms-docker.service` runs `docker compose up`
   from that folder as a unit and stops it with the same drain:

   ```
   sudo cp ~/ember-rooms/ember-rooms-docker.service /etc/systemd/system/
   sudo systemctl daemon-reload && sudo systemctl enable --now ember-rooms-docker
   ```

   Run this or `ember-rooms.service`, never both: they listen on the same
   address and bind the same ports.
6. Connect the bridge as before: `sudo bash ~/ember-rooms/connect-bridge.sh`.
   It reads the bind address and the secret from `/etc/ember-rooms/`, which
   is why the container keeps the host's layout. When the container wrote
   its own config, the script's defaults (`127.0.0.1:47830`, the secret
   file above) are what it used.

## How it runs

- `network_mode: host`: the supervisor listens on `127.0.0.1:47830` where
  the bridge already looks for it, and keeps its own loopback-only rule; the
  room hosts bind their UDP ports on the machine, so players reach them as
  before and nothing passes through a NAT of the container's own.
- `read_only` root with `/var/lib/ember-rooms` on a tmpfs owned by the
  service user (the helpers' state and logs), every capability dropped,
  `no-new-privileges`, and the memory and task limits above. At the memory
  limit the kernel kills one process, not the container, so one room dies
  and the supervisor reaps it (the unit's `OOMPolicy=continue`).
- `stop_grace_period: 660s`: `docker stop` and `docker compose down` send
  SIGTERM, on which the supervisor drains (no new rooms, open rooms until
  empty or `drain_secs`, 600 by default), then exits. Keep the grace period
  above the drain plus 10 s, as the unit's `TimeoutStopSec` is.
- `MALLOC_ARENA_MAX=2` for every process in the container, as in the unit.
- Logs: `docker compose logs -f`; the supervisor logs one line per room
  start and stop and never the secret or an invitation.

## Upgrading to a new client build

Players of the old build keep their rooms until they end, so the old image
drains while the new one is wanted at once. The supervisor serves one
address, so on one machine the simplest order is:

1. Load the new image, set `BUILD_ID` in `.env` to it.
2. `docker compose up -d`: compose replaces the container, which means a
   drain of the old one first (up to the grace period) and no new rooms of
   either build while it lasts. Do it when the rooms are quiet, or keep the
   old service up on another port range and bind address for the drain by
   starting the new image under a second compose project with its own `.env`
   and a config of its own, and pointing the bridge at the new one with
   `connect-bridge.sh` once it answers.

Either way the bridge keeps its `rooms.supervisor_url` and its secret; only
the supervisor behind it changes.

## Checking the image

- Both binaries in the image need only glibc: `docker run --rm --entrypoint ldd ember-rooms:<hash> /usr/local/lib/ember-rooms/builds/<hash>/sf4e-room-host`.
- With a config and secret in place, `/health` answers `ok`, `GET /rooms`
  with the secret answers `[]`, and a `POST /rooms` starts a room host inside
  the container (`docker top`); `docker stop` returns inside the grace period
  with exit code 0.
- `deploy/test-setup.sh` still covers the bare-metal install; nothing in it
  changes for the image.
