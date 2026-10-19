# start.gg developer API reference for a Rust OAuth sign-in and bracket connector

Read on 2026-10-18. Scope: (a) OAuth sign-in on a Rust server, (b) a read and report bracket connector in Rust. No live API call was made (no client id or token was available), so nothing here is confirmed against real server behavior beyond what the cited open-source clients show.

Labels used in every table:

- **STATED**: written in the official docs or schema reference. The source is named.
- **OSS**: shown by working code in a public client. The repository and file are named.
- **INFERENCE**: not written anywhere official; derived from the sources named in the row. Treat as a hypothesis to verify against the real API.
- **NOT STATED**: no source found. Do not assume; test or ask.

## 0. Sources

| Short name | URL | Notes |
|---|---|---|
| docs | https://developer.start.gg/docs/intro and the pages under it | Markdown source read from https://github.com/smashgg/developer-portal (`docs/oauth/oauth-overview.md`, `docs/oauth/example-oauth-flow.md`, `docs/oauth/scopes.md`, `docs/authentication.md`, `docs/sending-requests.md`, `docs/rate-limits.md`, `docs/glossary.md`, `docs/examples/queries/*`, `docs/examples/mutations/report-set.md`) |
| schema | https://developer.start.gg/reference/ redirects (307) to https://smashgg-schema.netlify.app/reference/<type>.doc.html | graphdoc 2.4.0 output. Pages read: query, mutation, event, set, setfilters, setslot, setconnection, pageinfo, setsorttype, setentrantsource, entrant, participant, user, profileauthorization, socialconnectiontype, authorizationtype, bracketsetgamedatainput, bracketsetgameselectioninput, tournament, activitystate, phase, phasegroup, player, seed, standing, score, game, round, timestamp. Note: a wrong path (without `/reference/`) returns a "Page not found" page with HTTP 200. |
| changelog | https://github.com/smashgg/developer-portal/blob/master/CHANGELOG.md | Latest entry read: 2026-10-14 (`Player.sets` gained `opponentId`; `SetFilters.entrantSize` fixed). |
| legacy REST help | https://help.start.gg/en/articles/13813315-rest-api-access-legacy-old | Legacy REST, not GraphQL. Used only for the set state integers. |
| wizzrobe | https://github.com/tcprescott/wizzrobe (`docs/features/startgg.md`, `application/utils/clients/startgg_client.py`, `application/services/startgg_service.py`, `models/startgg.py`) | A bracket connector plus OAuth sign-in, closest in shape to what Ember needs. |
| sasl | https://github.com/Kasama/sasl-omegastrikers (`docs/startgg.md`, `src/startgg/oauth.rs`, `src/routes/auth.rs`, `src/startgg/auth.rs`, `src/startgg/currentUser.graphql`) | A Rust (axum, reqwest) OAuth sign-in. |
| replay-manager | https://github.com/jmlee337/replay-manager-for-slippi (`src/main/startgg.ts`, `src/common/types.ts`) | Reports and manages sets with a personal token. Source of the mutation error strings and the full `State` enum. |

The fetch tool used for the official pages summarizes rather than quoting, so the raw markdown was read with `curl` from raw.githubusercontent.com for the docs repo and the open-source files. JSON response examples below that are marked ILLUSTRATIVE were written from the schema and are not captured responses.

## 0.1 Discrepancies found in the sources (read these first)

| # | Discrepancy | Resolution for the implementer |
|---|---|---|
| 1 | The authorize host. `docs/oauth/oauth-overview.md` says `https://start.gg/oauth/authorize`. `docs/oauth/example-oauth-flow.md` redirects to `http://api.start.gg/oauth/authorize?...&redirect_uri=&client_secret=...`, which has an empty `redirect_uri` and puts the client secret in the URL. | Use `https://start.gg/oauth/authorize` with no secret. Both working clients (wizzrobe, sasl) use that host. The example page is stale or wrong. |
| 2 | The refresh URL. docs and wizzrobe use `https://api.start.gg/oauth/refresh`. sasl (`src/startgg/auth.rs`) posts the refresh to `https://api.start.gg/oauth/access_token` with `"grant_type": "refresh_token"` and `"scope": []`. | Use `/oauth/refresh` as documented. That sasl works against `/oauth/access_token` is INFERENCE only (it ships, but whether refresh ever succeeded there is not shown). |
| 3 | The schema description of `markSetInProgress` reads "Update a set to called state", a copy of `markSetCalled`. | Documentation bug in the schema. The field name and replay-manager's behavior (state becomes 2, error "Set is already started") show it starts the set. |
| 4 | Set `id` JSON type is inconsistent in the docs. Responses show a string in `sets-in-event` and `sets-in-phase-group` (`"18368672"`) and a number in `sets-by-station`, `sets-by-player`, `set-score`, `set-game-data` (`17469065`, `6068546`, `65089253`). Variables are passed both ways (`65089253` and `"7851789"`). replay-manager types `Id = string | number` and tests `typeof set.id === 'number'` for real ids. | `Set.id` is the `ID` scalar. Deserialize with a string-or-number enum (section 6.3), store as a `String`, and send variables as strings. |
| 5 | The docs list four required authorize parameters and do not list `state`. wizzrobe and sasl both send `state`, and sasl requires it on the callback. | Send `state`. See section 1.1 (INFERENCE: it is echoed unchanged). |
| 6 | `Set.winnerId` and `Game.winnerId` are `Int`, but the mutation arguments `winnerId` are `ID`. Entrant ids are `ID` in the schema but arrive as JSON numbers. | Compare ids as strings after normalizing. See section 6. |

## 1. OAuth 2.0 authorization-code flow

The docs say: "Currently start.gg only supports the authorization code grant flow and refresh token." (oauth-overview). There is no implicit, client-credentials or device grant.

Register an application at https://start.gg/admin/profile/developer/applications. The application page shows the client id and secret and has a helper that generates the authorize URL (oauth-overview). The registered redirect URI list and whether matching is exact are NOT STATED (INFERENCE: exact match, as RFC 6749 recommends).

### 1.1 Step 1: send the user to the authorize URL

| Item | Value | Label and source |
|---|---|---|
| Method | GET (browser redirect, HTTP 302 or 307 from your server) | STATED (oauth-overview, step 1 "redirects") |
| URL | `https://start.gg/oauth/authorize` | STATED (oauth-overview); OSS (wizzrobe `AUTHORIZE_URL`, sasl `STARTGG_AUTHORIZE_URL`) |
| `response_type` | `code` (must be) | STATED |
| `client_id` | your application id | STATED |
| `scope` | scopes separated by a URL-encoded space (`%20`) | STATED. sasl builds the query with `url::Url::query_pairs_mut().append_pair`, which encodes a space as `+`; wizzrobe uses `urllib.parse.quote`, which gives `%20`. Both ship, so `+` is probably accepted too (INFERENCE). Use `%20`. |
| `redirect_uri` | URL-encoded absolute URL | STATED |
| `state` | opaque CSRF token you generate | NOT STATED in the docs. OSS: wizzrobe and sasl send it; sasl's callback handler requires `state` in the query and compares it to a cookie, so start.gg returns it. INFERENCE: it is echoed unchanged. |
| PKCE (`code_challenge`, `code_challenge_method`) | not mentioned | NOT STATED. No `code_challenge` in the docs, schema or either client. No evidence of support, and none is required: the token request authenticates with `client_secret`. Do not rely on PKCE. |

