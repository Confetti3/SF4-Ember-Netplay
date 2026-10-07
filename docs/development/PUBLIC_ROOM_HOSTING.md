# Hosting public rooms

Public rooms run on one Linux server next to the Ember bridge. This page says
what a room costs, how many rooms a server of a given size can hold, and what
work would let one server hold more. It is the place to start if you want to
help with hosting or with making rooms cheaper to run.

How the pieces fit is in [PUBLIC_ROOMS.md](../design/PUBLIC_ROOMS.md). How to
install and operate the room supervisor is in
[server/ember-rooms/deploy/README.md](../../server/ember-rooms/deploy/README.md).

## Where things stand

The server is a 2 core VPS. It started with 1.8 GB and a limit of 4 public
rooms, which players filled quickly. On 2026-10-05 the limit went up to 10. On
2026-10-06 the memory went to 3.8 GB with a 2 GB swapfile and the limit to 20,
past what 2 cores hold comfortably (14 by the table below), while a larger
server is on order. Players have asked for something closer to 100, and some
for 200; what that takes is under "Can one server hold 200 rooms?" below.
The service can also run as a container ([server/ember-rooms/docker](../../server/ember-rooms/docker/README.md)).

## What one room costs

Each public room is two processes: `sf4e-room-host` and its network helper
`sf4-net`. Measured on the VPS with 4 rooms open for 1 to 4 hours:

| | Per room | Notes |
|---|---|---|
| Memory | about 100 MB | helper 65 to 88 MB, room host about 20 MB. Flat over 15 minutes, so no steady leak was seen, but older rooms used more. |
| CPU | about 0.1 core on average | helper about 7%, room host about 4%; the helper peaked at 18%. |
| Threads | about 8 | well under the unit's `TasksMax`. |
| Upload | up to 80 KB/s | the whole machine sent 6 GB in 5 hours with 4 rooms open, so this is an upper bound. |
| UDP ports | 2 | from `port_range`; the default 45800 to 45899 fits 50 rooms. |

The system, the bridge, the short-link service and nginx take about 500 MB
together, so plan on 1 GB for them.

Two things to know when reading those figures. The memory column is resident
set size, which counts the helper binary's own pages (about 19 MB of a 36 MB
binary) in every helper although the kernel shares them between processes;
`Pss` in `/proc/<pid>/smaps_rollup` is the real per-room number, and the soak
monitor records that now. And most of the CPU was spent waiting: a helper run
on its own with no room open measured 3.7 percent of a core (4 threads, 24 MB
`Pss` of which 5 MB private) on a 2.3 GHz Xeon, because its actor loop woke
every 2 ms whether or not there was anything to do. After the October 2026
work below the same measurement is 0.3 percent of a core and 22 MB `Pss`
(3.8 MB private), and the room host binary is 1.9 MB instead of 3.9. What a
hosted room costs with those changes is still to be measured on the server
(a room cannot be hosted without the relay, so it was not measured where the
code was written); do it before raising `max_rooms`, and put the numbers in
the table above.

## Sizing

Rooms a server can hold at about 110 MB each, leaving 1 GB for everything
else, and keeping the average CPU under about 70%:

| Server | Rooms (memory) | Rooms (CPU) | About |
|---|---|---|---|
| 2 cores, 1.8 GB | 10 | 14 | 10 |
| 2 cores, 3.8 GB (today) | 25 | 14 | 14; set to 20 |
| 4 cores, 8 GB | 60 | 28 | 25 to 30 |
| 8 cores, 16 GB | 130 | 56 | 50 to 55 |
| 12 cores, 24 GB | 200 | 84 | 80 |
| 16 cores, 32 GB | 280 | 112 | 100 |

CPU is the limit above about 10 rooms, not memory. 100 rooms at today's cost
needs about 12 GB of memory, 14 to 16 cores and up to 8 MB/s (65 Mbit/s) of
upload, which is around 20 TB a month if every room is busy all the time.
Anything past 50 rooms also needs a wider `port_range` (and the firewall rule
setup.sh writes for it).

## Hosting options

Prices checked 2026-10-05 on each provider's site. Introductory prices change
often, so check before buying.

| Provider | Plan | Cores / memory | Traffic | Price |
|---|---|---|---|---|
| IONOS (current host) | VPS L+ | 4 / 8 GB | unlimited, 1 Gbit/s | $8 a month for 3 months, then $25 |
| IONOS | VPS XL+ | 8 / 16 GB | unlimited, 1 Gbit/s | $14, then $47 |
| IONOS | VPS XXL+ | 12 / 24 GB | unlimited, 1 Gbit/s | $20, then $68 |
| OVHcloud | VPS-3 | 6 / 12 GB | unlimited, 2 Gbit/s | about $12 to $15 |
| OVHcloud | VPS-4 | 8 / 24 GB | unlimited, 3 Gbit/s | about $23 to $28 |
| Hetzner (US) | CPX41 | 8 / 16 GB | 1 TB included | about $141 |

What matters when comparing:

