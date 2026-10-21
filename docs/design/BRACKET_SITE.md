# Bracket site

Status: design. Branch `fraccie/bracket-site` on
`fraccie/tournament-integrations`. Not reviewed. Code in
`server/bracket-site/` (README there for what runs today).

Ember's own bracket service, for events that need no Challonge or start.gg.
Its community lives on Discord, so a player is their Discord account, linked
to their Ember ID through the bridge. The site plugs in the way Challonge
does: a sign-in site on the bridge for the desk and sign-ups through Ember,
and the connector's `http` adapter for the sets. Everything an organizer or
player can do goes through one API, so the game's desk, the web desk, the
Discord bot and (later) the V2 web and mobile pages all do the same thing.

## 1. Decisions

- **Discord is required.** A site account is a Discord account; the site
  signs everyone in with Discord OAuth. Its Ember ID is the one the bridge's
  Discord lookup (`lookupPlayers`, `discord_lookup`) finds. There are no
  site passwords and no entrant without Discord.
- **The site is the event's truth.** States, sign-ups, check-in, bracket
  and results live on the site. The bridge keeps what it keeps for any site
  (grants, desk drafts, sets the connector reports, Discord calls).
- **Sites stay data.** The bridge learns the site from
  `deploy/sites/ember-bracket-site.json`, like Challonge. What the desk
  needs that Challonge's definition cannot say (state, transitions) is
  added to the definition format as optional fields, usable by any site.
- **Organizers are the bridge's.** Tournament Mode is invite-only (vetted
  streamers, single-use invites): only an Ember ID the bridge gives
  organizer access can sign in to the site as an organizer, since only the
  bridge holds the site's OAuth client secret and asks for the organizer
  scope. The site creates tournaments only for an organizer token (and its
  operator), and an organizer manages only the tournaments that name them.
  Players need no invite.
- **One bot.** A Discord application has one interactions URL, already the
  notifier's (`/room`). The site's commands and buttons go there too, and
  the notifier calls the site's API as the Discord user who pressed them.
- **No UI work now.** The API and the desk contract come first; the
  existing web `/desk` and the game's organizer screen get only the rows a
  new state or action needs. The V2 pages are built on the same API later.

## 2. States

| State | Means | Who moves it on |
|---|---|---|
| `draft` | Being set up; nothing public | organizer |
| `planned` | Date, format and rules set; public page and announcement | organizer |
| `sign_ups` | Players sign up (web, Discord, Ember) | organizer, or `sign_ups_from` |
| `check_in` | Signed-up players confirm they are here | organizer, or `check_in_from` |
| `live` | Bracket made and played | organizer starts it |
| `finished` | Final decided; placements fixed | the site, on the last result |
| `archived` | Read-only, out of lists; counts for its league | organizer, or 30 days after `finished` |
| `cancelled` | Stopped before the end; no placements | organizer, from any state but `archived` |

Transitions: `draft → planned → sign_ups → check_in → live → finished →
archived`, and back one step before `live` (`sign_ups → planned` closes
sign-ups, `check_in → sign_ups` reopens them). Check-in is optional: `sign_ups
→ live` skips it. `live → check_in` resets the bracket (every result lost),
only while no set is decided, as Challonge's reset. Each state change is
an event row (who, when, from, to), the organizer's log and the source of
Discord announcements.

Entering `live` seeds those checked in (or signed up, without check-in),
by join order, random, or an organizer's order, or by league points (§4).
Late changes while `live`: disqualify (the opponent advances, later sets
too), replace a no-show before their first set, correct a result while the
next set is not decided.

Times (`starts_at`, `sign_ups_from`, `check_in_from`, `check_in_until`)
are optional; with one set, the site moves on by itself and says so in the
log. Without check-in by `check_in_until`, a player is dropped from the
bracket.

## 3. Sign-ups

Every path ends in the same call, `POST /tournaments/{id}/entrants` as the
player, and the same rules: the tournament is in `sign_ups`, and there is
room (`cap`). A player whose Discord account is not connected to an Ember
ID yet is signed up all the same, and told how to connect it (Ember: Online
play, Ember ID, Discord); the entry waits for the link. At `start` (and at
every reseed) the site looks the waiting entries up once more and drops any
still not linked, before seeding. Leaving is the same call with `DELETE`.

- **Web.** The public page `/t/{id}`: Sign up with Discord, one OAuth
  round trip, then Sign up / Leave / Check in.
- **Discord.** The notifier posts the announcement with Sign up and Check
  in buttons and takes `/tournament signup|leave|checkin [id]`. Discord
  signs each interaction, so the user ID in it is the player; the notifier
  calls the site with its own bot credential and that ID (`X-Discord-User`),
  which the site takes only from that credential.
- **Ember.** The bridge's sign-ups through Ember (037): the event's site
  has `organizer.participants`, so the bridge adds the player under their
  account on the site, which is their Discord ID. That needs the player to
  have linked the site in Ember once, unless the definition says
  `"accounts": "discord"`: the site's accounts are Discord accounts, so
  the bridge signs the player up under the Discord account connected to
  their Ember ID, with no second link.

Check-in takes the same three paths (`POST /tournaments/{id}/check-in`).

## 4. Leagues

A league is a series of events with standings across them.