Example (docs, with `state` added per the OSS clients):

```
https://start.gg/oauth/authorize?response_type=code&client_id=0&scope=user.identity%20user.email&redirect_uri=http%3A%2F%2Fexampleurl.com%2Foauth&state=3f9a...
```

### 1.2 Step 2 and 3: user signs in and approves

The user signs in to start.gg and approves the token usage and scopes (docs). Nothing else is documented. Consent screen appearance, whether a previously approved grant skips the screen, and the error redirect for a denied consent (`error=access_denied&state=...`) are NOT STATED. INFERENCE: the denied case follows RFC 6749 section 4.1.2.1, so handle a callback that has `error` instead of `code`.

### 1.3 Step 4: callback to your server

| Item | Value | Label and source |
|---|---|---|
| Request | `GET <redirect_uri>?code=<code>&state=<state>` | `code` STATED (docs example `http://exampleurl.com/oauth?code=dsbf6h12sd8mdfwEzF`); `state` INFERENCE (sasl) |
| Code lifetime and single use | not given | NOT STATED. Treat as short lived and single use. |

### 1.4 Step 5: exchange the code for tokens

| Item | Value | Label and source |
|---|---|---|
| Method | POST | STATED |
| URL | `https://api.start.gg/oauth/access_token` | STATED (docs write `api.start.gg/oauth/access_token`); OSS (both clients use `https://`) |
| `Content-Type` | `application/json` | STATED: "must be sent with a `application/json` Content Type header". `application/x-www-form-urlencoded` is NOT STATED as accepted. Send JSON. |
| Client authentication | `client_id` and `client_secret` in the JSON body. HTTP Basic auth is NOT STATED. | STATED (body parameters) |
| Accept header | none documented | NOT STATED |

Request body (STATED parameter list; the JSON shape is shown by wizzrobe `_token_request` and sasl `AuthCodeTokenRequest`):

```json
{
  "grant_type": "authorization_code",
  "client_id": "<client id>",
  "client_secret": "<client secret>",
  "code": "<code from the callback>",
  "redirect_uri": "<the same redirect uri as in the authorize URL>",
  "scope": "user.identity tournament.reporter"
}
```

| Parameter | Required | Source |
|---|---|---|
| `grant_type` | yes, must be `authorization_code` | STATED |
| `client_id` | yes, same as before | STATED |
| `client_secret` | yes | STATED |
| `code` | yes | STATED |
| `redirect_uri` | yes, same as before | STATED |
| `scope` | listed in the docs as a parameter ("List of scopes separated by a space"). Whether it may be omitted is NOT STATED. wizzrobe and sasl always send it. | STATED (listed); omission NOT STATED |

Response (docs example; both clients parse exactly these four fields):

```json
{
  "access_token": "4eeFpL20Adsbf6h12sd8mdfwEzF",
  "token_type": "Bearer",
  "expires_in": 604800,
  "refresh_token": "d8md12s4eeAdsbf6fwEzF0hFpL2"
}
```

| Field | Type | Notes | Label |
|---|---|---|---|
| `access_token` | string | Send as `Authorization: Bearer <access_token>`. Restricted to the approved scopes. | STATED |
| `token_type` | string | `Bearer` | STATED |
| `expires_in` | integer seconds | The docs example is 604800 (7 days) and wizzrobe's docs say "Access tokens last 7 days". The field is authoritative; do not hardcode 7 days. | STATED (example), OSS |
| `refresh_token` | string | Present in the example. | STATED |
| `scope` in the response | not shown | NOT STATED. Do not depend on it; store the scopes you requested. |

Error shape (4xx from either token endpoint): **NOT STATED.** The docs show none. wizzrobe and sasl only test the HTTP status (`>= 400`) and log the body. INFERENCE: standard RFC 6749 `{"error": "invalid_grant", ...}` JSON, but this is unverified. Parse the body as optional JSON, never fail on a non-JSON body, and do not log it with the code or secret present. The exact status for a bad or reused code is NOT STATED.

### 1.5 Refresh

| Item | Value | Label and source |
|---|---|---|
| Method and URL | POST `https://api.start.gg/oauth/refresh` | STATED; OSS (wizzrobe `REFRESH_URL`). sasl diverges (section 0.1, item 2). |
| `Content-Type` | `application/json` | STATED |

Body (STATED parameter list):

```json
{
  "grant_type": "refresh_token",
  "refresh_token": "<refresh token>",
  "client_id": "<client id>",
  "client_secret": "<client secret>",
  "redirect_uri": "<same redirect uri as before>",
  "scope": "user.identity tournament.reporter"
}
```

The docs say `scope`, `client_id`, `client_secret` and `redirect_uri` are "the same ... as before". wizzrobe's comment agrees: "start.gg wants the original scope and redirect URI repeated on a refresh."

| Item | Label and source |
|---|---|
| Response shape | NOT STATED. INFERENCE: the same four fields as the exchange; wizzrobe parses both with one function. |
| Refresh token rotation (does the response carry a new `refresh_token` that invalidates the old one) | NOT STATED. wizzrobe's docs say "start.gg may rotate the refresh token" and serialize refresh under a per-community lock. INFERENCE: always store the `refresh_token` from each response and serialize refreshes per user. |
| Refresh token lifetime | NOT STATED |
| Recommended timing | wizzrobe refreshes one day before expiry; sasl refreshes within a 60 s buffer. Either works; refresh early and tolerate a failed refresh by sending the user back through authorize. |
| Revocation endpoint | NOT STATED |
| Personal access token alternative | Created at https://start.gg/admin/profile/developer, shown once, expires after 1 year, sent as `Authorization: Bearer <token>` (STATED, authentication page). Not an OAuth flow; usable for a server-side connector that has no signed-in organizer. |

### 1.6 Scopes

STATED, `docs/oauth/scopes.md`. The page also says new scopes are still being developed.

| Scope | Meaning (verbatim sense) |
|---|---|
| `user.identity` | Allows use of the `currentUser` query and all public fields for the user. Private fields need additional scopes. |
| `user.email` | Allows the `email` field in `currentUser`. `user.identity` is also required. |
| `tournament.manager` | Access to tournament seeding and bracket setup for tournaments the current user has access to. |
| `tournament.reporter` | Access to set reporting for tournaments the current user has access to. |

Scope separator in the request: space (docs), encoded `%20`.

Minimum scopes for Ember: sign-in only needs `user.identity`. Reporting results as the signed-in organizer needs `user.identity tournament.reporter` (wizzrobe requests exactly this pair for its staff sign-in; sasl requests all four). Do not request `user.email` unless you will use it.

### 1.7 Server-side implementation notes (recommendations, not start.gg facts)

- Generate `state` with a CSPRNG (32 bytes, URL-safe base64) and store it server side or in a short-lived HttpOnly, SameSite=Lax cookie (sasl uses a 5 minute cookie). Compare in constant time on the callback.
- Send the same `redirect_uri` string byte for byte in the authorize URL, the exchange and the refresh.
- Keep `client_secret` server side. The OAuth tokens are bearer secrets: store encrypted or hashed where possible, never log.
- Use one `reqwest::Client` with a request timeout of about 20 s (wizzrobe uses 20 s).

