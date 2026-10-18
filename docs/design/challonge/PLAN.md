# Challonge tournaments on Ember

Status: in progress on `feat/challonge`. The Challonge API details this relies on are collected in [CHALLONGE_API.md](CHALLONGE_API.md), with the points the docs leave open marked for a live check.

## Goal

An organizer runs a bracket on Challonge as usual. Players connect their Challonge account to their Ember ID once, in the game. From then on every set they are due to play appears in Ember (Online play > Ember ID, and the Home row while a match is ready), they play it with the normal tournament flow, and the score goes back to Challonge by itself.

Challonge is the first site. The same connector takes start.gg next and anything else with a read and a write API after that, and sites or bots that prefer to drive Ember themselves keep the existing generic API (`docs/guides/INTEGRATIONS.md`).

## Why a connector polls

Neither Challonge nor start.gg can call us: neither documents webhooks. So an Ember-run service reads the bracket and drives the bridge's generic match API as an ordinary `direct` connection. The bridge needs no Challonge-specific connection kind and no Challonge credentials of the organizer's. It only gains the player's account link and a lookup by Challonge username.

```
Challonge  <--poll / report--  ember-brackets  --POST /v1/matches-->  bridge  <-->  game
                                     ^                                  |
                                     +------ signed result ------------+
```

## Pieces

### 1. Protocol (`server/ember/protocol`)

- `challonge.rs`: features `challonge` and `challonge.accounts`; paths `/v1/challonge/start` (POST starts, DELETE cancels), `/v1/challonge/callback` (Challonge's redirect, and the page's answer) and `/v1/challonge` (GET reads, DELETE disconnects); `is_username` and `username_key`, since usernames are compared without case. The wire types are Discord's, shared in `sign_in.rs`: `SignInStarted { authorize_url, expires_at }`, `Connection { account }`, `Account { user_id, username, connected_at }`.
- `challenge.rs`: proof actions `challonge.connect`, `challonge.cancel`, `challonge.remove`. Moving an account held by another Ember ID is the answer on the bridge's page, as for Discord, so there is no proof action for it.
- `partner.rs`: `Lookup.challonge` (usernames) next to `Lookup.discord`, answered in `LookupAnswer.challonge` as `FoundChallongePlayer { username, challonge_user_id, ember_id, participant_id, connected_at }`. `challonge` is left out of a request and an answer when empty, so existing callers are unchanged; a lookup names at least one list.
- `api.rs`: error code `sign_in_busy` (503, retryable) for a start Challonge's limits refuse, so Ember can say so instead of "the service did not answer".

### 2. Bridge: Challonge sign-in and lookup (`server/ember/bridge`)

Challonge's authorization-code flow, through the same implementation as Connect Discord (`routes::sign_in`, one flow over both sites, with what differs per site in `routes::sign_in::provider`):

1. The game sends a `challonge.connect` proof to `POST /v1/challonge/start` and opens the returned `authorize_url` (`https://api.challonge.com/oauth/authorize`, scope `me`) in the browser. One sign-in per Ember ID; a new start replaces the old one. Starts are rate limited per Ember ID like Discord's.
2. Challonge sends the browser to `GET /v1/challonge/callback` with a code. The bridge exchanges it at the token path (form body only, with the client secret), reads `/v2.1/me.json` for the user ID and username, then drops the token. No Challonge token is stored.
3. The bridge's page names the Ember ID and asks before connecting, so a link someone else started cannot tie the person who opens it to their Ember ID. If another Ember ID holds the account, the page says so and the person signed in to Challonge chooses to move it or keep it there; the choice holds only while that Ember ID still has it. Connecting replaces this Ember ID's previous account, if any, and ends the links approved through the replaced account.
4. `GET /v1/challonge` reads the connected account. `DELETE /v1/challonge/start` cancels, `DELETE /v1/challonge` disconnects.

The device grant was the first plan. It was dropped because it needs polling, which Challonge's free tier of 500 requests a month cannot pay for, and because it has no bridge page in the loop: a code handed to someone else would connect their account to the wrong Ember ID.

Storage (migration 027): `challonge_accounts` (user ID, username and its lowercase key, Ember ID unique, cascade on account delete), `challonge_sign_ins` and `challonge_withdrawals` shaped like Discord's, and `site_requests` (requests per site and UTC day, summed for the month, for the budgets below). `links.approved_via` gains `challonge`.

