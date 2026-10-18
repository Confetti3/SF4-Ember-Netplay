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
| `set(id, purpose)` | one set, or `None` | `None` only when the site itself says the set does not exist. |
| `standing()` | `Finished`, `PlayedOut` or `Running` | `Finished` only when the site says the event is over: the connector then stops reading it for good. `PlayedOut` when every set looks played but more may come (a later stage, a reopened set). |
| `mark_underway(id)` | nothing | May be called again for a set already underway. A failure is only logged. |
| `report(id, report)` | `Sent` or `AlreadyHeld` | Records the winner and each entry's games won. May be called again for a report already recorded (see Delivery below): answer `AlreadyHeld` when the set already holds this winner, so the log does not claim the connector wrote it. If the site can tell, refuse (`Refused`) rather than write over a set that has a different result. |
| `names_can_change()` | `bool`, `false` unless overridden | Whether the site accounts given are names an account can change (Challonge usernames). The connector then points out, once per player, an account connected more than 30 days ago. |
| `wants_games()` | `bool`, `false` unless overridden | Whether `report` uses each game's winner, in the order played (`report.sequence`). Only then does the connector read the games from the bridge before reporting, and check that they add up to the score. |

`purpose` says why a read is made, `Scheduled` or `Result`: a site with a
request budget may refuse a scheduled read with `BudgetSpent`, but never a
read for a result.

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
    user ID). The bridge refuses a whole lookup that names an ID the site
    cannot have, or a site the connection may not look up. The connector
    then asks for each account alone, so only the entry with that account
    waits, and the reason is logged once for its set. Give IDs in the site's
    form and only sites the operator listed all the same;
  - `Account::Discord { user_id }`: a Discord account linked to the entry.
- `status`: `Pending` (waiting on an earlier set), `Open` (can be played,
  no result) or `Decided`.
- `underway`, when the site says so, and `winner` (the winning entry's
  `participant_id`) once decided.

Errors:

- `Unavailable`: the site as a whole cannot be used now. It is down,
  rate-limits, or refuses the credential. The pass stops, and the whole
  tournament waits under the growing wait below.
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
  restart never makes a second match.
- **Attempts.** A match that restarts (a disputed game) or expires unplayed
  is played again as a new attempt, up to 5; then the organizer decides.
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
- **Retries.** After a failure of the site as a whole nothing is asked of
  it, for any reason, until a wait of a minute, doubling up to an hour, has
  passed. One log line per run of failures. A deferred report waits the
  same way on its own, and holds nothing else back.
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
  "rounds": { "games_to_win": 2 }
}
```

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
connector would misread, answer a request of the older version with 400.
Unknown fields in your answers are ignored, so you may add fields freely.

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
        { "id": "kate", "accounts": [{ "site": "challonge", "id": "Kate_99" }] },
        { "id": "sam", "accounts": [{ "site": "startgg", "id": "1234567" }, { "discord": "274220342558756145" }] }
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
| `entries[].accounts` | no | In the order to try: `{ "site": "<bridge site>", "id": "<what that site looks up>" }` or `{ "discord": "<Discord user ID>" }`. Send only IDs in the site's form and sites the bridge connection may look up: an entry with one the bridge refuses waits, and its set is not played, until it is fixed. |
| `underway` | no (`false`) | |
| `winner` | no | The winning entry's `id`, once decided |

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
  "winner": "sam",
  "scores": [{ "entry": "kate", "games": 1 }, { "entry": "sam", "games": 2 }],
  "games": ["sam", "kate", "sam"]
}
```

`games` is each game's winning entry, in the order played. Answer `2xx` once
recorded.

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
| `404` on `/standing` | the tournament is running |
| `404` anywhere else | tries again later |
| `401`, `403`, `429`, `5xx`, no answer | tries again later, after the growing wait above |
| `409` on a report | leaves the set to the organizer |
| `423` with `{ "retry": true }` on a report | sends that report again later, on its own |
| other `4xx` | on a report, leaves the set to the organizer; on a read, tries again later |

A failed `/underway` is only logged, whatever the answer: the match is
played all the same.

These answers are about your service as a whole. A `5xx`, `429` or no
answer to one set's report stops the pass and holds back every other
result and read of the tournament until the growing wait is over, so
answer `5xx` only when the service itself is failing. A report you cannot
take yet is a `423` as above; one you will never take is a `409` or
another `4xx`.
