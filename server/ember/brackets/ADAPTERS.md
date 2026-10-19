# Bracket adapters

An adapter connects one bracket site to the connector. The connector does
everything Ember-side (finding players on the bridge, creating matches,
receiving results, retrying, keeping state); the adapter only reads the
site's sets and writes back to it. There are two ways to write one:

- **In Rust**, inside this crate: implement the `Site` trait
  (`src/site.rs`) and add a value to `Adapter` in `src/config.rs`. The
  Challonge (`src/challonge.rs`) and start.gg (`src/startgg.rs`) adapters
  are the examples.
- **As a service of your own, in any language**: configure the `http`
  adapter, and answer the small JSON contract below. Nothing in Ember
  changes.

Players are found on the bridge by an account the site shows for them. For
a sign-in site the bridge has (Challonge, start.gg, or one an operator
defines in its config) that is the account the player connected in Ember;
a Discord account the player connected in Ember works for any site that
shows the Discord account linked to an entry.

## The `Site` trait

This is the contract between the connector and an adapter written in
Rust. The trait is internal to this crate, so it has no version of its own:
it is versioned with the crate, and an adapter changes with it. The `http`
adapter's contract below is the one with a version an outside service can
rely on.

| Method | Returns | The adapter promises |
|---|---|---|
| `open_sets(purpose)` | every set the site lists as open | Each playable set it can see, entries player 1 first. A set it cannot see may simply be missing: the connector never acts on a set's absence. |
| `set(id, purpose)` | one set, or `None` | `None` only when the site itself says the set does not exist: Challonge answers 404 for the match's own URL (any 404 there, including one from a proxy in front of it), start.gg `Set not found for id: <id>`, an `http` service `{"set": null}`. |
| `standing()` | `Finished`, `PlayedOut` or `Running` | `Finished` only when the site says the event is over: the connector then stops reading it for good. `PlayedOut` when every set looks played but more may come (a later stage, a reopened set). |
| `mark_underway(id)` | nothing | May be called again for a set already underway. A failure is only logged. |
| `report(id, report)` | `Sent` or `AlreadyHeld` | Records the winner and each entry's games won. May be called again for a report already recorded (see Delivery below): answer `AlreadyHeld` when the set already holds this winner, so the log does not claim the connector wrote it. Answer `Sent` only on positive evidence: the site's answer shows the set decided for this winner, or, when it does not, a fresh read of the set does (one still open is `Deferred`). Report only a set that reads open now, not one gone pending. If the site can tell, refuse (`Refused`) rather than write over a set that has a different result, or games entered that disagree with `report.sequence`. |
| `wants_games()` | `bool`, `false` unless overridden | Whether `report` uses each game's winner, in the order played (`report.sequence`). Only then does the connector read the games from the bridge before reporting, and check that they add up to the score. |

`purpose` says why a read is made, `Scheduled` or `Result`: a site with a
request budget may refuse a scheduled read with `BudgetSpent`, but never a
read for a result.

