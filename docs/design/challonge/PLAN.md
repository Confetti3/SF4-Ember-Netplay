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

- `challonge.rs`: features `challonge` and `challonge.accounts`, paths `/v1/challonge/start`, `/v1/challonge`, `/v1/challonge/move`, and the wire types `SignInStarted`, `Connection`, `Account`, `SignIn`, `SignInState`, `MoveChoice`. Usernames are compared without case.
- `challenge.rs`: proof actions `challonge.connect`, `challonge.remove`, `challonge.cancel`, `challonge.move`.
- `partner.rs`: `Lookup.challonge` (usernames) next to `Lookup.discord`, answered in `LookupAnswer.challonge`. Both new fields are optional on the wire, so existing callers are unchanged.

### 2. Bridge: Challonge sign-in and lookup (`server/ember/bridge`)

Device sign-in, so nothing has to redirect back to the bridge:

1. The game sends a `challonge.connect` proof to `POST /v1/challonge/start`. The bridge asks Challonge for a device code (`/v2.1/oauth/authorize_device`, scope `me`), keeps it, and answers with the user code and the page to enter it on. One sign-in per Ember ID; a new start replaces the old one. Starts are rate limited per Ember ID like Discord's.
2. A background task polls Challonge's token endpoint for waiting sign-ins at the interval Challonge asks for (backing off on `slow_down`). On a token it reads `/v2.1/me` for the user ID and username, then drops the token. No Challonge token is stored.
3. If no other Ember ID holds that Challonge account, it is connected (replacing this Ember ID's previous one, if any, and ending links approved through the replaced account). If another Ember ID holds it, the sign-in waits in `moving` until the player chooses on `POST /v1/challonge/move` (`move` or `keep`, with a `challonge.move` proof).
4. `GET /v1/challonge` is a pure read of the account and any sign-in. `DELETE /v1/challonge/start` cancels, `DELETE /v1/challonge` disconnects.

Storage (migration 027): `challonge_accounts` (user ID, username, Ember ID unique, cascade on account delete), `challonge_sign_ins`, and `challonge_withdrawals` so a link the player removed is not recreated by the next lookup. `links.approved_via` gains `challonge`.

Config: a `challonge` block per bridge (`client_id`, `api_base` defaulting to `https://api.challonge.com`, and the token path, since the docs disagree between `/oauth/token` and `/v2.1/oauth/token`). The client secret, if Challonge needs one, lives in `integrations.json` like Discord's. The `challonge` feature is only advertised when sign-in is configured.

Lookup: a connection with `challonge_lookup: true` may send usernames to `POST /v1/players/lookup`. Each one with a connected Ember ID is linked on that connection (subject `challonge:<user id>`, `approved_via = 'challonge'`), as the Discord lookup does today.

### 3. Game and helper

- Helper (`rust/sf4-net/src/tournament`): `challonge_status`, `challonge_connect`, `challonge_cancel`, `challonge_move`, `challonge_remove`, mirroring the Discord calls. It opens only Challonge's own verification page.
- Game: Online play > Ember ID > Challonge. The screen shows the code in large type with an "Open Challonge" action (the page opens with the code filled in where Challonge allows it), waits while the player approves, and then shows the connected username with Disconnect. A `moving` sign-in asks whether to move the account here. Cancel and Try again behave like Connect Discord. Strings go into all 14 catalogs.

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

- The live API answers listed at the end of CHALLONGE_API.md (token URL, `/me` path, polling errors, whether device sign-in needs a client secret, 429 behaviour). Each is configurable or handled both ways until checked against a registered Connect app.
- Whether Challonge's paid API tier is needed for regular events.
- A participant's Challonge user ID is not exposed, only the username, so a player who renames on Challonge reconnects in Ember to match again.

## Later

- start.gg adapter: players are found through their linked Discord accounts with the existing Discord lookup, results go out with `reportBracketSet`.
- Letting the bridge's own brackets and lobbies create `ember-room-v1` sets, so they can be played in the game.
- An organizer page to add a bracket without editing connector config.
