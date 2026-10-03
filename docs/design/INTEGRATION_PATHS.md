# Integration paths

Status: in development on `staging/1.1.0`. This is the contract the bridge,
the notifier, the TypeScript SDK and the game are built against. Change it in
the same commit as any change to a shape it names.

BluMint was the first platform with its own way of working: it finds players
by Discord account, takes one result per match, pushed to it, and has no
review step. Those are now three settings any connection can have, and a
fourth, rooms, lets a service open public rooms for its players. A Discord
bot, a Twitch bot or a tournament site gets the same things BluMint has
without code of its own in the bridge.

Connections are still issued by the bridge's operator (`bridge.json` and
`ember-bridge credential`). Someone who wants none of that runs their own
bridge and room supervisor; nothing here assumes Ember's.

## Connection settings

A bridge that has these lists `players.lookup` and `results.signed` in its
capabilities `features`, and `rooms.connections` next to `rooms` when public
rooms are on. Whether a given connection may use them is its own settings:

A connection in `bridge.json` may carry, besides `id`, `kind`,
`environment`, `display_name`, `enabled` and `api_base`:

```json
{
  "id": "night-bot",
  "kind": "direct",
  "environment": "production",
  "display_name": "Fight Night bot",
  "discord_lookup": true,
  "disputes": "restart",
  "results_url": "https://bot.example/ember/results",
  "rooms": { "max_open": 8 }
}
```

- `discord_lookup` (default false): the connection may find players by
  Discord user ID (`POST /v1/players/lookup`). A player who connected their
  Discord account to their Ember ID is found, and linked on the connection
  (`approved_via` `discord`), exactly as BluMint's lookup does: a link the
  player removed is not made again until they sign in with Discord again.
- `disputes` (`"review"` by default): what a match on the connection does
  when it would wait in `needs_review`. `"restart"` cancels it instead, and
  the platform replays it. Stored with the connection the first time it is
  seen and never changed afterwards (like `kind`); configure a new connection
  to change it.
- `results_url` (optional): an `https` URL the bridge sends each finished
  match's result to. It passes the same destination check as a webhook URL
  (no loopback or private addresses unless `allow_private_webhooks`). It is
  signed with the connection's result secret (below). Matches created while
  the connection has one are queued for sending; others are not. Once the
  bridge starts sending a result (`delivering`) it is final and can no
  longer be corrected, as for BluMint.
- `rooms` (optional): the connection may open public rooms
  (`POST /v1/rooms` with its provider credential), at most `max_open`
  (1 to 64) open at once.

Kind `blumint` implies `discord_lookup: true` and `disputes: "restart"`,
sends results in BluMint's own format to BluMint's API (its `api_base`), and
keeps its rule that the generic match, lobby and tournament routes refuse it.
It may not set `discord_lookup`, `disputes` or `results_url` itself. Kind
`mock` may set all four, for local testing.

The policy code reads one model, `config::Policy`, derived from the stored
connection record (`kind`, `disputes`) and the configuration:

```rust
pub struct Policy {
    pub results: Results,          // None | BluMint | Signed
    pub own_api_only: bool,        // kind blumint
    pub reviews_disputes: bool,    // disputes == "review"
}
```

`results` is only consulted when a match is created (it sets the match's
`delivery_state` to `queued` or `not_required`); everything after reads the
match's own delivery state.

## Result secret and result body

The integration secrets file gains `result_secrets`, by connection ID, each a
Standard Webhooks secret (`whsec_` and base64 of 32 bytes, as a webhook
subscription's). `ember-bridge result-secret` prints a fresh one for the
operator to hand to the platform, and `set-integration-secret.sh result
<connection>` stores it. A connection with `results_url` and no
secret is served, and its results wait until the secret is there.

Each result is one POST of `application/json` with the Standard Webhooks
headers (`webhook-id`, `webhook-timestamp`, `webhook-signature`), the same
construction as event webhooks, so `verifyWebhook` in the SDK checks it.
`webhook-id` is `res_` plus the match ID, the same on every attempt.

```json
{
  "type": "io.ember.tournament.match.result.v1",
  "bridge_id": "brg_...",
  "connection_id": "night-bot",
  "match_id": "emt_...",
  "external_match_id": "your-id",
  "outcome": "completed",
  "revision": "9",
  "participants": [
    { "participant_id": "par_...", "ember_id": "emb1_...", "slot": 0, "score": 2 },
    { "participant_id": "par_...", "ember_id": "emb1_...", "slot": 1, "score": 1 }
  ],
  "winner_participant_id": "par_..."
}
```

`outcome` is `completed`, or `restart` for a cancelled or failed match (no
`winner_participant_id` then; scores are what was accepted before it ended).
The platform answers 2xx once it has stored the result, or 409 when it
already has it; both settle the result. 429 and 5xx are retried for a day,
any other 4xx is a refusal. The match's `provider_delivery_state` shows how
it went.

## Player lookup

`POST /v1/players/lookup` (provider credential, `discord_lookup` or kind
`blumint`):

```json
{ "discord": ["80351110224678912"] }
```

answers, for each ID connected to an Ember ID and linked (or now linked) on
the connection, in the order asked, at most 32:

```json
{ "players": [ { "discord_user_id": "80351110224678912", "ember_id": "emb1_...", "participant_id": "par_..." } ] }
```

An ID with no connected account is left out. Without permission the route
answers `forbidden`. BluMint's `/v1/blumint/lookup` uses the same code.

## Rooms for a connection

A connection with `rooms` uses the public room routes with its provider
credential. A player session keeps working exactly as before.

`POST /v1/rooms` (provider):

```json
{ "name": "Fight Night", "capacity": 8, "build_id": "...",
  "creator": { "participant_id": "par_...", "ember_id": "emb1_..." } }
```

The creator must be linked on the connection (the same check as a lobby
queue). The room is the creator's: they go in first and are its moderator,
as when they create it from Ember, and it counts as their one open room.
The connection's `max_open` replaces the per-address limit. Answers 201 with
a `ConnectionRoom`:

```rust
pub struct ConnectionRoom {
    pub room: RoomSummary,
    pub state: String,            // "waiting" (creator not in yet), "open", "closed"
    pub creator_ember_id: EmberId,
    pub join_url: String,         // https://embernetplay.link/r#<bridge id>/<room id>
}
```

Refusals are the player route's (`room_limit`, `unsupported_build`,
`invalid_name`), plus `not_linked` for a creator not linked on the
connection. `room_limit` because the creator already has an open room
carries that room's ID in `details.room_id` when the connection created it,
so a bot can hand out the existing link instead.