## 2. Reading the signed-in user

| Item | Value | Label and source |
|---|---|---|
| Method | POST | STATED (sending-requests: POST "highly recommended"; GET is also allowed) |
| URL | `https://api.start.gg/gql/alpha` | STATED (single endpoint) |
| `Authorization` | `Bearer <access_token>` | STATED (authentication); OSS (wizzrobe and sasl send the OAuth access token this way) |
| `Content-Type` | `application/json` | STATED |
| Body | `{"query": "...", "variables": {...}, "operationName": "..."}`; `variables` and `operationName` optional; `operationName` only needed with several operations | STATED |

Request body:

```json
{
  "query": "query CurrentUser { currentUser { id slug discriminator player { gamerTag } } }"
}
```

Schema facts (STATED, `query.doc`, `user.doc`, `player.doc`): `Query.currentUser: User` ("Returns the authenticated user"), nullable. `User.id: ID`, `User.slug: String`, `User.discriminator: String` ("Uniquely identifying token for user. Same as the hashed part of the slug"), `User.player: Player`, `Player.gamerTag: String`. All are nullable in the schema. `User.name` is "Public facing user name that respects user publishing settings". `User.email` needs `user.email`.

Response shape (ILLUSTRATIVE; field layout from the schema, the envelope from the docs examples, which also carry `extensions.queryComplexity` and `actionRecords`):

```json
{
  "data": {
    "currentUser": {
      "id": 1234567,
      "slug": "user/20718ad1",
      "discriminator": "20718ad1",
      "player": { "gamerTag": "Bbatts" }
    }
  },
  "extensions": { "queryComplexity": 4 },
  "actionRecords": []
}
```

| Item | Label and source |
|---|---|
| Slug format `user/<8 hex chars>` | STATED by example output (`entrants-by-tournament.md`: `user/20718ad1`) |
| `id` JSON type | NOT STATED for `User.id`. Other entity ids in the docs responses are JSON numbers (event 769488, participant 13985523, entrant 2401411). INFERENCE: number. wizzrobe wraps it in `opt_str()` to accept either. Use the string-or-number deserializer (section 6.3) and key your accounts on the string form, or better on `discriminator`, which is a string. |
| `player` may be null | schema nullable. sasl treats a null `player` as a failed lookup. A user with no player record or no gamer tag: handle `None`, fall back to `slug`. wizzrobe falls back to `slug`. |
| Behavior of `currentUser` with a token lacking `user.identity`, or with no token | NOT STATED. INFERENCE: `currentUser: null` or an `errors` entry. Check both. |
| Stable identity key | `User.id` and `User.discriminator` are the stable ones; `gamerTag` and `slug` can change (the docs warn that tags differ between Participant, Player and Entrant, `entrants-by-tournament.md` example 3). |

## 3. Reading a bracket

### 3.1 Terms (STATED, `docs/glossary.md`)

Tournament (a page with one or more Events) contains Event, which contains Phase, which contains Phase Group (aka Pool), which contains Set (aka Match), which contains Game. Entrant is a participant's representation in an Event; Participant (aka Attendee) is a person's representation in a Tournament; Player may not be linked to a User; User is the account. Seed attaches to an Entrant or Progression. Standing is placement within a phase group or event.

### 3.2 Finding the event

Slug format: `tournament/<tournament_name>/event/<event_name>`, for example `tournament/genesis-9-1/event/ultimate-singles` (STATED, `get-event.md`; the slug comes from the event page URL on start.gg). Tournament-only slugs also work for `tournament(slug:)`; the docs show both `genesis-x` and `tournament/20190128-oh-really-stream-queues`. Parsing a pasted URL: wizzrobe's `parse_event_slug` keeps the first four path segments and rejects a tournament link without `/event/` because one tournament can hold several events.

| Item | Value | Label and source |
|---|---|---|
| Query field | `event(id: ID, slug: String): Event`, "Returns an event given its id or slug" | STATED (query.doc) |
| Event id type in JSON | number (`"id": 769488`) | STATED by example (`get-event.md`) |
| Missing event | `data.event` is `null`. Check for null; a null event must not be read as "no entrants". | OSS (wizzrobe `get_entrants` raises on a null event); the null itself is INFERENCE |

Request and response (STATED, `get-event.md`):

```json
{
  "query": "query getEventId($slug: String) { event(slug: $slug) { id name } }",
  "variables": { "slug": "tournament/genesis-9-1/event/ultimate-singles" }
}
```

```json
{ "data": { "event": { "id": 769488, "name": "Ultimate Singles" } } }
```

Event shape query (OSS, wizzrobe `_EVENT_QUERY`, fields confirmed against the schema):

```graphql
query Event($slug: String) {
  event(slug: $slug) {
    id name slug state
    phases {
      id name state bracketType phaseOrder
      phaseGroups(query: {page: 1, perPage: 64}) {
        nodes { id displayIdentifier state bracketType }
      }
    }
  }
}
```

### 3.3 Event, phase, group and tournament state types (STATED, schema)

These are different types. Do not assume one.

| Field | GraphQL type | JSON | Source |
|---|---|---|---|
| `Event.state` | `ActivityState` enum | string | schema `event.doc`: "The state of the Event." |
| `Phase.state` | `ActivityState` | string | schema `phase.doc` |
| `PhaseGroup.state` | `Int` | number | schema `phasegroup.doc`; wizzrobe treats `3` as complete |
| `Tournament.state` | `Int` | number | schema `tournament.doc`: "can be ActivityState::CREATED, ActivityState::ACTIVE, or ActivityState::COMPLETED" |
| `Set.state` | `Int` | number | schema `set.doc` (no description) |
| `Game.state` | `Int` | number | schema `game.doc` |

`ActivityState` values (STATED, `activitystate.doc`, in declaration order): `CREATED` (created), `ACTIVE` (active or in progress), `COMPLETED` (done), `READY` (ready to be started), `INVALID`, `CALLED` ("Activity, like a set, has been called to start"), `QUEUED` (queued to run). Event `state` is therefore one of these seven strings. Which ones an event actually passes through is NOT STATED; the common ones are `CREATED`, `ACTIVE`, `COMPLETED`.

### 3.4 Set.state integers

| Int | Meaning | Source and label |
|---|---|---|
| 1 | Created / unstarted (not yet played) | STATED: legacy REST help ("1 is unstarted"); OSS: replay-manager `State.PENDING = 1` and its comment `{1: not started, 2: started, 3: completed}` |
| 2 | Started / active (in progress) | STATED: legacy REST help; docs example `set-score.md` (an in-progress set returns `"state": 2` with scores 1-1); OSS: `State.STARTED = 2` |
| 3 | Completed | STATED: legacy REST help; docs example `report-set.md` (reported set returns `"state": 3`); OSS: `State.COMPLETED = 3`; wizzrobe `_COMPLETED = 3` |
| 4 | READY | INFERENCE: position 4 in the `ActivityState` declaration order. No set example shows it. |
| 5 | INVALID | INFERENCE: position 5 in the declaration order. |
| 6 | Called (a set called to a station, waiting to start) | STATED: legacy REST help ("6 is called"); OSS: `State.CALLED = 6` |
| 7 | QUEUED | INFERENCE: position 7 in the declaration order. |

