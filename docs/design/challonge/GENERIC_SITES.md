# Sign-in sites: one generic path for Challonge, start.gg and the next site

Status: design for the rework on `feat/challonge`. Replaces the Challonge-specific account storage, routes, proof actions, lookup list and config from the earlier commits on this branch (none of which has shipped).

## Goal

Adding a bracket site that needs players' own accounts should be a provider preset in the bridge and a config entry, never a migration, a protocol change or new game strings. Discord stays exactly as it is, because BluMint depends on its tables, routes, wire and lookup.

Two sites ship on this path now: Challonge (players found by username) and start.gg (players found by user ID, or through their linked Discord with the existing Discord lookup).

## Bridge

### Sites and kinds

- A **site** is a configured sign-in, named by a site ID (`challonge`, `startgg`, or any `[a-z0-9-]{1,32}` the operator picks, e.g. `challonge-staging`).
- A **kind** is the code preset a site uses: `challonge` or `startgg`. The kind fixes what is not configurable: the authorize, token and user endpoints relative to the site's configured origins, the token request encoding, how the signed-in user is read (Challonge `/v2.1/me.json`; start.gg GraphQL `currentUser`), which identifiers it yields, and what a lookup ID matches (`username` for Challonge, `user_id` for start.gg).
- Config: one list, `sign_in_sites: [{ "id", "kind", "display_name", "client_id", origins with kind defaults, "scope" with kind default, optional "limits": { "monthly_request_budget", "daily_request_budget", "starts_per_hour" } }]`. Client secrets live in `integrations.json` keyed by site ID (`site_client_secrets: { "<id>": "..." }`), set with `set-integration-secret.sh site <id>`. A site without its secret is listed for accounts but not offered for sign-in, as Discord is today.
- Origin checks stay per kind: Challonge origins must be https on challonge.com or a subdomain, start.gg on start.gg or a subdomain; loopback http only where tests already allow it.
- Discord is not a configured site. It keeps its own `discord` config block, tables, routes, actions, features and lookup list, untouched.

### Storage (migration 027, rewritten)

- `site_accounts (site, user_id, username, username_key, ember_id, connected_at)`: primary key `(site, user_id)`, unique `(site, ember_id)`, unique `(site, username_key)` where not null, cascade with the identity like the rest.
- `site_sign_ins`: Discord's `discord_sign_ins` shape plus `site`.
- `site_withdrawals (site, user_id, connection_id)`.
- `site_requests (site, day, spent)`: as now.
- `links.approved_via` gains `site`; such links carry claim ID `<site>:<user_id>`.
- No per-site tables. A new site needs no migration.

### Routes and proofs

- `POST /v1/sites/{site}/start`, `DELETE /v1/sites/{site}/start`, `GET|POST /v1/sites/{site}/callback`, `GET /v1/sites/{site}`, `DELETE /v1/sites/{site}`.
- Proof actions `site.connect`, `site.cancel`, `site.remove`, bound to the exact path as every proof already is, so a proof for one site cannot be used on another.
- The browser pages are the shared ones with the site's display name.
- Unknown site: 404 `not_found`. Budget or hourly refusal: 503 `sign_in_busy`.

### Discovery

Capabilities gain `sign_in_sites: [{ "id", "kind", "display_name", "sign_in": bool, "lookup_by": "username" | "user_id" }]` (empty or absent on bridges without sites) and the feature `sites`. Discord's `discord` and `discord.accounts` features are unchanged.

### Lookup

- Request: `{ "discord"?: [...], "accounts"?: [{ "site", "id" }] }`. Omitted lists default to empty; at least one entry overall; `null` refused; 32 entries in total.
- Answer: `{ "players": [... Discord, unchanged], "accounts"?: [{ "site", "id", "user_id", "ember_id", "participant_id", "connected_at" }] }`, `accounts` omitted when empty so Discord-only answers are byte-identical.
- Connection setting `lookup_sites: ["challonge", "startgg"]` beside the existing `discord_lookup`. A list the connection is not allowed is 403. Never allowed on BluMint connections.
- `id` is matched by the site's `lookup_by`: Challonge usernames without case through `username_key`; start.gg user IDs exactly.

## Connector (`ember-brackets`)

- Entries carry `Account::Site { site, id }` and, when the platform exposes it, `Account::Discord { user_id }`. The bracket logic looks each kind up through the matching list and prefers the site account, then Discord.
- Each tournament names its adapter (`challonge` or `startgg`) and the bridge site ID its accounts live under (`bridge_site`, defaulting to the adapter name).
- start.gg adapter: event by slug, open sets through GraphQL with pagination, entrants' participants' `user.id` and Discord authorizations, `markSetInProgress`, `reportBracketSet` with the winner and per-game data in the order the bridge played them (read from the match snapshot), the set re-read before reporting, start.gg's own rate limit honoured. Organizer token from a file, https on start.gg only.

## Game

- One generic row per entry in `sign_in_sites` under Online play > Ember ID, after Discord, using generic strings with the site's display name (`Connect {0}`, `Waiting for {0}`, `{0} connected as {1}`, and so on) so a new site needs no translation.
- Helper IPC: `site_status`, `site_connect`, `site_cancel`, `site_remove` with `{bridge_id, site}`. The helper opens a sign-in page only for a kind it knows, on that kind's hosts and authorize path. A site of an unknown kind shows its account but no Connect.
- Discord's row, screen, IPC, strings, links and tests are untouched.

## Verification

- **BluMint:** BluMint, Discord and partner test files byte-identical and passing; migration 027 on a BluMint-shaped database keeps every link, account and withdrawal; Discord lookup and BluMint lookup bytes unchanged; the game's Discord journey tests untouched and passing.
- **Challonge:** bridge sign-in and lookup tests against a fake Challonge; connector end to end against a fake Challonge and the real bridge; game journey for a Challonge row.
- **start.gg:** bridge sign-in tests against a fake start.gg (OAuth plus GraphQL `currentUser`); lookup by start.gg user ID; connector end to end against a fake start.gg GraphQL and the real bridge, finding one player by start.gg account and the other by Discord; game journey with two site rows.
