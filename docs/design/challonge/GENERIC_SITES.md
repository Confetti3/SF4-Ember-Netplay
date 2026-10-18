# Sign-in sites: one generic path for Challonge, start.gg and the next site

Status: design for the rework on `feat/challonge`. Replaces the Challonge-specific account storage, routes, proof actions, lookup list and config from the earlier commits on this branch (none of which has shipped).

## Goal

Ember exposes only what the bridge needs, with nothing service-specific in it: who a player is (an Ember ID linked to an account on some site), what to play (two players, first to N), and where the result goes (a signed result, events, webhooks). Adapters translate a particular service to and from that, and anyone can write one: a sign-in provider is data in the bridge's config, and a bracket adapter is either a Rust adapter in the connector or the connector's `http` adapter calling a service of the organizer's own. People who want full control drive the bridge's generic API directly with the SDK.

Challonge and start.gg ship as the first two of each: example provider definitions and connector adapters. Discord stays exactly as it is, because BluMint depends on its tables, routes, wire and lookup. BluMint's push contract stays built into the bridge.

## Bridge

### Sign-in providers are data

- A **site** is a configured sign-in, named by a site ID (`[a-z0-9-]{1,32}`, e.g. `challonge`, `startgg`, `challonge-staging`). The bridge's code contains no site names.
- Each site's definition, in one `sign_in_sites` config list:
  - `id`, `display_name`, `client_id`, `scope`;
  - `authorize_url` (https; the browser goes here with the standard OAuth parameters plus optional `authorize_params`);
  - `token_url` and `token_encoding` (`form` or `json`), client secret sent in the body;
  - `user`: how to read who signed in: `method` (`GET` or `POST`), `url`, extra `headers`, optional JSON `body` (a GraphQL query for start.gg), and JSON pointers `id_pointer` and optional `username_pointer` into the answer (numbers are read as decimal strings);
  - `username_rule`: the characters and length a username may have, and whether it compares without case;
  - `lookup_by`: `user_id` or `username`;
  - `hosts`: the hosts the bridge may call and the game may open for this site (every URL above must be https on one of them; loopback http only where tests already allow it);
  - optional `limits`: `monthly_request_budget`, `daily_request_budget`, `starts_per_hour`.
- Client secrets live in `integrations.json` keyed by site ID (`site_client_secrets`), set with `set-integration-secret.sh site <id>`. A site without its secret is listed for accounts but not offered for sign-in, as Discord is today.
- Example definitions for Challonge and start.gg ship in `server/ember/bridge/deploy/sites/` and are what the tests load.
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

Capabilities gain `sign_in_sites: [{ "id", "display_name", "sign_in": bool, "lookup_by": "username" | "user_id", "authorize_origin", "authorize_path" }]` (empty or absent on bridges without sites) and the feature `sites`. Discord's `discord` and `discord.accounts` features are unchanged.

### Lookup

- Request: `{ "discord"?: [...], "accounts"?: [{ "site", "id" }] }`. Omitted lists default to empty; at least one entry overall; `null` refused; 32 entries in total.
- Answer: `{ "players": [... Discord, unchanged], "accounts"?: [{ "site", "id", "user_id", "ember_id", "participant_id", "connected_at" }] }`, `accounts` omitted when empty so Discord-only answers are byte-identical.
- Connection setting `lookup_sites: ["challonge", "startgg"]` beside the existing `discord_lookup`. A list the connection is not allowed is 403. Never allowed on BluMint connections.
- `id` is matched by the site's `lookup_by` and `username_rule`: with the shipped definitions, Challonge usernames without case and start.gg user IDs exactly.

## Connector (`ember-brackets`)

- The adapter interface (`Site` trait: open sets, one set, mark underway, report, standing) is the published contract, documented in `server/ember/brackets/ADAPTERS.md` with the guarantees the connector adds on top (one live match per set, attempts, evidence before cancelling, retries, budgets, verified results).
- Entries carry `Account::Site { site, id }` and, when the platform exposes it, `Account::Discord { user_id }`. The bracket logic looks each up through the matching lookup list and prefers the site account, then Discord.
- Each tournament names its `adapter` and the bridge site its accounts live under (`bridge_site`).
- Adapters shipped:
  - `challonge`;
  - `startgg`: event by slug, open sets through GraphQL with pagination, entrants' participants' `user.id` and Discord authorizations, `markSetInProgress`, `reportBracketSet` with the winner and per-game data in the order the bridge played them (read from the match snapshot), the set re-read before reporting, start.gg's own rate limit honoured;
  - `http`: the connector calls the organizer's own service with a small JSON contract (`GET {base}/sets?state=open`, `GET {base}/sets/{id}`, `POST {base}/sets/{id}/underway`, `POST {base}/sets/{id}/report`, optional `GET {base}/standing`), bearer token from a file, so any service can be adapted in any language. The contract is versioned and documented in ADAPTERS.md.
- Organizer credentials come from files; each adapter's origin is checked (https on its service, loopback only behind `allow_loopback_http`; the `http` adapter accepts any https origin the organizer configures).

## Game

- One generic row per entry in `sign_in_sites` under Online play > Ember ID, after Discord, using generic strings with the site's display name (`Connect {0}`, `Waiting for {0}`, `{0} connected as {1}`, and so on) so a new site needs no translation.
- Helper IPC: `site_status`, `site_connect`, `site_cancel`, `site_remove` with `{bridge_id, site}`. The helper opens a sign-in page only when it is https on the site's declared `authorize_origin` with its `authorize_path`, as the bridge's capabilities list them for that site. The bridge the player trusts is the authority on its sites; Ember's own service is the default.
- Discord's row, screen, IPC, strings, links and tests are untouched.

## Verification

- **BluMint:** BluMint, Discord and partner test files byte-identical and passing; migration 027 on a BluMint-shaped database keeps every link, account and withdrawal; Discord lookup and BluMint lookup bytes unchanged; the game's Discord journey tests untouched and passing.
- **Challonge:** bridge sign-in and lookup tests against a fake Challonge; connector end to end against a fake Challonge and the real bridge; game journey for a Challonge row.
- **start.gg:** bridge sign-in tests against a fake start.gg (OAuth plus GraphQL `currentUser`) loaded from the shipped definition; lookup by start.gg user ID; connector end to end against a fake start.gg GraphQL and the real bridge, finding one player by start.gg account and the other by Discord; game journey with two site rows.
- **Any service:** connector end to end through the `http` adapter against a small fake service; a bridge test with a third site defined only in config.
