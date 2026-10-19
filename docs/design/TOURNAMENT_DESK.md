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
| `PUT /v1/desk/events/{id}/sets` | The connector's latest view of an event's sets (033) |

Discord calls (036): an event's `discord_webhook` (sealed) gets each set's
call when the desk makes its match or the TO calls it again, with players
pinged by their Ember-connected Discord and the match link; `post_results`
adds the score of each called set when it is done (`delivery::calls`).

Sign-ups through Ember (037): `POST`/`DELETE /v1/events/{id}/sign-up`
adds a player whose account on the event's site is connected to the site's
event as a participant under that username (`organizer.participants`), so
the connector finds them by username as before. Open while the organizer
has `sign_ups` on and the event is a draft.

To the organizer, `GET /v1/organizer/events/{id}/sets` serves those sets
with each Ember match's score, state and players; `GET /v1/public/events/{id}`
serves them to anyone while the event is `public` (the mobile viewer).

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

## 4a. Creating a tournament on the Ember website

Ember does not run brackets: the bracket service does (Challonge or
start.gg). The Ember website is where a TO puts one tournament together and
keeps everything about it in one place. The steps, in order:

1. **Linked accounts.** The page lists the TO's sign-in sites and whether
   each holds organizer access; anything missing has its sign-in button
   there. Nothing else is offered until one bracket service is linked.
2. **New tournament.** A draft: a title is all it needs. Date, time and
   everything below can stay empty and be filled in later; a draft runs
   nothing and creates nothing on any site.
3. **Bracket service.** Challonge or start.gg, from the services the TO has
   organizer access to.
4. **Service settings.** What that service lets Ember set:
   - Challonge: format (single or double elimination, round robin, swiss),
     grand final reset, third place match, signup open or closed, and
     whether players register through Ember. When the TO saves this step,
     Ember creates the tournament on Challonge with their grant
     (`tournaments:write`; adding players needs `participants:write`) and
     keeps its URL.
   - start.gg: its public API cannot create tournaments or events, so the
     page links to start.gg's own create page and then lets the TO pick the
     event from their list (`GET /v1/organizer/tournaments`).
   - Both: first-to per round and phase (the `rounds` rules), and whether
     sets on stream are held.
5. **Links.** Any number of labelled links, kept with the tournament:
   bracket page (filled in from the service), Matcherino, Twitch or other
   stream, signup or subscription page, rules, Discord server.
6. **Ready.** When the required fields are filled (date, service, settings),
   the tournament can be marked ready, which makes it a running desk event
   at its start time (or at once).
7. **Discord announcement.** For a ready tournament the page builds one
   message with everything in it, for the TO to copy and paste:

   ```
   **[Tournament title]** — USF4 on Ember
   📅 <t:1793000000:F> (<t:1793000000:R>)
   🎮 Double elimination · pools FT2 · top 8 FT3
   🏆 Bracket: <bracket url>
   📝 Sign up: <signup url>
   📺 Stream: <twitch url>
   💰 Matcherino: <matcherino url>
   Play on Ember: https://embernetplay.link
   ```

   Discord's `<t:…>` timestamps show the time in each reader's own zone.
   Lines with no value are left out. The text is built in the page, not
   stored.

Storage: `organizer_events` (migration 031) gains the draft fields:
`title`, `starts_at` (nullable), `links` (JSON list of label and URL),
`status` (`draft` or `ready`), and `tournament` is nullable until a service
event is created or attached. Running is not stored: a ready event runs once
`starts_at` has passed (or at once without one), and ended is `ended_at`. A
draft has no site calls and is invisible to `ember-brackets`.

Web sign-in: the desk's pages are served by the bridge, so the TO signs in
there with a site account their Ember ID already holds (linked from the
game), through the site's existing callback (state marked `web.`, so no new
redirect URI to register). The browser keeps a session cookie; changes carry
a CSRF token instead of the game's proof, since the browser holds no Ember
key.

## 4b. Replay packs per set

Every game of a set is kept, packed per set, without anyone saving by hand,
also when the set moves to another room (a rematch after a disconnect).

What exists today: each PC already archives every game on its own, one
`.emberreplay` per game (`ReplayFiles.hxx`). Ember turns the Versus battle's
`recordReplay` on for both players (spectators by their own setting,
`sf4e__UserApp.cxx`), and the battle saves its replay when it ends
(`VsBattle::task_BATTLE_MAIN`, 0x63CA40). The `.names` side file holds
the two names, guessed from fighters and save time. Nothing ties a replay to
a room, a match or a set, nothing uploads one, and a file holds one game.

The set's id already survives a room change: a tournament room is bound to
the bridge `matchId` from the call link (`TournamentLink.hxx`), and a new
room for the same set binds to the same id with a higher revision. The pack
hangs off that id.

- **Tag.** `NoteMatchStart` also takes the bound `matchId` and the room's
  revision, and the `.names` record stores them (`EMBRNAM2`; version 1 files
  keep reading). A game without a binding stays untagged, as today.
- **Upload.** After a tagged game is archived, the player's client sends it
  to the bridge, `PUT /v1/matches/{matchId}/replays/{crc}`, signed with the
  Ember key as other match calls are. Only the set's two players may send;
  the same game from both players is kept once (same CRC). A failed upload
  stays in the outbox and is retried with it.
- **Pack.** The bridge orders the set's games by save time and serves them
  as one file, `GET /v1/matches/{matchId}/replays.pack`: a header with the
  tournament, round, phase, players, score and rooms, then each game's
  `.emberreplay` bytes.
- **Cut-off games don't count.** A game that ends without a confirmed
  result (a disconnect, a desync) is never scored and never packed: who was
  at fault can't always be proven, so the game is played again from the
  start, in the same room or a new one. Only games with a confirmed result
  are tagged and sent, so a 2–1 set ends with exactly 3 complete replays.
- **Game.** Replays › Tournaments lists sets, each with its pack (count of
  games and rooms); Watch set plays them in order; Save set pack writes the
  file; opening a `.emberpack` imports every game. The desk's set panel
  links to the same pack.

Open: how long the bridge keeps packs, and whether spectators' copies may
fill a gap when neither player's upload arrives.

## 5. Commits

1. Bridge: organizer sign-in and sealed grants, with refresh.
2. Bridge: organizer events and the TO's tournament list.
3. ember-brackets: run the desk's events.
4. Game: organizer access and event pick.
5. Game: the desk's sets queue, bracket view and actions.
6. Bridge: tournament drafts (title, optional date, service, settings,
   links, status) and creating a Challonge tournament with the TO's grant.
7. Web desk (`/desk`, `bridge/desk/`; `GET /web/accounts` and
   `POST /web/organize/{site}` added for it): pages on the bridge in the embernetplay.link style
   (`server/ember-short/deploy/site/assets/site.css`) for §4a: linked
   accounts, new tournament, service and settings, links, ready, and the
   Discord announcement to copy; then managing it with the game desk's
   actions.
8. Mobile mini-viewer: the bracket and live sets as a read-only phone
   layout of the web desk, at `/t/{id}` for a public event, with sign-up
   (built with the web desk's Live step: sets, bracket, actions, settings).
9. Game: tag tournament replays with the set's `matchId` and room revision
   (`EMBRNAM2`), only for games with a confirmed result.
10. Bridge: replay upload per set and the set pack.
11. Game: upload from the outbox, Replays › Tournaments by set, and
    `.emberpack` save and import.
12. Manual: the tournament mode guide.
