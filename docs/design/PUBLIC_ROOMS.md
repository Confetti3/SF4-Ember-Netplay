# Public rooms

Status: in development on `feat/public-rooms`. This file is the interface
contract the pieces are built against. Change it in the same commit as any
change to a shape it names.

## What a public room is

A public room is listed by a bridge, and any player with an Ember ID may join
it. Its room authority does not run in a player's game. A room host process on
the bridge's server runs the same `SessionServer` and `RoomModel` the game
uses, with its own helper, and is the room's only coordination voter. Players
are learners. Gameplay links are unchanged: fighters connect to each other and
spectators stream from P1.

Private rooms are unchanged in every respect: wire forms, checkpoints,
coordination and admission.

A public room is "server-owned". The differences from a private room:

| | Private | Server-owned |
|---|---|---|
| Authority | the host player's game | room host process |
| Coordination voters | 1 to 5 members | the room host only, always |
| Leader loss | election, new leader | the room is closed |
| Admission | room capability | signed ticket naming the Ember ID and endpoint |
| Kick | until the helper restarts | by Ember ID, for the room's life |
| Moderator (`host`) | hosting player, then oldest member | first member in (the creator), then oldest member |
| Empty room | closes | closes after a grace period |

Bans follow one rule. A server-owned room may ban at most 512 accounts in its
lifetime (`MAX_ROOM_BANS`), counted per room and not per session. A ban is
never evicted or expired while the room lives; it ends when the room does. The
room authority (the room model in the room host) closes the room when it would
have to ban a 513th account, so a kick never goes unrecorded and no one it
removed can return. The number is the same in the room model, the helper's ban
set (a ban past 512 is refused with the `ban_limit` error), the supervisor
(a status listing more than 512 banned accounts is a protocol error) and the
bridge's `room_bans` table, which keeps every reported ban.

## Pieces

```
game -- helper --HTTPS--> bridge /v1/rooms          (list, create, ticket)
                             | loopback HTTP, shared secret
                             v
                          ember-rooms supervisor     (one host per room)
                             v
helper --iroh--> sf4e-room-host + its sf4-net child  (authority, sole voter)
```

- `rust/ember/protocol/src/rooms.rs`: shared types below.
- `rust/ember/bridge/src/routes/rooms.rs`, `migrations/012_rooms.sql`.
- `rust/ember-rooms`: supervisor, loopback only.
- `src/roomhost/`: `sf4e-room-host`. `RoomHost` is the portable core; the
  helper process and its pipe are the one platform file: a named pipe on
  Windows (`RoomHostHelperWindows.cxx`), the stdio helper on Linux
  (`RoomHostHelperPosix.cxx`; build and deployment in `src/roomhost/README.md`).
- `rust/sf4-net`: Unix entry, public host mode, public join, client requests.

## Ticket

The bridge signs a ticket per join. Signature domain `Domain::RoomTicket`,
prefix `EMBER:ROOM-TICKET:1\n`, same construction as `Binding`.

```rust
pub struct RoomTicket {
    pub version: u8,            // play::VERSION
    pub bridge_id: String,      // brg_...
    pub room_id: String,        // 32 hex, the room's 16-byte id
    pub ember_id: EmberId,
    pub endpoint_id: String,    // 64 hex, the joiner's helper endpoint
    pub issued_at: u64,         // unix seconds
    pub expires_at: u64,        // issued_at + 60
}
pub struct SignedRoomTicket { pub ticket: RoomTicket, pub kid: String, pub signature: String }
```

`RoomTicket::check`, `sign`, `SignedRoomTicket::verify(key, kid)` follow
`Binding`. `verify` does not look at the clock; `RoomTicket::admits(room_id,
endpoint_id, now)` checks room, endpoint and `issued_at <= now + 30 &&
now < expires_at`.

## Bridge API

All three need a player session (`ems_`). A connection whose configuration
allows it also creates, lists, reads and closes rooms for its linked players
with its provider credential; that is in `INTEGRATION_PATHS.md`. Errors use
the bridge's existing error body.

`GET /v1/rooms?build_id=` returns at most 100 open rooms that have at least
one member, fullest last. A new room is listed once its creator is inside, so
the creator is its first member and moderator:

```rust
pub struct RoomList { pub rooms: Vec<RoomSummary> }
pub struct RoomSummary {
    pub room_id: String,
    pub name: String,           // 1..=64 bytes, single line
    pub build_id: String,
    pub members: u8,
    pub capacity: u8,           // 2..=16
    pub tables_playing: u8,
    pub region: String,         // the host's relay region code, e.g. "use1"
    pub created_at: u64,
}
```

`POST /v1/rooms` with `CreateRoom { name, capacity, build_id }` returns the
new room's `RoomSummary` (201). The creator then asks for a ticket like any
joiner. Refusals: `room_limit` (the caller already owns a room, or
the address or the server is at its limit), `unsupported_build` (no room host
for that build), `invalid_name`.