An adapter does not say whether its accounts are names an account can
change: the bridge does. For a player found by an account on a sign-in site
the bridge looks up by username (its capabilities' `lookup_by`), the
connector points out, once per player, an account connected more than 30
days ago, whichever adapter named it.

A `Set` carries:

- `id`: 1 to 64 letters, digits, `_` or `-`, since it goes into URLs and
  match IDs. Leave out a set whose ID is anything else.
- `round` (negative in a losers bracket) and `round_label`, shown to the
  players. The label goes into the match's metadata, where the bridge takes
  at most 1024 bytes: a longer one is cut there.
- `entries`: player 1 and player 2 once both are known. Each `Entry` has an
  `participant_id` (same rule as set IDs) and its `accounts`, the one to try
  first first:
  - `Account::Site { site, id }`: an account on the bridge's sign-in site
    `site`, by the ID that site looks up (a Challonge username, a start.gg
    user ID). An ID the site's accounts cannot have finds nobody, so only
    its entry waits. A site the bridge has no sign-in site for is not asked
    at all (the connector reads the bridge's sites from its capabilities),
    and the set's log line says so once. A site the bridge has but the
    connection may not look up (not in its `lookup_sites`) makes the bridge
    refuse the lookup: the connector asks again kind by kind, so only the
    entries that need that site wait, with the reason in their log line;
  - `Account::Discord { user_id }`: a Discord account linked to the entry.
    A connection without `discord_lookup` refuses these the same way, and
    only the entries that need one wait.
- `status`: `Pending` (waiting on an earlier set), `Open` (can be played,
  no result) or `Decided`.
- `underway`, when the site says so, and `winner` (the winning entry's
  `participant_id`) once decided.
- `phase`, the stage of the event, when the site has more than one: a name
  and, where the site has one, an ID. Round rules can name it (README,
  `rounds`), since stages number their rounds from 1 again.
- `stream`, the stream the set is assigned to, when the site says. With
  `hold.streamed`, such a set waits.

Errors:

- `Denied`: the site refuses the credential or the contract version. No
  one set is at fault, so none is given up for it: whatever asked, the pass
  stops (the matches are still brought in line with what was read) and the
  whole tournament waits under the growing wait below, until the token or
  version is fixed.
- `Unavailable`: the site cannot be used for a moment: it is down, times
  out or rate-limits. From a read, the pass stops as for `Denied`. From a
  report, after a read that worked in the same pass, it is taken as a
  failure on that set alone, and the result waits like a `Deferred` one.
- `Deferred` (from `report` only): this one report cannot be taken now, for
  a reason of its own set, which may pass (start.gg refusing it with an
  error it does not explain while the set is still open and unchanged, or
  an `http` service answering `423` as below).
  That result alone waits, a minute doubling up to an hour, while every
  other result and read goes on; after 12 tries (about 6 hours, counted
  while the connector runs) its set is left to the organizer.
- `Refused`: the site will not do it as it stands. A refused read is
  treated like `Unavailable`, and tried again after the growing wait. A
  refused report is final: the connector leaves that set to the organizer,
  who enters the score by hand. Refuse a report only when it can never be
  taken (the set is gone, has other entries, or has a different result).

## What the connector guarantees an adapter

- **One live Ember match per set.** Each attempt at a set has the fixed
  match ID `<adapter>:<tournament>:<set>:<attempt>`. A creation is saved
  before it is sent and sent again unchanged until the bridge answers, so a
  restart never makes a second match. A creation never answered whose set
  is then decided, changed or removed is sent again once and its match
  cancelled, whether or not the set is listed open.
- **Attempts.** A match that restarts (a disputed game) or expires unplayed
  is played again as a new attempt, up to 5; then the organizer decides. A
  set the organizer opens again after its score was reported or decided is
  the organizer's while it has the same two players: no new match is made
  (on Challonge, reopening a set to correct it reopens the sets after it,
  which are left the same way). Only a set that opens for other players
  gets a new attempt. A set whose match was cancelled because the set
  changed or went away before it was decided (a reopen sending it back to
  pending, say) gets a new attempt when it opens again, whoever plays it.
- **Evidence.** A match is cancelled, or a result set aside, only after the
  set itself, read on its own, shows it decided, changed or gone. A report
  goes out only for the two players its attempt was made for, and only while
  the site still shows the set open with the same entries; a read the
  connector made before another report in the same pass is read again.
- **Verified results.** A result counts only if it is signed with the
  connection's result secret, recent, and for this connection; each is
  recorded once. A result whose delivery is failing is read back from the
  bridge instead.
- **Delivery.** A report is sent at least once. When its answer is lost (a
  timeout, a `5xx`) the result stays pending and is sent again, unchanged,
  unless the next read already shows the set decided with that result. The
  same result always carries the same key (`report.key`, the Ember match
  ID).
- **Retries.** After a read fails nothing is asked of the site, for any
  reason, until a wait of a minute, doubling up to an hour, has passed. One
  log line per run of failures. A report that waits on its own (deferred,
  or failing after a read that worked) waits the same way, and holds
  nothing else back.
- **Pace.** Reads back off while nothing changes (`poll_secs` doubling up
  to `max_poll_secs`), stay inside the tournament's window, and stop once
  the site says the event is over.

## The `http` adapter, contract version 1

The connector calls your service. Configure it as a tournament:

```json
{
  "adapter": "http",
  "tournament": "spring-pools",
  "api_base": "https://brackets.example/ember",
  "credential": { "kind": "bearer", "file": "service.token" },
  "rounds": {
    "games_to_win": 2,
    "rules": [{ "phase": "Top 8", "games_to_win": 3 }]
  }
}
```

This one is first to 2 in pools and first to 3 in top 8, for a service that
gives each set's `phase` as `"Pools"` or `"Top 8"`. Rules are read in order,
and the first that covers a set sets its length (README, `rounds`).

- `api_base`: your service's base URL, https (loopback http only behind
  `allow_loopback_http`, for tests), with no user, query or fragment. The
  paths below are added to it.
- `tournament`: a name for this bracket, 1 to 64 letters, digits, `_` or
  `-`. It goes into the match IDs, so keep it as it is while the bracket
  runs.
- The token is sent as `Authorization: Bearer <token>`.
- `bridge_site` does not apply: each account in your answers names its
  site.

Every request carries `Accept: application/json` and
`Ember-Brackets-Contract: 1`. If you change a shape below in a way an older
connector would misread, answer a request of the older version with
`426 Upgrade Required`, and optionally `{ "contract": "<the version you
speak>" }`. The connector reads that as your service being unavailable as a
whole: nothing is given up, results wait, and it says so once in its log
until the connector or your service is upgraded. Never answer a version
mismatch with `400`, which would make each pending report final. Unknown
fields in your answers are ignored, so you may add fields freely.

### Limits

- Each answer is at most 1 MiB; a longer one counts as no answer.
- The connector waits 5 seconds to connect and 20 seconds for a whole
  request.
- Redirects are not followed: answer at the URL asked.
- `GET /sets` is not paged, so every open set must fit in one answer. At
  about 300 bytes a set, 1 MiB holds a few thousand.
- `round_label` goes into the match's metadata, which takes at most 1024
  bytes; a longer label is cut there.

### Reading

`GET {base}/sets?state=open` answers `200` with every open set:

```json
{
  "sets": [
    {
      "id": "s-12",
      "round": 2,
      "round_label": "Winners round 2",
      "status": "open",
      "entries": [
        { "id": "player1", "accounts": [{ "site": "challonge", "id": "Player_1" }] },
        { "id": "player2", "accounts": [{ "site": "startgg", "id": "1234567" }, { "discord": "274220342558756145" }] }
      ],
      "underway": false
    }
  ]
}
```

| Field | Required | Meaning |
|---|---|---|
| `id` | yes | 1 to 64 letters, digits, `_` or `-` |
| `status` | yes | `pending`, `open` or `decided` |
| `round` | no (0) | Negative in a losers bracket. Used by the tournament's round rules. |
| `round_label` | no (`Round <round>`) | Shown to the players, at most 1024 bytes |
| `entries` | no | Player 1 and player 2, once both are known, else `null` or absent |
| `entries[].id` | yes | Same rule as `id` |
| `entries[].accounts` | no | In the order to try: `{ "site": "<bridge site>", "id": "<what that site looks up>" }` or `{ "discord": "<Discord user ID>" }`. An ID in the wrong form finds nobody, a site the bridge does not have is not asked, and a site (or Discord) the connection may not look up is refused: in each case only that entry's set waits, with the reason logged once. |
| `underway` | no (`false`) | |
| `winner` | no | The winning entry's `id`, once decided |
| `phase` | no | The stage of the event the set is in (`"Pools"`, `"Top 8"`), 1 to 64 characters. A tournament's round rules can name it; a set without one is covered only by rules without `phase`. Added in contract 1 as an optional field, so a service without it stays valid. |

A set in the list that is not in this shape (an unknown `status`, a bad
`id`, three entries) is left out of the read, and logged once; the other
sets are played.

`GET {base}/sets/{id}` answers `200` with `{ "set": { ... } }` in the same
shape, or `200` with `{ "set": null }` when there is no such set. Only that
`null` tells the connector the set is gone, which cancels its live match.
The `set` key is required: an answer without it (`{}`, as a serializer that
leaves out nulls writes) is out of shape and tried again later. A `404`
here is read as your service not answering (a proxy, a route not mounted
yet), and tried again later.

`GET {base}/standing` is optional. It answers `200` with
`{ "standing": "finished" | "played_out" | "running" }`; `404` (or a service
without it) means running. `finished` stops the connector reading this
tournament for good.

### Writing

`POST {base}/sets/{id}/underway` with `{}`: mark the set started. Answer
`2xx`, also when it already was.

`POST {base}/sets/{id}/report`, with an `Idempotency-Key` header:

```json
{
  "winner": "player2",
  "scores": [{ "entry": "player1", "games": 1 }, { "entry": "player2", "games": 2 }],
  "games": ["player2", "player1", "player2"]
}
```

`games` is each game's winning entry, in the order played. Answer `2xx` once
recorded.

The body of a `2xx` to a write (`/report` or `/underway`) is not read: empty,
JSON or text are all taken. Only reads must answer JSON in the shapes above.

Reports are delivered at least once: when your answer is lost, the same
report is sent again, with the same `Idempotency-Key` and the same body.
Answer a repeat `2xx`, as you did the first time. If the set has a
different result (the organizer entered one, or another key), do not write
over it: answer `409`, and the connector leaves the set to the organizer.

If you cannot take one set's report now, for a reason of that set alone
(it is locked while a scorer edits it, say), answer `423 Locked` with:

```json
{ "retry": true, "reason": "the set is being edited" }
```

`retry` must be `true`; `reason` is optional, and its first 200 characters
go into the connector's log. That one result then waits on its own, a
minute doubling up to an hour, and is sent again with the same
`Idempotency-Key`, while every other result and read of the tournament goes
on. After 12 deferrals (about 6 hours) the set is left to the organizer. A
`423` without that body, or in answer to anything but a report (a read,
`/underway`), is read like any other `4xx`.

### Status codes

| Answer | The connector |
|---|---|
| `2xx` | carries on |
| `200` with `{ "set": null }` | the set is gone |
| `200` from a read whose JSON is out of shape (`{}` from `/sets/{id}`, say) | tries again later, after the growing wait above |
| `404` on `/standing` | the tournament is running |
| `401`, `403`, `426` | tries again later, after the growing wait above, whatever asked: nothing is given up for a token or version refused |
| `404` (anywhere but `/standing`), `429`, `5xx`, no answer | on a read, tries again later, after the growing wait above; on a report, sends that report again later, on its own |
| `409` on a report | leaves the set to the organizer |
| `423` with `{ "retry": true }` on a report | sends that report again later, on its own |
| other `4xx` | on a report, leaves the set to the organizer; on a read, tries again later |

A failed `/underway` is only logged, whatever the answer: the match is
played all the same.

A failing answer to a read, or a refused token or version on any request,
holds back the whole tournament until the growing wait is over. Any other
failing answer to one set's report, after a read that worked, holds back
only that result: it is sent again on its own, like a `423`, up to 12
times. A report you cannot take yet is best answered `423`
as above, which says so; one you will never take is a `409` or another
`4xx`.
