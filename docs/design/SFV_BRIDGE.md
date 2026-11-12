# SFV Ember's own bridge and database

Status: planned 2026-11-12, nothing deployed. Scripts: `server/sfv-bridge/`
(`stage.sh`, `upgrade.sh`), tenants file
`server/ember/bridge/deploy/tenants.sfv.json`.

## Why, and what is live today

SFV Ember used SF4 Ember's production bridge (`https://bridge.embernetplay.link`,
`/var/lib/ember-bridge/bridge.sqlite3`). To make that work, the staged upgrade
`~/ember-sfv-rooms/upgrade.sh` replaced SF4's production and staging bridge
binaries with the library-line bridge (a13ded4c, installed 2026-11-09 15:47Z)
and so applied the library's migrations `057_room_game`, `058_ranking_games`
and `059_match_games` to SF4's database. Its rooms and site phases ran too
(supervisor 3811fc3a since 2026-11-11, three SFV room builds installed,
`/rooms-sfv.json` and `/rankings-sfv.json` live, two SFV players on the board).

SF4 1.6.0 numbers its own migrations 060-062 and refuses an unnamed migration
record above 056, so it cannot run on that database as it is. Kate's decision:
SFV gets a bridge and database of its own, so the two games never share a
schema again. SF4's database still needs one SF4-side step before 1.6.0's
bridge goes in (see "SF4's database", last section); nothing in this plan
touches it.

## The new tenant

A third instance of the same bridge, the way staging is the second:

| | SF4 production | staging | SFV (new) |
|---|---|---|---|
| origin | bridge.embernetplay.link | bridge-staging.embernetplay.link | **sfv-bridge.embernetplay.link** |
| listen | 127.0.0.1:47820 | 127.0.0.1:47821 | **127.0.0.1:47822** (free on the VPS) |
| operator folder | ~/ember-bridge | ~/ember-bridge-staging | ~/ember-bridge-sfv |
| tenants file | tenants.json | tenants.staging.json | tenants.sfv.json |
| account, unit, state, backups | ember-bridge | ember-bridge-staging | ember-bridge-sfv, `/var/lib/ember-bridge-sfv`, `/var/backups/ember-bridge-sfv` |
| nginx site | sites-available/bridge.embernetplay.link | …/bridge-staging… | …/sfv-bridge.embernetplay.link |

`instance.sh` already derives every name from the folder (`ember-bridge-sfv`
gives instance `sfv`), and `setup.sh` already refuses a tenants file whose
origin or listen address another installed bridge has. `test-setup.sh` now
installs the SFV instance beside production and staging and checks its units,
site, port, zones and bridge ID, and that production's and staging's files and
units are untouched (224 checks pass).

`tenants.sfv.json`: display name "SFV Ember bridge", the Ember Discord
application, `integration_secrets`, and only the `ember` tenant's `ember-test`
direct connection (for organised SFV matches in testing). No BluMint
connections and no replay archive: SFV's replays are not in the archive.
Public rooms are not in the file: the bridge phase copies SF4 production's
`rooms` block (supervisor URL, `skill_badges`, `developers`) at install time,
so no Discord IDs are committed and both bridges rate and badge alike.

The binary is the library line's Linux bridge. The one SF4 production runs now
(a13ded4c, staged at `~/ember-sfv-rooms/ember-bridge`) will do; it has 057-059
and starts a fresh database at the newest schema.

## Origin, DNS, TLS and the reverse proxy (Kate's steps)

1. DNS: an A record `sfv-bridge.embernetplay.link` → 74.208.200.95 (it does not
   resolve today). Everything else can be installed before it resolves; the
   scripts check through nginx with `--resolve`.
2. TLS: nothing to do. `/etc/ssl/certs/embernetplay.link.fullchain.pem` is
   `*.embernetplay.link` (valid to 2027-03-30), and the site template uses it.