`POST /v1/rooms/{room_id}/tickets` with `TicketRequest { endpoint_id, build_id }`
returns `RoomAdmission`. Refusals: `room_not_found`, `room_not_open`,
`room_full`, `banned`, `unsupported_build`.

A room has no member until its creator is inside, and the creator is its first
member. Until the supervisor has first reported a member (`rooms.opened_at`), the
route issues a ticket to the creator only; anyone else is refused
`room_not_open` (404, like `room_not_found`, since the room is not yet public).
The creator can always ask while the room is open. Once a room has had a member
this never comes back: a room that empties again is not listed (that needs a
member right now), but takes tickets from anyone, as the helper's own
first-member rule does.

```rust
pub struct RoomAdmission {
    pub room: RoomSummary,
    pub invitation: String,     // the room host's current sf4e3 invitation
    pub ticket: SignedRoomTicket,
}
```

The invitation routes the joiner to the room host. In a server-owned room the
capability inside it admits nobody by itself; the ticket does.

Limits: one open room per Ember ID as creator; per address (keyed hash of
`X-Real-IP`, IPv6 by /64) 2 open rooms and 30 ticket requests per 10 minutes;
a server-wide room cap from supervisor configuration. Bans are rows
`(room_id, ember_id)` written when the room host reports a kick.

## Supervisor API (loopback, `Authorization: Bearer <shared secret>`)

- `POST /rooms` `{ room_id, name, capacity, build_id, creator, bridge_id, ticket_key, ticket_kid }` → `{ invitation, region }` once the host reports `hosted`.
- `GET /rooms` → `[{ room_id, members, capacity, tables_playing, invitation, banned: [ember_id], opened }]`, polled by the bridge. `members` is the latest count; `opened` is the supervisor's latch, set the first time a status with at least one member arrives and kept for the room's life. The bridge sets `rooms.opened_at` from `opened`, not from the count a poll happens to see, so a creator who joined and left between two polls still ends creator-only admission; it lists a room on the current count. A supervisor that omits the field is judged by the count.
- `DELETE /rooms/{room_id}`.
- `GET /health`.

`creator` is the Ember ID of the player who made the room. The room host
passes it to its helper as `HostPublic.creator`.

The supervisor starts `sf4e-room-host` with a JSON config on stdin and reads
one JSON status line per change on its stdout. It closes a room that has been
empty for 120 s, and a room whose creator never arrived within 120 s.

## Helper IPC additions

On Linux the room host starts its helper as `sf4-net --stdio --bind-port N`:
the same frames as the Windows pipe, commands on the helper's stdin and events
on its stdout. The helper stops when its stdin closes, so it never outlives the
room host.

Commands (`service/protocol.rs`):

```rust
HostPublic { epoch: u64, build: String, ticket_key: String, ticket_kid: String, bridge_id: String, room: [u8; 16], creator: String },
JoinPublic { epoch: u64, invitation: String, ticket: SignedRoomTicket, build: String },
BanAccount { epoch: u64, account: String },
```

`HostPublic` hosts like `Host`, with the room marked server-owned: control
admission requires `PublicRoomProof`, and coordination keeps one voter.
`creator` is required (JSON key `creator`, an Ember ID); a value that is not
one is refused with `invalid_request`.
`JoinPublic` joins like `Join`, sending `PublicRoomProof`, and marks the room
server-owned locally: this helper never campaigns and takes coordination
appends and snapshots only from the host it was admitted by.

Handshake on the control stream, a distinct struct so `RoomProof` is untouched:

```rust
struct PublicRoomProof { version: u16, room: [u8; 16], capability: [u8; 32], build: String, ticket: SignedRoomTicket }
```

The host checks, before a control slot is taken: the invite's own `admit`
rules, ticket signature and `kid`, `ticket.admits(room, remote endpoint, now)`,
`bridge_id`, that `ticket.ember_id` is not banned, and that no other endpoint
holds a control under that account. While the room has had no member, the
ticket must also name `creator`; once any member has been admitted ordinary
rules apply, and they stay in force when the room empties again (a creator who
never arrives is the supervisor's empty-room timer's business).

These are one decision (`AdmissionPolicy::decide`), made at three points. The
handshake makes it with the clock read after the proof has arrived. The
helper's actor makes it again, on its own state and a fresh clock, when the
handshake completes, using the verified ticket the handshake carried, so a
doomed connection is refused promptly. A control from an endpoint that
already has one waits behind the old worker (up to 5 s) and is decided a
third time immediately before it is installed, with no asynchronous gap: the
install is the admission boundary. A waiting candidate is a reservation for
the duplicate-account rule and nothing more; it becomes a member only when
installed, so it cannot grant itself the exception below.

