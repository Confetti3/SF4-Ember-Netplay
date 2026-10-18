# Challonge tournaments on Ember

Status: in progress on `feat/challonge`. Account linking is generic over sign-in sites, as [GENERIC_SITES.md](GENERIC_SITES.md) sets out: Challonge and start.gg are configured sites, not code. The Challonge API details this relies on are collected in [CHALLONGE_API.md](CHALLONGE_API.md) and start.gg's in [STARTGG_API.md](STARTGG_API.md), with the points the docs leave open marked for a live check.

## Goal

An organizer runs a bracket on Challonge as usual. Players connect their Challonge account to their Ember ID once, in the game. From then on every set they are due to play appears in Ember (Online play > Ember ID, and the Home row while a match is ready), they play it with the normal tournament flow, and the score goes back to Challonge by itself.

Challonge is the first site. The same connector takes start.gg next and anything else with a read and a write API after that, and sites or bots that prefer to drive Ember themselves keep the existing generic API (`docs/guides/INTEGRATIONS.md`).

## Why a connector polls

Neither Challonge nor start.gg can call us: neither documents webhooks. So an Ember-run service reads the bracket and drives the bridge's generic match API as an ordinary `direct` connection. The bridge needs no Challonge-specific connection kind and no Challonge credentials of the organizer's. It only gains the player's account link on a configured sign-in site and a lookup by that site's accounts, with nothing in its code naming any site.

```
Challonge  <--poll / report--  ember-brackets  --POST /v1/matches-->  bridge  <-->  game
                                     ^                                  |
                                     +------ signed result ------------+
```

## Pieces

### 1. Protocol (`server/ember/protocol`)

- `sites.rs`: the feature `sites`; paths `/v1/sites/{site}/start` (POST starts, DELETE cancels), `/v1/sites/{site}/callback` (the site's redirect, and the page's answer) and `/v1/sites/{site}` (GET reads, DELETE disconnects); site IDs (`[a-z0-9-]{1,32}`, never `discord`); `SignInSite` as capabilities list it (`id`, `display_name`, `sign_in`, `lookup_by`, `authorize_origin`, `authorize_path`); `SiteAccount { site, id }` for lookups and `FoundAccount { site, id, user_id, ember_id, participant_id, connected_at }` for answers. The wire types are Discord's, shared in `sign_in.rs`: `SignInStarted { authorize_url, expires_at }`, `Connection { account }`, `Account { user_id, username, connected_at }`.
- `challenge.rs`: proof actions `site.connect`, `site.cancel`, `site.remove`, bound to the exact path like every proof, so a proof for one site cannot be used on another. Moving an account held by another Ember ID is the answer on the bridge's page, as for Discord, so there is no proof action for it.
- `partner.rs`: `Lookup.accounts` (`[{ site, id }]`) next to `Lookup.discord`, answered in `LookupAnswer.accounts`. `accounts` is left out of a request and an answer when empty, so existing callers and Discord-only answers are unchanged; a lookup names at least one list.
- `api.rs`: `Capabilities.sign_in_sites`, and error code `sign_in_busy` (503, retryable) for a start a site's limits refuse, so Ember can say so instead of "the service did not answer".

### 2. Bridge: sign-in sites and lookup (`server/ember/bridge`)

A sign-in site is a definition in the bridge's `sign_in_sites` config (`config::sites`): where the browser goes, how the code is exchanged (`form` or `json`), how to read who signed in (method, URL, headers, an optional JSON body such as start.gg's GraphQL query, JSON pointers to the user ID and username), what a username may be, whether lookups match usernames or user IDs, the hosts every URL must be on, and optional request limits. The code names no site. Example definitions for Challonge and start.gg ship in `deploy/sites/`, and the tests load them; a test also runs a third site defined only in its configuration. Discord is not a configured site and keeps its own config, tables, routes, wire and lookup, which BluMint depends on.

Every site's authorization-code flow goes through the same implementation as Connect Discord (`routes::sign_in`, one flow over Discord and every configured site, with what differs per site read from its definition in `routes::sign_in::provider`):