Config: a `challonge` block per bridge (`client_id`, `api_base` defaulting to `https://api.challonge.com`, `token_path` defaulting to `/oauth/token` since the docs also name `/v2.1/oauth/token`, `me_path`, `scope`, `monthly_request_budget`, `daily_request_budget`, `starts_per_hour`). The client secret is required and lives in `integrations.json` like Discord's; the `challonge` feature is only advertised when both are there. A sign-in costs two requests (token and `/me`); `monthly_request_budget` (default 400 per calendar month in UTC) and `daily_request_budget` (default 40 per UTC day) are checked before each exchange, with a warning in the log at 80% of the month's, and `starts_per_hour` (default 30) limits proven starts across the bridge. A start any of these refuses answers `sign_in_busy`; each refusal is logged, at most hourly or daily. These count only the bridge's own requests, made with its OAuth app; ember-brackets counts its own, made with the organizer's key, and Challonge may count the two apart or together (open question). This bounds a day's cost, not a drain: throwaway Ember IDs cost nothing, so someone determined can spend the day's share every day and keep sign-in busy, and the month runs out after `monthly_request_budget / daily_request_budget` such days.

Lookup: a connection with `challonge_lookup: true` may send usernames to `POST /v1/players/lookup`. Each one with a connected Ember ID is linked on that connection (subject `challonge:<user id>`, `approved_via = 'challonge'`), as the Discord lookup does. A username is the one captured at connect time; when another account connects with a name the bridge holds, the older one stops answering for it. Each answer carries `connected_at`, since a player who renamed is still found under the old name until someone connects with it.

### 3. Game and helper

- Helper (`rust/sf4-net/src/tournament`): `challonge_status`, `challonge_connect`, `challonge_cancel`, `challonge_remove`, mirroring the Discord calls. It opens only the `authorize_url` the bridge answers, on Challonge's own site.
- Game: Online play > Ember ID > Challonge, behaving like Connect Discord: the browser opens Challonge, the bridge's page asks before connecting or moving, and the screen then shows the connected username with Disconnect. Cancel and Try again behave like Connect Discord. Strings go into all 14 catalogs.

### 4. Connector (`server/ember/brackets`, binary `ember-brackets`)

A small service with one adapter per site behind a common interface:

| Operation | Challonge v2.1 |
|---|---|
| Open sets | `GET /tournaments/{id}/matches.json?state=open` |
| Who is in a set | participants' `username` |
| Mark underway | `PUT .../matches/{id}/change_state.json` `mark_as_underway` |
| Report | `PUT .../matches/{id}.json` with `score_set`, `rank`, `advancing` |
| Reopen | `change_state` `reopen` |

For each configured tournament it:

1. Reads open sets, looks the two entries' usernames up on the bridge, and creates an `ember-room-v1` match for every set where both players have Ember IDs. `external_match_id` is `challonge:<tournament>:<match>:<attempt>`, so a restart or an expired match gets a fresh one.
2. Takes games to win from its config per round group (Challonge does not say), for example first to 2, first to 3 from a given round on.
3. Receives the bridge's signed result at its `results_url`, re-reads the set, and reports the set score only if Challonge has no result yet. A set the organizer reported, reset or removed by hand cancels the Ember match instead.
4. Re-reads the bracket right after each report (that is when new sets open) and otherwise only every couple of minutes, because Challonge's free API tier allows 500 requests a month. It counts requests and warns before the budget runs out.

The organizer's Challonge access is the connector's own config (a v1 API key used against v2.1, or an OAuth token), never the bridge's.

Disputes use the connection's `disputes: "restart"` for now: a disputed game is played again rather than waiting for an organizer screen we do not have yet.

## Open questions

- The live API answers listed at the end of CHALLONGE_API.md (token URL, `/me` path, that the Connect app may use the authorization-code flow with the bridge's redirect URI, 429 behaviour). Each is configurable or handled both ways until checked against a registered Connect app.
- Whether Challonge's paid API tier is needed for regular events, and whether Challonge counts the bridge's OAuth app and an organizer's key against one allowance or two.
- A participant's Challonge user ID is not exposed, only the username, so a player who renames on Challonge reconnects in Ember to match again.

## Later

- start.gg adapter: players are found through their linked Discord accounts with the existing Discord lookup, results go out with `reportBracketSet`.
- Letting the bridge's own brackets and lobbies create `ember-room-v1` sets, so they can be played in the game.
- An organizer page to add a bracket without editing connector config.
