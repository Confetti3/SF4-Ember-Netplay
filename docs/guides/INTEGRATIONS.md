# Integrating with Ember

Ember's tournament bridge is a provider-neutral web service. Tournament
platforms, organizers and bots talk to it over one HTTP and event contract,
and it never relays gameplay. This guide covers what works in this branch:
identity linking, matches played in Ember and reported by the game, matches
decided by an organizer, and signed events for webhooks, Discord and Twitch.

The full design is EMBER-TB-001 in `docs/design/identity-bridge/`. What is
implemented, and how it differs from the draft, is in `STATUS.md` there.

## What exists today

- Each player has a persistent Ember ID, created in the game on request and
  kept under `%LOCALAPPDATA%\Ember\Identity\v1`. It survives restarts and
  updates, and an encrypted backup carries it to another PC.
- A bridge links that ID to an account on your platform with a short code that
  the account holder approves.
- Matches are created by a provider. A match can be played in Ember, where the
  game opens a room for the two players, keeps everyone else out, and reports
  each game's result itself (`ember-room-v1`), or decided game by game by an
  organizer (`organizer-reported-v1`).
- Every match played in Ember has a Play link, the same for both players,
  that opens it in Ember or shows how to install Ember first.
- On top of matches, the bridge can run king-of-the-hill lobbies and whole
  tournaments (single elimination, double elimination, round robin), and it
  keeps each player's record of sets and games.
- Every change is an event, delivered to webhooks and readable by cursor or
  over SSE.
- A platform connects as a `direct` provider: it calls this API itself with
  its provider credential. The `mock` kind adds browser login pages for local
  testing. The `blumint` kind serves BluMint's game partner API instead (see
  "BluMint" below).
- Optionally, a player connects their Discord account to their Ember ID, so a
  platform that finds players by Discord account gets their Ember ID. The
  same goes for an account on any sign-in site the operator configures, such
  as Challonge (found by username) or start.gg (found by user ID).
- A bracket run on Challonge, start.gg or a service of your own plays in
  Ember through `ember-brackets`, which turns each open set into a match and
  reports the score back (see "Bracket sites" below).
- A connection's settings give any platform what BluMint has: finding players
  by Discord account, results sent to it, and disputed matches restarted
  rather than reviewed. With rooms allowed, a platform or bot opens public
  rooms for its players and hears what happens to them (see "Connection
  settings" below). The notifier is a ready-made room bot for Discord and
  Twitch.
- Ember's production bridge runs at `https://bridge.embernetplay.link`, and a
  staging bridge with its own bridge ID and data at
  `https://bridge-staging.embernetplay.link`. Platforms get a connection and
  credentials from the Ember team; BluMint starts from
  `BLUMINT_QUICKSTART.md`.

## Run a local bridge

From `server/ember` (the bridge build needs the Visual Studio C++ tools for its
bundled SQLite):

```sh
cargo run -p ember-bridge -- init ../../bridge-dev
cargo run -p ember-bridge -- credential ../../bridge-dev/bridge.json provider mock-local "my platform"
cargo run -p ember-bridge -- credential ../../bridge-dev/bridge.json organizer local "my organizer"
cargo run -p ember-bridge -- serve ../../bridge-dev/bridge.json
```

`init` writes `bridge.json`, a fresh `bridge-secrets.json` and a mock provider
connection, listening on `http://127.0.0.1:8787`. Each `credential` command
prints a token once; the bridge keeps only a keyed hash. Keep
`bridge-secrets.json` private. Players never need it.

Loopback `http` is accepted only because `init` sets `allow_loopback_http`
and `allow_private_webhooks` for local work. A deployed bridge uses an
`https` origin with both off, and `mock_browser` off.
`server/ember/bridge/deploy` holds the staging deployment: a systemd unit, an
nginx site, a daily backup and `setup.sh`.

`bridge.json` also takes `match_expiry_hours`, how long a match nobody plays
stays open (24 when absent; 1 to 336, which is 14 days). See "Matches nobody
plays" below.

`replay_retention` prunes old sets' replays, which are kept forever when it
is absent: `{ "days": 90, "backup": "replay-backup" }` removes a set's replays
once the set has not changed for `days` (at least 30), after writing its pack
to `backup` when set. See `docs/design/TOURNAMENT_DESK.md` 4b.

## Link a player

1. Your service, having verified who the user is, creates a link intent:
   `POST /v1/link-intents` with `{"subject": "<your user id>"}` and a provider
   credential. Show the returned code to that user only. It works once and
   expires after five minutes.
2. The player enters the code in Ember (Profile, Ember ID, Linked accounts).
   Ember's helper proves it holds the key and submits a claim.
3. Your service shows the claim's fingerprint (`GET /v1/link-intents/{id}`)
   next to the one Ember shows, and when the user confirms it, approves that
   exact claim: `POST /v1/link-intents/{id}/approve` with the claim ID, the
   Ember ID and your subject.

A code on its own never completes a link; someone who copies it can only
create a claim that the account holder sees and rejects. To try the browser
version of this flow, open `http://127.0.0.1:8787/mock/mock-local/login`.

`POST /v1/players/resolve` maps your subjects to their current Ember IDs.

To test linking without the game, `sdk/typescript/tools/test-player.ts` stands
in for a player: it holds a throwaway Ember key and claims a code the way Ember
does (`TestPlayer` in the SDK does the same from code).
`sdk/typescript/tools/walkthrough.ts` runs linking, a match and its events
end to end against any bridge.

## Create and decide matches

- `POST /v1/matches` (provider, `Idempotency-Key` required) takes the two
  linked participants, the set length (`games_to_win` of 1 to 10) and the
  rules profile: `ember-room-v1` for a match played in Ember (next section) or
  `organizer-reported-v1` for one an organizer decides. Retrying with the same
  key or the same `external_match_id` returns the same match.
- `POST /v1/matches/{id}/adjudications` (organizer only) records one game:
  `{"kind": "game_result", "winner_slot": 0, "reason": "...",
  "expected_revision": "1"}`, or `"draw": true`, which scores nothing. When a
  player reaches the set length the match completes. `{"kind": "void_game",
  "attempt_id": "..."}` removes a game and scores the match again. On a
  completed match that is a correction: it stays completed while a player
  still has enough wins, and otherwise reopens. Only a correction that would
  reopen the match can be refused (`lease_conflict`): when either player has
  another match currently active on the same connection, for a lobby set the
  lobby has moved past, or for a bracket set whose later sets have games.
  Corrections in a finished tournament are always refused. Read `state` and
  `revision` from the answer, and handle a refusal, before changing your own
  result.
- `POST /v1/matches/{id}/cancel` cancels with a reason.

Every write names the revision it expects, as the decimal string the match
shows, so two people acting at once cannot both win.

### Matches nobody plays

A match does not wait forever. Its `created_at` and `expires_at` (unix
seconds) are on `GET /v1/matches/{id}` and in the players' assignments, and
`expires_at` is the match's current deadline: `match_expiry_hours` after
creation, a day unless the operator changed it, and again that long after each
game that starts or is decided. On a BluMint connection games move it the
same way, but never past 24 hours after the match's creation, whatever
`match_expiry_hours` says. A provider cannot choose it per match yet.

Once `expires_at` has passed, the bridge's maintenance pass (every ten
seconds) moves the match to the terminal state `expired`, frees its players,
and writes `match.expired` (`state`, `reason: "not_played"`, `expires_at`,
`match_revision`). `expired` is not `cancelled`: nothing called the match off,
and it is not a request to play it again. Whether to schedule it again is your
decision. Ember never creates a replacement.

