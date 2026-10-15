# Public room soak

A long run of public rooms to measure what a room costs the server over time
(memory, CPU, network, sockets) and to catch leaks and instability. The room
hosts run on the VPS, started by hand without the supervisor. The members are
simulated on a Windows PC: 16 per room, each with its own `sf4-net` helper and
Ember ID, all doing what players do at a rate a lobby really has.

```
Windows PC                                        VPS (user katie, no root)
PublicRoomSoakTest.exe   ---- iroh ---->   soak-hosts.sh   -> 8 x sf4e-room-host + sf4-net
  8 rooms x 16 members                      soak-monitor.sh -> CSVs, watchdog
                                            soak-stop.sh
```

Nothing here touches the bridge, the supervisor or the live public rooms. The
soak's processes are recorded under `/tmp/sf4-soak` (change it with
`SOAK_DIR`), each with its pid, start time, boot id and executable
(`room-N/host.proc`, `holder.proc` and `filter.proc`; `soak-lib.sh`). The
scripts only ever signal or count a process while its record still matches
all four, so a pid the system has since given to another process, such as a
live ember service, is left alone.

## What the client does

Room N has the room id `5f1e0d3c2b4a69788796a5b4c3d2e1` followed by N as two hex
digits, and `soak-hosts.sh` gives host N the same id, so the tickets the client
mints are the ones that host admits. Rooms are numbered by the order of the
invitations on the command line (1, 2, ...), or from `--first-room`, or by
writing `N=<invitation>`. Member 0 of every room is the creator the hosts are
configured with and joins first; the others follow, one admission at a time per
room.

With the default `--activity 1`, each member sends a chat line every 1.5 to 5
minutes and does something at the tables every 45 to 150 seconds: members 0 and
1 sit at table 0 (the match table), members 2 to 7 sit at tables 1 to 3, the
rest watch or queue there, and a seated member changes fighter, readies and
unreadies, runs a connection check, or stands up. About every 4 minutes the two
fighters at table 0 play a match (connection checks, Ready, game link up, result,
acknowledgement; no GGPO session is started). About every 3 minutes a random
member leaves and rejoins on a fresh helper. `--activity 5` multiplies the
rate by five for a stress run.

## 1. Build the Linux binaries (in WSL, never on the VPS)

The VPS has too little memory to build, and the build needs `g++`,
`nlohmann-json3-dev`, `libspdlog-dev`, `libfmt-dev` and the Rust toolchain, all
installed in the WSL Ubuntu. From the commit to test (commit first; `git archive`
takes committed content only):

```
git -c core.autocrlf=false archive --format=tar -o soak-src.tar <commit> src rust server
```

In WSL:

```
export PATH=$HOME/.cargo/bin:$PATH RUSTUP_TOOLCHAIN=1.98.0
mkdir -p ~/soak-src && cd ~/soak-src && tar -xf /mnt/c/<path>/soak-src.tar
bash server/roomhost/build-linux.sh . ./sf4e-room-host
export CARGO_TARGET_DIR=$HOME/soak-target
(cd rust/sf4-net && cargo build --profile server --locked)
```

You now have `~/soak-src/sf4e-room-host` and `~/soak-target/server/sf4-net`.
Both link only against glibc, like the supervisor's pair
(`server/roomhost/README.md`).

## 2. Build the Windows side

From the build folder of this worktree (`build\current`), with the usual build
environment:

```
cmake --build build\current --target PublicRoomSoakTest IrohHelper RoomTicketTool
```

That gives `PublicRoomSoakTest.exe`, `sf4-net.exe` and
`rust-ember-target\debug\examples\room_ticket.exe` in `build\current`.

## 3. Check both sides agree

Hosts and tickets share a bridge id, build id, ticket key and kid, and creator
(the values of `PublicRoomHostTest --remote`). They are written at the top of
`soak-hosts.sh`; the client prints what it will use:

```
.\PublicRoomSoakTest.exe --print-config .\rust-ember-target\debug\examples\room_ticket.exe
```

Compare the `ticket_key`, `ticket_kid` and `creator` lines with the script.

## 4. Copy to the VPS

```
ssh vps "mkdir -p ~/soak"
scp ~/soak-src/sf4e-room-host ~/soak-target/server/sf4-net vps:soak/
scp server/roomhost/soak/*.sh vps:soak/
ssh vps "chmod +x ~/soak/*"
```