Reasoning check for the inferred rows: the four confirmed values 1, 2, 3 and 6 land exactly on `CREATED`, `ACTIVE`, `COMPLETED` and `CALLED` when `ActivityState` is numbered from 1 in declaration order, so 4, 5 and 7 are likely `READY`, `INVALID` and `QUEUED`. Still an inference; treat unknown integers as "other" and never crash on them.

Practical completeness test: wizzrobe counts a set complete when `state == 3` or `winnerId != null`. Docs example `report-set.md` shows that reporting a set returns sibling sets with `state` 1 and 3.

### 3.5 Paging sets: `Event.sets`

Schema (STATED, `event.doc`, `phasegroup.doc`, `phase.doc`, `entrant.doc`): `sets(page: Int, perPage: Int, sortType: SetSortType, filters: SetFilters): SetConnection` exists on Event, Phase, PhaseGroup, and as `paginatedSets` on Entrant. `SetConnection { pageInfo: PageInfo, nodes: [Set] }`. `PageInfo { total, totalPages, page, perPage, sortBy, filter }`, all nullable `Int` or `String` (`filter: JSON`).

| Item | Value | Label and source |
|---|---|---|
| First page | `page: 1` (1-based) | STATED by examples (`page: 1`); OSS (wizzrobe starts at 1) |
| `perPage` | "You can use a much larger `perPage` here, it is kept small in the example for demo purposes." No maximum is documented; the effective cap is the 1000 object complexity limit (section 5). | STATED (sets-in-event.md); max NOT STATED |
| Total pages | `pageInfo { totalPages }` | STATED (schema); OSS (wizzrobe loops until `page >= totalPages`) |
| Total count | `pageInfo { total }`. Example: `"pageInfo": {"total": 93}` | STATED (`sets-in-event.md`) |
| Sort | `SetSortType`: `NONE`, `CALL_ORDER` (suggested call order; the order of completed sets is reversed), `MAGIC` (relevancy by state and progress), `RECENT` (order started; sorts first by `completedAt` since 2022-08-03), `STANDARD` (deprecated, same as `CALL_ORDER`), `ROUND` (by round and identifier) | STATED (`setsorttype.doc`, changelog). Use `ROUND` or `NONE`. Whether paging is stable while sets change underneath is NOT STATED (INFERENCE: re-read the whole list, or key sets by id and re-fetch changed ones with `updatedAfter`). |

`SetFilters` (STATED, `setfilters.doc`, exact input type):

| Field | Type | Description from the schema |
|---|---|---|
| `entrantIds` | `[ID]` | Only Sets for these Entrants |
| `entrantSize` | `[Int]` | Only Sets for this Entrant size (1 for 1v1). Was broken and returned nothing until the 2026-10-14 fix. |
| `hasVod` | `Boolean` | Only Sets with an attached VOD |
| `hideEmpty` | `Boolean` | "Do not return empty Sets. For example, set this to true to filter out sets that are waiting for progressions." |
| `showByes` | `Boolean` | "Return sets that contain a bye" |
| `isEventOnline` | `Boolean` | Online events only; if omitted both are returned |
| `location` | `SetFilterLocation` | Geographic area |
| `participantIds` | `[ID]` | Only Sets for these Participants |
| `phaseGroupIds` | `[ID]` | Only Sets in these PhaseGroups |
| `phaseIds` | `[ID]` | Only Sets in these Phases |
| `eventIds` | `[ID]` | Only Sets in these Events |
| `tournamentIds` | `[ID]` | Only Sets in these Tournaments |
| `playerIds` | `[ID]` | Only Sets for these Players |
| `roundNumber` | `Int` | Only Sets for this round |
| `state` | `[Int]` | "Only returns Sets that are in these states" (use the integers of section 3.4) |
| `stationIds` | `[ID]` | Assigned to these Station IDs |
| `stationNumbers` | `[Int]` | Assigned to these Station numbers (example in `sets-by-station.md`) |
| `updatedAfter` | `Timestamp` | "Only return sets created or updated since this timestamp" |

`Timestamp` is a custom scalar (STATED, `timestamp.doc`, no description). Unix seconds is INFERENCE from the changelog wording "unix timestamp" on other Timestamp fields and from wizzrobe (`datetime.fromtimestamp(int(value))` for `completedAt`). Send an integer number of seconds.

Defaults when `filters` is omitted: the docs say "To get all sets, don't pass any filters" (sets-in-event.md). Whether byes and empty sets are included by default is NOT STATED; the existence of `showByes` and `hideEmpty` suggests byes are excluded by default and empty sets are included (INFERENCE).

Combined query for a bracket connector (assembled from the schema; every field below exists in the types read; not run against the live API). Per the complexity cap, start `perPage` low (wizzrobe uses 40 with slots, standings and no participants) and tune using `extensions.queryComplexity`.

```graphql
query EventSets($eventId: ID!, $page: Int!, $perPage: Int!, $filters: SetFilters) {
  event(id: $eventId) {
    id
    sets(page: $page, perPage: $perPage, sortType: ROUND, filters: $filters) {
      pageInfo { total totalPages page perPage }
      nodes {
        id state round fullRoundText identifier
        totalGames setGamesType hasPlaceholder
        winnerId completedAt startedAt updatedAt
        displayScore
        phaseGroup { id displayIdentifier }
        slots(includeByes: true) {
          id slotIndex prereqType prereqId prereqPlacement
          entrant { id name }
          standing { placement stats { score { value } } }
        }
      }
    }
  }
}
```

Variables:

```json
{ "eventId": "769488", "page": 1, "perPage": 40,
  "filters": { "hideEmpty": true, "updatedAfter": 1760000000 } }
```

Identity data per entrant is cheaper to fetch once, separately, because it multiplies the object count when nested in every set. wizzrobe reads it from `event.entrants` (STATED fields, OSS query):

```graphql
query Entrants($id: ID!, $page: Int!, $perPage: Int!) {
  event(id: $id) {
    entrants(query: {page: $page, perPage: $perPage}) {
      pageInfo { totalPages }
      nodes {
        id name
        participants {
          id gamerTag prefix
          user { id slug discriminator
                 authorizations(types: [DISCORD]) { externalId externalUsername type } }
        }
      }
    }
  }
}
```

`event.entrants(query: EventEntrantPageQuery)` takes `page` and `perPage` (docs `event-entrants.md`). wizzrobe uses `perPage` 50. Nesting the same `participants { user { authorizations } }` selection under `slots { entrant }` also works (it is the same type) and is what section 3.7 describes.

### 3.6 Set fields

All STATED in `set.doc` unless marked.

