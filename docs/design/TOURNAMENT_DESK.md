# Tournament desk

Status: design. Branch `fraccie/tournament-integrations` on `feat/challonge`
(#112). Not reviewed. Mockups (in Ember's branding, `src/ui/Theme.cxx`):
https://claude.ai/artifact/Q2nbseP2aJFAqoGngGoDYb (desk, correction, linking,
replays, watch, bracket).

A tournament organizer (TO) runs an online USF4 event on start.gg or
Challonge from one screen in Ember. They sign in to the site once, pick the
event, and Ember creates every match, calls the players, reports the scores
and keeps the replays, while the TO watches the queue and fixes what goes
wrong.

## 1. Goal and decisions

Goal: a TO with an Ember ID and a start.gg or Challonge event can run it
through Ember without an operator setting anything up for them.

Decisions:

- **The TO signs in to the site, not to the operator.** Today
  `ember-brackets` reads each tournament from `brackets.json` with a token
  file the operator writes. The desk replaces that, for TOs, with an OAuth
  sign-in on the bridge's existing sign-in sites (#112), asking for the
  site's organizer scopes. A static `brackets.json` keeps working.
- **The bridge keeps the TO's token, sealed.** Player sign-in drops the
  access token once it has read who signed in. Organizer sign-in keeps the
  access and refresh tokens, sealed with the bridge's encryption key
  (`Keys::seal`, the one room invitations and webhook secrets use), and
  refreshes them before they expire. Removing the organizer access, or the
  site account, deletes them.
- **One sign-in does both.** An organizer sign-in also connects the site
  account to the Ember ID, as a player sign-in does, with the same browser
  confirmation when the account belongs to another Ember ID. The grant is
  stored only once the account is this Ember ID's.
- **Sites stay data.** A site definition gains an optional `organizer`
  block: the scope to ask for and how to list the TO's tournaments. A site
  without it offers no organizer sign-in. No site is named in code.
- **Self-service events run on one connection.** The bridge config names a
  connection (`desk.connection`) for desk events; `ember-brackets` holds its
  provider credential as it does for a static bracket. The bridge allows a
  TO to act only on their own events.

## 2. Bridge

### 2.1 Site definition

```json
"organizer": {
  "scope": "user.identity tournament.reporter",
  "tournaments": {
    "method": "POST",
    "url": "https://api.start.gg/gql/alpha",
    "body": { "query": "..." },
    "list_pointer": "/data/currentUser/tournaments/nodes",
    "id_pointer": "/slug",
    "name_pointer": "/name"
  }
}
```

Challonge asks for `me tournaments:read matches:read matches:write
participants:read`. Neither site asks for these on its app page: scopes go
in each authorize request.

### 2.2 Routes (player session, proof as for sign-in)

| Route | Does |
|---|---|
| `POST /v1/sites/{site}/organize` | Starts an organizer sign-in; answers `authorize_url` like `start` |
| `DELETE /v1/sites/{site}/organize` | Deletes the grant; the TO's events wait until they sign in again |
| `GET /v1/sites/{site}/organize` | Whether a grant is held, its scope and when it was refreshed |
| `GET /v1/organizer/tournaments?site=` | The TO's tournaments on the site, read with their token |
| `POST /v1/organizer/events` | Runs an event: site, tournament, round rules, hold |
| `GET /v1/organizer/events` | The TO's running events |
| `DELETE /v1/organizer/events/{id}` | Stops running it |

The callback stays `/v1/sites/{site}/callback`; the sign-in row says whether
it is an organizer sign-in.

For `ember-brackets` (provider credential of `desk.connection`):

| Route | Does |
|---|---|
| `GET /v1/desk/brackets` | Running events, each with a fresh access token for its site |

### 2.3 Storage (migrations 029 and 030; 028 is match watch)

- `sign_ins.organizer` (0/1) and `sign_ins.grant` (sealed token, held only
  while the browser asks to move an account).
- `organizer_grants (site, ember_id, sealed, scope, expires_at, refreshed_at)`.
- `organizer_events (id, ember_id, site, tournament, name, settings,
  created_at, ended_at)` (migration 030), one running event per site and
  event.

## 3. ember-brackets

A `desk` source next to `tournaments`: each poll it reads
`/v1/desk/brackets` and runs those events as it runs a configured one, with
the token the bridge hands it (never written to its own database).

## 4. Game

- Online play > Ember ID > linked accounts: each site with an organizer
  block shows **Organizer access**, which opens the site's consent page.
- Tournament desk: pick an event from the TO's tournaments, set round
  lengths, start. Then the sets queue (mockups: TO desk screens).
- Bracket view: the event's winners and losers brackets by round, per
  phase, each set with its players, score and desk state; selecting one
  opens it on the desk. It is drawn from the sets the bridge already reads,
  so it needs every set of the phase, not only the open ones (commit 2's
  read adds the rest).
- A nightly build lists the nightly bridge (`ember-bridge.fraccie.be`).

## 5. Commits

1. Bridge: organizer sign-in and sealed grants, with refresh.
2. Bridge: organizer events and the TO's tournament list.
3. ember-brackets: run the desk's events.
4. Game: organizer access and event pick.
5. Game: the desk's sets queue, bracket view and actions.
6. Web desk: pages on the bridge in the embernetplay.link style
   (`server/ember-short/deploy/site/assets/site.css`) where a TO signs in
   with a site account, creates an event (Challonge; start.gg's public API
   has no tournament creation, so there it picks an existing one),
   configures and manages it with the game desk's actions.
7. Mobile mini-viewer: the bracket and live sets as a read-only phone
   layout of the web desk.
8. Manual: the tournament mode guide.