- `leagues (id, title, owner, discord_guild)`; organizers by Discord ID.
- `seasons (id, league, title, starts_at, ends_at, points, best_of)`:
  `points` maps a placement to points (`{"1": 100, "2": 70, "3": 50, "5":
  30, "9": 15, "*": 5}`, `*` for any other finisher); `best_of` counts only
  a player's best N events in the season (null: all).
- A tournament has an optional `season`. Its placements, once `finished`
  (single elimination: 1, 2, 3, 5, 9, ... by the round lost), add to the
  season's standings: points, then events won, then head-to-head sets.
- `GET /leagues/{id}/seasons/{season}/standings` is computed, not stored.
- Seeding by points: a season's event can be seeded by standings at its
  start, ties by join order.

Phase 1 builds single events only, with `season` stored and ignored; the
tables above come with leagues.

## 5. Discord

- **Announcements.** On `planned`, `sign_ups`, `check_in`, `live` and
  `finished` the notifier posts to the league's or event's channel: the
  desk's announcement text (TOURNAMENT_DESK.md §4a) with buttons, and the
  podium at the end.
- **Calls and results.** The bridge's Discord calls (036) already post each
  set's call with both players pinged and the match link, and the score
  with `post_results`. The site sets the event's `discord_webhook` from the
  desk as today.
- **Private match links.** The notifier DMs each player their match link
  and a reminder when a set waits on them; a player who closed DMs gets the
  channel ping only. Needs the bot to read the bridge's match events (it
  has the room events already).
- **Organizer control.** `/tournament call|hold|dq|report` for organizers
  of the event (by Discord ID on the site), the same API calls as the desk.

## 6. Plugging in

### Bridge (`deploy/sites/ember-bracket-site.json`)

A sign-in site whose authorize, token and user URLs are the site's own
OAuth endpoints (the site signs the user in with Discord behind them):

- `user`: `GET /oauth/me`, `id_pointer /discord_id`, `username_pointer
  /discord_id`; `lookup_by` the ID.
- `organizer.tournaments`: `GET /tournaments?mine=1`.
- `organizer.create`: `POST /tournaments` with `fields` for format, cap,
  check-in, sign-ups; `start_url`, `reset_body`, `delete_url` as Challonge.
- `organizer.participants`: `POST`/`DELETE /tournaments/{tournament}/entrants`.

New optional fields, for any site: `organizer.tournaments.state_pointer`
(the desk shows the site's state) and `organizer.transitions` (by name: a
URL and body), which the desk offers as actions when the event's state
allows them. The bridge's own draft/ready status stays; a site with
`transitions` is ready once it is past `draft`, and sign-ups through Ember
follow the site's state instead of the bridge's `draft`.

### Connector

Desk events are run only on `challonge` and `startgg` today
(`brackets/src/desk.rs`). It gains `desk.services` in `brackets.json`
(`{ "ember-bracket-site": "https://brackets.example" }`): a desk event on
such a site runs on the `http` adapter with `api_base`
`<base>/tournaments/<tournament>/ember` and the organizer's access token
from `/v1/desk/brackets` as the bearer, which the site checks belongs to an
organizer of that tournament. Configured `http` tournaments keep working
with the connector token.

Optional contract addition: `GET /sets` without `state` answers every set,
so the desk's bracket view of an `http` service is whole, as it is for
Challonge.

### Site

- OAuth server for the bridge (authorization code, refresh), signing in
  with Discord; tokens hashed at rest, organizer scope separate.
- Organizers per tournament (owner plus co-organizers by Discord ID).
- The states, sign-ups, check-in and event log above; the contract as now.

## 7. Game and web desk

The desk already lists a site's events, creates, starts and deletes them,
and shows sets. They need:

- the event's state from `state_pointer`, as a row in the event's header,
  and the actions `transitions` allow, as buttons (game: one row each,
  focusable, its state spoken, UI controls rules);
- in the game's linked accounts, the site appears like Challonge, from its
  definition: no game code names it.

New strings in all 14 `locales/*.po`. Nothing else in the game changes.

## 8. Waits for V2

The public event page, the organizer's state timeline, league standings
pages, the bracket drawn as a tree, and phone layouts. Until then the site
serves a plain read-only page and the web desk its current steps.

## 9. Build order

1. Site: states, event log, organizers, check-in; contract unchanged.
2. Site: OAuth server with Discord sign-in; tokens for the bridge.
3. Site definition `ember-bracket-site.json`; bridge `state_pointer` and
   `transitions`; sign-ups through Ember on the site's state.
4. Connector: `desk.services`, organizer token on the contract; e2e test of
   a desk event on the site.
5. Notifier: `/tournament` commands and buttons, announcements, DMs.
6. Desk and game rows for state and transitions, with the strings.
7. Leagues and seasons.

## 10. Operator

The user runs the site for now, with its operator token; who runs it later
is to be agreed with Katie.

## 11. Deployment

The public origin is `https://brackets.ember.fraccie.be`: Nginx Proxy
Manager on taelman ends TLS and proxies to `http://10.40.0.247:8490`, so
the site listens on `BRACKET_SITE_LISTEN=0.0.0.0:8490` there (beside the
nightly bridge on 8470, the connector on 8791 and the staging website on
8480). The proxy reaches it as 10.10.10.1 and sets `X-Real-IP`; any
per-address limit the site adds reads that header from 10.10.10.1 only, as
the bridge's `trusted_proxies` does. The site definition's URLs and
`hosts` name that origin, and the connector's `desk.services` and `api_base`
take it as the base.