## 5. Start the hosts

8 hosts on spare UDP ports 45860 to 45875 (each host uses two: 45860 and 45861,
45862 and 45863, and so on). Check that the ports are free and open in the
firewall if there is one; with them closed the helpers still connect through the
iroh relays, but the round trips then include the relay hop.

```
ssh vps
cd ~/soak
./soak-hosts.sh ./sf4e-room-host ./sf4-net 8 45860
cat /tmp/sf4-soak/invitations.txt
```

The script refuses to start over live soak hosts or a port that is in use. Each
host logs to `/tmp/sf4-soak/room-N/stdout.log` (timestamped status lines, the
invitation shortened) and `stderr.log`, and its invitation to `room-N/invitation`.
Invitations stay valid for a week, long enough for the run; the hosts' renewed
invitations are not tracked, the first one is what the client uses.

## 6. Start the monitor and the watchdog

Before any load, so the first rows are the baseline:

```
cd ~/soak
setsid nohup ./soak-monitor.sh ~/soak-mon.csv 30 > ~/soak-mon.log 2>&1 < /dev/null &
```

Every 30 s it appends one row to `soak-mon.csv` (memory, swap, load, the soak's
host and helper memory and CPU, the live bridge, supervisor and short-link
service by name, interface byte counters, UDP sockets) and one row per room to
`soak-mon.csv.rooms.csv`. Every 5 s the watchdog checks the box. If
`MemAvailable` falls under 250 MB or swap in use grows by more than 300 MB over
what it was at start, it runs `soak-stop.sh` and writes the reason to
`soak-mon.csv.watchdog.log`, so the live bridge is not starved. Change the limits
with `SOAK_MIN_AVAIL_MB` and `SOAK_MAX_SWAP_GROWTH_MB`, or turn it off with
`SOAK_WATCHDOG=0`. The monitor exits by itself once the hosts are gone.

## 7. Run the client (Windows)

Fetch the invitations, one per line, then start the run. Keep the PC awake and
leave it alone: 8 rooms of 16 members are 128 helper processes (about 40 MB each,
roughly 5 to 6 GB) and a few cores.

```
ssh vps cat /tmp/sf4-soak/invitations.txt > invitations.txt
.\PublicRoomSoakTest.exe .\sf4-net.exe .\rust-ember-target\debug\examples\room_ticket.exe `
    --members 16 --minutes 240 --log soak.csv (Get-Content invitations.txt)