A match is only expired when nothing is happening in it:

- no game is open (started and waiting for its reports) and no player's game
  holds the room's provisioning lease;
- a match with games recorded, such as a set at 1-0 that the players left,
  expires only after a whole lifetime with no game started or decided (a game
  started, or settled by agreeing reports, an organizer's result, a voided game
  or a correction), so `expires_at` moves to a full lifetime after each of those
  and a set being played is never cut off, even when an organizer decides a
  held game after the old deadline (on a BluMint connection, see above, never
  past a day after creation);
- a match waiting for an organizer in `needs_review` never expires, since a
  person owns it;
- a BluMint match expires 24 hours after its creation whatever is happening
  in it, and a game still open then is called off, since BluMint counts its
  players free from then;
- lobby and tournament sets never expire (their `expires_at` is null); they
  end with their lobby or bracket.

An expired match cannot be cancelled or adjudicated, and it stays in the
players' assignments, as `expired`, for a day after it expired. Matches that
were still open when this was added got a day from the later of their creation
and their last update, so old stale ones expired the first time the bridge ran
its maintenance.

A platform with a `results_url` is sent the result with outcome `expired`
(no winner; scores are what was recorded), never `restart`. BluMint is sent an
abort it must not retry (`retryable: false`), so its host takes the match.

## Matches played in Ember

With `native_rules_profile: "ember-room-v1"` the players play the set in Ember
and the game reports it:

1. Each player opens the match in Ember: from the Tournament matches screen
   (Settings > Ember ID), or from your Play button (below). The first player's
   game opens a room and the second joins it. Only the two assigned players
   can be in that room, and the room plays the set length you chose.
2. Before each game both games ask the bridge for permission to start it, and
   the game starts only when both have it. You see `match.room.ready` once
   the room is published, then `match.attempt.authorized` for each game.
3. When a game ends, each player's game reports what it saw, signed with that
   player's Ember ID (`match.game.reported`). When both agree the game counts
   (`match.game.confirmed`, `match.score.changed`), and at the set length the
   match completes (`match.completed`).
4. When the reports disagree, only one arrives within a minute, or a started
   game is never reported, the match waits in `needs_review`
   (`match.needs_review`). An organizer then decides that game with an
   adjudication naming its `attempt_id` (from the error detail or the match),
   or voids it so it is played again. Nothing is guessed: a missing report never
   becomes a win for the other player.

Signed reports show which Ember ID said what. Two players who agree on a false
result can still both report it, so treat them as the players' own account of
the game, not as proof against cheating.

Both players need the same Ember build; the second player's game is refused
with `incompatible_build` otherwise. `required_build_id` is your label for the
build and is not checked against the game.

### A Play button

The answer to `POST /v1/matches`, and `GET /v1/matches/{id}`, carry the
match's `play_url`: `https://embernetplay.link/m#<bridge id>/<match id>`. It is
the same for both players and safe to put in messages: only the match's two
assigned Ember IDs can claim the match, and anyone else's Ember finds no such
match in their list. The link itself does not expire, so you can store it;
the match it opens does if nobody plays it (see "Matches nobody plays").

The page asks the player to choose Open in Ember, which hands Ember
`ember://tournament/open?bridge=...&match=...`. Below that it shows how to get
Ember when it is not installed yet (download, start it once, come back to the
same link), so your site needs no explanation of its own. Ember opens the
Tournament matches screen on that match and the player presses Play there; a
player in a game or a room is never moved. Where the `ember:` link does not
open Ember (Linux browsers, or a PC where Ember has not been started yet), the
page's Copy link and Paste match link on that screen do the same, or the player
picks the match from the list.

## Discord sign-in (optional)

A bridge whose configuration has `discord` (the Discord application's
`client_id`, with `<origin>/v1/discord/callback` in its redirect list) and
whose integration secrets hold the client secret lists `discord` in its
capabilities `features`. A player then connects their Discord account from
Ember (Online play, then Ember ID, then Discord). A platform can send the
player there directly: the link `ember://discord/connect?bridge=<bridge_id>`,
offered by the page `https://embernetplay.link/start#<bridge_id>`, opens
Ember's Connect Discord screen for that bridge. At an idle menu it creates the
Ember ID if needed, trusts Ember's own bridge unless the player removed it,
and opens Discord's page by itself; a link that arrives during play asks
first. A platform may add its own name after the bridge ID
(`/start#<bridge_id>/<name>`, such as `/BluMint`), which the page shows the
player as the site that finds them. The page shows a name of 1 to 40
letters, digits, spaces and `.`, `'`, `&` or `-`, starting with a letter or
digit (percent-encoded in the link, as `%20` for a space); it drops any
other name without saying so and speaks of "the tournament you signed up
for" instead. It keeps only the bridge ID in its address and in the link it
copies. The bridge's Discord pages name the sites that find players by
Discord account by their tenants' `name`.
Where the browser cannot open Ember, the player pastes the page's link into
Connect Discord (Paste link) instead. Ember reads the link with or without
the name, though an Ember from before it read the name takes only the link
without it, as the page's Copy link gives it. The link names a bridge only;
Ember never trusts one by itself. The sign-in:

1. Ember's helper calls `POST /v1/discord/start` with a `discord.connect`
   proof and opens the returned Discord page in the browser.
2. Discord sends the browser to `GET /v1/discord/callback`. The bridge
   exchanges the code itself, with the `identify` scope only, and keeps the
   Discord user ID and username. The sign-in is one-use and lapses after ten
   minutes. Starting a sign-in ends the Ember ID's earlier ones, and
   `DELETE /v1/discord/start` (a `discord.cancel` proof, sent by Cancel in
   Ember) ends the one in flight, so a page left open connects nothing.
   Ember does not send a cancel that failed again, so until such a sign-in
   lapses, its page can still finish it.
3. If that Discord account is connected to another Ember ID, nothing moves
   yet: the page names both Ember IDs, says that the tournament links the
   account made for the other one end, along with the matches they hold, and
   asks. If the Ember ID being connected already has another Discord
   account, the page names it and says it is disconnected and its links end
   too. The player's answer (`POST /v1/discord/callback`) carries a secret
   made for that question and shown only on that page, never the sign-in's
   state, which the Ember that started the sign-in also holds. It moves the
   account or keeps it where it is.
4. `GET /v1/discord` (player session) shows the connected account and
   `DELETE /v1/discord` (a `discord.remove` proof) disconnects it.

One Discord account belongs to one Ember ID and the other way round; the
latest sign-in replaces both, once the player confirms moving an account
from another Ember ID. A platform's links that a sign-in approved
(`approved_via` `discord`) end when that account moves to another Ember ID or
is disconnected, and the platform's next lookup links it where it now
belongs. A link a sign-in approved that the player removes stays removed:
no account the player has connected (Discord's or a sign-in site's) makes
it again on that connection, until the player signs in again with one of
them, which gives that account's consent back. Removing a link made from a
code withdraws nothing, as before. Nothing else from Discord is stored. Without
the client secret the bridge offers no sign-in and says so in its log, but
every bridge lists `discord.accounts` and keeps `GET` and `DELETE /v1/discord`, so a player can
always see and disconnect an account connected earlier.

