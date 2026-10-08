# Server

Everything that runs on the Ember VPS, rather than on players' machines.
All of it runs on Linux, built in WSL (Ubuntu) and copied up; the VPS itself
has too little memory to build.

| Folder | What runs | Deploy |
|---|---|---|
| [ember/bridge](ember/bridge) | `ember-bridge`: Ember ID, tournaments, partner API, public room list and tickets | [ember/bridge/deploy](ember/bridge/deploy) |
| [ember/notifier](ember/notifier) | room bot and event poster for Discord and Twitch | see [INTEGRATIONS.md](../docs/guides/INTEGRATIONS.md#discord-and-twitch) |
| [ember-rooms](ember-rooms) | `ember-rooms`: the public room supervisor; starts one room host per room | [ember-rooms/deploy](ember-rooms/deploy) |
| [roomhost](roomhost) | `sf4e-room-host`: one public room (C++, shares the session code in `src/`) | built with [roomhost/build-linux.sh](roomhost/build-linux.sh), installed by ember-rooms |
| [ember-short](ember-short) | `ember-short`: short invitation links and the embernetplay.link pages | [ember-short/deploy](ember-short/deploy) |
| [ember-reports](ember-reports) | `ember-reports`: opt-in crash/problem reports and local symbolication, forwarded to Bugsink | [ember-reports/deploy](ember-reports/deploy) |

Shared with the game:

- [ember/protocol](ember/protocol) is also compiled into the player's network
  helper (`rust/sf4-net`), so a change there changes the client too.
- Each public room runs the game's own `sf4-net` helper next to the room host,
  and both must come from the same commit as the client release they serve.

More:

- [Hosting public rooms](../docs/development/PUBLIC_ROOM_HOSTING.md): what a
  room costs, server sizes and hosting options.
- [Public rooms design](../docs/design/PUBLIC_ROOMS.md) and
  [integration paths](../docs/design/INTEGRATION_PATHS.md).
- [Integrating with Ember](../docs/guides/INTEGRATIONS.md): running a local
  bridge, the partner API and the room bot.

## What travels between the VPS and players

Coordination and optional diagnostic reports reach the VPS. Match inputs go
player to player, directly or through the n0 iroh relays, so the VPS never
carries gameplay. Updates come
from GitHub releases, not the VPS. The Discord and Twitch room bot is left out.

```mermaid
flowchart LR
  subgraph PC["Player PC"]
    browser["Web browser"]
    launcher["Ember launcher"]
    game["SSFIV + Ember overlay"]
    helper["sf4-net helper"]
    launcher -->|"join link mailbox"| game
    game <-->|"named pipe"| helper
  end

  subgraph VPS["VPS (embernetplay.link)"]
    nginx["nginx :443<br/>TLS, rate limits"]
    short["ember-short"]
    bridge["ember-bridge"]
    rooms["ember-rooms supervisor<br/>loopback only"]
    subgraph RH["one per public room"]
      roomhost["sf4e-room-host<br/>room authority"]
      rhelper["sf4-net helper<br/>sole voter"]
    end
    nginx -->|"/s/v1/"| short
    nginx -->|"bridge.embernetplay.link"| bridge
    bridge <-->|"HTTP + shared secret:<br/>create, status poll every 5 s, close"| rooms
    rooms -->|"starts one per room<br/>config on stdin, status on stdout"| roomhost
    roomhost <-->|"stdio frames"| rhelper
  end

  subgraph EXT["Outside services"]
    relays["n0 iroh relays"]
    discord["Discord"]
    blumint["BluMint"]
    github["GitHub releases"]
  end

  other["Other players' helpers"]

  browser -->|"GET /j /m /r /start page<br/>(code stays in the # part)"| nginx
  browser -.->|"ember:// handoff"| launcher
  helper -->|"PUT sealed invite<br/>on change + every 10 min"| nginx
  helper -->|"GET sealed invite by locator"| nginx
  helper -->|"Ember ID sign-in, account links,<br/>room list / create / ticket,<br/>match claims, assignments, reports"| nginx
  helper <-->|"iroh QUIC: ticket check,<br/>room state, seats, chat"| rhelper
  helper <-.->|"fallback path"| relays
  rhelper <-.->|"fallback path"| relays
  helper <-->|"gameplay: fighters direct,<br/>spectators from P1<br/>(never through the VPS)"| other

  browser -->|"Discord sign-in"| discord
  browser -->|"redirect to /v1/discord/callback"| nginx
  bridge -->|"code exchange"| discord
  blumint -->|"lookup, match creation,<br/>match status"| nginx
  bridge -->|"POST match results"| blumint
  launcher -->|"update checks"| github
```

### Public rooms

The VPS owns the room. A room host on the VPS is the authority and the only
coordination voter; players need an Ember ID and a ticket from the bridge.

The bridge also answers `GET /v1/rooms/public` with no credential: the same
rooms and details a player's detailed list shows, plus the bridge ID. The
embernetplay.link/rooms page reads it as `/rooms.json` (nginx caches it for
5 s) and joins through `ember://room/open` links. At each release, add the new
Sidecar build ID to `VERSIONS` in `ember-short/deploy/site/assets/rooms.js`;
rooms on a build missing there show "Other version".

```mermaid
flowchart TB
  subgraph A["Creator's PC"]
    ga["Game"] <-->|"pipe"| ha["Helper"]
  end
  subgraph B["Joiner's PC"]
    gb["Game"] <-->|"pipe"| hb["Helper"]
  end

  subgraph VPS["VPS"]
    bridge["ember-bridge<br/>room list, tickets, bans"]
    sup["ember-rooms supervisor"]
    subgraph R["Public room"]
      host["sf4e-room-host<br/>room authority, moderation"]
      rh["Room helper<br/>sole voter"]
      host <-->|"stdio"| rh
    end
    bridge <-->|"3. create room<br/>7. poll every 5 s: members,<br/>tables playing, bans"| sup
    sup -->|"3. start host<br/>8. close when empty 120 s"| host
  end

  relays["n0 iroh relays"]

  ha -->|"1. sign in with Ember ID key<br/>2. POST /v1/rooms (create)<br/>4. POST tickets: get invitation<br/>+ ticket signed by the bridge (60 s)"| bridge
  hb -->|"1. sign in<br/>5. GET /v1/rooms (list)<br/>5. POST tickets"| bridge
  ha <-->|"6. iroh QUIC: ticket check, then<br/>room state, seats, chat, kicks"| rh
  hb <-->|"6. iroh QUIC: same"| rh
  rh <-.->|"if direct UDP fails"| relays
  ha <-->|"9. gameplay, fighter to fighter<br/>(connection checks pass through<br/>the room host first)"| hb
```

Step by step for one player joining:

```mermaid
sequenceDiagram
  autonumber
  participant G as Game + helper (player)
  participant B as ember-bridge (VPS)
  participant S as ember-rooms (VPS)
  participant H as Room host + its helper (VPS)

  G->>B: POST /v1/auth/challenges, /v1/sessions (Ember ID key)
  B-->>G: session token (ems_)
  G->>B: GET /v1/rooms?build_id=
  B-->>G: up to 100 open rooms
  opt creating a room
    G->>B: POST /v1/rooms (name, capacity, build)
    B->>S: POST /rooms
    S->>H: start sf4e-room-host
    H-->>S: hosted + invitation
    S-->>B: invitation, region
    B-->>G: RoomSummary
  end
  G->>B: POST /v1/rooms/{id}/tickets (endpoint id, build)
  B-->>G: invitation + ticket signed by the bridge (60 s)
  G->>H: iroh connect with the ticket
  H-->>G: admitted, room state
  loop every 5 s
    B->>S: GET /rooms
    S-->>B: members, tables playing, bans
  end
```

- If the room host stops, the room is closed; there is no new leader.
- A kick bans the Ember ID for the room's life (at most 512 per room).
- Players never learn each other's addresses through the room; only fighters
  in a match connect to each other.

### Private rooms

The host player's game owns the room. The VPS is only a mailbox for the short
link, and only when someone uses one. Pasted `sf4e3:` invites and Discord
invites never touch the VPS.

```mermaid
flowchart TB
  subgraph H["Host player's PC (leader)"]
    gh["Game<br/>room authority"] <-->|"pipe"| hh["Helper<br/>voter"]
  end
  subgraph J["Joiner's PC"]
    br["Browser"]
    la["Launcher"]
    gj["Game"] <-->|"pipe"| hj["Helper<br/>voter (up to 5 in all)"]
    br -.->|"3b. ember:// handoff"| la
    la -->|"3b. join link"| gj
  end

  subgraph VPS["VPS"]
    page["nginx<br/>/j page (static)"]
    short["ember-short<br/>sealed invites by locator"]
  end

  discord["Discord"]
  relays["n0 iroh relays"]

  hh -->|"1. PUT invite, sealed with a key<br/>from the room code; again on<br/>change and every 10 min"| short
  hh -.->|"2. full sf4e3 invite pasted in chat"| hj
  gh -->|"2. Discord invite (emd ticket)"| discord
  discord -->|"3a. Join from Discord"| la
  br -->|"3b. open embernetplay.link/j#CODE<br/>(code never sent)"| page
  hj -->|"4. GET sealed invite by locator,<br/>decrypt on the PC"| short
  hj <-->|"5. iroh QUIC: room proof with<br/>capability, then room state,<br/>coordination, chat"| hh
  hh <-.->|"if direct UDP fails"| relays
  hj <-.-> relays
  hj <-->|"6. gameplay, fighter to fighter"| hh
```

- If the leader leaves, the remaining voters elect a new leader, and the new
  leader keeps the short link updated, so the same link still works.
- A kick lasts until the host's helper restarts.
- Tournament match rooms are private rooms with one extra call: the helper
  tells the bridge where the room is (`POST /v1/matches/{id}/room`) so the
  opponent can find it.
