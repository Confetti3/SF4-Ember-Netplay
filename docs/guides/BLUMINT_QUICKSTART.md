# Ember bridge: getting started for BluMint

Ember is the netplay mod for Ultra Street Fighter IV. The Ember bridge is the
"game service" in BluMint's game integration guide: it serves BluMint's three
callbacks (player lookup, match creation, match status) and posts each match's
result to BluMint. Players play the matches in Ember, and both players' games
report every game, so results arrive without anyone typing them in.

## What you get

| | |
|---|---|
| Staging bridge | `https://bridge.embernetplay.link` |
| Your connection | `blumint-staging` (tenant `blumint`) |
| Player lookup | `POST https://bridge.embernetplay.link/v1/blumint/lookup` |
| Match creation | `POST https://bridge.embernetplay.link/v1/blumint/matches` |
| Match status | `GET https://bridge.embernetplay.link/v1/blumint/matches/status?matchId=...` |
| Getting started page for players | `https://embernetplay.link/start` |

Ember registers these three URLs with BluMint itself (the `setLookupPlayer`,
`match/setCreate` and `match/setRetrieveStatus` webhooks), with an
`authentication` object that has BluMint send `Authorization: Bearer <key>`.
For that, Ember needs a BluMint API key for this game.

This is a staging service. Its data may be reset before production, and it
carries no uptime promise yet.

## Player lookup

Ember supports one sign-in method: **Discord**. A player connects their
Discord account to their Ember ID once, from Ember (Settings, Ember ID, Linked
accounts, Discord), through Discord's own sign-in. The lookup answers only
`discord`:

```json
{ "discord": ["emb1_..."] }
```

with `[]` when none of the user's Discord IDs is connected. Other sign-in
methods are ignored, and so are the emails BluMint sends alongside Discord
IDs. The in-game ID is the player's Ember ID (`emb1_...`).

When a registering user has no Ember account, point them at
`https://embernetplay.link/start` (for example as a registration option's
`postambleLink`). It explains getting Ember and connecting Discord, then
sends them back to register again.

## Match creation

Send two teams of one player each, by in-game ID (`inGameId`; `playerId` is
accepted too). `matchSettings.gamesToWin` sets the set length: 1, 2, 3 or 5,
first to 2 when absent. Other settings are ignored.

```json
{ "teams": [ { "players": [ { "inGameId": "emb1_..." } ] },
             { "players": [ { "inGameId": "emb1_..." } ] } ],
  "matchSettings": { "gamesToWin": 2 } }
```

The answer:

```json
{ "matchId": "emt_...", "matchUrl": "https://embernetplay.link/m#brg_.../emt_..." }
```

`matchUrl` is the same for both players and safe to send in your messages:
only the two assigned Ember IDs can play the match, and anyone else's Ember
finds no such match. The page opens the match in Ember, or shows how to get
Ember first. It does not expire.

A player can be in one active match per connection; creating a second one
for them is refused until the first ends.

## Match status

`status` is `pending` until both players' Ember has joined, `running` while
they play, `complete` when someone reaches the set length, and `cancelled`
when the match was called off. Each player is `absent` until their Ember
claims the match, `present-not-ready` while the room is being set up, and
`present-ready` from then on. Each team's `score` is games won.

## Results

When a match completes, the bridge posts its score to
`/tournaments/match/submit` (`teams` with `score` and `inGameId`). A cancelled
match is posted as `mustRestart`. When the two games disagree about a game's
result, or one never reports it, there is no one at BluMint to review it, so
the bridge cancels the match and posts `mustRestart`; BluMint then creates a
new match. Posts are retried for a day; `409` counts as already received.

## Not built yet

- **Tested with real players.** Matches played in Ember are covered by
  automated tests but have not yet been played on two PCs.
- **A player build with Ember ID and Discord.** The current public Ember
  release does not include them yet. A test build can be provided.
- **Production.** There is no production bridge yet.

## Questions for BluMint

1. **API key.** Ember needs an API key for this game to register the three
   URLs and post results.
2. **Field names.** The guide's examples use `playerId` in match creation and
   results, while the OpenAPI schema uses `inGameId`. Ember accepts both and
   sends `inGameId`. Which does BluMint read?
3. **Status method.** The guide calls the status endpoint with `GET` and a
   `matchId` query; `setRetrieveStatus`'s description says `POST`. Ember
   serves both. Which will BluMint use?
4. **Restarts.** Is `mustRestart` the right answer for a disputed game, or is
   there another way to report it?