| Field | Type | Notes |
|---|---|---|
| `id` | `ID` | Real sets: an integer, serialized as a JSON string or number depending on the resolver (section 0.1 item 4, section 6). Preview sets: a string starting with `preview` (section 6.1). |
| `state` | `Int` | Section 3.4. |
| `round` | `Int` | "The round number of the set. Negative numbers are losers bracket." |
| `fullRoundText` | `String` | "Full round text of this set." Examples of values (not in the docs): `Winners Round 1`, `Grand Final`; replay-manager special-cases the text `Grand Final Reset`. The exact vocabulary is NOT STATED. |
| `identifier` | `String` | "The letters that describe a unique identifier within the pool. Eg. F, AT" |
| `winnerId` | `Int` | Entrant id of the winner, or null. Typed `Int`, not `ID`. |
| `completedAt` | `Timestamp` | "The time this set was marked as completed". Unix seconds (INFERENCE as above). |
| `startedAt`, `startAt`, `createdAt` | `Timestamp` | `startAt` falls back to the phase group round configuration. |
| `updatedAt` | `Timestamp` | "The time this set was last updated". Added 2025-06-20 (changelog). |
| `totalGames` | `Int` | "If setGamesType is in total games mode, this defined the number of games in the set." Exists. |
| `setGamesType` | `Int` | "Indicates whether the set is in best of or total games mode. This instructs which field is used to figure out how many games are in this set." The integer meaning (which value is best-of) is NOT STATED. The best-of count comes from `PhaseGroup.rounds { number bestOf }` (`Round.bestOf`, "the number of games one must win a majority out of to win a set in this round"). |
| `displayScore(mainEntrantId: ID)` | `String` | Human readable score. Example from `sets-by-player.md`: `"C9 \| Mang0 3 - The Moon 2"`. The format for DQ, byes or in-progress sets is NOT STATED. Do not parse it; use `slots.standing.stats.score.value`. |
| `slots(includeByes: Boolean)` | `[SetSlot]` | See below. |
| `games` / `game(orderNum: Int!)` | `[Game]` / `Game` | Per-game data when reported (`set-game-data.md`): `id`, `orderNum`, `winnerId` (Int), `entrant1Score`, `entrant2Score` (Int), `stage { id name }`, `selections { id character { id name } }`. |
| `hasPlaceholder` | `Boolean` | "Whether this set contains a placeholder entrant." |
| `entrant1Source`, `entrant2Source` | `SetEntrantSource` | `type`, `typeId`, `condition`, `conditionString` (for example "Winner of Set #1234" or "Seed #1"). Added 2025-06-20. |
| `winnerProgressionSeed`, `loserProgressionSeed` | `Seed` | Where winner and loser go. |
| `phaseGroup`, `event`, `station`, `stream`, `vodUrl` | objects and string | `station { id number }` per `sets-by-station.md`. |
| `wPlacement`, `lPlacement` | `Int` | Placement of winner and loser. |
| `resetAffectedData` | object | "The sets that are affected from resetting this set": `affectedSetCount`, `affectedSets`, `affectedPhaseGroupCount`. Useful before `resetSet`. |

`SetSlot` (STATED, `setslot.doc`): `id: ID` (example `"7794393-0"`, that is `<setId>-<slotIndex>`), `slotIndex: Int` ("The index of the slot. Unique per set."), `entrant: Entrant`, `seed: Seed`, `standing: Standing`, `prereqId: String` ("Pairs with prereqType, is the ID of the prereq"), `prereqPlacement: Int` ("the placement required in the origin set to end up in this slot"), `prereqType: String` ("Describes where the entity in this slot comes from").

Slot score: `standing.stats.score.value` (`Score { label, value: Float, displayValue }`), shown by `set-score.md` where a completed 2-0 set gave `"placement": 1, "score": {"label": "Score", "value": 2}` and `"placement": 2, "value": 0`. For fighting games the value is games won at set level (`set-score.md`) and stocks remaining at game level (`Game.entrant1Score`). The `StandingStats` type is documented as "experimental and very likely to change".

`prereqType` values: wizzrobe reads `'set'` (and uses `prereqPlacement == 2` to mean loser, otherwise winner) to label an empty slot "Winner of X" or "Loser of X" where X is the identifier of the prereq set. `'seed'` is INFERENCE (the `SetEntrantSource` example says "Seed #1"). The full list of values is NOT STATED.

Entrant and participant fields used (STATED): `Entrant { id, name, participants, initialSeedNum, isDisqualified, seeds, standing, team }`; `Entrant.name` is "the entrant name as it appears in bracket: gamerTag of the participant or team name" and can include a sponsor prefix. `Participant { id, gamerTag, prefix, user, player, connectedAccounts: JSON, requiredConnections, checkedIn, email, contactInfo }`. `Participant.user` is nullable in the schema, and wizzrobe handles a missing user (`user = first.get('user') or None`). That this happens for players an organizer added without a start.gg account is INFERENCE. Team events have several participants per entrant (`set-entrants.md`).

### 3.7 Identity fields for mapping players (Discord)

Schema (STATED, `user.doc`, `profileauthorization.doc`, `socialconnectiontype.doc`):

| Item | Value |
|---|---|
| Field | `User.authorizations(types: [SocialConnectionType]): [ProfileAuthorization]` |
| `SocialConnectionType` values | `TWITTER`, `TWITCH`, `DISCORD`, `XBOX`, `MIXER` |
| `ProfileAuthorization` fields | `id: ID`, `externalId: String` ("The id given by the external service"), `externalUsername: String` ("The username given by the external service (including discriminator if discord)"), `type: AuthorizationType`, `url: String`, `stream` |
| `AuthorizationType` values (the type of `ProfileAuthorization.type`, a different enum) | `TWITTER`, `TWITCH`, `STEAM`, `DISCORD`, `XBOX`, `EPIC`, `BATTLENET`, `RIOT`, `MIXER` |

So `authorizations(types: [DISCORD])` is valid and `type` in the response is the `AuthorizationType` string `"DISCORD"`. `externalId` is the Discord account id as a string. OSS: wizzrobe uses `authorizations(types: [DISCORD]) { externalId }` and treats `externalId` as the Discord id; sasl reads `externalId`, `type` and `externalUsername` from `currentUser.authorizations`.

Privacy and availability caveats:

- The 2020-03-05 changelog removed `ProfileAuthorization.externalId` as a "private field", yet the current schema lists it. Whether `externalId` is returned for any caller, only for admins of the tournament, or only for the user themselves is NOT STATED. wizzrobe's design (sign in as a tournament admin, fall back to a personal token) suggests it is returned at least to admins; INFERENCE only.
- Users can hide connections. If `authorizations` comes back empty, the player simply has no visible Discord link; fall back to a manual mapping.
- `Participant.connectedAccounts` (JSON) and `requiredConnections` (admin-only) are alternative routes. `Participant.email` is admin only and limited to 18 months after the tournament.

Example JSON response shape for a set slot (ILLUSTRATIVE):

```json
{
  "id": "7794393-0",
  "slotIndex": 0,
  "prereqType": "set", "prereqId": "7794300", "prereqPlacement": 1,
  "entrant": {
    "id": 2401411, "name": "Fumbles",
    "participants": [{
      "id": 13985523, "gamerTag": "Fumbles", "prefix": "",
      "user": { "id": 1234567, "slug": "user/20718ad1", "discriminator": "20718ad1",
                "authorizations": [{ "externalId": "123456789012345678",
                                     "externalUsername": "fumbles", "type": "DISCORD" }] }
    }]
  },
  "standing": { "placement": 1, "stats": { "score": { "value": 2 } } }
}
```