```

It fills the rooms (about a minute per room, all rooms in parallel), runs for
240 minutes, then leaves cleanly and prints a summary. Ctrl-C does the same
early. Output:

- `soak.csv`: one row per room per minute. Counters end in `_total`; latencies
  and the queue and helper figures are for that minute.
- `soak.csv.events.log`: every failure with its time, and each join and leave.
- the summary and `RESULT: PASSED` or `RESULT: FAILED` on the console; the exit
  code is 0 or 1.

The run fails if any room breaks one of: stayed up (not lost), 50 percent of its
members connected on average, at most 2 percent of actions timing out, p95 chat
round trip at most 5 s, at most 5 percent of timed chat lines never seen, at most
25 percent of matches failing, at most 0.25 dropped members (control losses,
helper crashes included) per member-hour, at most 20 percent of join attempts failing, and
the client loop not saturated for more than 20 percent of the minutes (a
saturated client makes the numbers about the client). Each limit has an option
(`--max-timeout-pct`, `--max-chat-p95-ms`, `--max-chat-lost-pct`,
`--max-match-fail-pct`, `--max-drops-per-member-hour`, `--max-join-fail-pct`).

## 8. Stop and collect

The client ends by itself. Then on the VPS:

```
cd ~/soak
./soak-stop.sh
```

That closes each host's stdin (the order to close the room and exit), waits 15 s
(change it with the first argument), and only then ends stragglers. It then
removes the records of the processes that are gone, so running it again does
nothing. A record whose pid now belongs to another process is reported in
`stop.log` and removed without a signal. Collect:

```
scp vps:soak-mon.csv vps:soak-mon.csv.rooms.csv vps:soak-mon.log .
scp vps:soak-mon.csv.watchdog.log .      # only exists if the watchdog tripped
ssh vps "cd /tmp/sf4-soak && tar -czf - --exclude=in --exclude=out ." > soak-state.tgz   # host logs, stop.log
```

Read memory per room from `soak-mon.csv.rooms.csv` (`host_rss_kb`,
`helper_rss_kb` against time: a leak is a line that keeps climbing after the
rooms are full), CPU from the `_cpu_pct` columns (percent of one core), and the
network from the byte counters' differences. The memory columns keep their
names but hold the proportional set size (`Pss` from `smaps_rollup`): the
binary's pages, which every helper shares, count once in all rather than once
per process, so the figure is what one more room costs. Runs before October
2026 recorded the resident set, which overstates a helper by the 19 MB or so
of its binary.

## Reading the client CSV

| column | meaning |
| --- | --- |
| `members_active`, `view_members_min/max` | members connected, and the member count they see |
| `joins_total`, `rejoins_total`, `join_failures_total`, `refused_total` | admissions and failures; refused is the host turning a proof away |
| `control_losses_total`, `helper_crashes_total`, `degraded_total` | members dropped because their room control or helper went away; helper crashes are the part of the control losses where the helper process exited, not extra drops; degraded is a control that came back in time |
| `actions_*`, `rejects_by_reason` | room actions sent, answered, rejected by reason, and unanswered after 15 s |
| `chat_rtt_*_ms` | send until the line is in another member's snapshot (one line in four is timed) |
| `stale_*_ms` | time since a member's last snapshot; it grows in a quiet room, `behind_members` is the better lag signal |
| `matches_*`, `probes_*` | table 0 matches and connection checks |
| `backlog_*`, `helper_lag_ms_max`, `helper_silent_ms_max` | deepest queue, helper scheduling lag and longest helper silence any member showed |
| `room_helpers*`, `all_helpers*`, `soak_ws_mb`, `client_cpu_pct`, `loop_*` | the Windows PC: helper count, memory, CPU, and how busy the client loop was |

## Rehearsal in WSL

The same scripts run in WSL (`SOAK_DIR=$HOME/soak-state ./soak-hosts.sh ... 2
45900`, then the client on Windows against those invitations). WSL2 is behind a
NAT, so the helpers meet through the public iroh relays; round trips are then
about a second and the connection checks often report `unavailable`, which
`PublicRoomHostTest --remote` does in the same setup. Do not read latency from
such a run.

To try the watchdog, start the monitor with a limit the box cannot meet:

```
SOAK_MIN_AVAIL_MB=999999 ./soak-monitor.sh ~/mon.csv 10
```

It should stop the hosts within 5 s and write `~/mon.csv.watchdog.log`.

## What the WSL rehearsals showed

Two rooms of 16 members on one WSL host machine, the client on Windows, through
the public relays:

- At the default rate: no unanswered actions, chat reaching another member in
  0.7 to 1.8 s at the median (3 s at p95, relay round trips), every match
  completing. Per room the host process used 35 MB resident and about 1 percent
  of a core, its helper 60 MB and 5 to 11 percent of a core.
- At 3 times the rate (about 0.6 actions a second per room) chat took 2 to 4 s at
  the median and a few actions went unanswered; at 5 times it took 15 to 25 s
  and two thirds of the actions timed out, while the host used a fraction of a
  core and the client's loops sat at a third busy. That is a pipeline limit in
  the room's commit path under relay latency, not CPU on either side; the VPS
  run shows where it falls with real round trips. If a run fails on
  timeouts, compare `backlog_msgs_max` and `helper_lag_ms_max` (client side) with
  the host rows in `soak-mon.csv.rooms.csv` before blaming either.
- Early runs reported accepted chat lines missing from every snapshot and up to
  a fifth of the timed lines never seen. Both were the client's own counting,
  not the room. The missing lines were all said by members who later left for
  a planned rejoin, and the room drops a leaver's lines; the checks now skip
  them. The unseen lines were stamped later in the client's loop than the time
  the check compared them with, so the unsigned difference wrapped and the line
  counted as never seen in the tick it was sent; every one of them had been
  accepted within a few seconds. With both fixed, a run at 3 times the rate
  shows neither. `chat_lines_missing_total` and `chat_lost_total` still fail
  the run (`--max-chat-missing-pct 1`, `--max-chat-lost-pct 5`), and an unseen
  line's event says whether and when the host accepted it.
