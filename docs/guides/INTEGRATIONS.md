# Integrating with Ember

Ember's tournament bridge is a provider-neutral web service. Tournament
platforms, organizers and bots talk to it over one HTTP and event contract,
and it never relays gameplay. This guide covers what works in this branch:
identity linking, matches played in Ember and reported by the game, matches
decided by an organizer, and signed events for webhooks, Discord and Twitch.

The full design is EMBER-TB-001 in `docs/design/identity-bridge/`. What is
implemented, and how it differs from the draft, is in `STATUS.md` there.

## What exists today

- Each player has a persistent Ember ID, created in the game on request and
  kept under `%LOCALAPPDATA%\Ember\Identity\v1`. It survives restarts and
  updates, and an encrypted backup carries it to another PC.
- A bridge links that ID to an account on your platform with a short code that
  the account holder approves.
- Matches are created by a provider. A match can be played in Ember, where the
  game opens a room for the two players, keeps everyone else out, and reports
  each game's result itself (`ember-room-v1`), or decided game by game by an
  organizer (`organizer-reported-v1`).
- Every match played in Ember has a Play link, the same for both players,
  that opens it in Ember or shows how to install Ember first.
- On top of matches, the bridge can run king-of-the-hill lobbies and whole
  tournaments (single elimination, double elimination, round robin), and it
  keeps each player's record of sets and games.
- Every change is an event, delivered to webhooks and readable by cursor or
  over SSE.
- A platform connects as a `direct` provider: it calls this API itself with
  its provider credential. The `mock` kind adds browser login pages for local
  testing. The `blumint` kind serves BluMint's game partner API instead (see
  "BluMint" below).
- Optionally, a player connects their Discord account to their Ember ID, so a
  platform that finds players by Discord account gets their Ember ID.
