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