- `GET /v1/rooms` (provider): the connection's rooms that are not closed,
  newest first, as `{ "rooms": [ConnectionRoom] }`.
- `GET /v1/rooms/{room_id}` (provider): one of the connection's rooms,
  closed ones included while the bridge keeps them (a day).
- `POST /v1/rooms/{room_id}/close` (provider) with `{ "reason": "..." }`:
  asks the supervisor to close it, marks it closed and answers the closed
  `ConnectionRoom`. Closing a closed room answers it again.

Rooms a connection created are public rooms like any other: listed to
players, joined with a ticket, moderated by their creator.

Each room route resolves its caller once, as a player session or a provider
credential (`rooms::Caller`), the way the event routes resolve a `Viewer`;
the handlers branch on that one value, never on headers.

### Room events

For rooms a connection created only, on that connection's event stream
(webhooks, polling, SSE). Subject `rooms/<room_id>`. Data is the
`ConnectionRoom` at that moment, plus `reason` on `room.closed`.

- `room.created`: the room is hosted and waits for its creator.
- `room.opened`: its first member is in; anyone may join now.
- `room.changed`: members or tables playing changed, at most once per poll.
- `room.closed`: `reason` is `closed_by_connection`, or `ended` (it emptied,
  its creator never came, or its host stopped).

## Room links

Until the embernetplay.link site update that carries `/m` and `/start` ships
with the public release, `join_url` pages are not live; test with the
`ember://` form or Ember's paste box.

`https://embernetplay.link/r#<bridge id>/<room id>` is a page like the match
page: Open in Ember hands Ember `ember://room/open?bridge=<id>&room=<32 hex>`,
with Copy link and the install steps below it. A room link is not a secret:
the room is public, and admission is still the ticket's.

In Ember the link opens Public rooms on that service and joins the room. Like
a match link it never moves a player who is in a game or a room; it asks
first. A link for a service the player does not trust says so and goes no
further; Ember never trusts a service because a link named it. In Ember's
paste box, either form of the link works.

## The notifier as a room bot

`rust/ember/notifier` keeps posting events and gains, both optional:

- `bot`: `{ "bridge_api": "<origin>", "credential": "env:NAME", "build_id":
  "...", "capacity": 8 }`, the provider credential of a connection with
  `rooms` (and `discord_lookup` for the Discord command).
- Discord `/room` (slash command over Discord's HTTP interactions endpoint,
  `POST /discord/interactions`, Ed25519-verified with the application's
  public key). The person who runs it is looked up by Discord ID; when found
  a room is created for them and its link is posted to the channel; when
  not, the reply (only they see it) sends them to
  `https://embernetplay.link/start#<bridge id>` to connect Discord. A
  creator who already has a room gets that room's link. `ember-notifier
  discord-register <config>` registers the command.
- Discord gives an interaction three seconds, so the bot answers at once
  with a deferred reply only the caller sees, before any bridge call, and
  edits it when the bridge has answered; a room's link follows as a public
  message. Room creation is given 45 s, as a room host may take 30 to start.
- Twitch `!room` over EventSub's WebSocket transport (`channel.chat.message`,
  no public endpoint needed): from the broadcaster or a moderator it opens a
  room for the configured creator (`twitch.room_creator`, the streamer's
  linked participant) and posts its link in chat.
- Room events (`room.created`, `room.opened`, `room.closed`) are posted like
  match news.
