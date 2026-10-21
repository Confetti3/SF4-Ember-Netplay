# ember-bracket-site

A small bracket site of Ember's own, for events that need no Challonge or
start.gg: single elimination, first to N, players known by their Ember IDs.
The bracket connector (`server/ember/brackets`) plays it like any organizer's
own service, through its `http` adapter (ADAPTERS.md, "The `http` adapter").

A player is their Discord account. The site asks the bridge, with the Ember
API SDK (`sdk/typescript`, `lookupPlayers`), which Ember ID each one has
connected. A player may sign up before connecting Discord in Ember: the entry
waits for the link, is looked up again at the start, and is dropped from the
bracket if still not linked. The design is docs/design/BRACKET_SITE.md.

## Running it

Node 22.18 or later with TypeScript support (`node -p process.features.typescript`
is not `false`). No packages to install: it uses `node:http`, `node:sqlite` and
the SDK's sources.

```sh
BRACKET_SITE_LISTEN=127.0.0.1:8790 \
BRACKET_SITE_DATABASE=/var/lib/ember-bracket-site/site.sqlite3 \
BRACKET_SITE_OPERATOR_TOKEN_FILE=/etc/ember-bracket-site/operator.token \
BRACKET_SITE_BOT_TOKEN_FILE=/etc/ember-bracket-site/bot.token \
BRACKET_SITE_CONNECTOR_TOKEN_FILE=/etc/ember-bracket-site/connector.token \
EMBER_BRIDGE_ORIGIN=https://bridge.example \
EMBER_BRIDGE_CREDENTIAL_FILE=/etc/ember-bracket-site/provider.token \
node src/main.ts
```

With `BRACKET_SITE_ORIGIN` (its public https origin) and the Ember bot's
Discord application, the site also signs people in (below):

```sh
BRACKET_SITE_ORIGIN=https://brackets.ember.fraccie.be \
DISCORD_CLIENT_ID=<the bot's application ID> \
DISCORD_CLIENT_SECRET_FILE=/etc/ember-bracket-site/discord.secret \
BRACKET_SITE_BRIDGE_CLIENT_ID=ember-bridge \
BRACKET_SITE_BRIDGE_CLIENT_SECRET_FILE=/etc/ember-bracket-site/bridge-client.secret \
BRACKET_SITE_BRIDGE_REDIRECT_URI=https://<bridge>/v1/sites/ember-bracket-site/callback \
```

`BRACKET_SITE_CONFIG` names a JSON file of these same names, for those not
in the environment (the environment wins). Under Docker,
`docker/compose.yml` runs the site with its data, `site.json` and secret
files in one folder; its header lists them.

The Discord application's redirects must hold
`<BRACKET_SITE_ORIGIN>/oauth/discord/callback`. Each `_FILE` holds one secret on its first line. The bridge credential is a
provider credential of a connection with `discord_lookup`: the connector's
own connection, so the players it finds are the ones the connector looks up.
Put the site behind an https proxy; the connector takes only https (or
loopback http with `allow_loopback_http`).

## API

Every write takes a bearer token: the operator's (who may do anything, and
acts as the user named in `X-Discord-User` when given), the Discord bot's
(always with `X-Discord-User`, the player who pressed its button), or a
signed-in user's. Only organizers create tournaments, and an organizer
manages the tournaments that name them in `organizers`.