3. nginx: `setup.sh` renders `sites-available/sfv-bridge.embernetplay.link`
   from the same template as the other two, with its own BluMint zones
   (`ember_bridge_sfv_*`) and the shared request zones; it moves nothing until
   `nginx -t` passes and puts everything back if the bridge does not answer.
4. Run `sudo bash ~/ember-sfv-bridge/upgrade.sh bridge`, then `import`, then
   `site` (below).

## Public rooms: two bridges, one room pool

The bridge talks to `rooms.supervisor_url`, which on the VPS is the room pool
(`ember-rooms-pool`, one supervisor API over the VPS supervisor and server1's
room box). SFV's bridge gets the same URL and the same caller secret
(`/etc/ember-rooms/supervisor.secret`, stored through its own
`set-integration-secret.sh rooms`).

This is safe because a bridge only reconciles rooms it has a row for
(`routes/rooms/poll.rs`: an unknown room in the supervisor's list is skipped,
and only its own published rooms are closed when the list leaves them out), and
ratings are recorded only for rooms in its own table (`skill::record_games`).
Room IDs are 128-bit random. Each SFV create names `game: "sfv"` and an SFV
build, which the pool sends to the backend that has it; SF4's bridge never sees
it. What the two share: `max_rooms` and the port range (as they do today), and
`ember-rooms-idle` closing idle lone hosts on either. The supervisor needs no
change. Later, if wanted, the pool can take a second caller secret so each
bridge has its own.

## The website

`server/ember-short` (embernetplay.link):

- `/rooms-sfv.json` and `/rankings-sfv.json` proxy to 127.0.0.1:47822 (still
  `?game=sfv`, since the library bridge lists USF4 when no game is named).
  `/rooms.json` and `/rankings.json` stay on 47820.
- `/link-sfv/MATCH` proxies to SFV's `GET /v1/matches/MATCH/link`.
- `open.js`: a `/r` or `/m` link naming any bridge other than SF4's two is
  checked against `bridge_id` in `/rooms-sfv.json`. When it is SFV's bridge,
  a room link opens SFV Ember (`ember-sfv://room/open`) and a match link asks
  `/link-sfv/` whether it is over and opens SFV Ember. SFV's bridge ID is made at
  install, so it is never built into the page. `rooms.js` already builds SFV room links from
  the listing's `bridge_id`.

The site phase keeps a tarball of `~/ember-short`'s setup, nginx and site
first, and checks that `/rooms-sfv.json` comes from SFV's bridge ID, that
`/rooms.json` still comes from SF4's, and that `/link-sfv/` reaches SFV's bridge.

## Discord sign-in

Same Ember Discord application (client ID 1546980049692135514), as staging
already does. The bridge phase copies SF4 production's Discord client secret
(read-only) into SFV's `integrations.json`; Kate adds
`https://sfv-bridge.embernetplay.link/v1/discord/callback` to the application's
OAuth2 redirects in the Developer Portal. One application means the consent
screen says "Ember" for both games and Discord user IDs are the same
everywhere, which is what lets the ratings move over (they are keyed by
Discord user ID). A separate SFV application is possible later; it changes only
`discord.client_id` and the stored secret, and the IDs stay the same.

## Backups

