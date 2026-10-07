# Deploying the room supervisor

These notes cover running `ember-rooms` on the VPS (`ssh vps`) from staged
binaries. The same service as a container, with the room host and helper
built into the image, is in [../docker/README.md](../docker/README.md). What
the supervisor does and its API are in `../README.md` and
`docs/design/PUBLIC_ROOMS.md`. What a room costs, how many rooms a server
size holds and hosting options are in
`docs/development/PUBLIC_ROOM_HOSTING.md`.

## What runs where

- `ember-rooms` is one systemd service, user `ember-rooms`, listening on
  `127.0.0.1:47830`. Only the bridge talks to it, with a shared secret. It is
  not behind nginx and nothing in nginx changes.
- For every public room it starts two child processes: `sf4e-room-host` and
  its helper `sf4-net`. They open UDP sockets to the Internet, so players
  connect to them directly (iroh QUIC) and nothing passes through nginx.
- Files on the server:

| Path | What |
|---|---|
| `/usr/local/lib/ember-rooms/ember-rooms` | the supervisor binary |
| `/usr/local/lib/ember-rooms/builds/<build_id>/` | `sf4e-room-host` and `sf4-net` for one game release |
| `/etc/ember-rooms/config.json` | settings, including the `builds` map |
| `/etc/ember-rooms/supervisor.secret` | shared secret, `0640 root:ember-rooms` |
| `/var/lib/ember-rooms/` | state for the room hosts |
| `/etc/systemd/system/ember-rooms.service` | the unit |

The unit allows 2400 MB of memory, 1 GB of swap and 512 tasks for the supervisor and all
rooms together, and the example config starts at `max_rooms` 10. A room (room
host plus helper) measured about 100 MB and 8 tasks on the VPS in October
2026. The machine has 3.8 GB and 2 cores and also runs the bridge (its own 256 MB limit)
and the short-link service (96 MB); each unit has its own limit, so a full
room supervisor cannot starve the bridge. Measure a room's memory (`systemctl
status ember-rooms` shows the total) before raising `max_rooms`; raise
`MemoryMax` by about 110 MB per room, or move to a larger machine. setup.sh
warns when `MemoryMax` or `TasksMax` looks too small for `max_rooms`.

To change the room limit on an installed server, stage this folder and run
setup.sh with `--max-rooms`. It refuses a limit the port range cannot hold
(two ports per room) and puts the old config back if the restart fails:

```
ssh -t vps "sudo bash ~/ember-rooms/setup.sh --max-rooms 10"
```

## Staging layout

As with the bridge, you copy files into your home folder and run a script with
sudo. Put everything from this `deploy` folder in `~/ember-rooms/`, plus:

```
~/ember-rooms/bin/ember-rooms                       Linux build of the supervisor
~/ember-rooms/builds/<build_id>/sf4e-room-host      Linux build, same commit as the release
~/ember-rooms/builds/<build_id>/sf4-net             Linux build, same commit as the release
```

`<build_id>` is the sidecar build hash of the client release. The folder name
is used as the key in `config.json`, so it must match the hash exactly
(letters, digits, dots, dashes and underscores only).

## First install

```
ssh -t vps "sudo bash ~/ember-rooms/setup.sh"
```

It creates the user, directories and secret, installs the binary and any
staged builds, creates `config.json` from `config.example.json` if there is
none, opens the UDP port range in ufw if ufw is active, installs and starts the
service and checks `/health`. It never overwrites an existing `config.json`
or secret. With no builds staged the service runs but refuses every room as
`unsupported_build`.

If the install, the restart or the health check fails, setup.sh puts back the
supervisor binary, unit and `config.json` it had saved before replacing them,
removes the builds this run added, restarts the previous service and checks
`/health` again. It then exits with an error saying whether the previous
installation came back. On a first install there is nothing to restore, so it
stops the service and removes what it added. `test-setup.sh` in this folder
runs these paths against a mocked server (no root, no systemd): `bash
test-setup.sh`.

Then connect the bridge, once the bridge build that supports public rooms is
installed:

```
ssh -t vps "sudo bash ~/ember-rooms/connect-bridge.sh"
```

That runs the bridge's backup, adds `"rooms": { "supervisor_url":
"http://127.0.0.1:47830" }` to `bridge.json`, stores the supervisor secret
through `set-integration-secret.sh rooms` (read from the secret file, so you
never copy it) and restarts the bridge. It undoes the config change if the
bridge does not come back. Running it again when everything is in place does
nothing.

Example `builds` entry, which setup.sh writes for you:

```json
"builds": {
  "<build_id>": {
    "room_host": "/usr/local/lib/ember-rooms/builds/<build_id>/sf4e-room-host",
    "helper": "/usr/local/lib/ember-rooms/builds/<build_id>/sf4-net"
  }
}
```

## Every client release needs its own room hosts

A room host only admits clients of its own build. For each client release,
build `sf4e-room-host` and `sf4-net` from the same commit as that release and
add them under the release's build hash. Keep the old release's build until
its players have moved on.

To add a build, stage `~/ember-rooms/builds/<build_id>/` and run setup.sh
again. It copies the files, adds the `builds` entry and restarts the service.
Existing builds stay as they are: a build id names one exact pair of binaries,
so if the staged files for an installed id differ from the installed ones,
setup.sh refuses before changing anything (running rooms use those files).
Stage changed binaries under a new build id.

To remove an old build, delete its folder from `~/ember-rooms/builds/` and run:

```
ssh -t vps "sudo bash ~/ember-rooms/setup.sh --prune"
```

Without `--prune`, setup.sh keeps builds that are no longer staged. Rooms of a
removed build keep running until they are empty; new rooms for that build are
refused as `unsupported_build`.

## Restarting

The supervisor reads `config.json` only at start, so adding a build or
changing a setting means a restart. A restart drains: SIGTERM stops new rooms,
existing rooms run until they are empty or `drain_secs` (600) has passed, then
the rest are closed and the service exits. The unit waits up to 660 seconds for
that, so `systemctl restart ember-rooms` and setup.sh can take up to eleven
minutes when rooms are busy. During the drain the supervisor refuses new
rooms. Restart when the rooms are quiet.

The unit uses `KillMode=mixed`: systemd sends the SIGTERM to the supervisor
only, so the room hosts keep running while it drains. Only if the unit is
still running at `TimeoutStopSec` does SIGKILL go to everything left in it.

```
ssh -t vps "sudo systemctl restart ember-rooms"
```

If you change `drain_secs`, change `TimeoutStopSec` in the unit to stay above
it by about a minute.

## Logs and checks

```
ssh -t vps "sudo journalctl -u ember-rooms -f"
ssh vps "curl -fsS http://127.0.0.1:47830/health"
```

The supervisor logs one line per room start and stop. Room host and helper
output goes to the same journal. Neither logs the secret or invitations.

## Firewall

Each room uses two UDP ports from `port_range` (45800 to 45899 by default),
so 8 rooms hold 16 of them and the range has room to spare. setup.sh opens the
range in ufw only if ufw is active, and says so if it is not. If the hosting
provider has its own firewall in front of the machine, open the same UDP range
there; setup.sh cannot see or change that. If you change `port_range`, run
setup.sh again to add the new rule, and remove the old rule by hand
(`sudo ufw status numbered`, then `sudo ufw delete N`).
