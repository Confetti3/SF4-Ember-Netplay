# Ember bridge: getting started for BluMint

Ember is the netplay mod for Ultra Street Fighter IV. The Ember bridge is the
"game service" in BluMint's game integration guide: it serves BluMint's three
callbacks (player lookup, match creation, match close) and reports each match
to BluMint as it is played. Players play the matches in Ember, and both
players' games report every game, so results arrive without anyone typing
them in.

## What you get

BluMint is to have two bridges, each with its own BluMint connection
(tenant `blumint`, kind `blumint`), credentials and registration: production
for BluMint's production, and staging for BluMint staging and its testers.
Neither is set up yet (see Not built yet): the table gives the addresses
each will have.

| | Production (planned) | Staging (planned) |
|---|---|---|
| Bridge | `https://bridge.embernetplay.link` | `https://bridge-staging.embernetplay.link` |
| Bridge ID | `brg_0dbc0598-2312-4ce3-9df8-e160330565e6` | `brg_...`, to be filled in once the staging bridge is up |
| BluMint API | Game partner API 2.0.0 at `https://app.blumint.com/api` | Game partner API 2.0.0 at `https://staging.blumint.io/api` |
| Player lookup | `POST https://bridge.embernetplay.link/v1/blumint/lookup` | `POST https://bridge-staging.embernetplay.link/v1/blumint/lookup` |
| Match creation | `POST https://bridge.embernetplay.link/v1/blumint/matches` | `POST https://bridge-staging.embernetplay.link/v1/blumint/matches` |
| Match close | `POST https://bridge.embernetplay.link/v1/blumint/matches/close` | `POST https://bridge-staging.embernetplay.link/v1/blumint/matches/close` |
| Connect page for players | `https://embernetplay.link/start#brg_0dbc0598-2312-4ce3-9df8-e160330565e6/BluMint` | `https://embernetplay.link/start#<staging bridge ID>/BluMint` |

Production is Ember's own bridge, the one every Ember trusts by default, so
players need nothing beyond Ember to use it. A tester's Ember has to trust
the staging bridge before it can connect Discord or play there.

The connect page opens Ember's Connect Discord screen
(`ember://discord/connect?bridge=...`) on Windows, and shows how to get Ember
first otherwise. BluMint shows it to a registering user that lookup finds no
Ember ID for. The `/BluMint` after the bridge ID is the name the page uses
for you when it speaks to the player, as in "BluMint finds you by your
Discord account"; without it the page says "the tournament you signed up
for". The page shows only a name of 1 to 40 letters, digits, spaces and
`.`, `'`, `&` or `-`, starting with a letter or digit, and silently drops
any other, so `BluMint (staging)` would show as "the tournament you signed
up for". A player can paste either link, with the name or without, into
Ember's **Paste link**; an Ember from before it read the name takes only the
link without it, as the page's **Copy link** gives it. The bridge's own
Discord pages take the name from your tenant's `name` in the bridge's
configuration.

`ember-bridge blumint-register` calls `setLookupPlayer`, `match/setCreate`
and `match/setClose`, registering the three callback URLs with a new
credential as `authentication.secret`. BluMint calls them with
`Authorization: Bearer <credential the bridge issued>`. Once all three calls
succeed, the bridge revokes the callback credential any earlier registration
handed over; a registration that fails partway revokes nothing, and running
it again finishes the change. There is nothing for
BluMint to configure for those callbacks. Reports use BluMint's API key, sent
as `x-api-key`; it is separate from the callback credential.

The staging bridge runs builds before they reach production, and its data
may be reset.

## Player lookup

Ember supports one sign-in method: **Discord**. A player connects their
Discord account to their Ember ID once, from the home menu: **Online play**,
**Ember ID**, then **Discord**. This uses Ember's own bridge, the production
bridge above, by default. BluMint sends one Discord ID per call, when a player
registers and before every match creation:

```json
{ "discord": ["274220342558756145"] }
```

and the lookup answers only `discord`:

```json
{ "discord": ["emb1_..."] }
```

with `[]` when that Discord account is not connected. Other sign-in methods
are ignored. The in-game ID is the player's Ember ID (`emb1_...`). Lookup also
links that ID to BluMint's connection so matches can be created for it, which
is why BluMint looks a player up again before each match.