- Traffic. Busy rooms can send many terabytes a month, so plans with metered
  traffic (Hetzner's US locations include 1 TB) get expensive fast. Prefer
  unlimited traffic.
- Cores. CPU runs out first. The per-room figures were measured on IONOS
  vCores; shared cores elsewhere may be slower or faster.
- Staying with IONOS. Upgrading the current plan avoids moving the bridge
  database, the certificates and the DNS records. Ask IONOS whether the
  upgrade keeps the same IP address.
- Moving. A new provider means a new IP address, so `embernetplay.link` and
  its subdomains need new DNS records, and the bridge, its database, the
  short-link service, nginx with the certificate and the room supervisor all
  move together.

## Can one server hold 200 rooms?

Players have asked for something near 200 rooms. On a 4 core, 8 GB server
with 1 GB kept for the system, the bridge, nginx and the short-link service
and the CPU held under 70 percent, that is 35 MB and 1.4 percent of a core
per room. At the October 2026 figures above (about 100 MB and 10 percent) a
room is three times too big and seven times too busy.

What the October work (below) is expected to change has not been measured
with a hosted room yet, so the figures in this paragraph are targets, not
results. An idle room no longer wakes up hundreds of times a second, so the
target for an empty room is about 1 percent of a core. A busy room does real
work: every action sends a checkpoint of about 320 KiB to every member,
hashed, compressed and parsed. On 2026-10-07, with the older build, 14 rooms
with matches running cost 2 to 9 percent of a core per helper and 0.5 to 3
per room host. Memory stays at two iroh endpoints, a runtime, a Raft log of
a few checkpoints and the HTTP and TLS clients per helper; the target is
45 MB per room with its host.

The arithmetic on a 4 core server, keeping 70 percent (2.8 cores) for rooms:

| Mix | CPU | Fits |
|---|---|---|
| 100 rooms, all busy at 3 to 5 percent | 3 to 5 cores | no |
| 100 rooms, 30 busy at 5 percent, 70 empty at 1 percent | 2.2 cores | yes |
| 50 rooms, all busy at 5 percent | 2.5 cores | yes |

So about 100 rooms holds only while most of them are quiet, and about 50
when every room is playing; the 2 core server is half of each. Memory at
45 MB is not the limit (100 rooms is 4.5 GB). Measure a hosted room with
the soak (`server/roomhost/soak`) before raising `max_rooms` on any of this,
and move the table above to what it shows.

200 on one server needs rooms that share a process (item 1 below): one
helper with one endpoint, one runtime and one set of relay, DNS and address
publication tasks, and one room host holding many room models. Per room
that would leave the Raft state and the control connections, and the CPU
would be only real activity; how much that is has to be measured once it
exists. Short of that, several small servers (item 2) reach the same count
with more machines to run.

## Work that would fit more rooms on one server

Each code change here ships with a client release, because a room host only
admits clients of its own build.

Done in October 2026, shipping with the next client release:

- **The idle CPU.** The helper's actor loop woke every 2 ms and, once a room
  was open, copied the committed checkpoint out of its Raft store on every
  wakeup to compare a revision number; the room host's pipe worker polled
  the helper every 2 ms and its main loop ticked every 16 ms whether or not
  anything had happened. Both now wait for work: the helper's tick follows
  what is in flight (2 ms during a game or a checkpoint transfer, 25 ms with
  a room open, 50 ms with none) and every arrival brings it forward; the room
  host sleeps until its helper has an event, 250 ms at most while the room is
  empty and settled. The once-a-second coordination refresh reads a digest
  hashed once per revision instead of copying and rehashing the checkpoint,
  and a headless helper no longer probes for a gateway port mapping.
- **Memory.** `MALLOC_ARENA_MAX=2` in the unit and the image, so glibc does
  not keep one arena per thread at its peak; the room host built with LTO,
  section garbage collection and no symbols; a received commit's bytes moved
  from the receiver to the decoder instead of copied twice. What a helper
  holds (two endpoints and their relay, DNS and address-publication state,
  the Raft log, the HTTP client) is still per process; the shared pages of
  its 36 MB binary are not per room, which the soak monitor now measures as
  proportional set size.
- **A container.** The room service as one image per client build,
  `server/ember-rooms/docker`, so the binaries are built in one place and a
  new or bigger machine is a `docker compose up`.

Still to do:

1. **Host many rooms in one process.** The path to 200 rooms on one server,
   and the largest change: the helper's IPC frames and events carry a room
   id; one actor per room runs behind one shared endpoint, which already
   receives the room id in every coordination RPC and control proof; the
   room host multiplexes rooms over one helper connection; the supervisor
   starts one pair per build instead of one per room; the two UDP ports per
   room become a shared pair.
2. **Run rooms on more than one machine.** The supervisor listens on
   loopback, so rooms run on the bridge's machine. A bridge that keeps a
   directory of supervisors (an id, an address and a secret each, reached
   over HTTPS behind nginx on the other machine), places rooms across them,
   polls each and routes a close to the room's host would add small servers
   instead of a larger one. It is the fallback if item 1 slips.
3. **The rest of the helper's memory.** With the copying gone, measure what
   a helper with an open room still holds (heaptrack on a Linux build) and
   whether its coordination endpoint can share the main one for a public
   host, which halves the relay, address and DNS work per room.

## Changing the room limit

After a server change, set the limit with setup.sh. It refuses a limit the
port range cannot hold and warns when the unit's `MemoryMax` looks too small:

```
sudo bash ~/ember-rooms/setup.sh --max-rooms 10
```

Raise `MemoryMax` in `server/ember-rooms/deploy/ember-rooms.service` by about
110 MB per room first. The restart drains: no new rooms for up to 10 minutes
while open rooms finish.
