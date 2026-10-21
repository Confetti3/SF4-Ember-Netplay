# ember-bracket-site

A small bracket site of Ember's own, for events that need no Challonge or
start.gg: single elimination, first to N, players known by their Ember IDs.
The bracket connector (`server/ember/brackets`) plays it like any organizer's
own service, through its `http` adapter (ADAPTERS.md, "The `http` adapter").

Players are entered by their Discord accounts. The site asks the bridge,
with the Ember API SDK (`sdk/typescript`, `lookupPlayers`), which Ember ID
each one has connected, and keeps that Ember ID as the entrant's ID. A
Discord account not connected in Ember is refused, so every entrant can be
found by the connector. There are no passwords: the organizer's API takes
one bearer token.

## Running it

Node 22.18 or later with TypeScript support (`node -p process.features.typescript`
is not `false`). No packages to install: it uses `node:http`, `node:sqlite` and
the SDK's sources.

```sh
BRACKET_SITE_LISTEN=127.0.0.1:8790 \
BRACKET_SITE_DATABASE=/var/lib/ember-bracket-site/site.sqlite3 \
BRACKET_SITE_ORGANIZER_TOKEN_FILE=/etc/ember-bracket-site/organizer.token \
BRACKET_SITE_CONNECTOR_TOKEN_FILE=/etc/ember-bracket-site/connector.token \
EMBER_BRIDGE_ORIGIN=https://bridge.example \
EMBER_BRIDGE_CREDENTIAL_FILE=/etc/ember-bracket-site/provider.token \
node src/main.ts
```

Each `_FILE` holds one secret on its first line. The bridge credential is a
provider credential of a connection with `discord_lookup`: the connector's
own connection, so the players it finds are the ones the connector looks up.
Put the site behind an https proxy; the connector takes only https (or
loopback http with `allow_loopback_http`).

## Organizer's API

Every write takes `Authorization: Bearer <organizer token>`.

| Request | Body | |
|---|---|---|
| `POST /tournaments` | `{ "id": "weekly-12", "title": "Weekly 12", "games_to_win": 2 }` | `id` is 1 to 64 letters, digits, `_` or `-` |
| `POST /tournaments/{id}/entrants` | `{ "discord": "<Discord user ID>", "name": "Player 1" }` | `name` is optional; answers the entrant, its `id` the Ember ID |
| `DELETE /tournaments/{id}/entrants/{ember id}` | | before the start |
| `POST /tournaments/{id}/start` | `{ "seeding": "join" }` or `"random"` | at least two entrants |
| `POST /tournaments/{id}/sets/{set}/result` | `{ "winner": "<ember id>", "scores": { "<ember id>": 2, "<ember id>": 1 } }` | a result entered by hand |
| `DELETE /tournaments/{id}` | | its live matches are cancelled |

`GET /tournaments/{id}` shows the tournament as JSON and
`GET /tournaments/{id}/bracket` as a page, to anyone.

Seeding follows the join order (or a shuffle). The bracket is the next power
of two; the top seeds get the byes, and a bye is not a set. Set IDs are
`r<round>-<position>`, and the last round is the final.

## The connector

Its base URL for a tournament is `/tournaments/{id}/ember`, with the
connector token:

```json
{
  "adapter": "http",
  "tournament": "weekly-12",
  "api_base": "https://brackets.example/tournaments/weekly-12/ember",
  "credential": { "kind": "bearer", "file": "bracket-site.token" },
  "rounds": { "games_to_win": 2 }
}
```

The connector's `rounds` decide how long each match is; keep them the same as
the tournament's `games_to_win`. Each entry's account is its Discord user ID.
A report is taken once, and its repeat under the same `Idempotency-Key` is
answered as the first; any other result for a decided set is `409`, which the
connector leaves to the organizer. `/standing` says `finished` once the final
is decided.

## Tests

```sh
npm test
```

runs the bracket (2 to 17 players, byes, results) and the routes against a
stand-in for the bridge. The connector and a real bridge play a bracket on
the site in `server/ember/brackets/tests/bracket_site.rs`, which needs Node,
so it is ignored by default:

```sh
cargo test -p ember-brackets --test bracket_site -- --ignored
```