The instance gets its own `ember-bridge-sfv-backup.timer`: daily 04:20 UTC
(±15 min), online SQLite copy plus `bridge-secrets.json` and `bridge.json`, 14
days in `/var/backups/ember-bridge-sfv`, restored with
`/usr/local/lib/ember-bridge-sfv/restore.sh <stamp>`. The import phase runs
one backup of SFV's database before it writes. Nothing copies these off the
VPS yet (the same holds for SF4's); an off-box copy of `/var/backups/ember-bridge*`
would cover all three.

## What is lost, and merging later

- **One account across games.** An Ember ID belongs to one bridge. SFV players
  approve the new bridge once (Linked accounts or Public rooms inspect it,
  as on first use) and connect Discord again on it. Their Ember key, name and
  settings on the PC are unchanged.
- **Friends, organised matches and history across games.** Friends, ranked
  opt-ins, match records and tournament data are per bridge. SFV's friends
  list starts empty. SF4's history stays on SF4's bridge.
- **Ratings move over.** `import` copies SFV's `skill_ratings`, `rated_pairs`
  and `ranking_choices` rows (`game = 'sfv'`) from a read-only snapshot of
  SF4's database, once, before anyone plays on the new bridge. They are keyed
  by Discord user ID, so a player who connects the same Discord gets their
  class and board entry back. Rooms (live state), and any SFV matches, lobbies
  or tournaments (owned by the connection and bridge that made them), are
  counted and left in SF4's database.
- **Merging later**, if a single account is wanted again: both databases
  have the same library schema with a `game` key on everything game-specific,
  so a merge is a one-time copy of SFV's game rows into the surviving database
  (ratings, choices, rooms, matches by `game`) plus a mapping of SFV Ember IDs
  onto SF4 ones by Discord user ID, then the SFV origin pointed at the
  surviving bridge (an nginx change; clients keep their origin). It needs SF4
  and SFV on the same migration line first, which is the thing this split avoids.

## The SFV client

`cmake/ember-x64-probe/CMakeLists.txt` (the library build SFV Ember adds;
SF4's x86 build never reads it) now sets `SF4E_EMBER_SERVICE` from
`EMBER_SERVICE`, default `https://sfv-bridge.embernetplay.link`, as a PUBLIC
definition so every unit that includes `ui/IdentityPanel.hxx` sees the same
origin. Linked accounts, Public rooms and Rankings all reach the bridge through
`IdentityPanel::EmberService`; nothing in the helper hard-codes production's
origin. SF4 Ember's x86 build keeps `https://bridge.embernetplay.link`
(`sf4e_helper.cmake`). The SFV build that ships this must come from a library
line that has this commit (SFV beta.8 builds from `b8/lib-integrate`, which
does not yet).

## Published SFV betas (beta.1 to beta.7)

They name `https://bridge.embernetplay.link` and keep doing so.

- **While SF4 production runs the library bridge** (today, until SF4 1.6.0's
  bridge goes in), they keep working completely: rooms, tickets, rankings.
  But their rooms and games land in SF4's database, which the new SFV builds
  never see: an old-beta player and a new-build player see different room
  lists and boards, and an old beta's rating changes after the import stay
  behind on SF4's bridge.
- **Once SF4 production runs an SF4-line bridge again** (1.6.0: it keeps the
  `game` columns but its protocol names no game, and lists no `ranking_games`):
  the room list asks with the SFV build ID, so it comes back empty (a list
  with a room of another game would be refused as an invalid answer);
  creating a room is refused, because SF4's `CreateRoom` refuses the unknown
  `game` field; Rankings shows "unavailable", because the helper asks for
  another game's ranking only from a bridge that lists `ranking_games`. Ember
  ID and Discord still work. The assignment list (`/v1/assignments?game=sfv`)
  comes back with USF4's matches from a bridge that ignores `game`, but every
  beta drops a match whose `game` is not its own (absent reads as usf4;
  `session/TournamentAnswers.cxx`, in the library since before beta.1), and a
  match link only focuses a row of that list, so no SFV beta claims a USF4
  match. SFV Ember sends no replay to a bridge's archive (its "archive" is the
  local replay folder).

  So nothing an old beta sends is taken as USF4's, with no SF4 bridge code for
  it, as long as SF4's bridge keeps two things: `CreateRoom` and
  `TicketRequest` refuse unknown fields and have no `game` (every SFV request
  names `game: "sfv"`), and its capabilities do not list `ranking_games`. If
  SF4's protocol ever takes the library's `game` fields, it must refuse any
  game but usf4 at the same time. A version or build gate on SF4's bridge is
  not needed.

Recommendation: install the SFV bridge (bridge, import, site) before
publishing beta.8, ship beta.8 with the new origin as a required update (the
in-game update check already offers it), and say so in the announcement. Do the
import close to that publish, since an old beta's ranked games after the
import do not carry over. Leave SF4's bridge as it is until SF4 1.6.0 deploys;
by then the old betas fail as above, with no wrong data.

## Rollout and rollback

1. Build the Linux binaries in WSL (on the laptop, one heavy build at a time):
   - the bridge: `cargo build --release --locked -p ember-bridge` in
     `server/ember`, or reuse a13ded4c (`vps:~/ember-sfv-rooms/ember-bridge`);
   - beta.8's room build: `sf4-net` with
     `cargo build --profile server --locked` in the library's `rust/sf4-net`
     at `b8/lib-integrate`, and `sfve-room-host` with sfv-ember's
     `scripts/build-room-host-linux.sh <library root> sfve-room-host` at
     `b8/integrate` (it embeds that tree's `data/pick-space.json`; it needs
     xxhash.h, as `laptop-setup/sfv-rooms-linux.sh` arranges). Both come
     from the commits beta.8 ships, as the earlier room builds did.
2. `bash server/sfv-bridge/stage.sh <ember-bridge> <folder> <sfve-room-host>
   <sf4-net> <sfv-ember>/data/pick-space.json` locally, then copy the folder
   to `vps:ember-sfv-bridge` (a deploy step; ask first). The room build ID is
   the pick space's `build`, for beta.8
   `sfv-ember/13173604/0.6/55c1d32e7a18ea97/layout-72200b5db368767a`.
3. DNS record (Kate).
4. `sudo bash ~/ember-sfv-bridge/upgrade.sh bridge`; add the Discord redirect.
5. `… import` (once, close to the beta.8 publish).
6. `… site`.
7. `… rooms`: beta.8's room build goes into the VPS supervisor beside the
   0.5 builds that beta.1-7 use. It checks the room host against its own
   build (and refuses another) before adding it, then reloads the supervisor
   (SIGHUP: no room is touched) under the build puller's lock, and takes the
   entry out again if the supervisor does not list it. Only the VPS backend
   gets it; the pool sends SFV creates only to a backend with the build, and
   server1 stays SF4's. A later SFV release with a new pick-space `build`
   stages its own room build and runs this phase again;
   `rollback-rooms` takes the staged one out.
8. Publish SFV beta.8 built with this library change.
9. `… check` at any time (read-only).

Never run `~/ember-rooms/setup.sh --prune`: it stages only SF4's room hosts
and would remove every SFV build.

Every phase prints at its end that SF4's bridge binary, `bridge.json`,
secrets, `integrations.json`, nginx site and running process (main PID) are
unchanged, and fails otherwise. `rollback-site` restores the site tarball
(SFV's feeds back on SF4's bridge); `rollback-bridge` (after `rollback-site`)
stops and disables `ember-bridge-sfv` and removes its nginx site, keeping its
state and backups; `rollback-rooms` removes the staged room build from the
supervisor and its folder. None of them touch SF4's bridge or database.

The old `~/ember-sfv-rooms/upgrade.sh` is done and must not be run again: its
`rollback-bridge` would restore SF4's database to the 2026-11-09 backup and
lose every write since. Its rooms phase is replaced by the `rooms` phase above,
which is the same install without anything of SF4's bridge.

## SF4's database (for the SF4 1.6.0 deploy, not done here)

SF4 production's database holds the library's unnamed 057-059 records, the
`game` columns and game-keyed rating tables they made, and SFV's rows. The
plan (agreed with the SF4 1.6.0 side, 2026-11-12) keeps that schema: SF4
1.6.0's bridge carries the library's `057_room_game.sql`, `058_ranking_games.sql`
and `059_match_games.sql` verbatim as its own 057-059 and adopts their unnamed
records; 060-062 stay 1.6.0's. The three files are the same blobs on the
deployed commit (237151932), `feat/ember-library` and `b8/lib-integrate`
(d517a402, e975322c, c8da82dc); the library line has no migration above 059,
and its 055 and 056 are SF4's files byte for byte.

**The code change adoption needed** (done on SF4's
`fix/160-adopt-library-057`, not yet on nightly). 058 keys `skill_ratings` and
`ranking_choices` by `(game, discord_user_id)` and `rated_pairs` by
`(game, first, second)`, and SQLite refuses a conflict target that matches no
primary key or unique index, so SF4's old `ON CONFLICT (discord_user_id)` and
`ON CONFLICT (first, second)` writes would all fail. SF4's bridge now names
`game` in every conflict target on those three tables and scopes every read,
update and delete on them to `game = 'usf4'`, so SFV rows left there are never
read or changed. Its `LAST_UNNAMED_VERSION` is 59: production's unnamed 057-059
are adopted, an unnamed 060 or above is refused.

`rooms`, `matches`, `lobbies` and `tournaments` reads are not scoped by game:
SF4's bridge would list an SFV room still in the database. That is why the
clearing below runs at the same switch as the 1.6.0 bridge install.

SF4 1.6.0's bridge (nightly 7aedc8b6a) refuses to start on a database that
still holds a row with `game <> 'usf4'` in `rooms`, `matches`, `lobbies`,
`tournaments`, `skill_ratings`, `rated_pairs` or `ranking_choices`; the error
names the tables and says to run `clear-sfv-rows.py` first. It adopts the
unnamed 057-059 records only when the schema shows them (`rooms.game`, `game`
in `skill_ratings`' primary key, `matches.game`).

**The switch, in this order** (production; staging runs steps 2-5 the same
way with `ember-bridge-staging`, `ember-bridge-staging-backup.service` and
`/var/lib/ember-bridge-staging/bridge.sqlite3`; step 1 imports from production
only, and staging's SFV test rows are not kept):

1. `sudo bash ~/ember-sfv-bridge/upgrade.sh import`: SFV's ratings into SFV's
   bridge (it reads SF4's database through a read-only snapshot). It must come
   first: after step 4 there is nothing left to import.
2. `sudo systemctl stop ember-bridge`.
3. `sudo systemctl start ember-bridge-backup.service`: a backup taken with the
   bridge stopped, so it holds every last write (`backup.sh` reads the file
   read-only and needs no running bridge).
4. `sudo -u ember-bridge python3 -I clear-sfv-rows.py
   /var/lib/ember-bridge/bridge.sqlite3`. If it stops ("nothing deleted"),
   do not go on: start the old bridge again and look at what it names.
5. Install SF4 1.6.0's bridge and start it.

Undo before step 5: restore step 3's backup and start the old bridge. Old SFV
betas that played between steps 1 and 2 lose those ranked games on both sides;
keep the gap short.

**SFV's rows leave** with `server/sfv-bridge/clear-sfv-rows.py` (step 4):

- `rooms` where `game = 'sfv'`, with their `room_bans` and `rated_games`
  (only `rated_games` cascades);
- `skill_ratings`, `rated_pairs` and `ranking_choices` where `game = 'sfv'`.

It deletes nothing, and says why, when SFV owns a `matches`, `lobbies` or
`tournaments` row or a room a connection made: those carry child rows with no
cascade (participants, claims, attempts, reports, replays, watchers, BluMint
progress, entrants, nodes, events by match) and need a look first. None are
expected: SFV's organised play never ran on production. Nothing else in the
schema is SFV's: `archive_games.game` (054) is a replay's own field, and
Ember IDs, Discord links and friends carry no game. Tried on a database built
from every library migration, with foreign keys on: it removes exactly the SFV
rows and stops on a connection-made SFV room.

Old SFV betas keep creating SFV rows on SF4's bridge for as long as it runs a
bridge that knows `game`, so run it at the 1.6.0 switch, not before.