## Sign-in sites (optional)

Besides Discord, a bridge can offer sign-in with any site that has an OAuth
authorization-code flow, such as Challonge or start.gg. A site is data, not
code: the operator defines it in `sign_in_sites` in `bridge.json`, and a new
site needs no new bridge build, migration or game translation. Example
definitions ship in `server/ember/bridge/deploy/sites/` (`challonge.json`,
`startgg.json`); copy one into `sign_in_sites` and set `client_id`:

```json
"sign_in_sites": [
  {
    "id": "startgg",
    "display_name": "start.gg",
    "client_id": "<the start.gg application's client ID>",
    "scope": "user.identity",
    "authorize_url": "https://start.gg/oauth/authorize",
    "token_url": "https://api.start.gg/oauth/access_token",
    "token_encoding": "json",
    "token_params": { "scope": "user.identity" },
    "user": {
      "method": "POST",
      "url": "https://api.start.gg/gql/alpha",
      "body": { "query": "query CurrentUser { currentUser { id player { gamerTag } } }" },
      "id_pointer": "/data/currentUser/id",
      "username_pointer": "/data/currentUser/player/gamerTag"
    },
    "username_rule": { "characters": "printable", "max_length": 64, "case_insensitive": false },
    "lookup_by": "user_id",
    "hosts": ["start.gg", "api.start.gg"]
  }
]
```

- `id`: the site ID, 1 to 32 of `a-z`, `0-9` and `-`, never `discord`. Routes,
  storage, lookups and claims name the site by it, so keep it once players
  connect (`challonge-staging` beside `challonge` is fine).
- `display_name`: what the pages and Ember show (Ember's row says
  "Connect start.gg").
- `client_id`, `scope`: the OAuth application's. Its client secret is
  `site_client_secrets.<id>` in the integration secrets
  (`set-integration-secret.sh site <id>`), and its redirect list must hold
  `<origin>/v1/sites/<id>/callback`.
- `authorize_url` and optional `authorize_params`: where the browser goes,
  with `response_type=code`, `client_id`, `scope` (left out when empty),
  `redirect_uri` and `state`, plus those parameters.
- `token_url`, `token_encoding` (`form` or `json`) and optional
  `token_params`: the code exchange, with `grant_type=authorization_code`,
  `code`, `redirect_uri`, `client_id` and `client_secret` in the body, plus
  those fields.
- `user`: how to read who signed in, with the token as a bearer: `method`
  (`GET` or `POST`), `url`, extra `headers`, an optional JSON `body` (POST
  only, such as start.gg's GraphQL query), and JSON pointers `id_pointer` and
  optional `username_pointer` into the answer. A body goes as
  `Content-Type: application/json` unless `headers` names its own
  `Content-Type`, which replaces it. `headers` names each header once and
  cannot set `Authorization`, `Host`, `Content-Length`, `Transfer-Encoding`
  or `Connection`. A whole number of zero or more is read as its decimal
  digits. Without a username (no pointer, or nothing there to show) the
  user ID is shown instead.
- `username_rule`: `characters` (`letters_digits`, with ASCII punctuation in
  `also`, or `printable`, anything but control characters), `max_length`, and
  `case_insensitive`.
- `lookup_by`: `username` or `user_id`, what a lookup's `id` names (see
  "Connection settings"). `username` needs a `username_pointer`.
- `hosts`: every URL above must be https on one of these hosts, with no port,
  credentials or fragment. Each host is a public DNS name: an IP address,
  `localhost`, a single-label name or one under a private suffix such as
  `.local` or `.internal` is refused (local tests aside). Ember opens a
  sign-in page only on the `authorize_url`'s origin and path, as the
  capabilities list them.
- `limits` (optional): `monthly_request_budget`, `daily_request_budget` and
  `starts_per_hour`, below.

A definition is trusted configuration, like the rest of `bridge.json`: it
receives the site's client secret and decides where players' sign-in codes
and tokens go, and what the bridge stores and shows about them. Write your
own or copy the shipped examples; never load one from someone you would not
give the client secret to. The bridge checks each definition when it starts
and refuses one it cannot use. Every configured site is listed in the capabilities' `sign_in_sites`
(`id`, `display_name`, `sign_in`, `lookup_by`, `authorize_origin`,
`authorize_path`), with `sign_in` false while the integration secrets lack
its client secret. Every bridge lists the `sites` feature, so Ember knows
it has these routes, with or without sites. To take a site out, remove it
from `sign_in_sites` and from every connection's `lookup_sites` in the same
edit: the bridge refuses to start while a connection still names a site it
does not have. The site then offers no sign-in and drops out of
`sign_in_sites` and of lookups, but while accounts are still connected on
it, `GET` and `DELETE /v1/sites/<id>` keep working for the signed-in player, so a player can still see and disconnect one
(Ember lists only the configured sites, so that is through the API; the
links it approved can also be removed from the player's links). Once none is
left, its routes answer 404. Its rows come back into use if the site is
configured again under the same ID.

Sign-in works as Discord's does (above), on the site's own routes and with
the same pages naming the Ember ID and the site:

1. Ember's helper calls `POST /v1/sites/<id>/start` with a `site.connect`
   proof and opens the returned `authorize_url` in the browser.
2. The site sends the browser to `GET /v1/sites/<id>/callback`. The bridge
   exchanges the code, reads the user ID and username, and drops the token.
3. The page asks before connecting, or before moving an account another
   Ember ID holds (`POST /v1/sites/<id>/callback`). An account whose username
   the site's `username_rule` does not allow is not connected when lookups
   match usernames, since no lookup could find it.
4. `GET /v1/sites/<id>` shows the connected account, `DELETE
   /v1/sites/<id>/start` (`site.cancel`) ends a sign-in in flight, and `DELETE
   /v1/sites/<id>` (`site.remove`) disconnects the account.

Proofs are bound to the exact path, so a proof for one site's route is
refused on another's. A site the bridge does not have answers 404
`not_found`, and its callback shows the browser a page saying the bridge
does not offer that sign-in. Each site keeps its own accounts, sign-ins, withdrawals and
request budget. Links a site's sign-in approved (`approved_via` `site`,
claim `<id>:<user ID>`) follow the account exactly as Discord's do.

### Request limits

A site with `limits` is held to them; one without has only each Ember ID's
ten starts an hour. A sign-in costs two requests (the token exchange and the
user read), and the bridge spends at most `monthly_request_budget` per
calendar month and `daily_request_budget` per day, both in UTC and counted in
the database: it takes a sign-in's two before the exchange, so one the site
refuses still counts both. At either budget a start answers `sign_in_busy`
(503, retryable, with `Retry-After` until the next UTC day for the daily
budget) and a sign-in already started ends with a page asking the player to
try again later. Starts are also limited to `starts_per_hour` across the
bridge, counted only for starts whose proof holds and that are stored; a
start it refuses answers `sign_in_busy` too. The bridge logs a warning when
80% of the month's budget is spent, and one line when a limit refuses (at
most once an hour for `starts_per_hour`, once a day for either budget).

Challonge's free API tier allows 500 requests a month, so its example
definition sets 400 a month, 40 a day and 30 starts an hour. These limits
bound what one day can cost; they do not stop a drain. Ember IDs cost nothing
to make, so a determined actor can make throwaway ones and spend the day's
share every day, keeping that site's sign-in busy for real players until the
next UTC day. The rest of the month keeps its room for as long as a day's
share times the days left fits in it (10 days at Challonge's example
limits). The operator's levers are a smaller `daily_request_budget` or
`starts_per_hour`, a paid tier with a larger `monthly_request_budget`, and
the refusal lines in the log.