### 3.8 Slot, bye and empty-set behavior

| Item | Label and source |
|---|---|
| `slots(includeByes: Boolean)`: byes are only returned when you ask | STATED (`set.doc` argument) |
| `SetFilters.showByes`: "Return sets that contain a bye" | STATED. Added 2020-05-11 (changelog). |
| `Seed.isBye: Boolean` | STATED (`seed.doc`) |
| `SetFilters.hideEmpty`: drops sets that are waiting for progressions | STATED |
| What a bye slot looks like in GraphQL (null `entrant`? an entrant with a placeholder name? a `prereqType` of `bye`?) | NOT STATED. Legacy REST states "If either of these is null, this is a bye set" about `entrant1Id` and `entrant2Id` (legacy REST help, not GraphQL). replay-manager skips sets whose REST `entrant1PrereqType` or `entrant2PrereqType` is `'bye'`. INFERENCE for GraphQL: a bye slot has `entrant: null` and likely `prereqType: "bye"`; verify with a live query using `includeByes: true`. |
| Whether a set containing a bye ever reaches `state` 3 | NOT STATED. |
| An undecided slot (waiting on another set) | `entrant: null` with `prereqType`, `prereqId`, `prereqPlacement` filled (OSS: wizzrobe builds "Winner of X" labels from these). `entrant` can also be `null` for byes, so use `prereqType` to tell them apart (INFERENCE). |

## 4. Mutations

All STATED in `mutation.doc`, `bracketsetgamedatainput.doc`, `bracketsetgameselectioninput.doc`, `report-set.md`. They are POSTed to the same endpoint (section 2) with the `Authorization: Bearer` header.

| Mutation | Signature | Returns | Schema description |
|---|---|---|---|
| `markSetCalled` | `(setId: ID!)` | `Set` | "Update a set to called state" |
| `markSetInProgress` | `(setId: ID!)` | `Set` | "Update a set to called state" (copy paste bug in the schema, section 0.1 item 3) |
| `reportBracketSet` | `(setId: ID!, winnerId: ID, isDQ: Boolean, gameData: [BracketSetGameDataInput])` | `[Set]` | "Report set winner or game stats for a H2H bracket set. If winnerId is supplied, mark set as complete. gameData parameter will overwrite any existing reported game data." |
| `updateBracketSet` | `(setId: ID!, winnerId: ID, isDQ: Boolean, gameData: [BracketSetGameDataInput])` | `Set` | "Update game stats for a H2H bracket set. Set winner cannot be changed with this function, use the resetSet mutation instead." |
| `resetSet` | `(setId: ID!, resetDependentSets: Boolean)` | `Set` | "Resets set to initial state, can affect other sets and phase groups" |
| `updateVodUrl` | `(setId: ID!, vodUrl: String)` | `Set` | (added 2025-06-02) |
| `assignStation`, `assignStream` | `(setId: ID!, stationId: ID!)`, `(setId: ID!, streamId: ID!)` | `Set` | Station or stream assignment (added 2025-06-06) |

History (STATED, changelog): `reportBracketSet`, `markSetCalled`, `markSetInProgress`, `resetSet` and `BracketSetGameDataInput` were added 2023-09-28; `isDQ` was added to `reportBracketSet` and `updateBracketSet` was added on 2024-02-02.

### 4.1 Input types

`BracketSetGameDataInput` (STATED):

| Field | Type | Notes |
|---|---|---|
| `gameNum` | `Int!` | Game number (replay-manager and the docs example start at 1) |
| `winnerId` | `ID` | Entrant id of the game winner |
| `entrant1Score` | `Int` | "Score for entrant 1 (if applicable). For smash, this is stocks remaining." |
| `entrant2Score` | `Int` | Same for entrant 2 |
| `stageId` | `ID` | Stage played, if applicable |
| `selections` | `[BracketSetGameSelectionInput]` | Typically character selections |

`BracketSetGameSelectionInput` (STATED): `entrantId: ID!`, `characterId: Int`.

Which entrant is "entrant 1": NOT STATED in the mutation docs. INFERENCE: the entrant in slot 0 (`slotIndex` 0) of the set. The docs example shows `entrant1Score: 0, entrant2Score: 2` with `winnerId` 14259653 (the entrant that has the 2), which fits entrant 2 being the winner of that game. Confirm the orientation with a live read of `games { entrant1Score entrant2Score }` before writing scores.

### 4.2 Request and response bodies

Winner only (STATED, `report-set.md` example 1; the `$setId`/`$winnerId` variable types are as in the docs):

```json
{
  "query": "mutation reportSet($setId: ID!, $winnerId: ID!) { reportBracketSet(setId: $setId, winnerId: $winnerId) { id state } }",
  "variables": { "setId": "65089253", "winnerId": "14259653" }
}
```

Response (STATED, `report-set.md`; note it is an array and holds the reported set and its dependents):

```json
{
  "data": {
    "reportBracketSet": [
      { "id": 65089254, "state": 1 },
      { "id": 65089260, "state": 3 },
      { "id": 65089261, "state": 3 },
      { "id": 65089262, "state": 1 },
      { "id": 65089253, "state": 3 }
    ]
  }
}
```

The reported set is not guaranteed to be first or last, so find it by id, not by position. The docs response shows ids as JSON numbers while the request variables were numbers; send strings and accept both.

Full game data (STATED, `report-set.md` example 2):

```json
{
  "query": "mutation reportSet($setId: ID!, $winnerId: ID!, $gameData: [BracketSetGameDataInput]) { reportBracketSet(setId: $setId, winnerId: $winnerId, gameData: $gameData) { id state } }",
  "variables": {
    "setId": 65089253,
    "winnerId": 14259653,
    "gameData": [
      { "winnerId": 14259653, "gameNum": 1, "entrant1Score": 0, "entrant2Score": 2, "stageId": 3,
        "selections": [ { "entrantId": 14259653, "characterId": 3 },
                        { "entrantId": 14250090, "characterId": 1 } ] },
      { "winnerId": 14259653, "gameNum": 2, "entrant1Score": 0, "entrant2Score": 3,
        "selections": [ { "entrantId": 14259653, "characterId": 3 },
                        { "entrantId": 14250090, "characterId": 1 } ] }
    ]
  }
}
```

Disqualification and update (field names STATED; semantics INFERENCE):

```json
{
  "query": "mutation($setId: ID!, $winnerId: ID, $isDQ: Boolean) { reportBracketSet(setId: $setId, winnerId: $winnerId, isDQ: $isDQ) { id state } }",
  "variables": { "setId": "65089253", "winnerId": "14259653", "isDQ": true }
}
```

What `isDQ` does is NOT STATED beyond the changelog line "Argument isDQ was added to mutation reportBracketSet". INFERENCE: with `winnerId` set to the advancing entrant, `isDQ: true` marks the other entrant as disqualified instead of recording a played result. Verify on a test event.

`markSetCalled`, `markSetInProgress` and `resetSet` (shapes from the schema, pattern shown by replay-manager; response fields shown are an example selection, ILLUSTRATIVE):

```json
{ "query": "mutation($setId: ID!) { markSetCalled(setId: $setId) { id state } }",
  "variables": { "setId": "65089253" } }
```

