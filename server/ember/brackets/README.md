# ember-brackets

Plays the sets of a bracket run on another site in Ember and reports each
score back to that site. The organizer runs the bracket on Challonge,
start.gg or a service of their own as usual; players who have connected
their account on that site in Ember (Online play > Ember ID), or the Discord
account the site shows for them, find each set they are due to play in the
game. `docs/guides/INTEGRATIONS.md` ("Bracket sites") places it among
Ember's integration paths.

Each site is reached through an adapter: `challonge`, `startgg`, or `http`
for an organizer's own service. [ADAPTERS.md](ADAPTERS.md) is the contract
every adapter meets, the guarantees the connector adds, and the `http`
adapter's JSON contract; it is also how a new site is added.

For each configured tournament the connector:

1. Reads the open sets. For each set whose two entries both have Ember, it
   creates an `ember-room-v1` match on its bridge connection, with
   `external_match_id` `<adapter>:<tournament>:<set>:<attempt>`, and marks the
   set underway on the site unless the read shows it already is. An entry is
   found by the first of its accounts the bridge knows: its site account,
   then a Discord account the site shows linked to it.
2. Receives the bridge's signed result at its results URL and reads the
   bracket at once. A completed match is reported (each player's games won,
   the winner advancing, and for start.gg and `http` each game's winner) if
   the site still has the set open with the same two entries and no score.
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
   organizer's: no new Ember match is made for it, nor, on Challonge, for
   the sets after it that reopening resets and that open again for the same
   players. A set that opens for other players is played as a new attempt.
   A score that fails to go in while the site is otherwise answering waits
   on its own and is sent again, up to 12 times (about 6 hours), while the
   rest of the tournament goes on.
5. Stops for good once a read finds no open set and the site says the
   tournament is over: Challonge's `state` is `complete` or `ended`,
   start.gg's event is `COMPLETED`, or the service's `/standing` is
   `finished`. When Challonge does not say, says it is awaiting the
   organizer's review (a set can still be reopened), and every set looks
   played, it only reads at the longest interval, since a later stage or
   swiss round may still be added.

Only what a set shows is evidence. A list read in pages can miss a set that
moved between two pages, so a set missing from it is asked for on its own
before its match is cancelled or its result set aside, and a report after
another one in the same pass reads its set first.

Every five minutes the connector also asks the bridge about each live match
(bridge requests cost nothing against a site's budget). A finished match
whose result the bridge has started sending (and so can no longer be
corrected) is settled from that, under the same ID as the delivery, so the
delivery, if it gets through, is a repeat. A result the bridge has not
started sending is left alone, since an organizer can still correct it on
the bridge until then. One still unsent at two read-backs in a row is
logged once per match: the bridge may have no `results_url` or result secret
for the connection, and then settles nothing.

What it has decided is kept in its SQLite file, so a restart carries on where
it stopped and never makes a set's match twice: a creation is saved before it
is sent, and one the bridge has not answered is sent again unchanged, which
finds the match if the bridge made it. Keep that file for as long as its
tournaments run, and keep each tournament's `tournament` value as it is:
with another file or value the connector no longer knows which sets it
already played.

## What the organizer provides

On Challonge:

- The tournament's ID or URL slug on Challonge.
- Access to it: a Challonge v1 API key (challonge.com/settings/developer),
  which v2.1 accepts, or an OAuth access token. Tokens expire after a week
  and the connector does not refresh them, so a key is simpler for now.
- How long sets are, by round: Challonge does not say. For example first to 2,
  and first to 3 from winners round 4 and in the losers bracket's last rounds.
- Entries that are Challonge accounts. An entry added by name only has no
  username, so it cannot be matched to a player and its sets are played
  outside Ember.

Players connect their Challonge account in Ember once. A set waits, and says
so once in the log, until both of its players have. The connector learns
each entry's account when it first sees the entry: an entry the organizer
gives an account after that is picked up when the connector restarts. A
player whose Challonge account was connected more than 30 days before a match
is pointed out in the log once, since Challonge lets an account rename (so
is one on any sign-in site the bridge looks up by username). The
bridge finds the player who last connected under a username, so a username
given up and taken by someone who has not connected in Ember still finds the
old player; for an event, ask players to connect again shortly before it.

On start.gg:

- The event's slug, `tournament/<tournament>/event/<event>`, as in the
  event page's address. One event per tournament entry in the config.
- A token of an account with an admin role of Reporter or higher on the
  tournament (its Settings, Admin Permissions): a personal access token from
  start.gg's developer settings (it lasts a year), or an OAuth token with
  `tournament.reporter`.
- How long sets are, by round, as for Challonge.

Players are found by the start.gg account they connected in Ember, or else
by the Discord account linked to their start.gg profile, if start.gg shows it
to the organizer's token. Each set is reported with its winner and every
game's winner in the order played, after reading the set again: a set that
already has a result is never written over. Requests wait their turn under
start.gg's limit of 80 a minute (the connector keeps to 70), shared by every
tournament using the same token. Sets of a phase that has not started (start.gg
"preview" sets) and sets with a bye are never played. The entrants are read
again every 15 minutes, so a Discord account a player links on start.gg
after the first read is found within that.

A token start.gg refuses (an HTTP 400 or 401) is waited out like any other
failure, until the organizer fixes it; no set is given up for it. A score
start.gg refuses with an error it does not explain, while the set is still
open with the same entrants and no winner, waits on its own and is sent
again, a minute later and then doubling up to an hour, up to 12 times
(about 6 hours) before the organizer decides the set. The tournament's other
results and reads go on meanwhile.

A set start.gg answers with "Set not found for id: <its ID>" (one of a phase
group the organizer reset or reseeded) is taken as gone: its live Ember match
is cancelled, and the rest of the tournament goes on. Any other error, or a
set left empty beside an error, is waited out. start.gg's documents give that
text only for preview sets; that it answers a removed set the same way is
not stated, and needs a live check. When the Discord accounts behind
entrants come back with an error (STARTGG_API.md does not say whether every
token may read them), the entrants' start.gg accounts are still used.

A service of the organizer's own: see [ADAPTERS.md](ADAPTERS.md).

## What the Ember team provides

A `direct` connection on the bridge for the connector, with:

```json
{
  "id": "brackets", "kind": "direct", "environment": "production",
  "display_name": "Bracket connector",
  "lookup_sites": ["challonge", "startgg"],
  "discord_lookup": true,
  "disputes": "restart",
  "results_url": "https://brackets.example/ember/results"
}
```

`lookup_sites` names the bridge's sign-in sites whose accounts the
connector may look players up by; `discord_lookup` lets it fall back to
Discord accounts. The bridge refuses a lookup that names a site not listed,
so every `bridge_site` the connector's tournaments use must be in it. An
entry with an account the bridge refuses waits, with the reason logged once,
and every other set is played.

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
  "monthly_request_budget": 500,
  "tournaments": [
    {
      "adapter": "challonge",
      "tournament": "ember_weekly_12",
      "credential": { "kind": "v1_key", "file": "challonge.key" },
      "active_from": "2026-10-24T17:30:00Z",
      "active_until": "2026-10-24T22:00:00Z",
      "rounds": {
        "games_to_win": 2,
        "rules": [
          { "from_round": 4, "side": "winners", "games_to_win": 3 },
          { "from_round": 5, "side": "losers", "games_to_win": 3 }
        ]
      }
    },
    {
      "adapter": "startgg",
      "tournament": "tournament/ember-weekly-12/event/ultra-street-fighter-iv",
      "credential": { "kind": "bearer", "file": "startgg.token" },
      "rounds": { "games_to_win": 2 }
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
  to `max_poll_secs` (default 600).
- `active_from`, `active_until`: optional, in UTC and in exactly this form:
  `2026-10-24T17:30:00Z`. Outside this
  window the bracket is not read on a schedule and no match is created. A
  result received after `active_until` is still read for and reported, which
  counts against the budget, so a final that runs over is not lost; sets it
  opens are left to the organizer.
- `adapter`: `challonge`, `startgg` or `http` (ADAPTERS.md).
- `bridge_site`: the bridge sign-in site the players' accounts are under,
  the adapter's name when absent (`challonge-staging`, say, on a bridge
  that names its sites so). Not used by `http`, whose service names each
  account's site.
- `credential.kind`: `v1_key` or `bearer` (an OAuth token) on Challonge;
  `bearer` on start.gg and for `http`.
- `api_base`: per tournament, optional, the site's API origin
  (`https://api.challonge.com` and `https://api.start.gg` when absent). The
  organizer's credential goes to it, so it must be an https origin on
  challonge.com or start.gg respectively; for `http` it is required, and is
  the service's https base URL. A loopback http origin is accepted only with
  `allow_loopback_http: true`, which is for local tests.
- `mark_underway`: per tournament, default `true`. Marking each set underway
  costs one request a set; `false` saves them.
- `rounds`: `games_to_win` (1 to 10) unless a rule covers the round; the first
  rule that does sets it. A rule covers `from_round` to `to_round` (no end when
  absent) on its `side`: `winners` (every round Challonge numbers above zero,
  which includes single elimination, round robin and swiss), `losers` (the
  losers bracket, which Challonge numbers below zero) or `any` (the default).

## Run

From `server/ember`:

```sh
cargo run -p ember-brackets -- serve brackets.json
```

On a server, build with `cargo build --release --locked -p ember-brackets`
and install `deploy/ember-brackets.service.in` with `@NAME@` replaced by the
service account and folder name (`ember-brackets`) and `@LABEL@` by an
optional suffix for the description:

```sh
sudo useradd --system --home-dir /var/lib/ember-brackets --shell /usr/sbin/nologin ember-brackets
sudo install -D -m 0755 target/release/ember-brackets /usr/local/lib/ember-brackets/ember-brackets
sed -e 's/@NAME@/ember-brackets/g' -e 's/@LABEL@//g' brackets/deploy/ember-brackets.service.in \
    | sudo tee /etc/systemd/system/ember-brackets.service >/dev/null
```

`brackets.json` and the secret files go in `/var/lib/ember-brackets`, owned by
that account. Then start it, and have it start with the server:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now ember-brackets
```

Each decision is one line on standard error
(`journalctl -u ember-brackets`). A new credential takes effect on restart.

When something fails the log says so once, not on every try:

- A delivery to the results URL that fails verification (a wrong
  `secret_file`, or this machine's clock more than five minutes off) is
  answered 503, which the bridge retries for a day, and logged once until
  one verifies again. Meanwhile the read-back above settles each set from
  the bridge within about ten minutes, but fix the cause.
- When Challonge cannot be read or refuses the credential, the bracket is
  read again after a minute, then after twice as long each time, up to an
  hour, whether a result or the schedule asks for the read. Requests Challonge refuses with 401 or 403 are not counted.

## Request budget

This section is about Challonge; start.gg counts requests by the minute (see
above) and a service of the organizer's own sets its own limits.

Challonge's free tier allows 500 requests a month per developer app. Every
request is counted per credential and calendar month (UTC) before it is
sent; the count is kept under a digest of the credential, so moving its file
keeps it. Two keys of one developer app are counted apart, so give the
connector one key per app. At 80% the connector warns; at 100% it stops
scheduled reads and underway marks, but a result received is still read for
and reported.

What a request is spent on:

- A read of the bracket: one request per 25 open sets.
- The participant list, when a set names an entry the connector does not
  know: one request per 25 entries, so once per event in practice. An ID
  the list does not show is remembered as an entry without an account, so
  it costs that one read, not one on every read.
- One report per set played in Ember, and one underway mark per new set.
- One read of a single set when it is missing from a list, or before a
  second report in one pass. Rare.
- One to three more when a read finds no open set after sets have been
  played, to tell whether the tournament is over. Before any set is open it
  is not asked.

A 64-player double elimination has 126 sets, 127 with a grand final reset.
Played entirely in Ember over about three hours, with the defaults:

| | Requests |
|---|---|
| Reports, one per set | 127 |
| Underway marks, one per set | 127 |
| Reads made for a result, one each (two while more than 25 sets are open) | about 155 |
| Scheduled reads in quiet spells: 120 s after a change, then 240, 480, 600 | about 40 |
| Participant list (64 entries) | 3 |
| Finished check | 3 |
| Total | about 455 |

A simulation of that event, with sets lasting 6 to 14 minutes, came to 440
to 470 requests. The fixed part is 254 for reports and underway marks; the
rest falls with fewer, longer quiet spells. Nothing is spent before
`active_from` or once the bracket is finished, and after `active_until` only
on results that arrive. One such
event fits the free tier per month; regular events need Challonge's paid tier,
with `monthly_request_budget` set to match.

## Not yet checked against Challonge

The adapter follows `docs/design/CHALLONGE_API.md`, which was read
from Challonge's documents without a live call. These points are handled as
described there and need one check against the real API:

- `score_set` sent as each participant's set score (`"2"` and `"1"`), one
  entry per participant, with `rank` and `advancing`.
- Where a match's players appear: inside `attributes.relationships` per the
  schema, or in the standard JSON:API `relationships`. Both are read.
- Pages of 25 (the documented default); a shorter page is the last.
- A tournament's `state` and a match's `underway_at`, which the schema does
  not list: read when present, and otherwise worked out as described above.
- `change_state.json` sent with `application/vnd.api+json`, as every other
  request.
- 429 and its headers: treated as "try again later".
- Whether Challonge refuses a score for a set that is already complete. The
  connector reads the set before reporting whenever its read may be out of
  date, but an organizer who enters a score in the same second could still be
  overwritten if Challonge accepts the second one.