The bridge counts only its own requests, made with its OAuth application.
ember-brackets, the bracket connector, counts the requests it makes with an
organizer's key separately, in its own process. Whether a site counts the
two against one allowance depends on the site.

### Usernames

For a site whose lookups match usernames (Challonge's), the bridge keeps the
username the site gave when the account connected, compared as the
`username_rule` says, and lookups match on it; the site does not tell the
bridge when an account renames. When another account connects with a name
the bridge already holds, the older account stops answering for that name
(it stays connected, and answers for whatever name it next connects with).
What this cannot catch: a player who renamed is still found under the old
name, so if someone who never connected to Ember takes that name, a lookup
for it finds the player who gave it up. Comparing the name with the site does
not help, since both had it. Each answer carries `connected_at`, when the
bridge last saw the account with that name: a connector can refuse names
older than it trusts, or ask its players to connect again before an event.

A site whose lookups name user IDs (start.gg's) has none of this: its
usernames (start.gg gamer tags, which many players can share) are only
shown, and lookups match the user ID exactly.

The form a lookup matches is stored when an account connects. Changing a
live site's `username_rule` (its characters, length or case) or its
`lookup_by` therefore does not apply to accounts already connected: lookups
by username miss them (a site switched to `username` stored no usernames to
match) until each player connects that account again. Plan such a change
like a new site, and ask players to reconnect.

### Organizer sign-in

A site definition with an `organizer` block also lets a tournament organizer
sign in to run their events through Ember (docs/design/TOURNAMENT_DESK.md):

```json
"organizer": {
  "scope": "user.identity tournament.reporter",
  "refresh_url": "https://api.start.gg/oauth/refresh"
}
```

- `scope`: what the organizer sign-in asks for, in place of the site's
  `scope`, and in place of a `scope` in `token_params`. Neither start.gg
  nor Challonge sets scopes on the application's page: each sign-in asks.
- `refresh_url`: optional, where a refresh token is exchanged when not at
  `token_url`. On one of `hosts`, like every other URL.

`POST /v1/sites/<id>/organize` starts it (proof action `site.connect` on that
path), and it connects the account exactly as a player sign-in does. The
bridge then keeps the access and refresh tokens, sealed with its encryption
key, as the account's grant, and refreshes them a day before they lapse
when they are used; a refresh spends from a limited site's budget like a
sign-in. `GET` on the same path shows the grant's scope and expiry (never
the tokens), and `DELETE` (proof action `site.remove`) drops it and keeps
the account. Disconnecting the account or moving it to another Ember ID
removes the grant; a player sign-in with the same account keeps it.

#### Organizers' own events

With `desk` in `bridge.json` naming a `direct` connection
(`"desk": { "connection": "desk" }`), an organizer holding a grant runs
their events on that connection, and `ember-brackets` holds its provider
credential. The `organizer` block's `tournaments` says how the site lists
the organizer's events (both shipped definitions have one):

- `method`, `url`, `headers`, `body`: the request, as `user`'s, with the
  organizer's access token.
- `list_pointer` (empty for the whole answer), `events_pointer` (optional,
  each tournament's events), `id_pointer` and `name_pointer` (each event's),
  `tournament_name_pointer` (optional, shown before the event's name). The
  ID is what `ember-brackets` takes as `tournament`.

Routes, all with the organizer's player session:

- `GET /v1/organizer/tournaments?site=<id>`: the events the site lists for
  them (at most 100), each `{ "id", "name" }`. One request to the site,
  spent from a limited site's budget.
- `POST /v1/organizer/events` (proof action `site.connect` on that path),
  command `{ "site", "tournament", "settings" }`: runs a listed event.
  `settings` takes `rounds`, `hold`, `active_from`, `active_until` and
  `mark_underway` as `brackets.json` does, in at most 4096 bytes; the
  connector checks their values. An event runs once at a time, and an
  organizer runs at most 8.
- `POST /v1/organizer/drafts` (`site.connect`), command `{ "title" }`
  (1 to 100 characters): a draft, which runs and creates nothing. An
  organizer keeps at most 20.
- `POST /v1/organizer/events/<id>` (`site.connect`): changes what the
  command gives: `title`, `starts_at` (seconds, or null), `links` (at most
  12 `{ "label", "url" }`, https only), `settings`, and for a draft
  `site` with `tournament`, an event the site lists for them. Also
  `discord_webhook` (a Discord channel webhook URL, or null) and
  `post_results` (see Discord calls below).
- `POST /v1/organizer/events/<id>/create` (`site.connect`), command
  `{ "site", "fields" }`: creates a draft's event on the site with the
  organizer's grant, when its definition has `organizer.create`, and gives
  it to the draft with its page as the first link (`Bracket`). When the
  site refuses, the answer is 400 with `details.reason` `site_refused` and
  `details.site_said`, the site's own words (at most 300 characters,
  nothing of the request), and the bridge logs one line; a refused sign-up
  answers the same.
- `POST /v1/organizer/events/<id>/ready` (`site.connect`): a draft with its
  event and a start time becomes ready; it runs from that time.
- `GET /v1/organizer/events`: their events that have not ended, each with
  its `status` (`draft`, `ready` or `running`), title, start time, event,
  settings and links.
- `DELETE /v1/organizer/events/<id>` (proof action `site.remove` on that
  path): deletes a draft, or stops an event.

`organizer.create` in a site definition (Challonge's has one; start.gg's
API creates no events): `url`, `headers`, the JSON `body` to POST,
`name_pointer` where the title goes, `fields` (each a `pointer` into the
body and the `values` it takes; every pointer's parent object is in the
body; an empty value leaves the field to the site; `with` maps a value to
more pointers to set alongside it, such as the options object Challonge
requires with round robin and swiss), `id_pointer` into the answer for the event's ID as `tournaments`
lists it, and optionally `link_pointer` with `link_prefix` for its page.

The web desk signs organizers in without the game: `GET
/web/sign-in/<site>` sends the browser to the site's sign-in (its plain
`scope`), which comes back through the site's usual callback with its state
marked `web.`, so no second redirect URI is registered. The account must
already be connected to an Ember ID from the game; then the browser keeps a
session cookie (`ember_desk`, HttpOnly, SameSite=Lax, Secure over https)
that ends 12 hours after the sign-in, or after 2 hours unused. One network
address starts at most 10 web sign-ins an hour. The organizer routes above
take that cookie in place of a player session; every change also sends the
session's CSRF token in `X-Ember-CSRF`, which `GET /web/session` gives with
the Ember ID, and needs no proof. `POST /web/sign-out` ends it.

The game opens the desk too, in two ways, each marked `from_game` in
`GET /web/session`:

- In this PC's browser: `POST /v1/web-sessions/handoff` (player session,
  `site.connect` proof) answers a one-time `token`, lasting two minutes and
  kept only as a keyed hash; the browser trades it at `GET /desk/h/<token>`
  for the session cookie.
