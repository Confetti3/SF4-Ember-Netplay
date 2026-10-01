# Integrating with Ember

Ember's tournament bridge is a provider-neutral web service. Tournament
platforms, organizers and bots talk to it over one HTTP and event contract,
and it never relays gameplay. This guide covers what works in this branch:
identity linking, matches with organizer-reported results, and signed events
for webhooks, Discord and Twitch.

The full design is EMBER-TB-001 in `docs/design/identity-bridge/`. What is
implemented, and how it differs from the draft, is in `STATUS.md` there.

## What exists today

- Each player has a persistent Ember ID, created in the game on request and
  kept under `%LOCALAPPDATA%\Ember\Identity\v1`. It survives restarts and
  updates, and an encrypted backup carries it to another PC.
- A bridge links that ID to an account on your platform with a short code that
  the account holder approves.
- Matches are created by a provider and decided game by game by an organizer.
  Ember does not yet report native results or enforce tournament rooms; those
  are the next work packages.
- On top of matches, the bridge can run king-of-the-hill lobbies and whole
  tournaments (single elimination, double elimination, round robin), and it
  keeps each player's record of sets and games.
- Every change is an event, delivered to webhooks and readable by cursor or
  over SSE.
- The only provider kind is a mock one, for local testing. A real platform
  adapter, BluMint first, waits on that platform's verified API contract.

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
`https` origin with both off.

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

## Create and decide matches

- `POST /v1/matches` (provider, `Idempotency-Key` required) takes the two
  linked participants, the set length (`games_to_win` of 1, 2, 3 or 5) and the
  rules profile `organizer-reported-v1`. Retrying with the same key or the same
  `external_match_id` returns the same match.
- `POST /v1/matches/{id}/adjudications` (organizer only) records one game:
  `{"kind": "game_result", "winner_slot": 0, "reason": "...",
  "expected_revision": "1"}`, or `"draw": true`, which scores nothing. When a
  player reaches the set length the match completes. `{"kind": "void_game",
  "attempt_id": "..."}` removes a game; on a completed match that reopens it as
  a correction.
- `POST /v1/matches/{id}/cancel` cancels with a reason.

Every write names the revision it expects, as the decimal string the match
shows, so two people acting at once cannot both win.

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
events with their own session.

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

To fix a wrong result, void the game as usual. That reopens the set and takes
back what its result fed: later sets that have no games yet are cleared and
their matches cancelled, and are created again once the set is decided. A
correction is refused once a later set it fed has a game recorded, and once
the tournament is over. Bracket sets cannot be cancelled through
`/v1/matches/{id}/cancel`, and `external_match_id` values starting with
`tournament:` are reserved.

Tournament events are `tournament.created`, `tournament.entrants.changed`,
`tournament.started`, `tournament.match.started`, `tournament.match.completed`
(who advanced where, and who was eliminated), `tournament.match.reopened`,
`tournament.completed` (the placements) and `tournament.cancelled`. Entrants
can read a tournament and its events with their own session.

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

`rust/ember/notifier` is a ready-made subscriber that posts match news to a
Discord channel and to Twitch chat. Configure it with a JSON file:

```json
{
  "listen": "127.0.0.1:8790",
  "bridge_origin": "http://127.0.0.1:8787",
  "secrets": ["env:EMBER_WEBHOOK_SECRET"],
  "database": "notifier.sqlite3",
  "discord": { "webhook_url": "env:DISCORD_WEBHOOK_URL" },
  "twitch": {
    "client_id": "your-app-client-id",
    "token": "env:TWITCH_TOKEN",
    "broadcaster_id": "channel user id",
    "sender_id": "bot user id"
  },
  "names": { "emb1_...": "Player name" }
}
```

Subscribe it to `match.created`, `match.score.changed`, `match.completed`,
`match.cancelled`, `match.needs_review` and `match.corrected`, for lobbies
`lobby.created`, `lobby.set.completed` and `lobby.closed`, and for tournaments
`tournament.created`, `tournament.started`, `tournament.match.completed`,
`tournament.completed` and `tournament.cancelled`, then run
`cargo run -p ember-notifier -- serve notifier.json`. The Twitch token is a
user access token with the `user:write:chat` scope for the sender account.
Discord posts never mention anyone. Players show by name when `names` lists
them, otherwise by their short fingerprint.

## TypeScript SDK

`sdk/typescript` wraps the API for platform developers and includes
`verifyWebhook` and `parseEvent`. See its README.