If the Discord account is already connected to another Ember ID, the
bridge's page asks before moving it. It names both Ember IDs and explains
that the other ID's tournament links made through that account will end.
Moving it also ends that ID's live BluMint matches: the game in progress
counts for nobody, and the bridge posts a retryable abort with the games
already won (`insufficientPlayers`, below), so you restart the set from that
score. An Ember ID holds one Discord account at a time, so if the Ember ID
it moves to, or a first connection's Ember ID, already has a different
Discord account, the page names that account too: connecting disconnects
it, and its links and live matches end the same way.
Pressing **Cancel** in Ember also ends the sign-in on the bridge once that
request reaches it, so a page left open then cannot finish it. Ember does
not send a failed cancel again, so if it never arrives, the page can still
finish the sign-in until it lapses ten minutes after it started.

The bridge ID in the connect page's fragment is also the `bridge_id` in each
bridge's `/.well-known/ember-bridge.json`, such as
`https://bridge.embernetplay.link/.well-known/ember-bridge.json`.

**Data use.** Ember uses and keeps the IDs BluMint sends, and the Ember IDs
its lookup returns, only to put those players into matches in BluMint's
events, to report those matches to BluMint, and to let the event staff who
chose to watch do so. It shares them with no one else and keeps them secure.

## Match creation

Send two teams of one player each, each player with the Ember ID lookup
returned among its identities, and an `Idempotency-Key`:

```http
POST /v1/blumint/matches
Authorization: Bearer <credential>
Idempotency-Key: tA1b2-round-1-match-1
```

```json
{ "teams": [ { "name": "kate", "players": [ { "identities": [ { "inGameId": "emb1_..." } ] } ] },
             { "name": "sam", "players": [ { "identities": [ { "inGameId": "emb1_..." } ] } ] } ],
  "spectators": [],
  "matchSettings": {},
  "series": { "gamesToWin": 2 } }
```

- `series.gamesToWin` is the set's length: 1 to 10 (best of 1 to best of 19).
  It has no default.
- `startingWins`, on every team or on none, is the score the set starts
  from. Send it when you ask for a set again: after a `retryable: true` abort
  it is the games each team had won, and after a host reset it is 0 on every
  team. Each is below `gamesToWin`. Every report counts it.
- The bridge reads the one identity whose `inGameId` is an Ember ID. Team
  names, `matchSettings` and `spectators` are yours, and are not read yet.

The answer:

```json
{ "matchId": "emt_...", "matchUrl": "https://embernetplay.link/m#brg_.../emt_..." }
```

Send the returned `matchUrl` to both players. It is the same for both and
safe to send in your messages: only the two assigned Ember IDs can play the
match, and anyone else's Ember finds no such match. The link itself does not
expire, but a match nobody plays does: the bridge ends it 24 hours after you
created it at the latest (see "Reports").

Every request under one `Idempotency-Key` answers the match that key made,
even after the match has ended, for a day. After that the key answers
`409 key_reused`: the bridge keeps each match's key for as long as it keeps
the match, so a key never makes a second match, and a close finds the match
however old it is. A renamed team or changed `matchSettings` under the key is
the same match. Send a new key for a new match. A refusal is never stored
against the key, so send the same key again once the players are linked or
free.

| Situation | Status | Body |
|---|---|---|
| Not JSON, or not this shape; a player with no Ember ID, or with two; the same player on both teams | `400` | `{ "code": "invalid_request" }` |
| No `Idempotency-Key` | `400` | `{ "code": "invalid_request" }` |
| `series` missing, or `gamesToWin` not a whole number of 1 or more | `400` | `{ "code": "invalid_request" }` |
| `startingWins` on some teams only, or not a whole number below `gamesToWin` | `400` | `{ "code": "invalid_request" }` |
| Not two teams of one player, or `gamesToWin` above 10 | `400` | `{ "code": "match_unsupported" }` |
| Players with no link on your connection, all of them named | `400` | `{ "code": "player_unlinked", "players": ["emb1_..."] }` |
| Players still in another match on your connection, all of them named | `423` | `{ "code": "player_busy", "players": ["emb1_..."] }` |
| The key already made a different set (other players, length or starting wins), made its match over a day ago, or a close named it | `409` | `{ "code": "key_reused" }` |
| The credential is missing, unknown or revoked (`401`), its connection is disabled or it is not a provider credential (`403`), or its connection is not a BluMint one (`404`) | `401`, `403` or `404` | the bridge's own error, which you read as final |
| The bridge cannot reach its database | `503` | the bridge's own error |

Players are named exactly as you sent them, P1 first.

## Match close

When you no longer need a match, close it by the `Idempotency-Key` you
created it under, whether or not you had the creation's answer:

```http
POST /v1/blumint/matches/close
Authorization: Bearer <credential>
```

```json
{ "idempotencyKey": "tA1b2-round-1-match-1", "reason": "decided" }
```