| Request | Who | |
|---|---|---|
| `POST /tournaments` | organizer | `{ "title", "games_to_win", "check_in", "cap", "starts_at", "season", "id" }`, all but `title` optional |
| `GET /tournaments` | signed in | the tournaments they manage |
| `GET /tournaments/{id}` | anyone | the tournament, with the `actions` its state allows |
| `PATCH /tournaments/{id}` | its organizers | settings, and `organizers`, before it is live |
| `PUT /tournaments/{id}/state` | its organizers | `{ "action": "<action>" }`; `start` takes `"seeding": "join"` or `"random"` |
| `POST /tournaments/{id}/entrants` | a player, or its organizers for anyone | `{ "name" }`; an organizer adds `"discord"` |
| `DELETE /tournaments/{id}/entrants/{discord}` | that player, or its organizers | before it is live |
| `POST /tournaments/{id}/check-in` | a player | while checking in |
| `POST /tournaments/{id}/sets/{set}/result` | its organizers | `{ "winner": "<ember id>", "scores": { "<ember id>": 2, "<ember id>": 1 } }` |
| `DELETE /tournaments/{id}` | its organizers | its live matches are cancelled |
| `GET /tournaments/{id}/feed?after=<n>` | anyone | what happened since item `n`: moves, sets live and done, upsets, streaks |
| `GET /tournaments/{id}/stats` | anyone | sets live and done, placements, per player games, rounds, perfects and characters, upsets, the longest set |
| `GET /tournaments/{id}/report` | anyone | once finished: podium, every player's path, stats, upsets, longest and shortest set, characters, perfects, time-outs; `null` where no game's facts say (also as a page, `/t/{id}/report`) |
| `POST /leagues` | organizer | `{ "id", "title" }` |
| `GET /leagues/{id}` | anyone | the league and its seasons |
| `POST /leagues/{id}/seasons` | its organizers | `{ "id", "title", "points", "best_of" }`; a season ID is unique on the site |
| `GET /leagues/{id}/seasons/{season}/standings` | anyone | points, events and wins of each player |
| `POST /leagues/{id}/seasons/{season}/close` | its organizers | closes it and gives its champion and top 3 their awards |
| `GET /players/{discord}` | anyone | a player's name, Ember ID and every award they hold |

At its finish a tournament gives Champion, Runner-up, Top 3 and (over 8
entrants) Top 8, and from its report Giant killer (the biggest upset, 2
seeds or more), Perfectionist, Iron (most games), Marathon (a set that
went the distance), One-trick and Mixer; each is announced in its feed and
kept on the player's profile once.

A tournament counts for the season its `season` names, once finished:
places 1 and 2, then everyone out in the same round together (3, 5, 9, ...),
worth the season's `points` (default `{"1": 100, "2": 70, "3": 50, "5": 30,
"9": 15, "*": 5}`), a player's best `best_of` events counting.

States: `draft`, `planned`, `sign_ups`, `check_in`, `live`, `finished`,
`archived`, `cancelled`. Actions: `plan`, `open_sign_ups`, `close_sign_ups`,
`open_check_in` and `reopen_sign_ups` (with `check_in` on), `start`, `reset`
(while no set is decided), `archive`, `cancel`. The site moves a live
tournament to `finished` on its final's result. Each move is in `log`.

`GET /tournaments/{id}/bracket` shows the tournament as a page.

Seeding follows the join order (or a shuffle) of those who can play: linked,
and checked in when check-in is on. The bracket is the next power of two;
the top seeds get the byes, and a bye is not a set. Set IDs are
`r<round>-<position>`, and the last round is the final.

## Signing in

The site is an OAuth server for one client, the bridge, as Challonge is
(`GET /oauth/authorize`, `POST /oauth/token` with the authorization code or
a refresh token, used once each, and `GET /oauth/me`, which answers
`{ "discord_id", "username" }`). Behind it each user signs in with Discord.
Scopes are `player` and `organizer`; only the bridge holds the client
secret, so only organizers the bridge lets sign in get `organizer`. Access
tokens last an hour, refresh tokens 60 days.

A browser signs in on the site's own page (`/signin?back=<path>`) and gets a
session cookie, a player's only, whose writes are taken only with the site's
own `Origin`. The bracket page then offers Sign up, Check in and Leave.

## The connector

Its base URL for a tournament is `/tournaments/{id}/ember`, with the
connector token, or with the token of one of the tournament's organizers
(a desk event):

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
is decided, or the tournament is cancelled or deleted, whose sets are then
gone (`{ "set": null }`).

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
