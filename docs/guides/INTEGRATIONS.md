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
  "expected_revision": 1}`, or `"draw": true`, which scores nothing. When a
  player reaches the set length the match completes. `{"kind": "void_game",
  "attempt_id": "..."}` removes a game; on a completed match that reopens it as
  a correction.
- `POST /v1/matches/{id}/cancel` cancels with a reason.

Every write names the revision it expects, so two people acting at once cannot
both win.

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
events, and players their own matches and links.

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
`match.cancelled`, `match.needs_review` and `match.corrected`, then run
`cargo run -p ember-notifier -- serve notifier.json`. The Twitch token is a
user access token with the `user:write:chat` scope for the sender account.
Discord posts never mention anyone. Players show by name when `names` lists
them, otherwise by their short fingerprint.

## TypeScript SDK

`sdk/typescript` wraps the API for platform developers and includes
`verifyWebhook` and `parseEvent`. See its README.