The match that key made ends, its players are free, and nothing more is
posted for it. Every reason closes the match, `decided`, `replaced`,
`removed` and `hostOverride` as well as one the bridge does not know; it is
kept for the log. A match that already ended answers `200` too, and a second
close answers what the first did. The close also closes the key: every later
creation under it answers `409 key_reused`, also when the bridge never made
a match under it, so a creation still on its way makes nothing. A creation
the bridge would refuse anyway, such as an invalid body or a set longer than
Ember plays, still answers that refusal (`400`).

The bridge keeps a match's key for as long as it keeps the match, so a close
finds the match however long ago you created it. A key that made no match
stays closed for a day after its close. A close of a key the bridge holds
nothing under for your connection answers as a match that already ended
does, every time you send it. The close's own `Idempotency-Key` is not read.

| Situation | Status | Body |
|---|---|---|
| Closed, or already ended; or the key made no match, its creations refused or none received | `200` | `{ "closed": true }` |
| No `idempotencyKey` (a `matchId` alone included), or one empty or longer than 128 bytes; `reason` missing, empty or longer than 64 bytes | `400` | `{ "code": "invalid_request" }` |
| The credential is missing, unknown or revoked (`401`), its connection is disabled or it is not a provider credential (`403`), or its connection is not a BluMint one (`404`) | `401`, `403` or `404` | the bridge's own error, which you read as final |
| The bridge cannot reach its database | `503` | the bridge's own error |

## Reports

Each bridge posts to its BluMint API's `/tournaments/match/submit`, such as
`https://staging.blumint.io/api/tournaments/match/submit` from the staging
bridge, with that environment's BluMint API key. Every report names each
player by the Ember ID you sent. Each team's `wins`, in a progress report, a
result and an abort, is the set's running total after that game, counting
the `startingWins` the match was created with, never that one game's outcome
alone.

A `status` report each time the match's status changes: as each player
opens the match in Ember or leaves it, presses Ready or takes it back, as
each game starts, and as each game ends with another to come. A match nobody
has opened has none.

```json
{ "mode": "status", "matchId": "emt_...", "status": "running",
  "revision": 3, "observedAt": "2026-10-16T18:04:12Z", "players": [
  { "inGameId": "emb1_...", "status": "present-ready" },
  { "inGameId": "emb1_...", "status": "present-ready" } ] }
```

`status` is `pending` until the players' first game starts and `running`
from then on, whatever games an organizer recorded. Each player is:

| `status` | When |
|---|---|
| `absent` | Their Ember is not connected to the match: it has not opened it, or stopped claiming it for 136 seconds |
| `present-not-ready` | Their Ember is connected to it and they have not pressed Ready for the next game, or took it back or left their seat; or it was refused because its build differs from their opponent's. Never while a game is being played |
| `present-ready` | They pressed Ready for the next game, or a game is being played, from its start until both games report it |

Each player's Ember says whether they have pressed Ready every time it
claims the match, and claims again as soon as that changes, at most once
every two seconds, so a player waiting for an opponent who never presses
Ready reads `present-ready` and the opponent `present-not-ready`. A played
game's end clears both seats at the table, so both read `present-not-ready`
until they press Ready for the next game. A game called off because one
player took Ready back leaves the other's Ready as it was. An Ember from
before this release does not say, and its player reads `present-not-ready`
until a game starts.

A refused Ember stops claiming the match, so the bridge cannot tell whether
that player stays: they read `present-not-ready` for the rest of the match,
until they open it again on their opponent's build, except while a game is
being played, when they read `present-ready` like every present player. Time the bridge itself
is down does not count toward those 136 seconds.

`revision` is an integer that rises with each change on the match, and
`observedAt` is when the bridge saw it. A report that waited is sent as the
status then stands, so keep the one with the highest `revision` and drop one
that arrives after it. Nothing is sent once the match has ended.

After each game won that does not end the set. In a first to 3, once the
first team has won two games and the second one:

```json
{ "mode": "progress", "matchId": "emt_...", "teams": [
  { "players": [ { "inGameId": "emb1_..." } ], "wins": 2 },
  { "players": [ { "inGameId": "emb1_..." } ], "wins": 1 } ] }
```

When the set is won, here by the first team's third game:

```json
{ "mode": "result", "matchId": "emt_...", "teams": [
  { "players": [ { "inGameId": "emb1_..." } ], "wins": 3 },
  { "players": [ { "inGameId": "emb1_..." } ], "wins": 1 } ] }
```

