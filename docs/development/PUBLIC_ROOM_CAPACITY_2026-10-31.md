# Public room capacity, 2026-10-31

What a public room costs after the October performance work and the allocator
fix, and how many rooms the VPS can hold on its own. The background and the
longer-term work are in [PUBLIC_ROOM_HOSTING.md](PUBLIC_ROOM_HOSTING.md).

## Target

**100 public rooms on the VPS alone** (4 vCPU, 8 GB, IONOS VPS L+), without
server1. Per room that means at most 40 MB of memory (room host plus helper)
and about 2.8% of a vCPU on average, which keeps the rooms under 70% of the
machine's CPU and inside the unit's 4500M.

Where it stands:

| Limit | 100 rooms needs | Measured | Fits |
|---|---|---|---|
| Memory | 40 MB a room, 4.0 GB | 29 MB a room, 2.9 GB | yes |
| CPU | 2.8 vCPU at 70%, so 2.8% a room | about 2.9% a room on the VPS today, 2.9 vCPU | not yet; see below |
| Ports | 200 UDP ports | `port_range` 45800-45899 holds 50 rooms; 45900-45999 is server1's | widen down to 45700 first |
| Tasks | 1600 at the setup check of 16 a room | `TasksMax` 2048 | yes |
| Upload | about 65 Mbit/s if every room is busy | 20 Mbit/s with 11 rooms today | yes on 1 Gbit/s unmetered |

So memory is no longer the limit; CPU is. Raise the VPS to **80 rooms** first
(about 2.3 vCPU, 58% of the machine at today's average). 100 rooms needs the
average room at or under 2.8% of a vCPU, measured over busy evenings at 80
(step 3 below), or the CPU work at the end of this page.

## What a room costs now

Two rooms of 4 members on each build, side by side in WSL (pinned to 4 cores,
rejoins every 60 s, matches every 60 s), 12 minutes, both runs passing every
soak criterion:

| After ~10 rejoins per room | Old helper | Fixed helper |
|---|---|---|
| Helper (Pss) | 55 MB | 15 MB |
| Room host (Pss) | 15.6 MB | 13.7 MB |
| Room total | about 70 MB | about 29 MB |
| CPU over 12 minutes | helper 7 s, host 2 s | helper 7 s, host 3 s |

Without rejoins (10 rooms of 4 members playing, before the fix) the helper sat
at 12 MB and the host at 14.5 MB, so the fix removes the growth and leaves the
base cost as it was. An empty room is about 5 MB on both builds.

The VPS itself at 23:58Z (11 rooms, real players, older build): room hosts and
helpers together used 0.32 of a core, 2.9% a room on average, the busiest
rooms about 7%. The machine was 10% busy in total. Those are `ps` figures,
averaged over each process's life, so they are a rough guide, not the gate.

## The memory fix

Each member who joined a room left about 9 MB held in the room's helper for
good, so a room's memory grew with every join and never came back: 50 to
55 MB after four to ten rejoins, 108 MB for the oldest live room in October.
It was not a leak. glibc kept the freed blocks: it raises its mmap threshold
to the largest block freed so far, so later large buffers came from the heap
and stayed there after being freed. With one arena, a fixed 128 KiB mmap
threshold and a 128 KiB trim threshold the same runs stayed flat.

- The room host and helper set those with `mallopt` at startup on glibc
  (`rust/sf4-net/src/main.rs`, `server/roomhost/main.cxx`), so builds from
  now on do not depend on the environment.
- The unit and the container image set the same three variables, so the older
  builds the supervisor still runs get the fix as well.
- The unit's room budget drops from 100 MB to 40 MB, `TasksMax` goes to 2048,
  and setup.sh warns at 40 MB a room.

## Steps on the VPS (sudo, Kate)

1. Stage the updated `server/ember-rooms/deploy` and run setup.sh. The unit's
   environment changed, so this restarts the supervisor with a drain (rooms
   run until empty, at most 10 minutes).
2. Widen `port_range` downwards to `[45700, 45899]`: 45900-45999 belongs to
   server1's room box, which the VPS forwards through the tunnel. Add
   45700-45799 to the reserved ports in `/etc/sysctl.d/60-ember-tunnel.conf`
   (`45700-45999`, as `setup-vps.sh` in the private ember-tunnel repo now
   writes) and apply it with `sysctl --system`, set the range in
   `/etc/ember-rooms/config.json`, and run setup.sh with `--max-rooms 80`. It
   adds the ufw rule for the new range; remove the old `45800:45899` rule by
   hand and open UDP 45700-45799 in the IONOS Cloud Panel firewall, which sits
   in front of ufw.
3. During a week at 80, on busy evenings, measure the rooms' CPU over an
   interval rather than with `ps` (which averages over each process's life).
   The service's cgroup counts the CPU of every room host and helper,
   including rooms that close during the interval; divide what it used in
   5 minutes by 300 s and by the average room count, sampled each minute.

   ```bash
   stat=/sys/fs/cgroup/system.slice/ember-rooms.service/cpu.stat
   usec() { awk '$1 == "usage_usec" {print $2}' $stat; }
   a=$(usec); rooms=0
   for i in 1 2 3 4 5; do sleep 60; rooms=$((rooms + $(pgrep -c -x sf4e-room-host))); done
   b=$(usec)
   awk -v d=$((b - a)) -v n=$rooms 'BEGIN {if (n) printf "per room: %.1f%% of a vCPU\n", 100 * d / 1e6 / 300 / (n / 5)}'
   ```

   Raise to 100 only if the busy-evening figure is 2.8% or less; at 3% the
   rooms would take 75% of the machine.

server1 keeps running as it is; it is no longer needed to reach the target.

## Costs

The VPS is IONOS VPS L+ (4 vCPU, 8 GB, unlimited traffic at 1 Gbit/s), $25 a
month after the introductory price (prices from PUBLIC_ROOM_HOSTING.md,
checked 2026-10-05). At 100 rooms that is $0.25 a room a month, with no new
machine and no traffic charge. The next step up, should CPU run out, is VPS
XL+ (8 vCPU, 16 GB) at $47 a month, about 200 rooms at today's per-room cost.

## Not measured, and what is next

- The benchmark's 50-room runs had only a third to a half of their members
  connected, because the Windows soak clients failed joins at that scale, so
  no 50-room figure here is used. The figures above come from runs where
  every room met every criterion.
- The room host's 14 MB is real data, mostly the room's sync state held as
  parsed JSON. Holding it serialized would roughly halve the room, but that
  is a change to the session code, not a setting.
- CPU per room is the next limit: every action sends the whole room
  checkpoint (about 320 KiB) to each member, hashed, compressed and parsed.
  Sending only what changed is the change that would make 100 busy rooms
  comfortable on 4 vCPU.