- On another device, the other way round, so a code seen on a stream is no
  use to anyone else: that browser's `POST /web/device` answers a six-digit
  `code` and sets a secret cookie, both for two minutes, and keeps what the
  browser is (browser and system, read from its user agent, which is not
  kept) and when it asked. The game first reads `GET
  /v1/web-sessions/device?code=…` (player session): `device`,
  `requested_at` and three `choices` from 10 to 99. The first Ember ID to
  read a code keeps it; from then on the browser's `GET /web/device`
  answers `pick`, one of those numbers. The game approves with `POST
  /v1/web-sessions/approve` (`{"code": …, "number": …}`, player session,
  `site.connect` proof); a wrong number ends the code (404, `details.reason`
  `number_wrong`). Once approved, the browser's next poll sets the session
  cookie, for the browser holding the secret only. One address asks at most
  10 codes or links in ten minutes, one Ember ID 10 handoffs, reads or
  approvals, one code 5 reads or approvals. No country is shown: the bridge
  has no address-to-country data, and looks nothing up elsewhere.

A session from a site sign-in alone is only as safe as the site account, so
the changes a stolen one must not make refuse it with 403 and
`details.reason` `needs_game_proof` ("Open the desk from Ember"): starting,
ending or readying an event, creating it on the site, and changing its
`discord_webhook`, `public` or `sign_ups`. A session opened from the game,
and the game itself, make them. Calls, holds, game decisions and draft
edits work from either.

The game lists the Ember ID's web sessions with `GET /v1/web-sessions`
(site, browser, `origin` `game` or `site`, `started_at`, `last_used_at`, and `new` until
`POST /v1/web-sessions/seen`), and `DELETE /v1/web-sessions` with a
`site.remove` proof signs them all out.

The desk itself is the page at `/desk` on the bridge, built in (`bridge/desk/`:
one page, its style and script, the emblem and Inter; no outside requests,
and a policy that allows none). It takes a TO from their linked accounts
(`GET /web/accounts`: each site's account, organizer grant and what a new
event there takes; `POST /web/organize/<site>` starts the organizer sign-in
in a new tab) through a draft, the bracket service and its settings, links
and ready, to the Discord post to copy. It is laid out for a phone first and
widens on a tablet or desktop.

`GET /v1/desk/brackets`, with the desk connection's provider credential,
lists every running event whose organizer still holds a grant, with the
site, the event, its settings and a fresh `access_token`. An event whose
organizer dropped their grant waits there until they sign in again.
`ember-brackets` with `"desk": {}` in its `brackets.json` runs them
(its README, "Configure"); under Docker it is the `brackets` service in
`server/ember/docker/compose.yml`.

After each read of a running event, `ember-brackets` sends its sets with
`PUT /v1/desk/events/{id}/sets` (at most 1024): every set of the bracket
and every set it tracks, with what it is doing about each. The
bridge keeps the latest and serves it with each Ember match's score, state
and players (by their account on the event's site):

- `GET /v1/organizer/events/{id}/sets`, to the event's organizer (game
  session or web desk), with Ember IDs and match IDs, and each set's
  `desk_state`: `waiting`, `not_ready`, `called`, `playing`,
  `needs_review`, `reporting`, `done`, `held` or `needs_organizer`.
  Each set with an Ember match also has the match's `revision` and its
  `games` (`attempt_id`, `outcome`, `state`), which a decision from the desk
  names (`POST .../matches/{match}/adjudications`).
- `GET /v1/public/events/{id}`, to anyone, only while the organizer has set
  `"public": true` on the event and has not stopped it: its title, start,
  links and sets without Ember IDs or match IDs, and `sign_ups_open`.

`/t/{id}` is that event's public page, a phone viewer of its live sets and
bracket that also lets a signed-in player sign up while sign-ups are open.
Its title and start (in UTC) are in the page itself for Discord's link
preview, and the desk's Discord post links to it once the event is public.
The web desk's Live step shows the same sets with the organizer's actions:
hold or let go, call again, and the games of a set to decide or void.

The whole bracket is read every 10 minutes (Challonge and start.gg; an
`http` service shows only its open sets), the open sets at every poll in
between, so a set decided on the site shows its new state within 10
minutes. On a paged site the whole read costs a request per page of
decided sets as well.

Discord calls: with a `discord_webhook` on the event (Server Settings,
Integrations, Webhooks in Discord; no bot to install), the bridge posts a
set's call to that channel when the desk creates the set's Ember match and
each time the organizer calls it again (`POST .../sets/{set}` with
`"call": true`): the round and phase, first to N, both players (pinged when
their Discord account is connected to their Ember ID; nobody else can be
pinged) and the match link. With `post_results`, the score of each set it
called is posted once the set is done. The webhook is kept sealed and read
back only masked (`{ "host", "id": "…1234" }`); each set's `call` says when
it was last posted and whether Discord has it (`pending`, `delivered`,
`failed`). A 429 is retried after Discord's `Retry-After`; a webhook
Discord no longer knows fails without retries.

Sign-ups through Ember: on an event Ember created on a site whose
definition has `organizer.participants` (Challonge's does), the organizer
sets `"sign_ups": true`. Until the event is ready, a player whose account
on that site is connected to their Ember ID signs up with
`POST /v1/events/{id}/sign-up` (game session with a `site.connect` proof,
or the web desk with its CSRF token), and the bridge adds them to the
site's event with the organizer's grant, under their site username and
marked `ember:<Ember ID>`, so `ember-brackets` finds them as it finds any
participant. Signing up again changes nothing. `DELETE` on the same path
(`site.remove`) takes them out again before the start; a removal the site
does not confirm keeps the sign-up so it can be tried again. The event list
shows `sign_ups` and the `signed_up` count. `organizer.participants` holds
`url` and `remove_url` (with `{tournament}`, and `{participant}` in the
second), `headers`, `body`, and `name_pointer`, `username_pointer`,
`marker_pointer` into the body and `id_pointer` into the answer.

## Testing the desk

Three layers, from fastest:

- **The page's own logic** (Discord post, tiers, bracket grouping, counts,
  title length), without a browser:
  `node --test 'server/ember/bridge/desk/test/*.test.js'`.
- **Local end to end**, with fakes and no real site: start the stack (a real
  bridge and connector, the fake Challonge, a stand-in Discord webhook, an
  organizer and four players), then drive Chrome through the whole desk:

  ```sh
  cd server/ember
  EMBER_E2E_OUT=/mnt/d/ember-build/e2e-out \
    cargo test -p ember-brackets --test desk_e2e_stack -- --ignored --nocapture &
  cargo build -p ember-bridge --example desk_game
  node ../../scripts/e2e-desk.mjs --local
  ```

  It signs in from Ember (a wrong number turned down, the right one signed
  in), creates a Challonge tournament in the four steps with tiers, sets the
  webhook and sign-ups, readies it, starts the bracket, decides a disputed game
  as a draw, gives one to player 1 and voids it, opens the bracket and the
  public page, and checks at 390 and 1440 px that nothing scrolls sideways
  and every button is at least 44 px. Pictures go to the out folder. Start a
  fresh stack for each run: sign-in rate limits are per bridge.
  `examples/desk_game.rs` plays the game's side: an Ember ID from a seed file
  signing what Ember signs, against any bridge.
- **Staging**: the same script with `--staging https://<bridge>`, on a real
  bridge and real Challonge. Its Ember ID seed lives in
  `~/tournament-lab/e2e/ember-id.seed`. Once, `--setup` prints the organizer
  sign-in to approve with the organizer's Challonge account. A run creates one
  Challonge tournament named `ember-e2e <time>` and removes its event
  afterwards; delete the Challonge tournament by hand. A Discord webhook in
  `~/tournament-lab/e2e/discord-webhook` is used when present.