- A connection's settings give any platform what BluMint has: finding players
  by Discord account, results sent to it, and disputed matches restarted
  rather than reviewed. With rooms allowed, a platform or bot opens public
  rooms for its players and hears what happens to them (see "Connection
  settings" below). The notifier is a ready-made room bot for Discord and
  Twitch.
- A staging bridge runs at `https://bridge.embernetplay.link`. Platforms get a
  connection and credentials on it from the Ember team; BluMint starts from
  `BLUMINT_QUICKSTART.md`.

## Run a local bridge

From `rust/ember` (the bridge build needs the Visual Studio C++ tools for its
bundled SQLite):

```sh
cargo run -p ember-bridge -- init ../../bridge-dev
cargo run -p ember-bridge -- credential ../../bridge-dev/bridge.json provider mock-local "my platform"
cargo run -p ember-bridge -- credential ../../bridge-dev/bridge.json organizer local "my organizer"
cargo run -p ember-bridge -- serve ../../bridge-dev/bridge.json
```

`init` writes `bridge.json`, a fresh `bridge-secrets.json` and a mock provider
connection, listening on `http://127.0.0.1:8787`. Each `credential` command
prints a token once; the bridge keeps only a keyed hash. Keep
`bridge-secrets.json` private. Players never need it.

Loopback `http` is accepted only because `init` sets `allow_loopback_http`
and `allow_private_webhooks` for local work. A deployed bridge uses an
`https` origin with both off, and `mock_browser` off.
`rust/ember/bridge/deploy` holds the staging deployment: a systemd unit, an
nginx site, a daily backup and `setup.sh`.

## Link a player

1. Your service, having verified who the user is, creates a link intent:
   `POST /v1/link-intents` with `{"subject": "<your user id>"}` and a provider
   credential. Show the returned code to that user only. It works once and
   expires after five minutes.
2. The player enters the code in Ember (Profile, Ember ID, Linked accounts).
   Ember's helper proves it holds the key and submits a claim.
3. Your service shows the claim's fingerprint (`GET /v1/link-intents/{id}`)
   next to the one Ember shows, and when the user confirms it, approves that
   exact claim: `POST /v1/link-intents/{id}/approve` with the claim ID, the
   Ember ID and your subject.

A code on its own never completes a link; someone who copies it can only
create a claim that the account holder sees and rejects. To try the browser
version of this flow, open `http://127.0.0.1:8787/mock/mock-local/login`.

`POST /v1/players/resolve` maps your subjects to their current Ember IDs.

To test linking without the game, `sdk/typescript/tools/test-player.ts` stands
in for a player: it holds a throwaway Ember key and claims a code the way Ember
does (`TestPlayer` in the SDK does the same from code).
`sdk/typescript/tools/walkthrough.ts` runs linking, a match and its events
end to end against any bridge.

## Create and decide matches

- `POST /v1/matches` (provider, `Idempotency-Key` required) takes the two
  linked participants, the set length (`games_to_win` of 1, 2, 3 or 5) and the
  rules profile: `ember-room-v1` for a match played in Ember (next section) or
  `organizer-reported-v1` for one an organizer decides. Retrying with the same
  key or the same `external_match_id` returns the same match.
- `POST /v1/matches/{id}/adjudications` (organizer only) records one game:
  `{"kind": "game_result", "winner_slot": 0, "reason": "...",
  "expected_revision": "1"}`, or `"draw": true`, which scores nothing. When a
  player reaches the set length the match completes. `{"kind": "void_game",
  "attempt_id": "..."}` removes a game and scores the match again. On a
  completed match that is a correction: it stays completed while a player
  still has enough wins, and otherwise reopens. Only a correction that would
  reopen the match can be refused (`lease_conflict`): when either player has
  another match currently active on the same connection, for a lobby set the
  lobby has moved past, or for a bracket set whose later sets have games.
  Corrections in a finished tournament are always refused. Read `state` and
  `revision` from the answer, and handle a refusal, before changing your own
  result.
- `POST /v1/matches/{id}/cancel` cancels with a reason.

Every write names the revision it expects, as the decimal string the match
shows, so two people acting at once cannot both win.

## Matches played in Ember

With `native_rules_profile: "ember-room-v1"` the players play the set in Ember
and the game reports it:

1. Each player opens the match in Ember: from the Tournament matches screen
   (Settings > Ember ID), or from your Play button (below). The first player's
   game opens a room and the second joins it. Only the two assigned players
   can be in that room, and the room plays the set length you chose.
2. Before each game both games ask the bridge for permission to start it, and
   the game starts only when both have it. You see `match.room.ready` once
   the room is published, then `match.attempt.authorized` for each game.
3. When a game ends, each player's game reports what it saw, signed with that
   player's Ember ID (`match.game.reported`). When both agree the game counts
   (`match.game.confirmed`, `match.score.changed`), and at the set length the
   match completes (`match.completed`).
4. When the reports disagree, only one arrives within a minute, or a started
   game is never reported, the match waits in `needs_review`
   (`match.needs_review`). An organizer then decides that game with an
   adjudication naming its `attempt_id` (from the error detail or the match),
   or voids it so it is played again. Nothing is guessed: a missing report never
   becomes a win for the other player.

Signed reports show which Ember ID said what. Two players who agree on a false
result can still both report it, so treat them as the players' own account of
the game, not as proof against cheating.

Both players need the same Ember build; the second player's game is refused
with `incompatible_build` otherwise. `required_build_id` is your label for the
build and is not checked against the game.

### A Play button

The answer to `POST /v1/matches`, and `GET /v1/matches/{id}`, carry the
match's `play_url`: `https://embernetplay.link/m#<bridge id>/<match id>`. It is
the same for both players and safe to put in messages: only the match's two
assigned Ember IDs can claim the match, and anyone else's Ember finds no such
match in their list. It does not expire, so you can store it.

The page asks the player to choose Open in Ember, which hands Ember
`ember://tournament/open?bridge=...&match=...`. Below that it shows how to get
Ember when it is not installed yet (download, start it once, come back to the
same link), so your site needs no explanation of its own. Ember opens the
Tournament matches screen on that match and the player presses Play there; a
player in a game or a room is never moved. Where the `ember:` link does not
open Ember (Linux browsers, or a PC where Ember has not been started yet), the
page's Copy link and Paste match link on that screen do the same, or the player
picks the match from the list.

## Discord sign-in (optional)

A bridge whose configuration has `discord` (the Discord application's
`client_id`, with `<origin>/v1/discord/callback` in its redirect list) and
whose integration secrets hold the client secret lists `discord` in its
capabilities `features`. A player then connects their Discord account from
Ember (Ember ID on the home menu, then Discord). A platform can send the
player there directly: the link `ember://discord/connect?bridge=<bridge_id>`,
offered by the page `https://embernetplay.link/start#<bridge_id>`, opens
Ember's Connect Discord screen for that bridge. At an idle menu it creates the
Ember ID if needed, trusts Ember's own bridge unless the player removed it,
and opens Discord's page by itself; a link that arrives during play asks
first.
Where the browser cannot open Ember, the player pastes the page's link into
Connect Discord (Paste link) instead. The link names a bridge only; Ember
never trusts one by itself. The sign-in:

1. Ember's helper calls `POST /v1/discord/start` with a `discord.connect`
   proof and opens the returned Discord page in the browser.
2. Discord sends the browser to `GET /v1/discord/callback`. The bridge
   exchanges the code itself, with the `identify` scope only, and keeps the
   Discord user ID and username. The sign-in is one-use and lapses after ten
   minutes. Starting a sign-in ends the Ember ID's earlier ones, and
   `DELETE /v1/discord/start` (a `discord.cancel` proof, sent by Cancel in
   Ember) ends the one in flight, so a page left open connects nothing.
3. If that Discord account is connected to another Ember ID, nothing moves
   yet: the page names both Ember IDs, says that the tournament links the
   account made for the other one end, and asks. The player's answer
   (`POST /v1/discord/callback`, carrying the sign-in's state) moves it or
   keeps it where it is.
4. `GET /v1/discord` (player session) shows the connected account and
   `DELETE /v1/discord` (a `discord.remove` proof) disconnects it.

One Discord account belongs to one Ember ID and the other way round; the
latest sign-in replaces both, once the player confirms moving an account
from another Ember ID. A platform's links that a sign-in approved
(`approved_via` `discord`) end when that account moves to another Ember ID or
is disconnected, and the platform's next lookup links it where it now
belongs. A link the player removes stays removed: lookups do not make it
again until the player signs in with Discord again. Nothing else from Discord
is stored. Without the client secret the
bridge offers no sign-in and says so in its log, but every bridge lists
`discord.accounts` and keeps `GET` and `DELETE /v1/discord`, so a player can
always see and disconnect an account connected earlier.

## BluMint

A connection of kind `blumint` serves BluMint's game partner API (v1.3.0) with
the connection's provider credential as `Authorization: Bearer`:

- `POST /v1/blumint/lookup` answers `discord` with the Ember IDs connected to
  those Discord accounts, and links each on the connection (`approved_via`
  `discord`) so matches can be created for it. An Ember ID already linked
  there by a code keeps that link.
- `POST /v1/blumint/matches` creates an `ember-room-v1` set from two teams of
  one and answers `matchId` and the match's `play_url` as `matchUrl`.
- `GET` (or `POST`) `/v1/blumint/matches/status?matchId=` maps the match to
  `pending`, `running`, `complete` or `cancelled`, with each player's presence
  and score.

The bridge posts each completed match's score to BluMint, and `mustRestart`
for a cancelled match. BluMint has no review step, so a match that would go
to `needs_review` (the two games disagree, a report never arrives, a player's
link ends) is cancelled instead. Posts need BluMint's API key for the
connection in the integration secrets; a match that ends before the key is
there is posted once it is. Posts are retried for a day; the match's
`provider_delivery_state` says how it went. BluMint takes one result per
match, so once the bridge starts posting it (`delivering`) an organizer can
no longer correct it. `ember-bridge
blumint-register <bridge.json> <connection>` registers the three endpoints
with BluMint, handing over a new provider credential that is never shown.
`BLUMINT_QUICKSTART.md` is the guide for BluMint's side.

A `blumint` connection makes matches only through BluMint's create
callback; the generic match, lobby and tournament routes refuse it, so every
match on it is one BluMint knows.

BluMint's bodies are parsed as ordinary JSON, since its match settings carry
decimals; every other route keeps the strict profile.

## Connection settings

The bridge's operator gives a connection its settings in `bridge.json`, and
issues its credential as usual. Anyone who wants other settings, or none of
this, can run their own bridge and room supervisor. The contract is
`docs/design/INTEGRATION_PATHS.md`; in short:

```json
{
  "id": "night-bot", "kind": "direct", "environment": "production",
  "display_name": "Fight Night bot",
  "discord_lookup": true,
  "disputes": "restart",
  "results_url": "https://bot.example/ember/results",
  "rooms": { "max_open": 8 }
}
```

- `discord_lookup`: `POST /v1/players/lookup` with `{"discord": [...]}` (at
  most 32 Discord user IDs) answers the players connected to them, with their
  Ember ID and `participant_id`, and links them on the connection the way
  BluMint's lookup does. A link the player removed is not made again until
  they sign in with Discord again.
- `disputes`: `"review"` (the default) or `"restart"`, which cancels a match
  that would wait in `needs_review`. It is recorded with the connection the
  first time it is seen and never changes; use a new connection to change it.
- `results_url`: each finished match is POSTed there as
  `io.ember.tournament.match.result.v1` (outcome `completed` with the scores
  and winner, or `restart`), signed exactly like a webhook with the
  connection's result secret, `webhook-id` `res_<match id>`. `ember-bridge
  result-secret` makes a secret, which the operator gives the platform and
  stores with `set-integration-secret.sh result <connection-id>`. Answer 2xx
  once stored, or 409 if you have it already; anything else is retried for a
  day. Once sending starts a result can no longer be corrected. The URL
  follows the same rules as a webhook destination.
- `rooms`: with the provider credential, `POST /v1/rooms` opens a public room
  for a player linked on the connection (`creator`: their `participant_id`
  and Ember ID), who goes in first and moderates it. It counts as their one
  open room, and the connection has at most `max_open` open. `GET /v1/rooms`
  lists the connection's open rooms, `GET /v1/rooms/{id}` reads one, and
  `POST /v1/rooms/{id}/close` closes one. Each answer carries the room's
  `state` (`waiting`, `open`, `closed`) and `join_url`
  (`https://embernetplay.link/r#<bridge id>/<room id>`), which opens the room
  in Ember. That page goes live with the embernetplay.link update for the
  1.1 release; until then use `ember://room/open?bridge=<id>&room=<id>` or
  Ember's Paste room link. A creator who already has a room the connection opened is refused
  `room_limit` with that room's ID in `details.room_id`. The connection's
  stream carries `room.created`, `room.opened`, `room.changed` and
  `room.closed` (with `reason` `closed_by_connection` or `ended`) for its
  rooms only.

A bridge with these lists `players.lookup` and `results.signed` in its
capabilities `features`, and `rooms.connections` when public rooms are on.
BluMint's connection kind has the first three settings built in.

## Run a lobby (first-to-N, king of the hill)

A lobby is a queue on your connection that plays one first-to-N set after
another, for a stream or a community night. Each set is an ordinary match, so
the organizer records its games exactly as above and every match event still
fires. Lobbies go beyond the EMBER-TB-001 package; `"lobbies"` in the
capabilities `features` and `lobby_rotations` say a bridge has them.

- `POST /v1/lobbies` (provider, `Idempotency-Key`) takes `external_lobby_id`,
  `games_to_win` (1, 2, 3 or 5), `rotation` and `required_build_id`, with
  optional display `metadata` such as `title`.
- `POST /v1/lobbies/{id}/queue` adds a linked player who asked to play:
  `{"participant_id": "...", "ember_id": "..."}`. Nobody is queued without
  asking, and a player can be in one open lobby per connection.
- When two players are available the next set starts at once. When a set
  completes the seats rotate in the same transaction:
  - `winner_stays` (king of the hill): the loser goes to the back of the queue.
  - `loser_stays`: the winner goes to the back.
  - `both_rotate`: both go to the back, winner first.

  The next queued players sit down; with nobody waiting the same two play a new
  set. A queued player with another active match on your connection keeps their
  place and is skipped until it ends. A seated player given another match while
  waiting for an opponent keeps the seat, and the set waits for that match. When
  such a match completes or is cancelled the lobby picks up where it left off,
  with a `lobby.queue.changed` event whose reason is `player_available`.
- `POST /v1/lobbies/{id}/queue/{participant_id}/leave` takes a player out.
  Leaving a seat cancels the running set; the other player stays seated. A
  player whose link ends leaves the same way, with the reason
  `identity_unlinked`.
- `POST /v1/lobbies/{id}/close` (provider or organizer, with a reason and the
  lobby revision) cancels the running set and empties the lobby.
- `GET /v1/lobbies/{id}` shows the seats, the queue in order, the running set,
  the current streak (who has won sets back to back, and how many) and the
  lobby's `standings`: everyone who has finished a set there, ranked by sets
  won, then fewest lost, then game difference, then best streak.

A lobby set cannot be cancelled through `/v1/matches/{id}/cancel`, and a
correction that would reopen a finished lobby set is refused, because the
lobby has already moved on. `external_match_id` values starting with `lobby:`
are reserved for lobby sets.

Lobby events are `lobby.created`, `lobby.queue.changed`, `lobby.set.started`,
`lobby.set.completed` (winner, loser, scores, streak and the queue after the
rotation) and `lobby.closed`. Players who have joined a lobby can read its
events with their own session; those events name players by Ember ID only,
and each set's match events stay visible to that set's two players.

The game's own rooms follow the same set lengths and rotations, but today they
run on their own: a room table does not report to a bridge lobby yet.

## Run a tournament (brackets and round robin)

A tournament is a bracket the bridge runs for you: players register, you start
it, and every set is created as a match the moment both of its players are
known and free. Results are entered game by game like any other match, and the
bracket moves on in the same step.

- `POST /v1/tournaments` (provider, `Idempotency-Key`) takes
  `external_tournament_id`, `format` (`single_elimination`,
  `double_elimination` or `round_robin`), `games_to_win`, and optionally
  `finals_games_to_win` (the final, or the grand final and its reset),
  `grand_final_reset` (double elimination, on by default), `required_build_id`
  and display `metadata` such as `title`.
- `POST /v1/tournaments/{id}/entrants` registers a linked player who asked to
  enter, until the start. Elimination brackets take up to 128 players, a round
  robin up to 32.
- `POST /v1/tournaments/{id}/start` (provider or organizer, with the
  tournament revision) seeds the players and starts the first sets. Pass
  `seeding` with every registered `participant_id`, top seed first, or leave
  it out for registration order. Brackets use standard seeding, so the top
  seeds get the byes when the count is not a power of two.
- `POST /v1/tournaments/{id}/entrants/{participant_id}/withdraw` (provider or
  organizer) takes a player out. Before the start that is all; once running, a
  set they are playing is cancelled and every set they have left goes to their
  opponent as a walkover. A player whose link ends is withdrawn the same way.
- `POST /v1/tournaments/{id}/cancel` (with a reason and the revision) cancels
  every running set.
- `GET /v1/tournaments/{id}` shows the entrants with their seed and, at the
  end, their placement; every set with its label (for example "Winners
  semifinals" or "Grand final"), status, players, match and score; and for a
  round robin the standings.

Each set's match carries `round_label` and `tournament_node` in its metadata,
plus your tournament metadata, so `match.created` and `match.completed` read
like any other match. A set whose player is still busy in another match on
your connection waits and starts as soon as that match ends. A lobby does not
seat a player whose bracket set is waiting for them: the bracket comes first.

Placements follow how far each player got, so players who go out at the same
stage share a place (two 3rds in single elimination, for example). A round
robin ranks by sets won, then game difference, then games won, then the
head-to-head result when exactly two players are level, then seed; a walkover
counts as a set won.

To fix a wrong result, void the game as usual. When that leaves nobody with
enough wins, the set reopens and takes back what its result fed: later sets that have no games yet are cleared and
their matches cancelled, and are created again once the set is decided. A
correction is refused once a later set it fed has a game recorded, and once
the tournament is over. Bracket sets cannot be cancelled through
`/v1/matches/{id}/cancel`, and `external_match_id` values starting with
`tournament:` are reserved.

Tournament events are `tournament.created`, `tournament.entrants.changed`,
`tournament.started`, `tournament.match.started`, `tournament.match.completed`
(who advanced where, and who was eliminated), `tournament.match.reopened`,
`tournament.completed` (the placements) and `tournament.cancelled`. Entrants
can read a tournament and its events with their own session. Tournament events
name players by Ember ID only, a player reading the snapshot sees only their
own `participant_id`, and each set's match events stay visible to that set's
two players. A correction is refused for a set whose player has withdrawn.

## Player records

`GET /v1/players/{ember_id}/record` adds up a player's finished matches: sets
played, won and lost, games won, lost and drawn, cancelled matches, and the
most recent matches (`?limit=`, 10 by default, at most 50) with the opponent,
the score, `round_label` and the lobby a set belonged to. A provider sees the
matches on its connection, an organizer every match in its tenant, and a player
only their own record, without other players' account IDs.

Records are worked out from the accepted games each time they are read, so a
voided game or other correction shows straight away. Lobby standings work the
same way; only a lobby player's best streak is kept as sets are played.

## Receive events

Events are CloudEvents 1.0 with an `emberseq` cursor, for example
`io.ember.tournament.match.completed.v1`. Read them in any of three ways:

- **Webhooks.** `POST /v1/webhook-subscriptions` with an `https` URL and the
  event types you want. The response holds the signing secret once. Each
  delivery carries `webhook-id`, `webhook-timestamp` and `webhook-signature`
  (Standard Webhooks, HMAC-SHA256 over `id.timestamp.body`). Verify against the
  raw body before parsing, reject timestamps more than five minutes off,
  deduplicate on the event ID, and answer 2xx only once the event is stored.
  Failed deliveries are retried for up to 24 hours. `rotate-secret` signs with
  both secrets for an overlap period you choose.
- **Polling.** `GET /v1/events?after=<cursor>` returns events after a cursor.
- **SSE.** `GET /v1/events/stream`, resuming with `Last-Event-ID`.

Providers see their connection's events, organizers their tenant's match
events, and players their own matches, links, lobbies and tournaments.

## Discord and Twitch

`rust/ember/notifier` is a ready-made subscriber that posts news to a
Discord channel and to Twitch chat. With a `bot` section it is also a room
bot: `/room` in Discord and `!room` in Twitch chat open a public Ember room
and answer with its link. Configure it with a JSON file:

```json
{
  "listen": "127.0.0.1:8790",
  "bridge_origin": "http://127.0.0.1:8787",
  "secrets": ["env:EMBER_WEBHOOK_SECRET"],
  "database": "notifier.sqlite3",
  "discord": {
    "webhook_url": "env:DISCORD_WEBHOOK_URL",
    "interactions": {
      "application_id": "your application id",
      "public_key": "64 hex characters from the Discord developer portal",
      "bot_token": "env:DISCORD_BOT_TOKEN"
    }
  },
  "twitch": {
    "client_id": "your-app-client-id",
    "token": "env:TWITCH_TOKEN",
    "broadcaster_id": "channel user id",
    "sender_id": "bot user id",
    "room_creator": { "participant_id": "par_...", "ember_id": "emb1_..." }
  },
  "bot": {
    "bridge_api": "https://bridge.example",
    "credential": "env:EMBER_PROVIDER_CREDENTIAL",
    "build_id": "the build rooms are created for",
    "capacity": 8,
    "room_name": "Fight Night"
  },
  "names": { "emb1_...": "Player name" }
}
```

`discord`, `twitch` and `bot` are each optional. Without `discord.webhook_url`
nothing is posted to a channel and only `/room` is served.

Subscribe it to `match.created`, `match.score.changed`, `match.completed`,
`match.cancelled`, `match.needs_review` and `match.corrected`, for lobbies
`lobby.created`, `lobby.set.completed` and `lobby.closed`, for tournaments
`tournament.created`, `tournament.started`, `tournament.match.completed`,
`tournament.completed` and `tournament.cancelled`, and for rooms a connection
opened `room.created`, `room.opened` and `room.closed`, then run
`cargo run -p ember-notifier -- serve notifier.json`. Room news names the room,
how full it is and its link (`room.closed` says how it ended and has no link).
The Twitch token is a user access token for the sender account; its scopes
are listed under "Twitch `!room`" below. Discord posts never mention anyone.
Players show by name when `names` lists them, otherwise by their short
fingerprint.

### The room bot

`bot` is the provider credential of a bridge connection that has `rooms` and
`discord_lookup` in its settings (see `docs/design/INTEGRATION_PATHS.md`):

- `bridge_api`: the bridge's origin.
- `credential`: the provider credential, or `env:NAME`.
- `build_id`: the build rooms are created for.
- `capacity`: players per room when a command gives none (default 8).
- `room_name`: the name when a command gives none (default "Ember room").

The bot finds a person by Discord ID (`POST /v1/players/lookup`), opens the
room for them (`POST /v1/rooms`) and reads one back (`GET /v1/rooms/{id}`).
Someone who already has a room the connection opened gets that room's link
again instead of a refusal.

### Discord `/room`

Set the application's Interactions Endpoint URL to
`https://<your host>/discord/interactions`, which the notifier serves on its
`listen` address (put it behind HTTPS). `discord.interactions` takes the
application's `application_id` and `public_key`; every request is checked
against the key (Ed25519 over the timestamp and the body) and refused with
401 when it does not match. `ember-notifier discord-register notifier.json`
registers the command, using `bot_token` (or `env:NAME`); with `guild_id` set
it registers for that server only, which Discord applies at once.
`api_base` (default `https://discord.com/api/v10`) is for tests.

`/room` takes an optional `name` and `capacity`. Discord allows three seconds
for an answer, so the notifier answers at once with a deferred reply only the
person who ran the command sees, then looks them up and edits that reply when
the bridge has answered. A slow bridge does not cost the interaction; Discord
keeps the reply editable for fifteen minutes:

- Found: a room is opened for them and its link is posted to the channel for
  everyone to see. Their own reply becomes a short note that the room is open.
- Not connected: only they see a reply that sends them to
  `https://embernetplay.link/start#<bridge id>` to connect Discord in Ember.
- Any other refusal (unsupported build, a name the rules refuse, the bridge
  not reachable): a short plain message, visible only to them.

Opening a room is given up to 45 seconds, since the bridge may need that long
to start a host.

### Twitch `!room`

With `twitch.room_creator` (the streamer's linked player on the connection)
and `bot` set, the notifier opens an EventSub WebSocket session
(`eventsub_url`, default `wss://eventsub.wss.twitch.tv/ws`) and subscribes to
`channel.chat.message` for `broadcaster_id`, read as `sender_id`. No public
endpoint is needed. A `!room` message from the broadcaster or a moderator
opens a room for the creator and posts its link in chat as the sender; a
second `!room` while the room is open posts the same link. Messages from
anyone else are ignored. The session follows Twitch's reconnect requests and
reconnects with a growing delay after a drop or a silence longer than the
keepalive timeout.

The token is the sender account's user access token (EventSub WebSocket
subscriptions need a user token whose user is the subscription's `user_id`).
Twitch's reference for `channel.chat.message` asks for these scopes:

- `user:read:chat`, to read chat.
- `user:write:chat`, to post the news and the link.

Twitch lists `user:bot` and `channel:bot` (or the sender being a moderator in
the channel) only for subscriptions made with an app access token, which this
notifier does not use. The reference at
`https://dev.twitch.tv/docs/eventsub/eventsub-subscription-types/` is the
authority if that changes. A subscription Twitch refuses (a missing scope
shows as status 401 or 403) is logged by status and retried with the same
delay as a dropped session.

## TypeScript SDK

`sdk/typescript` wraps the API for platform developers and includes
`verifyWebhook` and `parseEvent`. See its README.