```json
{ "query": "mutation($setId: ID!) { markSetInProgress(setId: $setId) { id state } }",
  "variables": { "setId": "65089253" } }
```

```json
{ "query": "mutation($setId: ID!, $resetDependentSets: Boolean) { resetSet(setId: $setId, resetDependentSets: $resetDependentSets) { id state } }",
  "variables": { "setId": "65089253", "resetDependentSets": false } }
```

Expected `state` after each (INFERENCE from section 3.4): `markSetCalled` gives 6, `markSetInProgress` gives 2, `resetSet` gives 1. `resetSet` "can affect other sets and phase groups"; check `resetAffectedData { affectedSetCount }` first and treat the default of `resetDependentSets` (NOT STATED) as unsafe by passing it explicitly.

### 4.3 Who may call them

| Item | Label and source |
|---|---|
| Scope `tournament.reporter`: "access to set reporting for tournaments the current user has access to" | STATED (scopes) |
| Scope `tournament.manager`: "tournament seeding and bracket setup for tournaments the current user has access to" | STATED (scopes) |
| Which scope each mutation needs | NOT STATED. INFERENCE: `reportBracketSet`, `updateBracketSet`, `markSetCalled` and `markSetInProgress` are set reporting (`tournament.reporter`). `resetSet`, `assignStation`, `assignStream` are not covered by either description; do not assume `tournament.reporter` is enough for `resetSet`. |
| The signed-in user must hold an admin role on the tournament | INFERENCE, from wizzrobe's docs: "Reporting needs the signed-in account to hold an admin role, Reporter or higher, on the start.gg tournament (its Settings, Admin Permissions)." wizzrobe determines this by checking `currentUser.tournaments(query: {filter: {tournamentView: "admin"}})` or `tournament(slug) { owner { id } admins { id } }`. The docs scopes page says "the current user has access to", consistent with this. |
| The token used by replay-manager | A personal access token (`Authorization: Bearer <PAT>`), which acts as the owning user with that user's own roles. |
| Error text when the caller lacks the role or scope | NOT STATED |

### 4.4 Errors

Mutation errors arrive as a standard GraphQL `errors` array in a 200 response (replay-manager reads `json.errors[0].message` after a successful HTTP status; whether the HTTP status is ever 4xx for these is NOT STATED). Known message texts (OSS, replay-manager `startgg.ts`):

| Message | When |
|---|---|
| `Set is already called` | `markSetCalled` on a set that is already called |
| `Set is already started` | `markSetInProgress` on a set that is already started |
| `Set not found for id: preview...` | any call with a preview set id (section 6.1); replay-manager adds "Refresh the pool and try again." |
| `An unknown error has occurred` | generic server failure; replay-manager treats it as retryable after a refresh |

Reporting a set that is already completed: **NOT STATED.** No message text is documented or seen in the clients. Design as follows: re-read the set (`set(id:) { id state winnerId }`, as wizzrobe does before writing) and only report if `state != 3` and `winnerId` is null. Use `updateBracketSet` only for game data; it cannot change the winner. To change a completed result, call `resetSet` and then `reportBracketSet` again.

## 5. Limits and failure shapes

| Item | Value | Label and source |
|---|---|---|
| Request rate | "You may not average more than 80 requests per 60 seconds." | STATED (rate-limits) |
| Objects per request | "a maximum of 1000 objects per request (this includes nested objects)" | STATED |
| Excess | "your request will be rejected" | STATED |
| Per what the rate is counted (token, IP, app) | The error text says `api-token`. Per token is INFERENCE; whether OAuth tokens from one app share a bucket is NOT STATED. | STATED (message), INFERENCE (meaning) |
| Complexity counter in responses | `"extensions": {"queryComplexity": 13}` appears in the docs responses. The exact counting rule is NOT STATED. The complexity error reports the actual number. | STATED (examples) |
| 2022-08-04 fix | Complexity errors used to return "Unknown error"; now they state the limit and the exact count | STATED (changelog) |

Rate limit exceeded body (STATED, `rate-limits.md`). This is a flat JSON object, not a GraphQL `errors` array:

```json
{ "success": false, "message": "Rate limit exceeded - api-token" }
```

Complexity exceeded body (STATED):

```json
{
  "success": false,
  "fields": null,
  "message": "Query complexity too high. A maximum of 1000 objects may be returned by each request. (actual: 1235)"
}
```

| Item | Label and source |
|---|---|
| HTTP status of the rate limit response | NOT STATED in the docs. wizzrobe retries once after 5 s on HTTP 429, so 429 is its assumption, not a documentation fact. Handle the body shape (`success == false`) as well as the status. |
| HTTP status of the complexity response | NOT STATED. |
| `Retry-After` or `X-RateLimit-*` headers | NOT STATED. |
| Other statuses handled by the clients | 400 and 401 (replay-manager reads them as invalid or expired key), 401 and 403 (wizzrobe: refused sign-in), 500, 502, 503, 504 (replay-manager retries once after 1 s). These are client behavior, not documented API contracts. |
| GraphQL errors | `{"errors": [{"message": "..."}], "data": ...}`; a request can return both partial `data` and `errors`. |

Client rules to implement (recommendations):

1. Parse every response as one of three shapes: normal GraphQL (`data`, optional `errors`, optional `extensions`), the flat `{"success": false, "message": ...}` shape (rate limit and complexity), and non-JSON.
2. Throttle in the client to stay under 80 requests per 60 s, with margin (for example 60 per minute for a background sync), shared across everything that uses one token. A bracket of 128 players with 40 sets per page needs only a handful of requests; the costly pattern is one request per set.
3. Keep `perPage` such that the worst case stays under 1000 objects. Rough budget: each set node costs its own object plus its slots, entrants, standings, score and participant objects, which is easily 10 to 20 objects per set once participant and user data are nested. Fetch identity data in the separate entrants query.
4. On a rate limit, sleep to the end of the rolling window and retry once; surface the failure instead of looping.
5. Poll with `updatedAfter` (section 3.5) rather than refetching the whole event; wizzrobe resyncs every 2 minutes and reads whole pools, which is also within budget.

## 6. Ids, preview sets and byes

### 6.1 Preview sets

| Item | Label and source |
|---|---|
| Sets of a phase that has not started carry ids that start with `preview` (wizzrobe writes `preview_...`), and start.gg replaces them when the phase starts | INFERENCE from two clients. wizzrobe `_is_real_set()` is `not set_id.startswith('preview')` and its model docstring says "Sets of a phase that hasn't started carry start.gg's `preview_...` ids; they display but can't be booked, since start.gg replaces them when the phase starts". replay-manager errors with `Set not found for id: preview...`. The official docs and schema do not mention preview ids. |
| Exact format of a preview id (what follows `preview_`) | NOT STATED |
| JSON type | string (it is not an integer) | INFERENCE: replay-manager treats a non-number id as a preview set (`typeof set.id === 'number'`) |
| Mutating a preview set | A call that targets a preview id can fail with `Set not found for id: preview...` (OSS, replay-manager matches that message in its generic request wrapper, so it is not tied to one mutation). `reportBracketSet` on a preview id: replay-manager contains code that handles a reported preview set coming back under a real numeric id by matching on entrants and round, so the call does return a real set. Treat as INFERENCE. |
| How to treat them | Display only. Never persist a preview id as a durable key, never write against it, and drop or re-key it on the next sync after the phase starts. Match on (phase group id, round, identifier) or on both entrant ids when re-keying. |
| Slot ids of preview sets | NOT STATED (INFERENCE: `<previewId>-<slotIndex>`). Placeholder standings use ids like `placeholder_standing_seeds_5093733_sets_7794393` (STATED by example, `set-score.md`). |