1. The game sends a `site.connect` proof to `POST /v1/sites/{site}/start` and opens the returned `authorize_url` in the browser. One sign-in per Ember ID and site; a new start replaces the old one. Starts are rate limited per Ember ID like Discord's.
2. The site sends the browser to `GET /v1/sites/{site}/callback` with a code. The bridge exchanges it at the token URL with the client secret, reads the user ID and username, then drops the token. No site's token is stored.
3. The bridge's page names the Ember ID and asks before connecting, so a link someone else started cannot tie the person who opens it to their Ember ID. If another Ember ID holds the account, the page says so and the person signed in to the site chooses to move it or keep it there; the choice holds only while that Ember ID still has it. Connecting replaces this Ember ID's previous account on that site, if any, and ends the links approved through the replaced account.
4. `GET /v1/sites/{site}` reads the connected account. `DELETE /v1/sites/{site}/start` cancels, `DELETE /v1/sites/{site}` disconnects. A site the bridge does not have is 404 `not_found`.

The device grant was the first plan. It was dropped because it needs polling, which Challonge's free tier of 500 requests a month cannot pay for, and because it has no bridge page in the loop: a code handed to someone else would connect their account to the wrong Ember ID.

Storage (migration 027), one set of tables for every site so a new site needs no migration: `site_accounts` (site, user ID, username and, for a site whose lookups match usernames, its key as the site's rule compares it; unique per site by user ID, by Ember ID and by key; cascade on account delete), `site_sign_ins` and `site_withdrawals` shaped like Discord's plus `site`, and `site_requests` (requests per site and UTC day, summed for the month, for the limits below). `links.approved_via` gains `site`, with claim `<site>:<user id>`.

Config: `sign_in_sites` in `bridge.json`. Each site's client secret lives in `integrations.json` under `site_client_secrets.<site>` (`set-integration-secret.sh site <site>`); a site without it is listed with `sign_in: false`, its accounts still answering lookups. A sign-in costs two requests (token and user read). A site with `limits` has `monthly_request_budget` (per calendar month in UTC) and `daily_request_budget` (per UTC day) checked before each exchange, with a warning in the log at 80% of the month's, and `starts_per_hour` limiting proven starts across the bridge; Challonge's example definition sets 400, 40 and 30 against its free tier. A start any of these refuses answers `sign_in_busy`; each refusal is logged, at most hourly or daily. These count only the bridge's own requests, made with its OAuth app; ember-brackets counts its own, made with the organizer's key, and Challonge may count the two apart or together (open question). This bounds a day's cost, not a drain: throwaway Ember IDs cost nothing, so someone determined can spend the day's share every day and keep that site's sign-in busy, and the month runs out after `monthly_request_budget / daily_request_budget` such days.

Lookup: a connection whose `lookup_sites` names a site may send `{ site, id }` entries to `POST /v1/players/lookup`. Each one with a connected Ember ID is linked on that connection (subject `<site>:<user id>`, `approved_via = 'site'`), as the Discord lookup does. Challonge's definition matches usernames without case: the username is the one captured at connect time, and when another account connects with a name the bridge holds, the older one stops answering for it, so each answer carries `connected_at`, since a player who renamed is still found under the old name until someone connects with it. start.gg's definition matches user IDs exactly, and its gamer tags are only shown.

### 3. Game and helper

- Helper (`rust/sf4-net/src/tournament`): `site_status`, `site_connect`, `site_cancel`, `site_remove` with `{bridge_id, site}`, mirroring the Discord calls. It opens only the `authorize_url` the bridge answers, and only when it is https on that site's `authorize_origin` and `authorize_path` from the bridge's capabilities.
- Game: one row per entry in `sign_in_sites` under Online play > Ember ID, after Discord, behaving like Connect Discord: the browser opens the site, the bridge's page asks before connecting or moving, and the screen then shows the connected username with Disconnect. Cancel and Try again behave like Connect Discord. The strings are generic with the site's display name, in all 14 catalogs, so a new site needs no translation.

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
- A participant's Challonge user ID is not exposed, only the username, so a player who renames on Challonge reconnects in Ember to match again. start.gg exposes user IDs, so its lookups need no username.

## Later

- Letting the bridge's own brackets and lobbies create `ember-room-v1` sets, so they can be played in the game.
- An organizer page to add a bracket without editing connector config.
