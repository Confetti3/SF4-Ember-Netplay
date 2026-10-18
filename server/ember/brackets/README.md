# ember-brackets

Plays the sets of a Challonge bracket in Ember and reports each score back to
Challonge. The organizer runs the bracket on Challonge as usual; players who
have connected their Challonge account in Ember (Online play > Ember ID >
Challonge) find each set they are due to play in the game. The design is part
4 of `docs/design/challonge/PLAN.md`.

For each configured tournament the connector:

1. Reads the open sets. For each set whose two entries both have Ember, it
   creates an `ember-room-v1` match on its bridge connection, with
   `external_match_id` `challonge:<tournament>:<set>:<attempt>`, and marks the
   set underway on Challonge unless the read shows it already is.
2. Receives the bridge's signed result at its results URL and reads the
   bracket at once. A completed match is reported (each player's games won,
   the winner advancing) if Challonge still has the set open with the same
   two entries and no score. Sets the report opens are found by the next
   read, at most `poll_secs` later or at the next result.
3. Plays a set again, as a new attempt, when its match restarts (a disputed
   game) or expires unplayed, up to 5 attempts. After that it logs that the
   organizer decides the set, as it does when Challonge refuses a score.
4. Leaves a set the organizer reported, reset or changed by hand to the
   organizer: its live Ember match is cancelled, and a result that arrives
   for it anyway is not reported.
5. Stops for good once a read finds no open set and the bracket finished:
   the tournament's `state` says so, or, since Challonge's schema does not
   document that field, some sets are complete and none is pending. It logs
   this once, and a restart does not read it again.

What it has decided is kept in its SQLite file, so a restart carries on where
it stopped and never makes a set's match twice. Keep that file for as long as
its tournaments run: without it the connector no longer knows which sets it
already played and reported.

## What the organizer provides

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
so once in the log, until both of its players have.

## What the Ember team provides

A `direct` connection on the bridge for the connector, with:

```json
{
  "id": "challonge-brackets", "kind": "direct", "environment": "production",
  "display_name": "Challonge brackets",
  "challonge_lookup": true,
  "disputes": "restart",
  "results_url": "https://brackets.example/ember/results"
}
```

its provider credential
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
    "connection_id": "challonge-brackets",
    "build_id": "ember-1.4"
  },
  "results": {
    "listen": "127.0.0.1:8791",
    "path": "/ember/results",
    "secret_file": "result.secret"
  },
  "database": "brackets.sqlite3",
  "poll_secs": 120,
  "max_poll_secs": 600,
  "monthly_request_budget": 500,
  "tournaments": [
    {
      "site": "challonge",
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
    }
  ]
}
```

- `bridge.build_id`: `required_build_id` of every match, a label for the
  Ember build the players use (not checked against the game).
- `results.path`: defaults to `/results`.
- `poll_secs`, `max_poll_secs`: how often each bracket is read when no result
  makes it read at once. After a read that finds a change, or creates,
  reports or restarts a match, the next comes `poll_secs` later (at least 30,
  default 120); each read that finds the same open sets doubles the wait, up
  to `max_poll_secs` (default 600).
- `active_from`, `active_until`: optional, RFC 3339 in UTC. Outside this
  window the bracket is not read on a schedule and no match is created. A
  result received after `active_until` is still read for and reported, which
  counts against the budget, so a final that runs over is not lost; sets it
  opens are left to the organizer.
- `credential.kind`: `v1_key` or `bearer` (an OAuth token).
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
that account. Each decision is one line on standard error
(`journalctl -u ember-brackets`). A new credential takes effect on restart.

## Request budget

Challonge's free tier allows 500 requests a month per developer app. Every
request is counted per credential file and calendar month (UTC) before it is
sent. At 80% the connector warns; at 100% it stops scheduled reads and
underway marks, but a result received is still read for and reported.

What a request is spent on:

- A read of the bracket: one request per 25 open sets.
- The participant list, when a set names an entry the connector does not
  know: one request per 25 entries, so once per event in practice.
- One report per set played in Ember, and one underway mark per new set.
- Two or three more for each read that finds no open set, to tell a finished
  bracket from one not started yet. Before the start that is every read, so
  set `active_from`.

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

The adapter follows `docs/design/challonge/CHALLONGE_API.md`, which was read
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
