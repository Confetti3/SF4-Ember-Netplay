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

This is the published contract between the connector and an adapter.
Version 1, as in this crate.

| Method | Returns | The adapter promises |
|---|---|---|
| `open_sets(purpose)` | every set the site lists as open | Each playable set it can see, entries player 1 first. A set it cannot see may simply be missing: the connector never acts on a set's absence. |
| `set(id, purpose)` | one set, or `None` | `None` only when the site says the set does not exist. |
| `standing()` | `Finished`, `PlayedOut` or `Running` | `Finished` only when the site says the event is over: the connector then stops reading it for good. `PlayedOut` when every set looks played but more may come (a later stage, a reopened set). |
| `mark_underway(id)` | nothing | May be called again for a set already underway. A failure is only logged. |
| `report(id, report)` | nothing | Records the winner and each entry's games won. A site that takes games one by one gets them, in the order played, in `report.sequence`. If the site can tell, refuse (`Refused`) rather than write over a set that already has a result. |
| `names_can_change()` | `bool`, `false` unless overridden | Whether the site accounts given are names an account can change (Challonge usernames). The connector then points out, once per player, an account connected more than 30 days ago. |

`purpose` says why a read is made, `Scheduled` or `Result`: a site with a
request budget may refuse a scheduled read with `BudgetSpent`, but never a
read for a result.

A `Set` carries:

- `id`: 1 to 64 letters, digits, `_` or `-`, since it goes into URLs and
  match IDs. Leave out a set whose ID is anything else.
- `round` (negative in a losers bracket) and `round_label`, shown to the
  players.
- `entries`: player 1 and player 2 once both are known. Each `Entry` has an
  `participant_id` (same rule as set IDs) and its `accounts`, the one to try
  first first:
  - `Account::Site { site, id }`: an account on the bridge's sign-in site
    `site`, by the ID that site looks up (a Challonge username, a start.gg
    user ID). The bridge refuses a whole lookup that names an ID the site
    cannot have, or a site the connection may not look up, so give only IDs
    in the site's form and only sites the operator listed;
  - `Account::Discord { user_id }`: a Discord account linked to the entry.
- `status`: `Pending` (waiting on an earlier set), `Open` (can be played,
  no result) or `Decided`.
- `underway`, when the site says so, and `winner` (the winning entry's
  `participant_id`) once decided.

Errors: `Unavailable` means try again later (the site is down, rate-limits,
or refuses the credential); `Refused` means the site will not do it as it
stands, and the connector leaves the set to the organizer.

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
- **Retries.** After a failure nothing is asked of the site, for any
  reason, until a wait of a minute, doubling up to an hour, has passed. One
  log line per run of failures.
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
| `round_label` | no (`Round <round>`) | Shown to the players |
| `entries` | no | Player 1 and player 2, once both are known, else `null` or absent |
| `entries[].id` | yes | Same rule as `id` |
| `entries[].accounts` | no | In the order to try: `{ "site": "<bridge site>", "id": "<what that site looks up>" }` or `{ "discord": "<Discord user ID>" }`. Send only IDs in the site's form and sites the bridge connection may look up: one that is not makes the bridge refuse the lookup for every set of the read. |
| `underway` | no (`false`) | |
| `winner` | no | The winning entry's `id`, once decided |

`GET {base}/sets/{id}` answers `200` with `{ "set": { ... } }` in the same
shape, or `404` if there is no such set.

`GET {base}/standing` is optional. It answers `200` with
`{ "standing": "finished" | "played_out" | "running" }`; `404` (or a service
without it) means running. `finished` stops the connector reading this
tournament for good.

### Writing

`POST {base}/sets/{id}/underway` with `{}`: mark the set started. Answer
`2xx`, also when it already was.

`POST {base}/sets/{id}/report`:

```json
{
  "winner": "sam",
  "scores": [{ "entry": "kate", "games": 1 }, { "entry": "sam", "games": 2 }],
  "games": ["sam", "kate", "sam"]
}
```

`games` is each game's winning entry, in the order played. Answer `2xx` once
recorded. If the set already has a result, do not write over it: answer
`409`, and the connector leaves the set to the organizer.

### Status codes

| Answer | The connector |
|---|---|
| `2xx` | carries on |
| `404` | reads it as no such set (or no `/standing`) |
| `401`, `403`, `429`, `5xx`, no answer | tries again later, after the growing wait above |
| other `4xx` | leaves the set to the organizer |
