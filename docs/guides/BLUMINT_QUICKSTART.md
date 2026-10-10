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
| Bridge ID | `brg_0dbc0598-2312-4ce3-9df8-e160330565e6` |
| Your connection | `blumint-partner-staging` (tenant `blumint`, kind `blumint`) |
| BluMint API | Game partner API v1.3.0 at `https://staging.blumint.io/api` |
| Player lookup | `POST https://bridge.embernetplay.link/v1/blumint/lookup` |
| Match creation | `POST https://bridge.embernetplay.link/v1/blumint/matches` |
| Match status | `GET` or `POST https://bridge.embernetplay.link/v1/blumint/matches/status?matchId=...` |
| Connect Discord link for players now | `ember://discord/connect?bridge=brg_0dbc0598-2312-4ce3-9df8-e160330565e6` |
| Getting started page for players later | `https://embernetplay.link/start#brg_0dbc0598-2312-4ce3-9df8-e160330565e6` |

The BluMint API key is stored, and registration with BluMint staging is done.
The bridge has called `setLookupPlayer`, `match/setCreate` and
`match/setRetrieveStatus`, registering the three callback URLs and their
authentication. BluMint calls them with
`Authorization: Bearer <credential the bridge issued>`. There is nothing for
BluMint to configure for those callbacks. Results use BluMint's API key,
sent as `x-api-key`; it is separate from the callback credential.

This is a staging service. Its data may be reset before production, and it
carries no uptime promise yet.

## Player lookup

Ember supports one sign-in method: **Discord**. A player connects their
Discord account to their Ember ID once, from the home menu: **Online play**,
**Ember ID**, then **Discord**. This uses Ember's own bridge, the staging bridge above,
by default. The lookup answers only `discord`:

```json
{ "discord": ["emb1_..."] }
```

with `[]` when none of the user's Discord IDs is connected. Other sign-in
methods are ignored, and so are the emails BluMint sends alongside Discord
IDs. The in-game ID is the player's Ember ID (`emb1_...`). Lookup also links
that ID to BluMint's connection so matches can be created for it.

When a registering user has no connected Ember ID, give them the Connect
Discord link above, for example as a registration option's `postambleLink`.
It opens Ember's Connect Discord screen. At an idle menu, Ember creates the
Ember ID if needed, trusts Ember's own bridge unless the player removed it,
and opens Discord's sign-in by itself. A link arriving during play asks first.
After connecting, the player returns to BluMint to register again.

If the Discord account is already connected to another Ember ID, the
bridge's page asks before moving it. It names both Ember IDs and explains
that the other ID's tournament links made through that account will end.
Pressing **Cancel** in Ember also ends the sign-in on the bridge, so a page
left open cannot finish it.

The getting started page will offer **Open in Ember** and instructions for
getting Ember. Its fragment names this bridge. The bridge ID is also the
`bridge_id` in
`https://bridge.embernetplay.link/.well-known/ember-bridge.json`.
The page currently returns 404; see "Testing on staging now".

## Match creation

Send two teams of one player each, using the Ember IDs returned by lookup
(`inGameId`; `playerId` is accepted too). `matchSettings.gamesToWin` sets the
set length: 1 to 10 (best of 1 to best of 19), first to 2 when absent. Other settings are ignored.

```json
{ "teams": [ { "players": [ { "inGameId": "emb1_..." } ] },
             { "players": [ { "inGameId": "emb1_..." } ] } ],
  "matchSettings": { "gamesToWin": 2 } }
```

The answer:

```json
{ "matchId": "emt_...", "matchUrl": "https://embernetplay.link/m#brg_.../emt_..." }
```

Send the returned `matchUrl` to both players. It is the same for both and
safe to send in your messages: only the two assigned Ember IDs can play the
match, and anyone else's Ember finds no such match. The link itself does not expire, but
a match nobody plays does: 24 hours after you created it the bridge ends it
(see "Match status" and "Results").

The match page will open the match in Ember or show how to get Ember first.
It currently returns 404. Testers can paste `matchUrl` into Ember instead,
and assigned matches also appear automatically.

