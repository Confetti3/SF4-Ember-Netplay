# Hosting public rooms

Public rooms run on one Linux server next to the Ember bridge. This page says
what a room costs, how many rooms a server of a given size can hold, and what
work would let one server hold more. It is the place to start if you want to
help with hosting or with making rooms cheaper to run.

How the pieces fit is in [PUBLIC_ROOMS.md](../design/PUBLIC_ROOMS.md). How to
install and operate the room supervisor is in
[rust/ember-rooms/deploy/README.md](../../rust/ember-rooms/deploy/README.md).

## Where things stand

The server is a 2 core, 1.8 GB VPS. It started with a limit of 4 public rooms,
which players filled quickly. On 2026-10-05 the limit went up to 10, which is
as many as this machine holds safely. Players have asked for something closer
to 100.

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

## Sizing

Rooms a server can hold at about 110 MB each, leaving 1 GB for everything
else (today's server can only spare about 650 MB) and keeping the average CPU
under about 70%:

| Server | Rooms (memory) | Rooms (CPU) | About |
|---|---|---|---|
| 2 cores, 1.8 GB (today) | 10 | 14 | 10 |
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

## Work that would fit more rooms on one server

These need code changes. Each one ships with a client release, because a room
host only admits clients of its own build.

1. **Profile the room helper's memory.** `sf4-net` holds 65 to 88 MB per room,
   which is most of a room's memory. Find out what it holds (heaptrack or
   similar on a Linux build) and whether it can drop to around 30 MB.
2. **Find the idle CPU.** A room with nobody playing should cost close to
   nothing, but the averages above include idle time. Check whether the room
   host or the helper polls or wakes up more often than it needs to.
3. **Host many rooms in one process.** Today every room is its own room host
   plus its own helper. One process serving many rooms would share the
   runtime, the network stack and the binary, and is the biggest possible
   saving. It is also the largest change.
4. **Run rooms on more than one machine.** The supervisor only listens on
   loopback, so rooms must run on the same machine as the bridge. Letting
   the bridge use supervisors on other machines would allow adding small
   servers instead of buying one large one.

## Changing the room limit

After a server change, set the limit with setup.sh. It refuses a limit the
port range cannot hold and warns when the unit's `MemoryMax` looks too small:

```
sudo bash ~/ember-rooms/setup.sh --max-rooms 10
```

Raise `MemoryMax` in `rust/ember-rooms/deploy/ember-rooms.service` by about
110 MB per room first. The restart drains: no new rooms for up to 10 minutes
while open rooms finish.