## Bracket sites (Challonge, start.gg, your own)

The bridge knows players, matches and results, and names no site. Everything
specific to a service is an adapter around it:

- A sign-in site (above) lets players connect their account on that service,
  so the service's entries can be matched to Ember IDs.
- `ember-brackets` (`server/ember/brackets`) is a small service that runs as
  a `direct` connection with `lookup_sites` (and `discord_lookup` for
  start.gg's linked Discord accounts) and a `results_url`. For each
  tournament in its config it reads the open sets, finds both players,
  creates an `ember-room-v1` match with the round's games to win, marks the
  set underway, and reports the score when the bridge sends the result. It
  never writes over a set the organizer decided.
- Its adapters are `challonge`, `startgg`, and `http`. The `http` adapter
  calls a service of your own with a small JSON contract, so any bracket
  tool written in any language can be played in Ember without changes to
  Ember. The adapter interface and the `http` contract are in
  `server/ember/brackets/ADAPTERS.md`; setup, request budgets and limits are
  in its `README.md`.

A platform that would rather create matches itself keeps using the API in
this guide directly; the connector is one client of it.

## BluMint

A connection of kind `blumint` serves BluMint's game partner API (2.0.0) with
the connection's provider credential as `Authorization: Bearer`:

- `POST /v1/blumint/lookup` answers `discord` with the Ember IDs connected to
  those Discord accounts, and links each on the connection (`approved_via`
  `discord`) so matches can be created for it. An Ember ID already linked
  there by a code keeps that link.
- `POST /v1/blumint/matches` creates an `ember-room-v1` set from two teams of
  one, of `series.gamesToWin`, starting from the teams' `startingWins`, and
  answers `matchId` and the match's `play_url` as `matchUrl`. Every request
  under one `Idempotency-Key` answers the same match for a day, and the key
  is the match's `external_match_id` (`blumint:<key>`), so it makes no other
  match for as long as the bridge keeps that one. Refusals answer
  BluMint's registry codes at the top level (`invalid_request`,
  `match_unsupported`, `key_reused`, `player_unlinked` and `423`
  `player_busy`), never the bridge's error envelope.
- `POST /v1/blumint/matches/close` takes `{ idempotencyKey, reason }`: it
  ends the match made under that creation key, however old, and closes the
  key to every later creation. A key that made no match, its creations
  refused or none received, answers `200` as a match already over does,
  and is closed all the same.

