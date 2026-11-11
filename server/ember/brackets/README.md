# ember-brackets

Plays the sets of a bracket run on Ember's bracket site (`server/bracket-site`)
or a service of the organizer's own in Ember, and reports each score back to
it. Players are found by the Discord account they connected in Ember (Online
play > Ember ID), which the site shows for each entry.
`docs/guides/INTEGRATIONS.md` ("Bracket sites") places it among Ember's
integration paths.

The site is reached through the `http` adapter. [ADAPTERS.md](ADAPTERS.md) is
its JSON contract, the guarantees the connector adds, and how another
adapter would be added.

For each configured tournament the connector:

1. Reads the open sets. For each set whose two entries both have Ember, it
   creates an `ember-room-v1` match on its bridge connection, with
   `external_match_id` `http:<tournament>:<set>:<attempt>`, and marks the
   set underway on the site with that match. An entry is found by the
   Discord account the site shows linked to it.
2. Receives the bridge's signed result at its results URL and reads the
   bracket at once. A completed match is reported (each player's games won,
   the winner advancing and each game's winner) if the site still has the
   set open with the same two entries and no score.
   Sets the report opens are found by the next read, at most `poll_secs`
   later or at the next result.
3. Plays a set again, as a new attempt, when its match restarts (a disputed
   game) or expires unplayed, up to 5 attempts. After that it logs that the
   organizer decides the set, as it does when the site refuses a score.
4. Leaves a set the organizer reported, reset or changed by hand to the
   organizer: its live Ember match is cancelled, and a result that arrives
   for it anyway is not reported. A result is reported only for the two
   players its attempt was made for. A set the organizer reopens after its
   score went in (to correct it), with the same two players, stays the
   organizer's: no new Ember match is made for it. A set that opens for
   other players is played as a new attempt, and so is a set whose live match was cancelled because the set changed
   or went back to pending before it was decided, whoever plays it when it
   opens again. A score that fails to go in for a moment while the site is
   otherwise answering waits on its own and is sent again, up to 12 times
   (about 6 hours), while the rest of the tournament goes on; a refused
   token or contract version holds the whole tournament instead, and gives
   no set up.
5. Stops for good once a read finds no open set and the service's
   `/standing` is `finished`. When it says `played_out`, it only reads at
   the longest interval, since a later stage may still be added.

Only what a set shows is evidence. A list read in pages can miss a set that
moved between two pages, so a set missing from it is asked for on its own
before its match is cancelled or its result set aside, and a report after
another one in the same pass reads its set first.

Every five minutes the connector also asks the bridge about each live match. A finished match whose result the bridge has started sending (and so can no longer be
corrected) is settled from that, under the same ID as the delivery, so the
delivery, if it gets through, is a repeat. A result the bridge has not
started sending is left alone, since an organizer can still correct it on
the bridge until then. One still unsent at two read-backs in a row is
logged once per match: the bridge may have no `results_url` or result secret
for the connection, and then settles nothing.

What it has decided is kept in its SQLite file, so a restart carries on where
it stopped and never makes a set's match twice: a creation is saved before it
is sent, and one the bridge has not answered is sent again unchanged, which
finds the match if the bridge made it. If the organizer decides, changes or
removes that set before the creation is answered, the creation is still
sent again once, on the next read, and the match it gives is cancelled, so
no match is left waiting for its players until it expires. When the bridge
never had the creation, this makes the match only to cancel it at once (the
bridge has no way to find a match by its external ID), so the bridge's
announcement channel and the two players may see a match appear and be
cancelled straight away. Keep that file for as long as its
tournaments run, and keep each tournament's `tournament` value as it is:
with another file or value the connector no longer knows which sets it
already played.

## What the organizer provides

A tournament on the bracket site, run from the site's desk or the game's
(docs/design/BRACKET_SITE.md), or a service of their own that answers the
contract in [ADAPTERS.md](ADAPTERS.md). Each entry needs a Discord account,
and its player needs that account connected in Ember. A set waits, and says
so once in the log, until both of its players have connected.

## What the Ember team provides

A `direct` connection on the bridge for the connector, with:

```json
{
  "id": "brackets", "kind": "direct", "environment": "production",
  "display_name": "Bracket connector",
  "discord_lookup": true,
  "disputes": "restart",
  "results_url": "https://brackets.example/ember/results"
}
```

`discord_lookup` lets the connector look players up by their Discord
account. Without it the bridge refuses the lookup: every set waits, with the
reason logged once, and the connector asks again after 15 minutes.

The connector also needs the connection's provider credential
(`ember-bridge credential <bridge.json> provider <connection> "<label>"`) and
its result secret (`ember-bridge result-secret`, stored on the bridge with
`set-integration-secret.sh result <connection>`). The results URL is served by
the connector; put it behind HTTPS.

## Configure

`brackets.json`. Relative paths are taken from the file's own directory.
Secrets are only ever read from files, never written to the log.

```json
{
  "bridge": {
    "api": "https://bridge.embernetplay.link",
    "credential_file": "provider.token",
    "connection_id": "brackets",
    "build_id": "ember-1.4"
  },
  "results": {
    "listen": "127.0.0.1:8791",
    "path": "/ember/results",
    "secret_file": "result.secret",
    "previous_secret_file": "result.previous.secret"
  },
  "database": "brackets.sqlite3",
  "poll_secs": 120,
  "max_poll_secs": 600,
  "tournaments": [
    {
      "adapter": "http",
      "tournament": "ember_weekly_12",
      "api_base": "https://brackets.example/tournaments/ember_weekly_12/ember",
      "credential": { "kind": "bearer", "file": "site.token" },
      "active_from": "2026-10-24T17:30:00Z",
      "active_until": "2026-10-24T22:00:00Z",
      "rounds": {
        "games_to_win": 2,
        "rules": [
          { "from_round": 4, "side": "winners", "games_to_win": 3 },
          { "from_round": 5, "side": "losers", "games_to_win": 3 }
        ]
      }
    }
  ]
}
```

- `bridge.build_id`: `required_build_id` of every match, a label for the
  Ember build the players use (not checked against the game).
- `results.path`: defaults to `/results`. The connector limits a delivery to
  64 KiB; connection and header timeouts come from the HTTPS proxy in front
  of it.
- `results.previous_secret_file`: optional. During a rotation of the result
  secret, the one being replaced, accepted too until the bridge signs with
  the new one.
- `poll_secs`, `max_poll_secs`: how often each bracket is read when no result
  makes it read at once. After a read that finds a change, or creates,
  reports or restarts a match, the next comes `poll_secs` later (at least 30,
  default 120); each read that finds the same open sets doubles the wait, up
  to `max_poll_secs` (default 600). Each tournament's worker sleeps until its
  next read is due (a minute at most, and a result wakes it at once), so a
  `poll_secs` below 60 is kept.
- `active_from`, `active_until`: optional, in UTC and in exactly this form:
  `2026-10-24T17:30:00Z`. Outside this
  window the bracket is not read on a schedule and no match is created. A
  result received after `active_until` is still read for and reported, so a
  final that runs over is not lost; sets it
  opens are left to the organizer.
- `adapter`: `http` (ADAPTERS.md).
- `credential.kind`: `bearer`.
- `api_base`: required, the service's https base URL. A loopback http
  origin is accepted only with `allow_loopback_http: true`, which is for
  local tests.
- `desk`: optional, to run the bracket site's live tournaments as well.
  `desk.services` maps a site name to its base URL (`{ "ember-bracket-site":
  "https://brackets.ember.fraccie.be" }`), and `desk.site` (`{ "service":
  "ember-bracket-site", "credential_file": "site-connector.token" }`) names
  the one to read: each poll the connector reads its `GET <base>/ember/tournaments`
  with that connector token, and plays every tournament listed there on the
  `http` adapter at `<base>/tournaments/<id>/ember`, first to the
  `games_to_win` the site gives. A tournament the site stops listing is no
  longer read, its sets left as they are; one a tournament here already
  runs is left to that tournament. Without `desk.site` the events come from
  the bridge's `/v1/desk/brackets` instead, as they did before the bracket
  site had a desk, and `bridge.connection_id` must be the bridge's desk
  connection.
- `mark_underway`: per tournament, default `true`. `false` leaves the
  sets unmarked (the site then cannot void or decide a game of them).
- `rounds`: `games_to_win` (1 to 10) unless a rule covers the set. Rules are
  read in order and the first that covers a set sets its length, so put a
  narrower rule before a wider one. A rule covers `from_round` (1 when
  absent) to `to_round` (no end when absent) on its `side`: `winners`
  (every round above zero, which includes single elimination, round robin
  and swiss), `losers` (the losers bracket, numbered below zero) or `any`
  (the default). With `phase`, it covers only sets of that stage of the
  event (the `phase` the service gives each set), since each stage numbers
  its rounds from 1 again; a rule without `phase` covers every stage. A rule
  whose phase is none the reads have shown is said once ("rule for phase X
  matches no phase; phases seen: ..."), since a mistyped name gives its sets
  the default length; the configuration is not refused for it, since a
  phase such as top 8 appears only when it starts.

## Run

From `server/ember`:

```sh
cargo run -p ember-brackets -- serve brackets.json
```

On a server, build with `cargo build --release --locked -p ember-brackets`
and install it with the scripts in `deploy/`, the way the bridge's
`deploy/setup.sh` installs the bridge. The folder name picks the connector:
copy `deploy/` to `~/ember-brackets` (production) or `~/ember-brackets-<name>`
(for example `~/ember-brackets-staging`, beside `~/ember-bridge-staging`), put
the binary in its `bin/ember-brackets`, and add the instance's two files:

- `brackets.json` (`brackets.<name>.json` for an instance): the configuration
  above. `results.listen` is a loopback port of its own and `results.path` a
  plain path; every file it names is a plain name in the state folder.
- `site.json` (`site.<name>.json`): `{ "host": "brackets.embernetplay.link",
  "bridge": "ember-bridge" }`, the public host of the results URL and the
  installed bridge the connector connects to. `brackets.staging.json` and
  `site.staging.json` are the staging pair.

Add the connection to the bridge's tenants file first (see "What the Ember team
provides", with `results_url` set to `https://<host><results.path>`), and apply
it with the bridge's `setup.sh`. Then:

```sh
sudo bash ~/ember-brackets-staging/issue-secrets.sh
# put the organizer's credential in ~/ember-brackets-staging/secrets/ (mode 0700),
# in a file named as the configuration's credential.file
sudo bash ~/ember-brackets-staging/setup.sh
```

`issue-secrets.sh` issues the connection's provider credential and a new
result secret, stores the result secret on the bridge, and leaves both in
`secrets/` (neither is printed). `setup.sh` creates the service account
(`ember-brackets` or `ember-brackets-<name>`), moves the secrets into
`/var/lib/<name>` with mode 0600, installs the binary, the configuration, the
unit from `deploy/ember-brackets.service.in` and an nginx site for the host
that serves the results path and nothing else, enables the service, and
checks that the path answers through nginx. A secret still missing leaves
everything installed and the service stopped, with the missing files named.
Running it again installs a newer binary and applies the configuration; the
database and the secrets stay. `remove.sh` takes the installation away and
keeps the state (`--purge` deletes it). `test-setup.sh` runs all of this
against a mock server, with no root and no network.

Each decision is one line on standard error
(`journalctl -u ember-brackets`). A new credential takes effect on restart.

When something fails the log says so once, not on every try:

- A delivery to the results URL that fails verification (a wrong
  `secret_file`, or this machine's clock more than five minutes off) is
  answered 503, which the bridge retries for a day, and logged once until
  one verifies again. Meanwhile the read-back above settles each set from
  the bridge within about ten minutes, but fix the cause.
- When the site cannot be read or refuses the credential, the bracket is
  read again after a minute, then after twice as long each time, up to an
  hour, whether a result or the schedule asks for the read. A credential
  refused on a report waits the same way, and no set is given up for it.

## Known limitations

- A match that finishes while its set is briefly pending (the organizer
  re-entering an earlier set, say) has its result dropped: the set reads as
  changed, and the result is not reported. The set is played again, as a
  new attempt, when it opens, even for the same two players who already
  played it. Holding the result until the set reopens unchanged would need
  a state the connector does not keep yet.