What the joiner is told. Only the refusals made while the handshake is open
are told: the handshake's check (ticket, ban, duplicate account, invitation,
and a full room, which reads the proof first) answers with one `PublicRefused`
frame in place of `Accepted`, which names no reason, and the joiner's helper
ends the join with `join_failed` and reason `refused`. A proof that does not
parse, or a connection on the wrong ALPN, is closed on without an answer
(`join_failed`, no reason). The refusals at the second and third points above
come after `Accepted` has gone out, so the joiner has installed the control and
sees it close; it redials, and the join ends as `join_failed` with reason
`control_lost` once the helper gives up. The other reasons are
`host_unreachable` and `relay_unreachable` (no connection opened) and `timeout`
(the connection opened and the handshake ran out of time). The private
handshake never sends `PublicRefused`.

`decide` keeps two things apart. Account reservations are held by controls
(installed and waiting) and feed only the duplicate-account rule. Reconnect
eligibility belongs to a seat: the account an installed control was admitted
with, bound to that member's coordination admission (incarnation) once the
host holds it. A returning member's exception (a known endpoint redialing with
the ticket it joined with, whatever the ticket's clock) holds only while the
endpoint's seat is bound and the ticket names that account. The seat ends when
its admission retires, whatever controls remain open, and a control with no
admission yet confers nothing. After that the endpoint is a newcomer: it needs
a current ticket, and, if its old control is still open, the same endpoint is
admitted (it holds its own reservation) while another endpoint under that
account is refused until the control goes. Every other ticket must be current
then.

A connection check between two members: members hold no control to one
another, so the source sends its `ProbeReservation` frame to the host, which
checks the sender is the source it names and passes the frame to the target.
The target accepts it from the host and, as in a private room, only once its
own replica holds the committed reservation.

Event change: `Connected` gains `account: Option<String>`, `serde(default)`
and skipped when empty, set only on a public host.

Client requests on the tournament worker (`tournament::Request`), replies by
`request_id` like `assignment_list`:

```rust
RoomList { bridge_id: String, build: String },
RoomCreate { bridge_id: String, name: String, capacity: u8, build: String },
RoomTicket { bridge_id: String, room_id: String, build: String },
```

On the wire these are `{"op":"room_list",...}`, `room_create` and
`room_ticket` inside the `tournament` command, like `assignment_list`. A bridge
without public rooms answers `rooms_unavailable`.

`RoomCreate` creates the room and requests the creator's ticket in one step.
Creating takes up to the bridge's 30 s wait for the supervisor, so that
request alone is allowed 45 s in the helper; every other bridge request keeps
its 15 s.
`RoomCreate` and `RoomTicket` reply with the `RoomAdmission`; the game then
sends `JoinPublic`.

## Native

- `room::Snapshot` gains `serverOwned` (bool), serialized only when true.
- Server-owned `RoomModel`: `Join` is allowed while `host == 0`, and that
  member becomes `host`; when `host` leaves, the oldest remaining member
  becomes `host`; an empty room stays open with `host == 0`.
- `SessionServer` in server-owned mode takes each connection's Ember ID
  from the helper's `connected` event, and keys kicks on the
  Ember ID.
- The game treats a server-owned room's authority loss as "room closed".
- Told to close (stdin end of file), the room host closes the room in the
  model first (`SessionServer::CloseServerOwnedRoom`, the host's Close action
  with no member behind it), waits up to 2 s for that commit to reach the
  members, then leaves. Members see `closed` in their snapshot within about a
  second. A host that dies instead is noticed by the member's helper after
  its 20 s grace: `room_closed`, with the last snapshot still not closed.
  The game ends that room as it ends a closed one ("The room was closed."),
  and while it waits it offers no replacement room.
- The coordination roster of a server-owned room is the members and the host, so
  `control_rebound` may list `MaxMembers + 1` endpoints there; a private room's
  bound is unchanged.
- A kick bans the account in the model at once; the room host gives the ban
  to its helper (`ban_account`) a second later, after the removal notice has
  gone out on the control the ban closes.
- A joining member is sent the commits from before it joined. One whose roster
  names members who have since left cannot be bound and is skipped when a
  newer commit exists (`RoomRecoveryRuntime`, server-owned rooms only).
- Ember IDs are in the authority checkpoint, which every member's helper
  receives, and are not in the snapshot the room UI reads.

## Operating rules

- A room host only admits its own build. Every client release needs room hosts
  built from the same commit and its sidecar build hash in the supervisor
  configuration.
- Redeploys drain: no new rooms, existing rooms run until empty or a timeout.
  The unit uses `KillMode=mixed`, so systemd's SIGTERM reaches the supervisor
  only; room hosts are closed by the supervisor, not signalled.
- A status line from a room host is at most 128 KiB. The largest valid one (512
  Ember IDs and a 4096 byte invitation) is about 35 KB.