A draw or an abort both games agree on changes no score and posts nothing.
An organizer voiding a game lowers the score. The void posts nothing
itself, but a progress report still waiting to be sent goes out with the
lowered score. Once the bridge has tried to send a progress report on the
set, it refuses to void any game of it: you refuse a report below the wins
an earlier one gave a team, and the set could never be reported again. When a match ends without a result,
the bridge posts an abort with the wins so far:

```json
{ "mode": "abort", "matchId": "emt_...", "reason": "disputed", "retryable": true, "teams": [
  { "players": [ { "inGameId": "emb1_..." } ], "wins": 1 },
  { "players": [ { "inGameId": "emb1_..." } ], "wins": 0 } ] }
```

| How the match ended | `reason` | `retryable` |
|---|---|---|
| The two games disagree about a game, or one never reports it | `disputed` | `true` |
| Neither game reported a game, or a player's link ended | `insufficientPlayers` | `true` |
| Called off another way, such as by the bridge's operator | `serverFailure` | `true` |
| Still open when it expires: a lifetime after its last game with none being played, or 24 hours after its creation whatever is being played | `insufficientPlayers` | `false` |

BluMint has no review step, so a match that would wait for one is ended and
posted as a retryable abort, and you ask for the set again under a new key
with `startingWins`. A match that expired is not worth asking for again, so
its host takes it. Each game started or decided gives the match another
lifetime from then, but never past 24 hours after its creation, since you
free a player held by a match you could not close only for 24 hours after
it was made. At that time the match ends even with a game in progress,
which is called off.

| BluMint answers | The bridge |
|---|---|
| `200` | is done |
| `409` (`already_scored`, `match_closed`) | is done |
| `503 match_not_ready` with `Retry-After` | tries again, no sooner than `Retry-After` |
| `500`, another `5xx`, `429`, a timeout or no connection | tries again |
| `401` or `403` (a key you rotated, or bound to no game) | tries again, so the reports go out once the key is fixed |
| `404 unknown_match` | stops, except for a status, which it tries again |
| Another `4xx` | stops |

The bridge tries again on a schedule from a second to an hour, for a day.
Once it starts posting a result, the result is final on the Ember side too,
so BluMint never receives a second, different result for a match.

## Testing on staging

These steps need the staging bridge, which is not up yet (see Not built
yet).

1. **On BluMint:** use Discord player lookup for the test tournament, once
   Ember has registered the three callbacks on the staging bridge. Create
   matches with the Ember IDs returned by lookup and send players the
   returned `matchUrl`.
2. **Trust the staging bridge:** in Ember, choose **Online play > Ember ID >
   Linked accounts**, enter `https://bridge-staging.embernetplay.link` under
   **Add a service**, choose **Look up service**, then **Trust** it. Ember
   trusts only its own bridge by itself.
3. **Connect Discord:** open the staging connect page (`https://embernetplay.link/start#<staging bridge ID>/BluMint`)
   and choose its button, or paste its link into **Online play > Ember ID >
   Discord > Paste link**. Not the **Discord** row by itself: that connects
   Discord on the production bridge, where BluMint staging's lookup finds
   nobody. Sign in with the same Discord account used on BluMint, then
   return to BluMint to register again.
4. **Play:** assigned matches appear automatically, with a notice and a
   **Tournament matches** row on Home, and a list under
   **Online play > Ember ID > Tournament matches**. Players can also paste
   the `matchUrl` there. Open the assigned match and press **Play**. Check
   the reports and the result on BluMint.

## Not built yet

- **Production.** `https://bridge.embernetplay.link` serves only BluMint
  staging today: its one BluMint connection, `blumint-partner-staging`, has
  environment `staging` and reports to `https://staging.blumint.io/api`. A
  production connection reporting to `https://app.blumint.com/api` is not
  deployed or registered, so BluMint production cannot call it yet.
- **The staging bridge.** `https://bridge-staging.embernetplay.link` is not
  up, and its bridge ID, connection and registration do not exist yet.
- **Tested on real hardware.** Automated tests cover tournament play, but a
  two-PC match on real hardware has not been done yet.
- **Watching.** `spectators` is sent and not read yet; the listed staff
  cannot watch a match through Ember.

## Answers from BluMint

1. **Field names.** BluMint sends each player's Ember ID as an identity's
   `inGameId`, and reads it there in every report.
2. **Status.** BluMint never calls a status URL. The bridge registers none,
   and posts a `status` report each time a player arrives or leaves, a game
   starts, or a game ends with another to come (see Reports).
3. **Restarts.** A match that ended without a result is an abort with
   `retryable`. BluMint asks for the set again under a new key after a
   retryable one, carrying `startingWins`.
4. **Unplayed matches.** An expired match is an abort with `retryable:
   false`, and BluMint's host takes the match.
