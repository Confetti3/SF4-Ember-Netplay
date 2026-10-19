# Challonge API v2.1 reference for a Rust client

Source: the official docs at https://challonge.apidog.io, read on 2026-10-18 through the `.md` page variants and the `llms.txt` index. Every statement below comes from those pages unless it is under a heading or in a row marked **NOT STATED** or **INFERENCE**. Where the docs contradict themselves, both versions are given.

Method note: the pages were read through a summarizing fetch tool, so JSON examples that the docs do not publish (many of them) are labeled "derived from schema defaults". No live API call was made, so nothing here is confirmed against real server behavior.

What Ember uses: the bridge signs players in with the authorization-code flow (section 2.4) and reads `/me` (section 3); ember-brackets reads and reports tournaments, participants and matches (sections 4 to 6) with the organizer's key. The device grant (section 2.3) was the first plan and is not used; it is kept below as reference only.

## 1. Conventions

| Item | Value |
|---|---|
| Base URL | `https://api.challonge.com/v2.1` |
| Developer portal (app registration, client id, usage) | `https://connect.challonge.com` |
| Format | JSON:API style request bodies; responses are `application/json` |
| Request headers on every API call | `Content-Type: application/vnd.api+json`, `Accept: application/json` |
| OAuth auth headers | `Authorization-Type: v2` and `Authorization: Bearer <access_token>` |
| API v1 key auth headers | `Authorization-Type: v1` and `Authorization: <api key>` (key from https://challonge.com/settings/developer) |
| `If-None-Match` | Optional on GET endpoints, "caching key for future responses". The docs do not mention ETag or 304 behavior. |
| Path suffix | Most documented v2.1 paths end in `.json` (for example `/me.json`, `/tournaments/{id}.json`). The Game Integration page writes some of the same routes without the suffix (`/v2.1/me`). **NOT STATED** whether the suffix is optional. Use the `.json` form from the endpoint pages. |
| Attribute naming | snake_case everywhere in v2.1 (Upgrading page) |
| Tournament ids | Integer ids again in v2.1; the tournament URL slug is still accepted as `{tournament_id}`. Docs recommend the id. |
| Header default quirk | The endpoint pages list `Authorization-Type` as required with default `v1`. Set it to `v2` for OAuth. |

### 1.1 Id types (important for Rust types)

The docs mix representations. Model them leniently.

| Field | Documented type | Notes |
|---|---|---|
| `data.id` (every resource) | string | Example `"30201"`, `"76"`, `"8008135"`, `"321"` |
| `participant.attributes.tournament_id` | integer | Example `21` |
| `participant.attributes.group_id` | integer | Placeholder text in the schema, so nullable in practice (**INFERENCE**) |
| `match.attributes.winner_id` | integer | Example `355` |
| `match.relationships.player1.data.id` | string | Example `"355"` |
| `points_by_participant[].participant_id` | integer | Example `355` |
| Update Match request `participant_id` | string | Example `"76"` |

**INFERENCE for implementers:** the same participant id appears as a string in `data.id` and `relationships.*.data.id` and as an integer in `winner_id` and `points_by_participant`. Parse ids as strings and accept integer or string with a custom serde deserializer.

### 1.2 Error response format (JSON:API errors)

Source: Error Responses page and the ErrorResponse and ErrorModel schemas.

```json
{
  "errors": [
    {
      "status": 422,
      "detail": "Tournament Format is invalid",
      "source": { "pointer": "/data/attributes/tournament_format" }
    }
  ]
}
```

| Field | Type | Notes |
|---|---|---|
| `errors` | array | Top-level. Several errors may come back together (validation failures). |
| `errors[].status` | integer | HTTP code, "usually 400 or 422" |
| `errors[].detail` | string | Developer-friendly message |
| `errors[].source` | object | Present only when the error comes from the JSON payload; omitted for query/path parameter errors |
| `errors[].source.pointer` | string | JSON pointer to the offending attribute |

Status codes the page lists: 200 OK (retrieve/update), 201 Created, 204 No Content (delete), 400 Bad Request, 401 Unauthorized (token missing or wrongly passed), 403 Forbidden (authenticated but not allowed), 404 Not Found, 422 Unprocessable Entity, 500 Internal Server Error.

Per-endpoint pages additionally list 406 Not Acceptable and 415 Unsupported Media Type (wrong `Accept` or `Content-Type`).

Discrepancy: on several endpoint pages (List Participants, Get Participant, Get Match, List Matches, Update Match) the error responses are typed as a bare array of ErrorModel instead of the `{ "errors": [...] }` wrapper used on Get User, Get Tournament and the Error Responses page. **INFERENCE:** the wrapper form is the real one; code defensively and accept both an object with `errors` and a bare array.

Request signing failure (Connect Request Signing page): HTTP 401 with detail "Signature is invalid".

### 1.3 Rate limits

| Fact | Source |
|---|---|
| A paid plan is required above 500 requests per month per developer app | Getting Started, Welcome |
| Free apps over the limit receive HTTP 429 after the grace period | Getting Started, Welcome |
| Grace period end date | Getting Started and Welcome say **July 6, 2026**. The Changelog (March 31, 2026 entry) says **July 1**. The docs conflict; both dates are in the past as of 2026-10-18, so assume the limit is enforced. |

**NOT STATED:** the 429 response body format, any `Retry-After` header, any `X-RateLimit-*` headers, whether the limit is per app or per token, and per-second throttling. Handle 429 as a generic error and surface it to the user.

## 2. Authorization

### 2.1 Flows available

| Flow | `grant_type` | Intended for |
|---|---|---|
| Authorization Code | `authorization_code` | Websites and apps with browsers |
| Device Authorization Grant | `urn:ietf:params:oauth:grant-type:device_code` (Device Grant Access Token Request page). The Authorization overview page labels it `device_code`; the endpoint page value is the one to send. | TVs, consoles, games |
| Refresh | `refresh_token` | All flows with a refresh token |
| Client Credentials | not given | Servers you control. Open to all apps since 2026-01-27 (Changelog). The only application-scoped scope reachable this way is `application:manage`. |

The Authorization overview page also says the token request headers appear only in an image, so the Bearer header format is taken from Getting Started and the Apidog security config (`Authorization: Bearer <token>`).

### 2.2 Scopes

Source: Scopes page and the OAuth scheme on endpoint pages.

| Scope | Meaning |
|---|---|
| `me` | Read the user's details. **This is the scope that gives the current user's id and username.** |
| `tournaments:read` | Read all of the user's tournaments |
| `tournaments:write` | Create, update, delete the user's tournaments |
| `matches:read` | Read matches in the user's tournaments |
| `matches:write` | Update matches in the user's tournaments |
| `attachments:read` | Read match attachments |
| `attachments:write` | Create, update, delete match attachments |
| `participants:read` | Read participants in the user's tournaments |
| `participants:write` | Create, update, delete participants |
| `communities:manage` | Access resources of communities the user administers |
| `application:organizer` | Full access to the user's resources associated with the application |
| `application:player` | Read the user's application resources, register the user for tournaments, report their scores |
| `application:manage` | Full access to all tournaments connected to the application. Client credentials flow only. |

Scopes are passed as one space-separated string, for example `me tournaments:read matches:read participants:read`.

**NOT STATED:** which scope each individual endpoint requires. The per-endpoint OAuth blocks list an empty scope array for almost every operation. Exceptions where the prose names a scope: Get User requires `me`; Register Me requires `application:player`; Update Match under the application path requires `application:organizer` or `application:player` (Game Integration page). Mapping the rest (`tournaments:read`, `matches:read`, `participants:read`, `matches:write`) is **INFERENCE** from the scope descriptions.

### 2.3 Device Authorization Grant (reference only, not used by Ember)

Ember does not use this flow: it needs polling that the free tier's 500 requests a month cannot pay for, and it has no bridge page that names the Ember ID before connecting. The bridge uses section 2.4.

#### Step 1: Device Grant Authorization Request

| Item | Value |
|---|---|
| Method and URL | `POST https://api.challonge.com/v2.1/oauth/authorize_device` |
| Authentication | None (`security: []`) |
| Headers | None specified |
| Parameters | `client_id` (string, required, "Your Application's Client ID in Challonge"), `scope` (string, required, space-separated, example `me tournaments:read matches:read participants:read`) |
| Encoding | **Query string.** The spec defines no request body at all, for any encoding. Sending the same values as a form body is **NOT STATED** as supported. |
| `client_secret` | Not a parameter. Not required per the spec. |
| Response codes | Only 200 is documented |

Example request (built from the spec, not published in the docs):

```
POST https://api.challonge.com/v2.1/oauth/authorize_device?client_id=CLIENT_ID&scope=me%20tournaments%3Aread%20matches%3Aread%20participants%3Aread
```

Response 200 (published example):

```json
{
  "device_code": "2H9SCPn_ahg-YVEjsX8NAyxQNXl8xBDEpYMIvnDFmsM",
  "user_code": "FQFREU",
  "verification_uri": "https://auth.challonge.com/oauth/device",
  "verification_uri_complete": "https://auth.challonge.com/oauth/device?user_code=FQFREU",
  "expires_in": 300,
  "interval": 2
}
```

| Field | Type (from example) | Notes |
|---|---|---|
| `device_code` | string | Secret polled against the token endpoint |
| `user_code` | string | Shown to the user, 6 characters in the example |
| `verification_uri` | string | `https://auth.challonge.com/oauth/device` in the example. Note the host `auth.challonge.com`, not `api.challonge.com`. |
| `verification_uri_complete` | string | The same URL with `?user_code=...`, good for a QR code or direct browser open |
| `expires_in` | integer, seconds | 300 in the example |
| `interval` | integer, seconds | 2 in the example, the minimum polling interval |

The response schema declares no properties; types above are read from the example. Whether the verification URL is a fixed string or varies by environment is **NOT STATED**.

#### Step 2: Device Grant Access Token Request (polling)

| Item | Value |
|---|---|
| Method and URL | `POST https://api.challonge.com/v2.1/oauth/token` per the endpoint's server plus path |
| Alternate URL | `https://api.challonge.com/oauth/token` (no `/v2.1`). This is the `tokenUrl` in the OAuth security scheme on every endpoint page, in the Authorization page code samples, and the Game Integration page lists `POST /oauth/token` without a prefix. |
| Authentication | None (`security: []`) |
| Headers | None specified |
| Parameters (all query) | `grant_type` = `urn:ietf:params:oauth:grant-type:device_code` (required, the documented default), `device_code` (required), `client_id` (required) |
| Encoding | **Query string** per the spec, no body defined. The Authorization page code samples for the authorization code flow instead use `Content-Type: application/x-www-form-urlencoded` with the same parameter names in the body, so form encoding is probably accepted by the server (**INFERENCE**, not stated for the device flow). |
| `client_secret` | **Not listed** as a parameter for the device flow, so the docs do not say it is required. (The Authorization page code samples for the authorization code and refresh flows do send `client_secret` in the form body.) Whether the server would accept or reject it for a device flow is **NOT STATED**. |

Example request (built from the spec):

```
POST https://api.challonge.com/v2.1/oauth/token?grant_type=urn:ietf:params:oauth:grant-type:device_code&device_code=DEVICE_CODE&client_id=CLIENT_ID
```

Which token URL to use: the docs do not resolve the conflict. Both `/oauth/token` and `/v2.1/oauth/token` appear. What the bridge does: the shipped definition `server/ember/bridge/deploy/sites/challonge.json` sets `token_url` to `https://api.challonge.com/oauth/token`, the URL the OAuth security scheme and the authorization-code samples (section 2.4) use, with no fallback probe. An operator changes `token_url` in their copy if a live sign-in shows the other path is the one.

Success, HTTP 200 (published example):

```json
{
  "access_token": "superlongaccesstokenhere",
  "token_type": "Bearer",
  "expires_in": 604800,
  "refresh_token": "jBk8JIOzgbB_6NjukEAp9_1WE2kbOE8Izpf0AWMkBkk",
  "scope": "me tournaments:read matches:read participants:read",
  "created_at": 1762279561
}
```

| Field | Type | Notes |
|---|---|---|
| `access_token` | string | |
| `token_type` | string | `Bearer` |
| `expires_in` | integer, seconds | 604800 (one week) |
| `refresh_token` | string | Store it. The Authorization page says a new refresh token may be returned on refresh but the old one still works. |
| `scope` | string, space-separated | |
| `created_at` | integer, Unix seconds | |

Polling errors, HTTP 400 with an OAuth-style body of `error` and `error_description` (both strings). Documented values:

| `error` | Documented? | Meaning |
|---|---|---|
| `authorization_pending` | Yes | User has not approved yet. Keep polling. The published `error_description` text for it is a generic "unexpected condition" message. |
| `invalid_grant` | Yes | The grant is invalid, expired, revoked, or issued to another client. The page uses it to cover an expired device grant. |
| `slow_down` | **NOT STATED** | RFC 8628 value. Handle it by adding 5 seconds to the interval (RFC behavior, **INFERENCE**). |
| `expired_token` | **NOT STATED** | RFC 8628 value. Treat like `invalid_grant`. |
| `access_denied` | **NOT STATED** | RFC 8628 value. Treat as user declined and stop. |

**Device grant only (not used by Ember), INFERENCE:** a client of this flow would poll per RFC 8628 (poll no faster than `interval`, stop at `expires_in`), treat `authorization_pending` as continue, `slow_down` as back off, and every other `error` as terminal. Status codes other than 200 and 400 for this endpoint are **NOT STATED**.

#### Refresh

The Game Integration page says to refresh by sending `grant_type: "refresh_token"` to the same `/oauth/token` endpoint.

Refresh Token Request page: `POST https://api.challonge.com/v2.1/oauth/token`, no auth, all query params and all required: `refresh_token`, `client_id`, `grant_type` (`refresh_token`), `redirect_uri`. Response 200 has no documented schema (presumably the same token object). The Authorization page sample sends the same fields plus `client_secret` as a form body.

**NOT STATED:** whether `redirect_uri` is truly required when refreshing a device-grant token (the page marks it required; a device-grant app may have none), and whether `client_secret` is needed. Test against the real server. A refresh failure error body is **NOT STATED**.

### 2.4 Authorization Code flow (used by the bridge)

`GET https://api.challonge.com/oauth/authorize` with query `client_id`, `redirect_uri`, `response_type=code`, `scope`, optional `community_id`. The bridge also sends `state` (standard OAuth; Challonge's pages do not mention it, see the checklist) and exchanges the code with a form body that includes `client_secret`. The Grant Request page's server base would imply `/v2.1/oauth/authorize`, but the scheme's `authorizationUrl` is the unversioned form. Exchange the code with `POST /oauth/token` (`grant_type=authorization_code`, `code`, `client_id`, `redirect_uri`; samples also send `client_secret`). The security scheme metadata sets `challengeAlgorithm: S256`, which suggests PKCE is supported; the prose does not state it.

## 3. Current user

### Get User

| Item | Value |
|---|---|
| Method and URL | `GET https://api.challonge.com/v2.1/me.json` |
| Also written as | `GET /v2.1/me` on the Game Integration page. The endpoint page path is `/me.json`. |
| operationId | `findUser` |
| Description | "Returns the user logged in via OAuth, or the API key owner for v1 authentication. Requires `me` OAuth scope." |
| Headers | `Content-Type: application/vnd.api+json`, `Accept: application/json`, `Authorization-Type: v2`, `Authorization: Bearer <token>` |
| Query params | None |
| Responses | 200, 401, 406, 415 (errors use the ErrorResponse wrapper) |

Response 200 (derived from schema defaults, no published example):

```json
{
  "data": {
    "id": "321",
    "type": "user",
    "attributes": {
      "email": "hello@challonge.com",
      "username": "APIUser",
      "image_url": "https://s3.amazonaws.com/challonge_app/users/images/003/004/005/original/Decoy_Duck_w__background.png"
    }
  }
}
```

| Field | Type | Notes |
|---|---|---|
| `data.id` | string | The user id. Only the `me` scope is documented for this endpoint. |
| `data.type` | string | `user` |
| `data.attributes.username` | string | Challonge username |
| `data.attributes.email` | string | Appears in the schema. **NOT STATED** whether it is returned for every scope or null for users without a verified email. Do not depend on it. |
| `data.attributes.image_url` | string | |

**NOT STATED:** whether the id is numeric inside the string, whether `username` can be null, and any other attributes.

## 4. Tournaments

### Get Tournament

| Item | Value |
|---|---|
| Method and URL | `GET https://api.challonge.com/v2.1/tournaments/{tournament_id}.json` |
| operationId | `showTournament` |
| Path param | `tournament_id` (string, required): id (recommended) or URL slug |
| Query param | `community_id` (string, optional): community subdomain or permalink, required for community tournaments. Alternative: prefix the path with `/v2.1/communities/{community_id}/`. |
| Headers | Standard four plus optional `If-None-Match` |
| Scope | **NOT STATED** (empty scope list; `tournaments:read` by inference) |
| Responses | 200, 401, 404, 406, 415 |
| `include` parameter or `included` section | None defined |

Application-scoped form (Resource Scoping and Game Integration pages): `GET /v2.1/application/tournaments/{tournament_id}` for tournaments connected to your app. Tournaments created under the `/application/` prefix are linked to the app automatically.

Response 200 shape (schema `TournamentModel`; no published example):

```json
{
  "data": {
    "id": "30201",
    "type": "tournament",
    "attributes": {
      "name": "Rainbow Six: Siege Tournament",
      "url": "r6siege_tournament_xx",
      "tournament_type": "single elimination",
      "game_name": "Tom Clancy's Rainbow Six Siege",
      "private": false,
      "starts_at": "2026-01-01T00:00:00Z",
      "description": "",
      "tie_breaks": [],
      "notifications": { "upon_matches_open": true, "upon_tournament_ends": true },
      "match_options": { "consolation_matches_target_rank": 3, "accept_attachments": false },
      "registration_options": { "open_signup": false, "signup_cap": 0, "check_in_duration": 0 },
      "seeding_options": { "hide_seeds": false, "sequential_pairings": false },
      "station_options": { "auto_assign": false, "only_start_matches_with_assigned_stations": false },
      "group_stage_enabled": false,
      "group_stage_options": {},
      "double_elimination_options": { "split_participants": false, "grand_finals_modifier": "" },
      "round_robin_options": {},
      "swiss_options": {},
      "free_for_all_options": { "max_participants": 4 }
    }
  }
}
```

The values shown above are schema defaults and placeholders (the `starts_at`, `description` and `signup_cap` values are invented to make the shape valid). Attribute definitions:

| Attribute | Type | Notes |
|---|---|---|
| `name` | string | Required |
| `tournament_type` | string | Required. Enum with spaces: `single elimination`, `double elimination`, `round robin`, `swiss`, `free for all` |
| `url` | string | Slug |
| `game_name` | string | |
| `private` | boolean | Hidden from search engines |
| `starts_at` | string, date-time | Challonge does not start the tournament automatically |
| `description` | string | |
| `tie_breaks` | array of string | Round robin and swiss, max 3 |
| `notifications.upon_matches_open`, `.upon_tournament_ends` | boolean | |
| `match_options.consolation_matches_target_rank` | integer | |
| `match_options.accept_attachments` | boolean | |
| `registration_options.open_signup` | boolean | |
| `registration_options.signup_cap` | integer | |
| `registration_options.check_in_duration` | integer | Minutes, multiple of 5 |
| `seeding_options.hide_seeds`, `.sequential_pairings` | boolean | |
| `station_options.auto_assign`, `.only_start_matches_with_assigned_stations` | boolean | |
| `group_stage_enabled` | boolean | |
| `group_stage_options` | object | `stage_type`, `group_size`, `participant_count_to_advance_per_group`, `rr_iterations`, `ranked_by`, `rr_pts_for_match_win`, `rr_pts_for_match_tie`, `rr_pts_for_game_win`, `rr_pts_for_game_tie`, `split_participants`, `tie_breaks` |
| `double_elimination_options` | object | `split_participants` (bool), `grand_finals_modifier` (`""`, `skip`, `single match`) |
| `round_robin_options` | object | `iterations`, `ranking`, `pts_for_game_win`, `pts_for_game_tie`, `pts_for_match_win`, `pts_for_match_tie` |
| `swiss_options` | object | `rounds`, `pts_for_game_win`, `pts_for_game_tie`, `pts_for_match_win`, `pts_for_match_tie` |
| `free_for_all_options.max_participants` | integer | |

**NOT STATED** in the Get Tournament schema: the tournament's current `state` (the Tournament States page lists states but the Tournament attributes in the schema do not include a `state` field), `timestamps`, `participants_count`, and an `application` link. The List Tournaments `state` filter uses `pending`, `in_progress`, `ended`, which differ from the Tournament States page names below. If the client needs the current state, verify against a live response. Make every attribute optional in the Rust struct.

Tournament States page (state names, no transition rules published):

`pending`, `checking_in`, `checked_in`, `accepting_predictions`, `group_stages_underway`, `group_stages_finalized`, `underway`, `awaiting_review`, `complete`.

Change Tournament State: `PUT /v2.1/tournaments/{tournament_id}/change_state.json`, body `{"data":{"type":"TournamentState","attributes":{"state":"<value>"}}}` with `state` one of `process_checkin`, `abort_checkin`, `start_group_stage`, `finalize_group_stage`, `reset_group_stage`, `start`, `finalize`, `reset`, `open_predictions`. Content-Type `application/vnd.api+json`. Responses 200 (TournamentModel), 401, 404, 406, 415, 422.

List Tournaments: `GET /v2.1/tournaments.json` (user scope) or `GET /v2.1/application/tournaments` (application scope, path form without suffix on the Game Integration page). Query params documented on the endpoint page: `community_id`, `page` (default 1), `per_page` (default 25), `state` (`pending`, `in_progress`, `ended`), `type` (`single_elimination`, `double_elimination`, `round_robin`, `swiss`, `free_for_all`, `time_trail`, `grand_prix`), `created_after`, `created_before` (documented as mm/dd/yyyy though typed date-time). The Game Integration page also names `organizer_id` and `participant_user_id` filters for the application route; these are **not** in the List Tournaments schema, so they are **NOT CONFIRMED**. Resource Scoping names `include_community_tournaments=1`. Response: `{"data":[TournamentModel, ...]}`.

## 5. Participants

### List Participants

| Item | Value |
|---|---|
| Method and URL | `GET https://api.challonge.com/v2.1/tournaments/{tournament_id}/participants.json` |
| operationId | `findParticipants` |
| Query params | `community_id` (optional), `page` (integer, default 1), `per_page` (integer, default 25) |
| Headers | Standard four plus optional `If-None-Match` |
| Scope | **NOT STATED** (`participants:read` by inference) |
| Responses | 200, 401, 406, 415 (no 404 listed) |
| Pagination metadata | None in the schema. No `links`, `meta`, totals, or max `per_page` documented. Page until a short or empty page comes back (**INFERENCE**). |

Response 200: `{"data":[ParticipantModel, ...]}`.

### Get Participant

`GET https://api.challonge.com/v2.1/tournaments/{tournament_id}/participants/{participant_id}.json` (operationId `showParticipant`). Path params both strings, query `community_id` optional. Responses 200, 401, 404, 406, 415. Response: `{"data": ParticipantModel}`.

### ParticipantModel

Derived from schema defaults, no published example:

```json
{
  "data": {
    "id": "76",
    "type": "participant",
    "attributes": {
      "name": "Player 1's Name",
      "seed": 1,
      "group_id": null,
      "tournament_id": 21,
      "username": null,
      "final_rank": 1,
      "states": { "active": true },
      "misc": "Store whatever string you like here",
      "timestamps": {
        "created_at": "2023-04-21T14:29:06.374Z",
        "updated_at": "2023-04-21T14:29:06.374Z"
      }
    }
  }
}
```

| Field | Type | Notes |
|---|---|---|
| `id` | string | |
| `type` | string | `participant` |
| `attributes.name` | string | Display name |
| `attributes.seed` | integer | |
| `attributes.group_id` | integer | Only for group stage; null or absent otherwise (**INFERENCE**; schema default is placeholder text) |
| `attributes.tournament_id` | integer | |
| `attributes.username` | string | Associated Challonge username, "if present" |
| `attributes.final_rank` | integer | Nullable until the tournament ends (**INFERENCE**) |
| `attributes.states.active` | boolean | |
| `attributes.misc` | string | API-only field "for your own foreign key or other use" |
| `attributes.timestamps.created_at`, `.updated_at` | string (ISO 8601) | |

**NOT STATED:** a participant's Challonge user id. The model only exposes `username`, and the create request has no `user_id`. To match a Challonge user to a participant, compare `username` from `/me.json` with participant `username`, or store your own key in `misc`. Create Participant (`POST .../participants.json`) takes `{"data":{"type":"participant","attributes":{"name":..., "seed":..., "misc":..., "email":..., "username":...}}}`, `name` required; setting `email` or `username` invites a new or existing Challonge user. Register Me: `POST /v2.1/application/tournaments/{tournament_id}/participants/register_me.json` with scope `application:player`, body `{"data":{"type":"participant","attributes":{"name":"..."}}}` (`name` required; `seed`, `misc`, `email`, `username` optional); response `ParticipantModel`.

## 6. Matches

### List Matches

| Item | Value |
|---|---|
| Method and URL | `GET https://api.challonge.com/v2.1/tournaments/{tournament_id}/matches.json` |
| operationId | `findMatches` |
| Query params | `community_id`, `page` (default 1), `per_page` (default 25), `state` (`pending`, `open`, `complete`), `participant_id` (string) |
| Scope | **NOT STATED** (`matches:read` by inference) |
| Responses | 200, 401, 406, 415 |
| Pagination | `page` and `per_page`. Changelog 2026-09-14: matches are now properly paginated; before that "pagination pointers continued indefinitely". No metadata fields documented. |

Response 200: `{"data":[MatchModel, ...]}`.

### Get Match

`GET https://api.challonge.com/v2.1/tournaments/{tournament_id}/matches/{match_id}.json` (operationId `showMatch`). Query `community_id` optional. Responses 200, 401, 404, 406, 415. Response: `{"data": MatchModel}`.

### MatchModel

Derived from schema defaults, no published example:

```json
{
  "data": {
    "id": "8008135",
    "type": "match",
    "attributes": {
      "state": "complete",
      "round": 1,
      "identifier": "A",
      "suggested_play_order": 1,
      "scores": "2 - 0",
      "score_in_sets": [[3, 1], [4, 2]],
      "points_by_participant": [
        { "participant_id": 355, "scores": [3, 4] },
        { "participant_id": 354, "scores": [1, 2] }
      ],
      "timestamps": {
        "created_at": "2023-04-21T14:29:06.374Z",
        "updated_at": "2023-04-21T14:29:06.374Z"
      },
      "winner_id": 355,
      "relationships": {
        "player1": { "data": { "id": "355", "type": "participant" } },
        "player2": { "data": { "id": "354", "type": "participant" } }
      }
    }
  }
}
```

| Field | Type | Notes |
|---|---|---|
| `id` | string | |
| `type` | string | `match` |
| `attributes.state` | string | Enum `pending`, `open`, `complete` |
| `attributes.round` | integer | |
| `attributes.identifier` | string | Letter label such as `A` |
| `attributes.suggested_play_order` | integer | |
| `attributes.scores` | string | Display text such as `2 - 0` |
| `attributes.score_in_sets` | declared `object`, default is an array of `[a, b]` pairs | Treat as `serde_json::Value` |
| `attributes.points_by_participant` | declared `object`, default is an array of objects | Treat as `serde_json::Value` |
| `attributes.timestamps.created_at`, `.updated_at` | string | |
| `attributes.winner_id` | integer | "The participant ID of the winner". Renamed from `winners` in v2.0. Null or absent before completion (**INFERENCE**). |
| `attributes.relationships.player1.data`, `player2.data` | object with `id` (string) and `type` (`participant`) | Note: `relationships` sits inside `attributes` per this schema, unlike standard JSON:API. Player slots can be empty before a match is fed (**INFERENCE**, not stated). |

**NOT STATED:** `group_id` on matches (the Upgrading page says group stage matches carry a `group_id`, the schema does not list it), the loser id, per-player scores as separate fields, `underway_at`, and `scheduled_time`/`location` in the output (they exist only in the Update request).

### Update Match (report scores)

| Item | Value |
|---|---|
| Method and URL | `PUT https://api.challonge.com/v2.1/tournaments/{tournament_id}/matches/{match_id}.json` |
| Application form | `PUT /v2.1/application/tournaments/{tournament_id}/matches/{match_id}` (Game Integration; scope `application:organizer` or `application:player`) |
| Scope for user route | `matches:write` by inference (the page lists none) |
| Content-Type | `application/vnd.api+json` |
| Responses | 200 (MatchModel), 401, 404, 406, 415, 422 |
| Optional signing | `Signature` header (JWT HS256) for apps with signing enabled; see section 8 |

Request body schema `MatchInput` (derived from defaults, no published example):

```json
{
  "data": {
    "type": "match",
    "attributes": {
      "match": [
        { "participant_id": "76", "score_set": "4,2,4", "rank": 1, "advancing": true },
        { "participant_id": "77", "score_set": "1,4,0", "rank": 2, "advancing": false }
      ],
      "tie": false,
      "location": "Station 1",
      "scheduled_time": "2026-10-18T20:00:00Z"
    }
  }
}
```

| Field | Type | Notes |
|---|---|---|
| `data.type` | string | `match` |
| `attributes.match` | array of objects, 1 to 8 items | One entry per participant being scored |
| `match[].participant_id` | string, required | |
| `match[].score_set` | string, required | Comma-separated per-game scores for that participant. **The field is `score_set`, not `scores_csv`.** The example in the docs is `4,2,4`, and the opponent's complementary values (for 4-1, 2-4, 4-0 the other player has `1,4,0`) must be sent as another array entry (**INFERENCE** that both entries are expected for a 2-player match; the page says the format "scales to 3+ participants in Free For All"). |
| `match[].rank` | integer | Default 1. Used for 3+ player formats. |
| `match[].advancing` | boolean | Default false. Marks the winner/advancing participant. For ties, set false for everyone and set `tie: true`. |
| `attributes.tie` | boolean | Default false. For round robin and swiss only. |
| `attributes.location` | string | Text only |
| `attributes.scheduled_time` | string | Date-time string |

No `required` array on `MatchInput`. Example values above (the second participant, `location`, `scheduled_time`) are illustrative, not from the docs.

### Change Match State

| Item | Value |
|---|---|
| Method and URL | `PUT https://api.challonge.com/v2.1/tournaments/{tournament_id}/matches/{match_id}/change_state.json` |
| Content-Type | The endpoint page says request body content type `application/json` (all other write endpoints say `application/vnd.api+json`). Send `application/vnd.api+json` per the global header rule; the discrepancy is **NOT RESOLVED**. |
| Body | `{"data":{"type":"MatchState","attributes":{"state":"mark_as_underway"}}}` |
| `state` values | `reopen`, `mark_as_underway`, `unmark_as_underway` |
| Responses | 200 (MatchModel), 401, 404, 406, 415, 422 |

## 7. Pagination summary

All list endpoints take `page` (default 1) and `per_page` (default 25). Both are declared integer with a stray `format: string`, so send them as plain numbers in the query string. The response carries no totals, links, or meta. **NOT STATED:** maximum `per_page`, behavior past the last page (empty array assumed), and whether `Link` headers are returned.

## 8. Other facts

| Topic | Fact |
|---|---|
| Resource scoping | User scope is default (`/v2.1/tournaments`). Community scope: `/v2.1/communities/{community_id}/tournaments` or `?community_id=`; needs `communities:manage`. Application scope: `/v2.1/application/tournaments`. |
| Request signing (Connect, closed beta) | For POST and PUT match requests. Compute SHA-256 hex of the exact body bytes, make a JWT signed HS256 with the app's validation secret and payload `{"data":{"checksum":"<hex>"}}`, send it in header `Signature`. A bad signature gives HTTP 401, detail "Signature is invalid". Signature is validated whenever sent, and is mandatory only if the app owner enables it in the Developer Portal. |
| Game Integration (beta) | Recommends device grant for in-game sign-in, scopes `me`, `application:player`, `application:organizer`; client-side score reporting is trust-based, so server-side reporting with `application:manage` is advised for competitive use. Players may report only their own score in 3+ player formats, and only open matches they are in for 2-player formats. |
| Changelog highlights | 2026-09-14 matches paginated; 2026-03-31 stations docs added; 2026-01-27 client credentials open to all apps and `communities:manage` UI changed; 2025-11-05 docs moved to apidog.io. The changelog says nothing about device grant, token URL, `/me`, error format, or id changes. |
| v1 key auth | Header pair `Authorization-Type: v1` and `Authorization: <key>`. Do not use for a distributed client; a player signs in with OAuth (Ember: the authorization-code flow on the bridge). ember-brackets uses the organizer's key on the organizer's own server. |

## 9. Inconsistencies and unknowns checklist

Resolve each against the real server before relying on it. Rows 3 to 7 are about the device grant, which Ember does not use; rows 23 to 25 are what the bridge's authorization-code sign-in depends on.

| # | Item | Status |
|---|---|---|
| 1 | Token URL: `/v2.1/oauth/token` (device pages) or `/oauth/token` (OAuth scheme, code samples) | Docs conflict; the shipped `challonge.json` uses `/oauth/token` as its `token_url`, no probe |
| 2 | `/me.json` (endpoint page) versus `/me` (Game Integration) | Docs conflict; use `/me.json` |
| 3 | Device grant (not used): parameters sent as query string (spec) versus form body (other flows' samples) | Spec says query; form unverified |
| 4 | Device grant (not used): `client_secret` for token polling | Not stated; docs list no such parameter |
| 5 | Device grant (not used): `slow_down`, `expired_token`, `access_denied` error values | Not documented; only `authorization_pending` and `invalid_grant` are |
| 6 | Device grant (not used): `grant_type` label `device_code` (overview) versus the URN (endpoint page) | Use the URN |
| 7 | Device grant (not used): whether `redirect_uri` is needed to refresh a token | Page marks it required; unverified |
| 8 | Per-endpoint required scopes | Mostly unlisted; `me` and `application:player` are explicit |
| 9 | 429 body, `Retry-After`, rate-limit headers | Not documented |
| 10 | Rate limit grace period end (July 6 versus July 1, 2026) | Docs conflict, both past |
| 11 | Error body for list endpoints: wrapper object versus bare array | Spec inconsistent; accept both |
| 12 | Tournament `state` attribute on the Tournament response | Not in the schema |
| 13 | User `email` presence and nullability, `username` nullability | Not stated |
| 14 | Participant has no Challonge user id field | Only `username` |
| 15 | Match `group_id`, empty player slots, `winner_id` null before completion | Not in schema |
| 16 | `score_set` semantics for 2-player matches (one entry versus two) | Complement values implied; unverified |
| 17 | Change Match State content type (`application/json` versus `application/vnd.api+json`) | Spec inconsistent |
| 18 | Whether `.json` suffix is optional | Not stated |
| 19 | `organizer_id` and `participant_user_id` filters on application tournament list | Mentioned only in Game Integration |
| 20 | Maximum `per_page` and empty-page behavior | Not stated |
| 21 | JSON:API `included`, `links`, `meta` | None defined on any documented endpoint |
| 22 | PKCE support, and whether it is required | Only a scheme hint (`challengeAlgorithm: S256`); the bridge sends a client secret and no code challenge |
| 23 | Whether a Connect app may use the authorization-code flow with the bridge's redirect URI (`<origin>/v1/sites/challonge/callback`) | Documented for websites; registering the URI is unverified |
| 24 | Whether `/oauth/authorize` carries `state` back to the redirect with the `code` | Standard OAuth, not stated by Challonge's pages; the bridge relies on it |
| 25 | Whether the token endpoint takes the authorization-code exchange as a form body with `client_secret` | The Authorization page's samples do; unverified live |

## 10. Page index

| Topic | URL |
|---|---|
| Index | https://challonge.apidog.io/llms.txt |
| Getting Started | https://challonge.apidog.io/getting-started-1726706m0.md |
| Authorization | https://challonge.apidog.io/authorization-1726705m0.md |
| Scopes | https://challonge.apidog.io/scopes-1726710m0.md |
| Error Responses | https://challonge.apidog.io/error-responses-1726721m0.md |
| Resource Scoping | https://challonge.apidog.io/resource-scoping-1730219m0.md |
| Tournament States | https://challonge.apidog.io/tournament-states-1726857m0.md |
| Device Grant Authorization Request | https://challonge.apidog.io/device-grant-authorization-request-23762867e0.md |
| Device Grant Access Token Request | https://challonge.apidog.io/device-grant-access-token-request-23762909e0.md |
| Token Request | https://challonge.apidog.io/token-request-23703396e0.md |
| Refresh Token Request | https://challonge.apidog.io/refresh-token-request-23703401e0.md |
| Get User | https://challonge.apidog.io/get-user-23619794e0.md |
| Get Tournament | https://challonge.apidog.io/get-tournament-23619741e0.md |
| List Participants | https://challonge.apidog.io/list-participants-23619749e0.md |
| Get Participant | https://challonge.apidog.io/get-participant-23619751e0.md |
| Register Me | https://challonge.apidog.io/register-me-23820808e0.md |
| List Matches | https://challonge.apidog.io/list-matches-23619745e0.md |
| Get Match | https://challonge.apidog.io/get-match-23619746e0.md |
| Update Match | https://challonge.apidog.io/update-match-23619747e0.md |
| Change Match State | https://challonge.apidog.io/change-match-state-23619748e0.md |
| Game Integration (beta) | https://challonge.apidog.io/game-integration-beta-1726722m0.md |
| Connect Request Signing | https://challonge.apidog.io/connect-request-signing-1726704m0.md |
| Changelog | https://challonge.apidog.io/changelog-1734841m0.md |