The bridge reports each match to BluMint: a `status` with each player's
presence, under a rising `revision`, as each player opens or leaves the
match, presses Ready or takes it back (each claim's `ready`, sent to a
bridge that lists `matches.claim_ready`), as each game starts and as each
game ends with another to come; a
`progress` after each game won that does not end
the set; and the `result`, each team with its `wins`. A cancelled match is an `abort` with
the wins so far and `retryable: true`, which BluMint answers by asking for
the set again with those wins as `startingWins`. BluMint has no review step,
so a match that would go to `needs_review` (the two games disagree, a report
never arrives, a player's link ends) is cancelled instead. A match nobody
plays expires after a day at most and is an `abort` with `retryable: false`, so
BluMint's host takes it. Posts need BluMint's API key for the connection in
the integration secrets; a match that ends before the key is there is posted
once it is. Posts are retried for a day; the match's
`provider_delivery_state` says how its result went. BluMint takes one result
per match, so once the bridge starts posting it (`delivering`) an organizer
can no longer correct it. BluMint also refuses a report below the wins an
earlier one gave a team, so once a `progress` report has been posted an
organizer can no longer void a game of the set. A closed match is sent nothing more. `ember-bridge
blumint-register <bridge.json> <connection>` registers the three endpoints
with BluMint, handing over a new provider credential that is never shown.
Once all three are registered it revokes the callback credential an earlier
registration handed over; one that fails partway leaves it in place.
`BLUMINT_QUICKSTART.md` is the guide for BluMint's side.

A match BluMint asks for again from a score carries `starting_wins` in its
`match.completed` data, P1 first, and leaves it out at 0-0. Its `scores`
count those wins, so each player's wins are their starting wins plus the
games they won in this match, and `accepted_attempt_ids` lists only the
games played in it. EMBER-TB-001's `MatchCompletedData` schema does not list
the field, so a subscriber that validates events against
`docs/design/identity-bridge/schemas/core.schema.json` must allow it.

A `blumint` connection makes matches only through BluMint's create
callback; the generic match, lobby and tournament routes refuse it, so every
match on it is one BluMint knows.

BluMint's bodies are parsed as ordinary JSON, since its match settings carry
decimals; every other route keeps the strict profile.

## Connection settings

The bridge's operator gives a connection its settings in `bridge.json`, and
issues its credential as usual. Anyone who wants other settings, or none of
this, can run their own bridge and room supervisor. The contract is
`docs/design/INTEGRATION_PATHS.md`; in short:

```json
{
  "id": "night-bot", "kind": "direct", "environment": "production",
  "display_name": "Fight Night bot",
  "discord_lookup": true,
  "lookup_sites": ["challonge", "startgg"],
  "disputes": "restart",
  "results_url": "https://bot.example/ember/results",
  "rooms": { "max_open": 8 }
}
```

- `discord_lookup`: `POST /v1/players/lookup` with `{"discord": [...]}` (at
  most 32 Discord user IDs) answers the players connected to them, with their
  Ember ID and `participant_id`, and links them on the connection the way
  BluMint's lookup does. A link a sign-in approved that the player removes
  stays removed: no account the player has connected (Discord's or a
  sign-in site's) makes it again on that connection, until the player signs
  in again with one of them, which gives that account's consent back.
  Removing a link made from a code withdraws nothing, as before.
- `lookup_sites`: the sign-in sites (by ID) whose accounts the connection
  may find players by. The same lookup takes `{"accounts": [{"site": "...",
  "id": "..."}]}`, each `id` a username or user ID as that site's
  `lookup_by` says (Challonge usernames without case, start.gg user IDs
  exactly), and answers them in `accounts` (`site`, `id`, `user_id`,
  `ember_id`, `participant_id`, `connected_at`), left out when empty. An
  `id` the site's accounts cannot have finds no one, as a Discord ID that is
  no snowflake does, and the rest of the lookup still answers. Each list,
  and each site, needs its own setting; together they name at most 32
  accounts.
- `disputes`: `"review"` (the default) or `"restart"`, which cancels a match
  that would wait in `needs_review`. It is recorded with the connection the
  first time it is seen and never changes; use a new connection to change it.
  Where disputes restart, a game only one player reported as aborted or called
  off is settled after 20 seconds instead of a minute.
- `results_url`: each finished match is POSTed there as
  `io.ember.tournament.match.result.v1` (outcome `completed` with the scores
  and winner, `restart` for a cancelled match, or `expired` for a match nobody
  played, which is not a request to restart), signed exactly like a webhook with the
  connection's result secret, `webhook-id` `res_<match id>`. `ember-bridge
  result-secret` makes a secret, which the operator gives the platform and
  stores with `set-integration-secret.sh result <connection-id>`. Answer 2xx
  once stored, or 409 if you have it already. 429 and server failures are
  retried for a day; other 4xx answers refuse the result for good. Once
  sending starts a result can no longer be corrected. The URL
  follows the same rules as a webhook destination.
- `rooms`: with the provider credential, `POST /v1/rooms` opens a public room
  for a player linked on the connection (`creator`: their `participant_id`
  and Ember ID), who goes in first and moderates it. It counts as their one
  open room, and the connection has at most `max_open` open. `GET /v1/rooms`
  lists the connection's open rooms, `GET /v1/rooms/{id}` reads one, and
  `POST /v1/rooms/{id}/close` closes one. Each answer carries the room's
  `state` (`waiting`, `open`, `closed`) and `join_url`
  (`https://embernetplay.link/r#<bridge id>/<room id>`), which opens the room
  in Ember. That page goes live with the embernetplay.link update for the
  1.1 release; until then use `ember://room/open?bridge=<id>&room=<id>` or
  Ember's Paste room link. A creator who already has a room the connection opened is refused
  `room_limit` with that room's ID in `details.room_id`. The connection's
  stream carries `room.created`, `room.opened`, `room.changed` and
  `room.closed` (with `reason` `closed_by_connection` or `ended`) for its
  rooms only.

A bridge with these lists `players.lookup` and `results.signed` in its
capabilities `features`, and `rooms.connections` when public rooms are on.
BluMint's connection kind has the first three settings built in.

## Run a lobby (first-to-N, king of the hill)

A lobby is a queue on your connection that plays one first-to-N set after
another, for a stream or a community night. Each set is an ordinary match, so
the organizer records its games exactly as above and every match event still
fires. Lobbies go beyond the EMBER-TB-001 package; `"lobbies"` in the
capabilities `features` and `lobby_rotations` say a bridge has them.

- `POST /v1/lobbies` (provider, `Idempotency-Key`) takes `external_lobby_id`,
  `games_to_win` (1 to 10), `rotation` and `required_build_id`, with
  optional display `metadata` such as `title`.
- `POST /v1/lobbies/{id}/queue` adds a linked player who asked to play:
  `{"participant_id": "...", "ember_id": "..."}`. Nobody is queued without
  asking, and a player can be in one open lobby per connection.
- When two players are available the next set starts at once. When a set
  completes the seats rotate in the same transaction:
  - `winner_stays` (king of the hill): the loser goes to the back of the queue.
  - `loser_stays`: the winner goes to the back.
  - `both_rotate`: both go to the back, winner first.

  The next queued players sit down; with nobody waiting the same two play a new
  set. A queued player with another active match on your connection keeps their
  place and is skipped until it ends. A seated player given another match while
  waiting for an opponent keeps the seat, and the set waits for that match. When
  such a match completes or is cancelled the lobby picks up where it left off,
  with a `lobby.queue.changed` event whose reason is `player_available`.
- `POST /v1/lobbies/{id}/queue/{participant_id}/leave` takes a player out.
  Leaving a seat cancels the running set; the other player stays seated. A
  player whose link ends leaves the same way, with the reason
  `identity_unlinked`.
- `POST /v1/lobbies/{id}/close` (provider or organizer, with a reason and the
  lobby revision) cancels the running set and empties the lobby.
- `GET /v1/lobbies/{id}` shows the seats, the queue in order, the running set,
  the current streak (who has won sets back to back, and how many) and the
  lobby's `standings`: everyone who has finished a set there, ranked by sets
  won, then fewest lost, then game difference, then best streak.

A lobby set cannot be cancelled through `/v1/matches/{id}/cancel`, and a
correction that would reopen a finished lobby set is refused, because the
lobby has already moved on. `external_match_id` values starting with `lobby:`
are reserved for lobby sets.

Lobby events are `lobby.created`, `lobby.queue.changed`, `lobby.set.started`,
`lobby.set.completed` (winner, loser, scores, streak and the queue after the
rotation) and `lobby.closed`. Players who have joined a lobby can read its
events with their own session; those events name players by Ember ID only,
and each set's match events stay visible to that set's two players.

The game's own rooms follow the same set lengths and rotations, but today they
run on their own: a room table does not report to a bridge lobby yet.

## Run a tournament (brackets and round robin)

A tournament is a bracket the bridge runs for you: players register, you start
it, and every set is created as a match the moment both of its players are
known and free. Results are entered game by game like any other match, and the
bracket moves on in the same step.

- `POST /v1/tournaments` (provider, `Idempotency-Key`) takes
  `external_tournament_id`, `format` (`single_elimination`,
  `double_elimination` or `round_robin`), `games_to_win`, and optionally
  `finals_games_to_win` (the final, or the grand final and its reset),
  `grand_final_reset` (double elimination, on by default), `required_build_id`
  and display `metadata` such as `title`.
- `POST /v1/tournaments/{id}/entrants` registers a linked player who asked to
  enter, until the start. Elimination brackets take up to 128 players, a round
  robin up to 32.
- `POST /v1/tournaments/{id}/start` (provider or organizer, with the
  tournament revision) seeds the players and starts the first sets. Pass
  `seeding` with every registered `participant_id`, top seed first, or leave
  it out for registration order. Brackets use standard seeding, so the top
  seeds get the byes when the count is not a power of two.
- `POST /v1/tournaments/{id}/entrants/{participant_id}/withdraw` (provider or
  organizer) takes a player out. Before the start that is all; once running, a
  set they are playing is cancelled and every set they have left goes to their
  opponent as a walkover. A player whose link ends is withdrawn the same way.
- `POST /v1/tournaments/{id}/cancel` (with a reason and the revision) cancels
  every running set.
- `GET /v1/tournaments/{id}` shows the entrants with their seed and, at the
  end, their placement; every set with its label (for example "Winners
  semifinals" or "Grand final"), status, players, match and score; and for a
  round robin the standings.

Each set's match carries `round_label` and `tournament_node` in its metadata,
plus your tournament metadata, so `match.created` and `match.completed` read
like any other match. A set whose player is still busy in another match on
your connection waits and starts as soon as that match ends. A lobby does not
seat a player whose bracket set is waiting for them: the bracket comes first.

Placements follow how far each player got, so players who go out at the same
stage share a place (two 3rds in single elimination, for example). A round
robin ranks by sets won, then game difference, then games won, then the
head-to-head result when exactly two players are level, then seed; a walkover
counts as a set won.

To fix a wrong result, void the game as usual. When that leaves nobody with
enough wins, the set reopens and takes back what its result fed: later sets that have no games yet are cleared and
their matches cancelled, and are created again once the set is decided. A
correction is refused once a later set it fed has a game recorded, and once
the tournament is over. Bracket sets cannot be cancelled through
`/v1/matches/{id}/cancel`, and `external_match_id` values starting with
`tournament:` are reserved.

Tournament events are `tournament.created`, `tournament.entrants.changed`,
`tournament.started`, `tournament.match.started`, `tournament.match.completed`
(who advanced where, and who was eliminated), `tournament.match.reopened`,
`tournament.completed` (the placements) and `tournament.cancelled`. Entrants
can read a tournament and its events with their own session. Tournament events
name players by Ember ID only, a player reading the snapshot sees only their
own `participant_id`, and each set's match events stay visible to that set's
two players. A correction is refused for a set whose player has withdrawn.

## Player records

`GET /v1/players/{ember_id}/record` adds up a player's finished matches: sets
played, won and lost, games won, lost and drawn, cancelled matches, and the
most recent matches (`?limit=`, 10 by default, at most 50) with the opponent,
the score, `round_label` and the lobby a set belonged to. A provider sees the
matches on its connection, an organizer every match in its tenant, and a player
only their own record, without other players' account IDs.

Records are worked out from the accepted games each time they are read, so a
voided game or other correction shows straight away. A set a platform asked
for again from a score (BluMint's `startingWins`) counts that score in the
set's result and its recent score, but only the games played in it in games
won and lost. Lobby standings work the same way; only a lobby player's best
streak is kept as sets are played.

## Receive events

Events are CloudEvents 1.0 with an `emberseq` cursor, for example
`io.ember.tournament.match.completed.v1`. Read them in any of three ways:

- **Webhooks.** `POST /v1/webhook-subscriptions` with an `https` URL and the
  event types you want. The response holds the signing secret once. Each
  delivery carries `webhook-id`, `webhook-timestamp` and `webhook-signature`
  (Standard Webhooks, HMAC-SHA256 over `id.timestamp.body`). Verify against the
  raw body before parsing, reject timestamps more than five minutes off,
  deduplicate on the event ID, and answer 2xx only once the event is stored.
  Failed deliveries are retried for up to 24 hours. `rotate-secret` signs with
  both secrets for an overlap period you choose.
- **Polling.** `GET /v1/events?after=<cursor>` returns events after a cursor.
- **SSE.** `GET /v1/events/stream`, resuming with `Last-Event-ID`.

Providers see their connection's events, organizers their tenant's match
events, and players their own matches, links, lobbies and tournaments.

## Discord and Twitch

`server/ember/notifier` is a ready-made subscriber that posts news to a
Discord channel and to Twitch chat. With a `bot` section it is also a room
bot: `/room` in Discord and `!room` in Twitch chat open a public Ember room
and answer with its link. Configure it with a JSON file:

```json
{
  "listen": "127.0.0.1:8790",
  "bridge_origin": "http://127.0.0.1:8787",
  "secrets": ["env:EMBER_WEBHOOK_SECRET"],
  "database": "notifier.sqlite3",
  "discord": {
    "webhook_url": "env:DISCORD_WEBHOOK_URL",
    "interactions": {
      "application_id": "your application id",
      "public_key": "64 hex characters from the Discord developer portal",
      "bot_token": "env:DISCORD_BOT_TOKEN"
    }
  },
  "twitch": {
    "client_id": "your-app-client-id",
    "token": "env:TWITCH_TOKEN",
    "broadcaster_id": "channel user id",
    "sender_id": "bot user id",
    "room_creator": { "participant_id": "par_...", "ember_id": "emb1_..." }
  },
  "bot": {
    "bridge_api": "https://bridge.example",
    "credential": "env:EMBER_PROVIDER_CREDENTIAL",
    "build_id": "the build rooms are created for",
    "capacity": 8,
    "room_name": "Fight Night"
  },
  "names": { "emb1_...": "Player name" }
}
```

`discord`, `twitch` and `bot` are each optional. Without `discord.webhook_url`
nothing is posted to a channel and only `/room` is served.

Subscribe it to `match.created`, `match.score.changed`, `match.completed`,
`match.cancelled`, `match.expired` (announced as "Match expired, not played."),
`match.needs_review` and `match.corrected`, for lobbies
`lobby.created`, `lobby.set.completed` and `lobby.closed`, for tournaments
`tournament.created`, `tournament.started`, `tournament.match.completed`,
`tournament.completed` and `tournament.cancelled`, and for rooms a connection
opened `room.created`, `room.opened` and `room.closed`, then run
`cargo run -p ember-notifier -- serve notifier.json`. Room news names the room,
how full it is and its link (`room.closed` says how it ended and has no link).
The Twitch token is a user access token for the sender account; its scopes
are listed under "Twitch `!room`" below. Discord posts never mention anyone.
Players show by name when `names` lists them, otherwise by their short
fingerprint.

### The room bot

`bot` is the provider credential of a bridge connection that has `rooms` and
`discord_lookup` in its settings (see `docs/design/INTEGRATION_PATHS.md`):

- `bridge_api`: the bridge's origin.
- `credential`: the provider credential, or `env:NAME`.
- `build_id`: the build rooms are created for.
- `capacity`: players per room when a command gives none (default 8).
- `room_name`: the name when a command gives none (default "Ember room").

The bot finds a person by Discord ID (`POST /v1/players/lookup`), opens the
room for them (`POST /v1/rooms`) and reads one back (`GET /v1/rooms/{id}`).
Someone who already has a room the connection opened gets that room's link
again instead of a refusal.

### Discord `/room`

Set the application's Interactions Endpoint URL to
`https://<your host>/discord/interactions`, which the notifier serves on its
`listen` address (put it behind HTTPS). `discord.interactions` takes the
application's `application_id` and `public_key`; every request is checked
against the key (Ed25519 over the timestamp and the body) and refused with
401 when it does not match. `ember-notifier discord-register notifier.json`
registers the command, using `bot_token` (or `env:NAME`); with `guild_id` set
it registers for that server only, which Discord applies at once.
`api_base` (default `https://discord.com/api/v10`) is for tests.

`/room` takes an optional `name` and `capacity`. Discord allows three seconds
for an answer, so the notifier answers at once with a deferred reply only the
person who ran the command sees, then looks them up and edits that reply when
the bridge has answered. A slow bridge does not cost the interaction; Discord
keeps the reply editable for fifteen minutes:

- Found: a room is opened for them and its link is posted to the channel for
  everyone to see. Their own reply becomes a short note that the room is open.
- Not connected: only they see a reply that sends them to
  `https://embernetplay.link/start#<bridge id>` to connect Discord in Ember.
- Any other refusal (unsupported build, a name the rules refuse, the bridge
  not reachable): a short plain message, visible only to them.

Opening a room is given up to 45 seconds, since the bridge may need that long
to start a host.

### Twitch `!room`

With `twitch.room_creator` (the streamer's linked player on the connection)
and `bot` set, the notifier opens an EventSub WebSocket session
(`eventsub_url`, default `wss://eventsub.wss.twitch.tv/ws`) and subscribes to
`channel.chat.message` for `broadcaster_id`, read as `sender_id`. No public
endpoint is needed. A `!room` message from the broadcaster or a moderator
opens a room for the creator and posts its link in chat as the sender; a
second `!room` while the room is open posts the same link. Messages from
anyone else are ignored. The session follows Twitch's reconnect requests and
reconnects with a growing delay after a drop or a silence longer than the
keepalive timeout.

The token is the sender account's user access token (EventSub WebSocket
subscriptions need a user token whose user is the subscription's `user_id`).
Twitch's reference for `channel.chat.message` asks for these scopes:

- `user:read:chat`, to read chat.
- `user:write:chat`, to post the news and the link.

Twitch lists `user:bot` and `channel:bot` (or the sender being a moderator in
the channel) only for subscriptions made with an app access token, which this
notifier does not use. The reference at
`https://dev.twitch.tv/docs/eventsub/eventsub-subscription-types/` is the
authority if that changes. A subscription Twitch refuses (a missing scope
shows as status 401 or 403) is logged by status and retried with the same
delay as a dropped session.

## TypeScript SDK

`sdk/typescript` wraps the API for platform developers and includes
`verifyWebhook` and `parseEvent`. See its README.