### 6.2 Byes

See section 3.8. In short: NOT STATED for GraphQL; the schema gives `slots(includeByes:)`, `SetFilters.showByes`, `Seed.isBye` and `Set.hasPlaceholder`; the legacy REST API says a null entrant id marks a bye. A connector should skip any set that has a bye slot when it counts played sets, and never try to report one.

### 6.3 Id types for Rust

| Field | Schema type | JSON seen in docs | Advice |
|---|---|---|---|
| `Event.id`, `Phase.id`, `PhaseGroup.id`, `Tournament.id` | `ID` | number | Accept either, store as string |
| `Entrant.id`, `Participant.id`, `Player.id`, `User.id`, `Seed.id` | `ID` | number | Accept either, store as string |
| `Set.id` | `ID` | number or string (real); string (preview) | Accept either, store as string |
| `SetSlot.id` | `ID` | string `"<setId>-<index>"` | string |
| `Standing.id` | `ID` | number or string such as `placeholder_standing_...` | string |
| `Set.winnerId`, `Game.winnerId` | `Int` | number | `Option<i64>`, convert to string to compare with entrant ids |
| `Round.number`, `Set.round`, `Set.state`, `PageInfo.*` | `Int` | number | `i64` or `i32` |
| Timestamps (`completedAt`, `updatedAt`, `startAt`) | `Timestamp` | number (unix seconds, INFERENCE) | `Option<i64>` |
| Mutation `setId`, `winnerId`, `stageId` variables | `ID` | the docs pass both numbers and strings | send strings |
| `characterId` | `Int` | number | `i64` |

A serde helper (a sketch, not taken from a source):

```rust
#[derive(Debug, Clone, serde::Deserialize)]
#[serde(untagged)]
enum WireId { Num(i64), Str(String) }

impl WireId {
    fn into_string(self) -> String {
        match self { WireId::Num(n) => n.to_string(), WireId::Str(s) => s }
    }
}
```

Use `Option<WireId>` on every nullable id field and `#[serde(default)]` for fields a query might omit.

## 7. Minimal request and response checklist

| Need | Method and URL | Headers | Body |
|---|---|---|---|
| Send user to sign in | GET `https://start.gg/oauth/authorize?response_type=code&client_id=..&scope=user.identity%20tournament.reporter&redirect_uri=..&state=..` | none | none |
| Exchange code | POST `https://api.start.gg/oauth/access_token` | `Content-Type: application/json` | section 1.4 |
| Refresh | POST `https://api.start.gg/oauth/refresh` | `Content-Type: application/json` | section 1.5 |
| Who is signed in | POST `https://api.start.gg/gql/alpha` | `Authorization: Bearer <access_token>`, `Content-Type: application/json` | section 2 |
| Resolve event | same | same | section 3.2 |
| Read sets, entrants | same | same | section 3.5 |
| Report, call, start, reset | same | same | section 4.2 |

## 8. Unknowns and things to verify with a live token

1. The OAuth error bodies and status codes at `/oauth/access_token` and `/oauth/refresh`, and the consent-denied redirect shape. NOT STATED.
2. Whether `state` is echoed (INFERENCE from sasl, which ships), whether PKCE is accepted (NOT STATED, assume no), and whether form-encoded token requests are accepted (NOT STATED, send JSON).
3. Refresh response shape, whether the refresh token rotates, and its lifetime. NOT STATED (INFERENCE: store whatever the response returns).
4. Exact `redirect_uri` matching rules. NOT STATED.
5. The JSON type of `currentUser.id` (INFERENCE: number) and what `currentUser` returns when the token lacks `user.identity`.
6. The full `Set.state` map. 1, 2, 3 and 6 are confirmed; 4, 5 and 7 are INFERENCE from the `ActivityState` order.
7. The preview set id format, and what exactly happens when `reportBracketSet` targets one. INFERENCE only, from two clients.
8. How byes appear in GraphQL (null `entrant`, `prereqType` value, `state`, whether a bye set is returned by default). NOT STATED.
9. The error text, if any, when reporting an already completed set, a set with no entrants, or when the caller lacks the admin role. NOT STATED. Only the three messages in section 4.4 are known.
10. Which scope each mutation requires, in particular `resetSet`. NOT STATED.
11. The meaning of `isDQ`, and the orientation of `entrant1Score` and `entrant2Score` in `BracketSetGameDataInput`. INFERENCE only.
12. `setGamesType` values (which integer is best-of) and `fullRoundText` vocabulary. NOT STATED.
13. Whether `ProfileAuthorization.externalId` (the Discord id) is visible to every caller or only to admins and the user. NOT STATED; the 2020 changelog removed it as private and the current schema lists it.
14. The HTTP status and headers of the rate-limit and complexity responses (the clients assume 429), and whether OAuth tokens share a bucket per application or per user. NOT STATED.
15. How the 1000 object count is computed, and the maximum `perPage`. NOT STATED. Measure with `extensions.queryComplexity`. ember-brackets asks a page of sets for `phaseGroup { phase { id name } }` only when a round rule names a phase, and for `stream { id streamName }` only when sets on a stream are held, and never in the single-set read, so a tournament that uses neither pays for neither; the count with both (about 3 more objects a set at `perPage` 40) is unmeasured.
16. Default behavior of `SetFilters` when `hideEmpty` and `showByes` are omitted, and whether `updatedAfter` is unix seconds. INFERENCE.
17. Paging stability while sets change, and `Timestamp` units. INFERENCE (unix seconds).
18. `Set.stream`: that it is filled (`stream { id streamName }`) for a set the organizer assigned to a stream (`assignStream` or the stream queue page), and null otherwise. The schema lists the field; its contents for an assigned set are NOT STATED. ember-brackets holds such a set when a tournament's `hold.streamed` is on, so this needs one live check.
19. `Set.phaseGroup { phase { id name } }`: that `phase.name` is the name the event's bracket page shows (such as "Pools" or "Top 8"). The schema lists `Set.phaseGroup` (section 3.6) and the phase fields `id` and `name` (the `phases { id name }` of section 3.2); ember-brackets' round rules match on that name or the ID. INFERENCE until checked live.
20. `Set.games { entrant1Score entrant2Score stage { id } selections { id } }` on a set with organizer-entered game data and no winner: which game detail fields start.gg fills by default when only a winner was entered (a score of 0 or null, a `stage` object with a null `id`, an empty `selections`, or something else). ember-brackets leaves the set to the organizer when a game has a score above 0, a stage with an `id`, or a selection with an `id`, and reads 0 and null scores, a stage or selection without an `id`, and an empty list as empty. INFERENCE: if start.gg fills any other default, every set the organizer touched is left to them.