A player can be in one active match per connection; creating a second one
for them is refused (`409`) until the first ends. Sending the same request
again (same players in the same order, same `gamesToWin`) while that match is
active answers with the same match, so a creation whose answer was lost can
be retried.

## Match status

`status` is `pending` while players join and the room is prepared, `running`
while they play, `complete` when someone reaches the set length, and
`cancelled` when the match was called off or expired. A match that nobody
played within 24 hours of its creation also reads `cancelled`, because that is
the only ended-without-a-result status the API has; the players are free and a
new creation for them makes a new match. (A set with games already recorded
is ended only after 24 hours with no game started or decided.) Each player is `absent` until
their Ember claims the match, `present-not-ready` while the room is being
set up, and `present-ready` from then on. Each team's `score` is games won.

Both `GET` and `POST` accept `matchId` in the query string.

## Results

When a match completes, the bridge posts its score to
`https://staging.blumint.io/api/tournaments/match/submit`, using BluMint's
API key. The body contains `matchId` and `teams` with `score` and `inGameId`.

A cancelled match is posted as `mustRestart`. When the two games disagree
about a game's result, or one never reports it, BluMint has no review step,
so the bridge cancels the match and posts `mustRestart`; BluMint then
creates a new match. A match nobody plays within 24 hours is not posted at
all: your result format has no way to say "not played", and `mustRestart`
would ask you to restart a match you may have meant to drop. See the
questions below. Retryable failures are retried for a day; `409` counts
as already received. Once the bridge starts posting a result, it is final
on the Ember side too, so BluMint never receives a second, different result
for a match.

## Testing on staging now

1. **On BluMint:** use Discord player lookup for the test tournament. Give
   players the `ember://discord/connect?bridge=brg_0dbc0598-2312-4ce3-9df8-e160330565e6`
   link as the connect/register-again option. The three callbacks are already
   registered. Create matches with the Ember IDs returned by lookup and send
   players the returned `matchUrl`.
2. **Test build:** Kate sends
   `sf4-ember-netplay-1.1.0-tournament-test5.zip` separately. Testers need this
   build because the current public release has no Ember ID or Discord.
3. **Connect Discord:** start Ember and open the connect link, or use
   **Online play > Ember ID > Discord** from Home. Sign in with the same Discord account
   used on BluMint, then return to BluMint to register again.
4. **Paste fallback:** the `/start` and `/m` web pages currently return 404.
   Testers can copy the getting started page URL from the table and use
   **Paste link** in Connect Discord. They can also copy the returned
   `matchUrl` and use the paste action in **Online play > Ember ID > Tournament matches**.
   Pasting these URLs works without loading the web pages.
5. **Play:** assigned matches also appear automatically, with a notice and a
   **Tournament matches** row on Home, and a list under
   **Online play > Ember ID > Tournament matches**. Open the assigned
   match and press **Play**. Check status and the submitted result on BluMint.

Both web pages go live with the first public Ember release that includes
Ember ID and Discord.

## Not built yet

- **Tested on real hardware.** Automated tests cover tournament play, but a
  two-PC match on real hardware has not been done yet.
- **A public player build with Ember ID and Discord.** These are available
  in the test build. The getting started and match pages go live with the
  first public release that includes them.
- **Production.** There is no production bridge yet.

## Questions for BluMint

Already sent; still open:

1. **Field names.** The guide uses `playerId`, while the OpenAPI schema uses
   `inGameId`. Ember accepts both in match creation and sends `inGameId` in
   status and results. Confirmation of which field BluMint reads is pending.
2. **Status method.** The guide says `GET`; `setRetrieveStatus` describes
   `POST`. Ember serves both with a `matchId` query. Confirmation of which
   method BluMint uses is pending.
3. **Restarts.** Confirmation that `mustRestart` is the intended response
   for a disputed game is pending.
4. **Unplayed matches.** Ember ends a match nobody played after 24 hours. It
   sends you nothing and answers status `cancelled`. Does your API have a way
   to say "not played" (a result field or a status), and does a polled
   `cancelled` status make you restart or reschedule the match by itself?
