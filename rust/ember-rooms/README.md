# ember-rooms

Public room supervisor for SF4 Ember Netplay. The bridge asks it, over
loopback HTTP with a shared secret, to start, list and close public rooms.
Each room is one `sf4e-room-host` child process. The contract is in
`docs/design/PUBLIC_ROOMS.md`.

```
ember-rooms <config.json>
```

## Configuration

```json
{
  "bind": "127.0.0.1:47830",
  "secret_file": "/etc/ember-rooms/secret",
  "max_rooms": 8,
  "port_range": [45800, 45899],
  "empty_close_secs": 120,
  "drain_secs": 600,
  "builds": {
    "<build_id>": {
      "room_host": "/usr/local/lib/ember-rooms/<build_id>/sf4e-room-host",
      "helper": "/usr/local/lib/ember-rooms/<build_id>/sf4-net"
    }
  }
}
```

Only `secret_file` is required; the values above are the defaults. The
supervisor refuses to start if `bind` is not a loopback address, the secret
(trailing whitespace trimmed) is under 32 bytes, the port range is not two
different ports from 1024 up with the lowest first, or `max_rooms` is not 1 to 1024.
Unknown keys are errors. Zero builds is allowed.

## HTTP

Every route except `/health` needs `Authorization: Bearer <secret>`, compared
in constant time. Refusals are JSON `{ "reason": "..." }`.

| Route | Answer |
|---|---|
| `POST /rooms` `{ room_id, name, capacity, build_id, creator, bridge_id, ticket_key, ticket_kid }` | 201 `{ invitation, region }` once the host reports `hosted` |
| | 409 `room_limit` (room limit, no free port, or draining) |
| | 409 `unsupported_build`, 409 `exists`, 400 `invalid_request` (names the `field`) |
| | 502 `host_failed` (the host exited, broke the protocol or took over 30 s) |
| `GET /rooms` | 200 `[{ room_id, members, capacity, tables_playing, invitation, banned, opened }]` |
| `DELETE /rooms/{room_id}` | 204 once the host has been asked to close, or 404 |
| `GET /health` | `ok` |

`opened` is false until the host has reported a status with at least one member
and true from then on, even after the room empties; `members` is the latest
count. The bridge reads it so a member who came and went between two polls still
opens the room.

`room_id` is 32 hex characters (compared case-insensitively), `name` 1 to 64
bytes on one line, `capacity` 2 to 16. A room holds two UDP ports, the two lowest in
`port_range` that no room holds and nothing else on the machine has bound
(not necessarily adjacent), and gets `room_limit` when fewer than two are free.

## Child protocol

This is the interface `sf4e-room-host` implements.

The supervisor starts `room_host` with no arguments, stdin and stdout piped
and stderr inherited. It writes one line of JSON to stdin and keeps the pipe
open:

```json
{ "room_id": "...", "name": "...", "capacity": 8, "build_id": "...",
  "creator": "...", "bridge_id": "...", "ticket_key": "...",
  "ticket_kid": "...", "helper": "/path/to/sf4-net", "port": 45800,
  "coordination_port": 45801 }
```

`port` is the primary endpoint port and `coordination_port` the second UDP
port the room needs.

The child writes one JSON object per line to stdout:

- `{ "type": "hosted", "invitation": "...", "region": "..." }`, once, first.
- `{ "type": "status", "members": 0, "tables_playing": 0, "invitation": "...",
  "banned": ["emb_..."] }` whenever something changes. The invitation changes
  as it renews, so every status carries the current one.
- `{ "type": "closed", "reason": "..." }`, optionally, before exiting on its
  own.

Closing the child's stdin means "close the room and exit". A child that has
not exited 10 s after that is killed. A child that exits for any reason is
reaped, both its ports are freed and its room is removed.

A line over 128 KiB, a line that is not a known JSON shape, a `status` before
`hosted`, an empty invitation and a `status` listing more than 512 banned
accounts are protocol errors; the child is killed. 512 is a room's lifetime
cap on bans (bans are never evicted; the room closes at the cap), and a
status carrying 512 of them and a full-size invitation is about 35 KB.
Objects with an unknown `type` and blank lines are ignored.

## Lifecycle

- A room is closed once its latest status has had `members == 0` for
  `empty_close_secs` without a break. For a room nobody has entered, the time
  runs from `hosted`.
- A host that has not said `hosted` in 30 s is killed.
- On SIGTERM (Ctrl-C on Windows) the supervisor drains: it refuses new rooms,
  keeps serving the API, waits until every room is gone or `drain_secs` has
  passed, closes the rest and exits 0.
- It logs one line per room start and stop to stderr (room id, ports, build,
  exit status, reason). It never logs the secret or invitations.

## Tests

`cargo test` builds `fake_room_host`, a stand-in child that behaves by room
name (see its doc comment), and drives the router with real processes. The
`test-fake-host` feature gates it; the crate lists itself as a dev-dependency
with that feature so plain `cargo test` finds it. A release build does not
contain it.
