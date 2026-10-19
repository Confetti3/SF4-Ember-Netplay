# Match watch

Status: design, revised after the design review and its round 2
(`review-match-watch-design.md`, CHANGES REQUIRED). §13 maps every finding
to its resolution. Not built. Branch `feat/match-watch` off nightly
`c98ac83f`.

Spectators and tournament organizers (TOs) join a bridge tournament match's
room to watch it. This document fixes the wire shapes, storage, helper, room
model and game changes, and splits the work so separate agents can build it.

## 1. Goal and decisions

Goal: a TO, a caster or another approved viewer opens a bridge match and
watches it live in SF4, inside the match's own room, without being able to
affect the set.

Decisions:

- Who may watch is the organizer's choice.
  - TOs, and casters or staff they approve, can always watch.
  - Per connection, the organizer also chooses whether anyone else holding
    the match link may watch.
  - The default keeps today's behaviour: no watchers. BluMint and existing
    connections change nothing unless they opt in.
- Every watcher needs an Ember ID.
  - It is made in the game in a moment, and no account linking is required
    (§2.5).
  - The bridge knows who watches and can revoke a watcher.
  - The room shows watchers by name.

Design summary:

- **Watchers in the binding.** Watchers travel in the signed room Binding as
  an optional `watchers` list, left out when empty.
  - The room learns them through the one bridge-verified channel it already
    applies: the leader fighter's own binding.
  - The bridge's `match_watchers` table is the source, so the list survives
    rebinds and room replacement.
- **The watch route.** A player-session watch route checks policy, records
  the watcher's endpoint and returns the room id and invitation. It returns
  no signed binding, since the watcher's game never applies one.
- **Revisions.** A change to the watcher set moves `binding_revision`, which
  orders bindings in the room. The bridge also records the revision of the
  last fighter-endpoint change, and prepares are checked against that. A
  watcher change therefore never fails a prepare (§2.1).
- **Watchers only watch.** They are placed as spectators once, when they
  join, and they stop by leaving. They have no table actions. They never
  vote in coordination, never lead, never hold host powers and never hold a
  start.
- **The match link.** The existing match link
  (`embernetplay.link/m#<bridge>/<match>`) is the watch link too: a
  non-fighter who opens it is offered Watch (§12 Q4).

## 2. Current behaviour and verified findings

Paths are relative to the repository root. Bridge paths start at
`server/ember/`.

### 2.1 How a binding change reaches the room (brief item 1)

It works today for a revision bump with no endpoint change.

- **Fresh bindings.** The bridge signs a fresh binding on every claim and
  publish answer:
  - `binding()` and `room_answer` (`server/ember/bridge/src/routes/play.rs:218-278`)
    rebuild from `match_rooms` with the current `binding_revision`;
  - so a fighter's 10 s claim (`ClaimIntervalMs`, `src/netplay/TournamentPlay.hxx:25`)
    always brings the current revision.
- **Endpoint-only bumps.** `rebind` (`play.rs:182-215`) moves the revision
  only when the `bound_endpoints` text changes (`:192-199`). So watchers need
  their own stored set and their own bump (§4.2).
- **The game takes the newer binding.**
  - `OnRoom` replaces its binding when the reply supersedes it (`src/netplay/TournamentPlay.cxx:320-322`).
  - `SupersededBy` accepts an equal or higher revision (`src/session/RoomModelTournament.cxx:41-45`).
- **The room applies it.**
  - `FollowClaim` offers Bind when this game is the writable leader and the
    revision differs (`TournamentPlay.cxx:221-240`).
  - `BindTournament` skips its equality shortcut on a new revision (`RoomModelTournament.cxx:225-226`)
    and evicts every member the binding does not name (`:244-252`).
  - The server sends evicted members away as kicked (`src/session/sf4e__SessionServer__Tournament.cxx:46-61`).
- **Latency.** A watcher change reaches the room within one leader claim
  interval (10 s), plus the bind commit.

**Watcher churn must not reach the permit path** (review finding 1).

- `check_descriptor` requires a prepare to name the current
  `binding_revision` (`play.rs:675-683`).
- `prepare()` agrees two descriptors with `Prepare::same_game`, which
  compares `binding_revision` too (`play.rs:637-646`, `server/ember/protocol/src/play.rs:307-316`).
- If every watcher change moved the revision that prepares are checked
  against, each bump would restart convergence. A watcher who keeps joining
  and leaving could hold a start off until `PermitHoldMs` (105 s, `src/session/TournamentBinding.hxx:30`)
  calls it off.

The fix is bridge-only, and leaves the helpers and the protocol's
`same_game` untouched:

- A new column `match_rooms.fighters_revision` holds the `binding_revision`
  of the last fighter-endpoint change. `rebind` writes it whenever it bumps
  (`play.rs:196-199`).
- `check_descriptor` accepts any `binding_revision` in
  `[fighters_revision, binding_revision]`. Endpoints have not changed in
  that range, which is what the check protects.
- `prepare()` agrees two descriptors with a bridge-local comparison. It is
  `same_game` with `binding_revision` ignored, provided both descriptors are
  at or above `fighters_revision`.
- A permit carries no binding revision (`protocol/src/play.rs:336-353`).
  Once issued, it is answered before `check_descriptor` runs (`play.rs:612-630`).
- The migration sets `fighters_revision = binding_revision` on every
  existing row. Every live match then behaves exactly as today.

### 2.2 Signature and an additive field (brief item 2)

- **The signature covers canonical bytes of the typed value.**
  - Signing is `prefix || JCS(json::to_value(binding))` (`protocol/src/play.rs:227-266`,
    `protocol/src/sign.rs:32-37`, `protocol/src/json.rs:1-7`).
  - The verifier re-serializes the parsed struct, so the signed bytes are
    exactly what each side's struct serializes.
- **An omitted field is byte-identical.**
  - `starting_wins` is the precedent: `#[serde(default, skip_serializing_if = ...)]`
    (`protocol/src/play.rs:183-184`), with a byte-identity test (`:628-677`).
  - A binding with no watchers is exactly what every released helper
    accepts.
- **What an old helper does with `watchers`.** It fails the whole answer:
  - `Binding` is `deny_unknown_fields` (`protocol/src/play.rs:162-163`);
  - the helper parses the whole `ClaimAnswer` (`rust/sf4-net/src/tournament/play.rs:216-217`,
    and `:309-310` for publish), failing with `bridge_invalid_response`;
  - `Final()` does not list that code (`TournamentPlay.cxx:25-28`), so the
    fighter retries every 5 s and never gets the room again.
- **Why each fighter's claim says it reads watchers** (review finding 11
  reversed, after the bridge review of part A):
  - The first design deleted `Claim.watch`, arguing that a watcher can reach
    the watch route only with a build whose DLL sends `match_watch`, and that
    build's paired helper parses `watchers`. That holds for an honest watcher
    only. The route needs a player session and a proof, Ember IDs are free,
    and the build ID of a released build is public: every install of it sends
    it in its own claims. A staff member, or any link holder once a
    connection turns `link_holders` on, could send the Stable build as its
    `build_id`. The bridge would then name the watcher in a binding that
    Stable fighters' helpers refuse as an unknown field, so the fighters'
    claim answers would fail and a set on Stable 1.3.0 could not start.
  - The party at risk is the fighter, so the fighter vouches. A claim gains an
    optional `"watch": true`, meaning this fighter's helper reads a binding
    that names watchers. It is the `ready` precedent: `Option<bool>`, serde
    default, left out when unset (so a claim from a helper that predates it is
    byte for byte what an older bridge accepts), sent only to a bridge that
    lists `matches.watch`, and stored per fighter in `match_claims.watch`
    (default 0).
  - The bridge names watchers in a binding only when both fighters' latest
    claims say `watch: true` and the watcher's `build_id` equals both
    fighters' claimed build (§4.2). A fighter who claims again without it
    takes the watchers out of the binding at that claim.
  - The helper is not hashed or version-checked at run time, so the claim
    field, not the build ID, is what carries the pairing of a helper with the
    bridge's `watchers`.
- **No room-snapshot compatibility issue.** A room admits one build: the
  native join refuses another sidecar hash (`src/session/sf4e__SessionServer.cxx:504-506`).

### 2.3 Strangers holding a leaked invitation (brief item 3)

Today, a stranger holding the room invitation gets real coordination
standing before the native refusal, and the host never takes it back.

- **The invitation is the only proof.**
  - A control connection is accepted while `controls.len() < MAX_CONTROL_PEERS`
    (15) (`rust/sf4-net/src/service/games.rs:19-25`, `rust/sf4-net/src/service/mod.rs:43`).
  - The handshake checks room, build, capability and expiry only (`rust/sf4-net/src/transport.rs:470-500`,
    `rust/sf4-net/src/invite.rs:475-502`).
- **Admission comes before the native join.**
  - The host queues `add_and_promote` for the joiner's Admission (`service/controls.rs:449-518`).
  - That adds a Raft learner and promotes voters from `admission_order`, with
    no roster filter (`service/members.rs:402-459`).
  - Native frames wait for that operation (`service/controls.rs:679-687`).
  - Host, opponent and stranger count as three members, so three voters
    (`rust/sf4-net/src/recovery.rs:888-894`).
- **The native refusal comes after.** `RoomAuthority::Join` refuses an
  endpoint outside the binding (`src/session/RoomModel.cxx:428-430`), and
  the server answers `JR_REQUEST_INVALID` (`sf4e__SessionServer.cxx:542-549`).
- **Nothing takes the standing back.**
  - Reconciliation retires only endpoints that were in the previous
    committed native roster (`service/members.rs:139-153`).
  - The C++ server never sends `close_control` after a rejection, and
    control links have no idle timeout.
- **Worst case with a modified client that stays connected.**
  - It keeps its slot, learner seat and any voter seat.
  - Fifteen of them lock out a reconnecting fighter (`games.rs:47-48`).
  - Voter strangers can stall commits (`members.rs:222-224`).
  - Each new incarnation also uses membership history (`rust/sf4-net/src/coordination.rs:47-49`).
- **Who holds the invitation today:**
  - the two fighters;
  - the bridge, sealed in `match_rooms`;
  - the fighters' Discord friends, because presence advertises it in every
    non-public room (`src/sf4e/sf4e__NetplayRuntime__Status.cxx:209-213`,
    `src/discord/Presence.hxx:47-51`).

Watching widens who holds the invitation. §5.4 and §8 close this for bound
rooms and narrow it before the bind.

### 2.4 Voter selection and host or leader inheritance (brief item 4)

**Coordination voters (helper).**

- **Count.** `stable_voter_count` gives 1 voter for up to two members, 3 for
  three or four, and 5 above that (`rust/sf4-net/src/recovery.rs:888-894`).
  Two fighters plus one watcher jumps from 1 voter to 3, and promotes the
  watcher.
- **Every place that picks voters:**
  - (a) promotion at admission: candidates from `admission_order`, count
    from `desired_voters` (`service/members.rs:402-459`, `service/server_owned.rs:32-38`);
  - (b) the replacement set when voters leave (`service/members.rs:225-264`);
  - (c) `restore_stable_voters`, which shrinks and grows (`service/members.rs:594-655`),
    run from the per-second refresh (`service/refresh.rs:220-234`);
  - (d) the learner handoff of a departing sole-voter leader, which takes
    any applied member with a control link (`service/departure.rs:166-199`);
  - (e) the leader's `remove` RPC, which counts learners in `staying` (`rust/sf4-net/src/coordination.rs:1536-1546`);
  - (f) recovery election, which picks only among voters (`service/controls.rs:283-305`).
- **Leadership.** Only voters can lead. The game's `authorityWritable` is
  the coordination leader, and only the writable leader's game offers Bind
  (`TournamentPlay.cxx:224`). So keeping watchers out of the voter set also
  keeps them out of the writable authority.
- **Precedent.** Server-owned rooms keep members as learners for good, but
  room-wide (`members.rs:416-417, 201`). There is no per-endpoint hook.
- **What the helper knows of the binding.**
  - `Host` and `Join` carry only epoch, build and invitation (`service/protocol.rs:9-17`),
    and tournament commands never reach the room actor (`service/entry.rs:45-48`).
  - But every room change, `BindTournament` included, commits through a
    recovery candidate proposed as a full checkpoint (`sf4e__SessionServer__Tournament.cxx:34-39, 64`;
    `src/session/sf4e__SessionServer__Recovery.cxx:303-307`).
  - The actor parses each committed checkpoint once, through
    `committed_roster` (`service/refresh.rs:210-219`).
  - The checkpoint holds the room snapshot, and so the binding, at
    `checkpoint.room.snapshot.tournament` (`src/session/sf4e__SessionServer__RecoveryCheckpoint.cxx:50`,
    `src/session/RoomModelJson.cxx:68, 466`).
  - The bound roster is therefore available on the same path that drives
    voter reconciliation, with no new IPC.

**Native host (room model).**

- `Leave` hands host to the oldest remaining member, watchers included (`src/session/RoomModel.cxx:676-690`),
  and `TransferHost` accepts any member (`RoomModel.cxx:147-156`).
- Host powers in a bound room:
  - Close (`src/session/RoomModelActions.cxx:8-14`);
  - SetCapacity (`:16-20`; this is not refused in a bound room);
  - Lock (`:22-25`);
  - Rename (`:79`);
  - CancelResult of a live game (`:372-375`).
- So a watcher who inherited host could close the room or cancel a live
  game.

**The smallest correct change.** One predicate, used where voters are
picked:

> In a room whose committed snapshot carries an active tournament binding, an
> endpoint may vote only if the binding names it as a fighter.

- (a) to (d) filter candidates by it, and count only eligible members for the
  desired voter count.
- Two eligible fighters give `stable_voter_count(2) = 1`: the leader alone.
  - That is today's two-member shape, and the learner handoff stays between
    the fighters.
  - So the stall profile of a two-fighter match is unchanged, with 0 to 8
    watchers.
- (e) and (f) need no change.
  - In a bound room the voter set is the single leader, so `remove` never
    has a non-leader voter as its source.
  - The one transition left is a room that had three voters before its bind
    (host, opponent, and a stranger with a leaked invitation).
    - If that stranger leaves gracefully after watchers are admitted as
      learners, `remove` can count fighters plus watchers as staying, and
      commit two voters.
    - The next refresh (at most about 1 s, `refresh.rs:220-234`) runs (c)
      and shrinks back to one voter.
    - Freezing the room needs a fighter to drop inside that second.
  - The §5.4 retirement rule makes this rarer still, because the stranger is
    retired at the first refresh after the bind.
  - The earlier draft's rule that a watcher must wait for one voter is
    dropped (review finding 13). It added a refusal state to close a
    one-second window that (c) already closes.
- **When the predicate first applies, the leader is a fighter.** Bind is
  applied only by the writable leader's `TournamentPlay`, which holds a
  binding naming it as a fighter.
- **If both fighters are gone,** no one is eligible and the room ends for
  its watchers. That is correct: there is nothing left to watch.

### 2.5 Does a watcher's Ember ID need linking? (brief item 5)

No. A player session is enough.

- The public-room ticket route needs only a `tournament:participate` player
  session (`bridge/src/routes/rooms/ticket.rs:38`, through `playing()` at
  `bridge/src/routes/play.rs:94-104`). It never checks a provider link.
- Sessions come from a challenge and a proof, with no link (`bridge/src/routes/sessions.rs:26, 40, 253-299`).
- The watch route follows that model: a session plus a proof over the exact
  command. It needs new `Action` variants (`protocol/src/challenge.rs:23-46`)
  and `route_matches` entries (`bridge/src/routes/sessions.rs:113-137`).
- The watcher's helper needs a bridge it trusts, to sign the proof. That is
  the built-in bridge, or one approved on Linked accounts (`src/ui/IdentityPanel.cxx:791-799`).

### 2.6 Other current behaviour this design relies on

- **Bound tables.** `seatingOrRules` refuses Queue, Unqueue, Watch, Unwatch
  and SetRules at a bound table (`src/session/RoomModelActions.cxx:477-479`).
- **Table revisions.** A Ready is a table action checked against the table
  revision (`RoomModelActions.cxx:420-427`). The client resends after a
  `StaleTable` at most 3 times (`src/session/sf4e__SessionClient__Room.cxx:456-458`).
  `ApplyWatch` and `StopWatching` `Touch` the table (`RoomModelActions.cxx:204-230`).
- **UI.** The UI hides invite rows, Leave seat, Queue, Watch next and Kick
  in tournament rooms (`src/ui/RoomPanel.cxx:243-251, 344-347, 360-365, 400-403`),
  but not the seat chooser (`RoomPanel.cxx:200`, `src/ui/RoomControls.cxx:78-91`).
- **Spectators.** Slot 2 and above spectate from P1 over a QUIC game link
  (`src/sf4e/sf4e__UserApp.cxx:207-240`, `src/session/MatchAuthority.cxx:199-221`).
  - GGPO withholds RUNNING until every spectator synchronizes, for up to
    `SyncDeadlineMs` (5 s). After that, `SpectatorPolicy` drops slow
    spectators (`src/common/SpectatorPolicy.hxx:7-28, 96-101`).
  - Limits: `MaxSpectators = 14` (`src/common/RoomLimits.hxx:10`) and
    `MAX_GAME_LINKS = 15` (`rust/sf4-net/src/service/mod.rs:44`).
  - A spectator added during a game waits in `watchingNext`, and moves into
    `spectators` when the game closes (`RoomModelActions.cxx:215-216`,
    `RoomModel.cxx:571-583`).
- **Lock-in hold.** A locked-in spectator still retiring the last game holds
  a start for up to 10 s (`src/session/RoomModel.cxx:903-926`).
- **Capacity.** It is checked before the bound-fighter exception in Join
  (`RoomModel.cxx:411` before `:428-430`), and can be as low as 2 (`:15-16`).
- **Address exposure.** `relay_only` is a whole-helper setting chosen at
  helper start (`rust/sf4-net/src/main.rs:20-38`), plus the room RPC bind
  (`rust/sf4-net/src/recovery.rs:82-113`). There is no per-peer relay rule
  for game links.
- **Organizers.**
  - Organizer credentials are tenant-wide; provider credentials are per
    connection (`bridge/src/auth.rs:32-44, 261-274`).
  - Organizer credentials are issued only by the operator, through the CLI
    (`bridge/src/lib.rs:228-274`, `bridge/src/main.rs:210`).
  - `provider_connections` is a database table (`bridge/migrations/001_identity.sql:9-16`).
  - `GET /v1/matches/{id}` has no room info (`bridge/src/routes/matches.rs:351-418`).
  - Matches are never deleted: `routes/expiry.rs` only moves their state.
- **Prior spec.**
  - §14.6 asks for explicit, separately authorized observer access (`docs/design/identity-bridge/EMBER_IDENTITY_AND_TOURNAMENT_BRIDGE_SPEC.md:755-759`),
    and ROOM-06 forbids a spectator queueing into a fighter seat (`:1500`).
  - Code validates only `observer_policy: "authorized_only"` (`protocol/src/matches.rs:123-125`).
  - BluMint drops `spectators` (`bridge/src/routes/blumint/create.rs:30-36`),
    and its guide says staff cannot watch (`docs/guides/BLUMINT_QUICKSTART.md:357-358`).
- **Links.**
  - The match link `https://embernetplay.link/m#<bridge>/<match>`, and its
    `ember://` form, are parsed in `src/common/TournamentLink.hxx:113-175`.
  - They open Online, then Ember ID, then Tournament matches (`src/ui/ApplicationShell.cxx:739-747`).
- **Discord.**
  - `PublishAndTickDiscordInvite` (`Status.cxx:590-605`) accepts invites; it
    can leave a room to join another. It does not advertise.
  - Advertising is only the presence secret at `Status.cxx:209-213`.

## 3. Wire changes

All bodies are strict JSON (`protocol/src/json.rs`). Request types are
`deny_unknown_fields`.

### 3.1 Capabilities

- Add the feature `matches.watch` (`ember_protocol::play::WATCH_FEATURE`) to
  `capabilities()` (`bridge/src/routes/discovery.rs:28-55`). It means the
  bridge serves the watch routes and may sign bindings with `watchers`.
- It also means the bridge reads `watch` in a claim.
- The helper reads it before sending `match_watch` (§5.3), and before putting
  `watch: true` in a claim (§5.1).

### 3.2 Binding

```rust
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BoundWatcher {
    pub ember_id: EmberId,
    pub endpoint_id: String,
}

pub struct Binding {
    // ...every field as today, starting_wins included...
    /// Endpoints admitted to watch the match, oldest first. Left out when
    /// empty, so a binding without watchers is the bytes every released
    /// helper accepts.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub watchers: Vec<BoundWatcher>,
}
```

- `version` stays 1. `roster_digest` still covers only the fighters.
- New checks in `Binding::check`:
  - at most `MAX_WATCHERS` (8) watchers. This is the wire maximum; the
    bridge binds at most 6 (`WATCHER_CEILING`, §3.6);
  - each `endpoint_id` is 64 lowercase hex;
  - endpoints and Ember IDs are distinct among watchers, and none equals a
    fighter's.
- Only fighters receive bindings. The watch route returns none (§3.3).

### 3.3 Watch routes (player)

**`POST /v1/matches/{match_id}/watch`**

- Auth: a player session with `tournament:participate`, plus a proof with
  `Action::MatchWatch` (`"match.watch"`).
- `route_matches` gets `(MatchWatch, Post) => id_between("/v1/matches/", "emt", "/watch")`.

The command:

```json
{ "endpoint_id": "<64 hex>", "helper_instance_id": "ins_...", "build_id": "<build>" }
```

The answer is `200`, tagged by `state` (`WatchAnswer`):

```json
{ "state": "waiting", "retry_after": 10, "reason": "not_bound" }
{ "state": "room", "room_id": "<32 hex>", "invitation": "sf4e3:..." }
```

`reason` is one of:

- `no_room`: no room is published yet;
- `not_bound`: the bridge has no binding that can name the watcher yet. A
  fighter has not claimed, or the fighters' claims do not both say their
  helpers read watchers (§2.2);
- `full`: the binding has no place for this watcher now, and one may free up.
  The link-holder cap (2) or the ceiling (6 in all) is reached, another watcher holds the
  same endpoint (§4.2 step 3), or the endpoint is a fighter's own. This is
  transient, so it is a waiting state, not an error. A client cannot tell
  these apart, and does not need to: it asks again.

Refusals use existing codes with `details.reason`, as public rooms do
(`bridge/src/routes/rooms/mod.rs:175-200`):

| HTTP | code | details.reason | Meaning |
|---|---|---|---|
| 403 | `forbidden` | (none) | Session lacks `tournament:participate` |
| 404 | `not_found` | (none) | No such match |
| 422 | `unsupported_rules` | (none) | Not an `ember-room-v1` match |
| 409 | `stale_revision` | `finished` | Match is terminal |
| 403 | `forbidden` | `fighter` | The caller plays in this match |
| 403 | `forbidden` | `not_allowed` | Not staff, and the connection is not open to link holders |
| 403 | `forbidden` | `revoked` | Denied for this match |
| 422 | `incompatible_build` | (none) | `build_id` differs from the fighters' claimed build |
| 429 | `rate_limited` | (none) | More than 30 calls per minute per Ember ID |

On every allowed call, the route:

1. upserts the caller into `match_watchers` (`last_seen_at = now`);
2. runs `rebind` (§4.2);
3. answers `room` if `rebind` placed the caller in the bound set, and
   `waiting` otherwise.

**`POST /v1/matches/{match_id}/watch/leave`**

- Proof `Action::MatchUnwatch` (`"match.unwatch"`), with the command `{}`.
- It deletes the caller's row and runs `rebind`.
- Answer: `200 {"left": true}`.

### 3.4 Policy routes (organizer)

These are sized to the decisions in §1: a per-connection flag, a staff list and
a per-match revocation. Nothing else.

**Auth.**

- Writes need an Organizer credential whose tenant owns the connection, or
  the match's connection.
- A Provider credential for that connection may read.
- Anyone else gets `not_found`.

**BluMint.** Writes for a `blumint` connection (`bridge/src/config.rs`,
`Connection::is_blumint`) answer `403 forbidden`, with reason
`connection_fixed`, in phase 1. That guarantees no BluMint behaviour change,
whoever holds that tenant's organizer credential (§12 Q5).

**Audit.** Every write is audited in its transaction (`bridge/src/audit.rs:7-22`).
The actions are `watch.link_holders.set`, `watch.staff.add`,
`watch.staff.remove`, `watch.deny.set` and `watch.deny.clear`.

**Connection policy**

`GET /v1/connections/{connection_id}/watch` answers:

```json
{ "connection_id": "...", "link_holders": false, "link_holder_cap": 2, "ceiling": 6,
  "staff": [ { "ember_id": "...", "added_at": 1760000000 } ] }
```

`PUT /v1/connections/{connection_id}/watch` takes `{"link_holders": true}`.
Its description, in the route docs and the guide, states the address
exposure of §8.

**Staff (approved for every match of the connection)**

- `PUT /v1/connections/{connection_id}/watch/staff/{ember_id}`, with the
  body `{}`. Answer: `200 {"ember_id", "added_at"}`. Putting an Ember ID
  that is already staff answers `200` with its original `added_at`.
- `DELETE` on the same path. Answer: `204`.
- A TO who wants to watch adds their own Ember ID.

**Revocation for one match**

- `PUT /v1/matches/{match_id}/watch/denials/{ember_id}`, with the body `{}`
  like the staff `PUT` (one contract for both). Answer:
  `200 {"ember_id", "set_at"}`. `DELETE` on the same path answers `204`.
- A denial overrides both staff and link-holder access.

**The organizer's view**

`GET /v1/matches/{match_id}/watchers` answers (`via` is how the policy treats
that Ember ID now: `staff`, `link_holders`, `denied`, or `none` when the
connection is closed to link holders and the Ember ID is not staff; rows
outlive denials and policy changes, so all four occur):

```json
{ "match_id": "emt_...", "link_holders": false,
  "watchers": [ { "ember_id": "...", "via": "staff", "bound": true, "first_seen_at": 1760000000, "last_seen_at": 1760000300 } ],
  "denied": [ "..." ] }
```

Every write is audited under the organizer's credential, with the Ember ID it
approved, removed, denied or allowed again as the entry's `reason`, or `on` /
`off` for `link_holders`. The organizer routes check the caller before they
check the path and body, so an unauthenticated caller gets `401`.

`GET /v1/matches/{id}` and the created-match body are unchanged. The
existing `play_url` is the link watchers open (§12 Q4).

### 3.5 Events

No new event kinds in phase 1. `Kind::ALL` and every webhook consumer stay
as they are (`protocol/src/event.rs:20-103`).

The later notifier phase (part D) adds one optional field to
`match.room.ready`: `watch_url`. It is present only when the match's
connection has staff or `link_holders` on, which is never true for BluMint
in phase 1.

### 3.6 Limits

| Name | Value | Where |
|---|---|---|
| `MAX_WATCHERS` (wire maximum) | 8 | protocol `play.rs`; room `MaxWatchers` |
| `WATCHER_CEILING` (watchers in all, staff included) | 6 | bridge constant |
| `LINK_HOLDER_CAP` | 2 | bridge constant |
| watch poll | 10 s | game `WatchIntervalMs` |
| watcher liveness | 60 s since `last_seen_at` | bridge |
| watcher GGPO sync deadline at a bound table | 2 s | `SpectatorPolicy` (§6.6) |

The cap and ceiling are constants until an organizer needs a knob. They are
lower than the 4 and 8 first designed, because of the load measurement in §6.6
(§12 Q1). The wire and room maximum stays 8, so the ceiling can be raised again
without a protocol change.

## 4. Bridge storage

### 4.1 Migration `028_match_watch.sql`

```sql
-- Anyone holding the match link may watch (the organizer's choice).
ALTER TABLE provider_connections ADD COLUMN watch_link_holders INTEGER NOT NULL DEFAULT 0;

-- Ember IDs approved to watch every match of a connection.
CREATE TABLE connection_watch_staff (
    connection_id TEXT NOT NULL REFERENCES provider_connections(id),
    ember_id TEXT NOT NULL,
    added_at INTEGER NOT NULL,
    added_by TEXT NOT NULL,
    PRIMARY KEY (connection_id, ember_id)
);

-- One match's revocations.
CREATE TABLE match_watch_denials (
    match_id TEXT NOT NULL REFERENCES matches(id),
    ember_id TEXT NOT NULL,
    set_at INTEGER NOT NULL,
    set_by TEXT NOT NULL,
    PRIMARY KEY (match_id, ember_id)
);

-- Watchers who asked to watch, with the endpoint their room joins from.
CREATE TABLE match_watchers (
    match_id TEXT NOT NULL REFERENCES matches(id),
    ember_id TEXT NOT NULL,
    endpoint_id TEXT NOT NULL,
    helper_instance_id TEXT NOT NULL,
    build_id TEXT NOT NULL,
    first_seen_at INTEGER NOT NULL,
    last_seen_at INTEGER NOT NULL,
    PRIMARY KEY (match_id, ember_id)
);
CREATE INDEX match_watchers_endpoint ON match_watchers (match_id, endpoint_id);

-- Two watchers who asked for one endpoint, once per pair and endpoint, so
-- the audit log records a loss when it first happens.
CREATE TABLE match_watch_conflicts (
    match_id TEXT NOT NULL REFERENCES matches(id),
    loser TEXT NOT NULL,
    winner TEXT NOT NULL,
    endpoint_id TEXT NOT NULL,
    PRIMARY KEY (match_id, loser, winner, endpoint_id)
);

-- The watcher set the current binding names, as a JSON list of
-- {"ember_id", "endpoint_id"}. NULL means none, which is every row today,
-- so deploying changes no live match.
ALTER TABLE match_rooms ADD COLUMN bound_watchers TEXT;

-- The binding revision of the last fighter-endpoint change; prepares are
-- checked against it (section 2.1). Backfilled so live matches keep today's
-- check exactly.
ALTER TABLE match_rooms ADD COLUMN fighters_revision INTEGER NOT NULL DEFAULT 0;
UPDATE match_rooms SET fighters_revision = binding_revision;

-- Whether a fighter's latest claim said its helper reads a binding that
-- names watchers. A claim that does not say reads false.
ALTER TABLE match_claims ADD COLUMN watch INTEGER NOT NULL DEFAULT 0;
```

The number is 028, not 027: 027 is taken on `feat/challonge`, and `migrate`
applies per version, so the gap is safe. Register it with `include_str!` in
`bridge/src/db.rs`, after 026.

### 4.2 How watchers are kept per match

`match_watchers` is the source. The bound set is derived, never edited in
place. One derivation serves the whole binding: `rebind` in
`bridge/src/routes/play/mod.rs` works out both the fighters' endpoints and the
watchers, and `room_answer` calls it, so every answer that carries a room or a
binding is derived afresh. The watcher half is `named` in
`bridge/src/routes/play/watch/rewatch.rs`. It returns a typed `Named` (no room,
not bound, or the set), which `rebind` stores through `room::bind` and the
watch route turns into its answer, so the two never decide separately. The
room row keeps the set as a typed list (`room::Room::bound_watchers`).

1. **Short-circuit.** `named` names nobody when the room is not published or a
   fighter has not claimed, and when no watcher row is live. Deploying
   therefore touches nothing, and BluMint and Stable matches never get past
   this step (review finding 2).
2. **Select.** Read the rows with `last_seen_at > now - 60`, oldest
   `first_seen_at` first. Keep a row only when all of these hold:
   - both fighters' latest claims say `watch: true` (§2.2). Otherwise nobody
     is named, whatever the watchers declare;
   - its Ember ID has no denial for the match;
   - it is staff, or the connection has `watch_link_holders = 1`;
   - it is not a fighter, by Ember ID or by endpoint (a fighter who was
     reassigned, or who claimed a watcher's endpoint, takes precedence);
   - its `build_id` equals both fighters' current claimed `build_id`.
3. **Endpoint conflicts.** Two kept rows naming one endpoint (review
   finding 9):
   - a staff row beats a link-holder row;
   - between equals the later `first_seen_at` wins, the newest occupant. The
     first design said the most recent `last_seen_at`, but two watchers
     polling every 10 s would then swap the endpoint on every poll, moving
     `binding_revision` and forcing a leader bind each time. `first_seen_at`
     is not moved by polling. A watcher whose row had gone quiet (over 60 s)
     starts a new arrival when it asks again (its `first_seen_at` is reset). For
     the cap, a returning watcher does not push out one who took its place: it
     waits with `full` (`bridge/tests/watch_claims.rs`). For an endpoint tie it
     is the other way round: the returning watcher is now the later arrival,
     so it takes its endpoint back (the cost is in §12 Q6);
   - the loser is dropped, and the conflict is audited once per pair and
     endpoint (`watch.endpoint_conflict`, keyed in `match_watch_conflicts`)
     so the organizer can deny the other Ember ID. The cost: an allowed link
     holder who registers another link holder's endpoint takes it at once and
     keeps it. The organizer sees it in the audit and can deny the squatter
     (§12 Q6).
4. **Cap.** Take staff first, then link holders up to `LINK_HOLDER_CAP`,
   stopping at `WATCHER_CEILING`.
   - A staff arrival can therefore push a link holder out when the ceiling
     is reached. That evicts the link holder at the next bind, even
     mid-game. This is intended: staff "can always watch".
5. **Compare and store.** An empty result is NULL. A non-empty result
   serializes as `[{"ember_id", "endpoint_id"}, ...]`. `rebind` compares both
   derived values with the stored ones in one step:
   - a change of the fighters' endpoints bumps `binding_revision` and sets
     `fighters_revision` to it;
   - a change of watchers alone bumps `binding_revision` and leaves
     `fighters_revision` alone.

`rebind` runs, inside the existing write transactions:

- the watch route and the leave route;
- every fighter claim and every room publish, through `room_answer`, including
  the refresh of the current room's invitation.

Every fighter claim therefore applies revocations, policy changes, liveness
expiry and a fighter's change of `watch` within one claim interval. The
organizer routes only write rows.

Other bridge changes:

- `binding()` adds `watchers` from `bound_watchers`.
- `check_descriptor` and `prepare()` change as described in §2.1.
- **A replaced room keeps its watchers.** `publish` resets `bound_endpoints`,
  not `match_watchers`. The new room's binding names the same watchers, and
  their next poll returns the new room.
- **Rows persist with the match.** Matches are never deleted (§2.6). Rows
  are bounded by the distinct Ember IDs that asked to watch, and they serve
  the organizer's view afterwards. A later cleanup job is out of scope.

## 5. Helper changes

### 5.1 Claims and fighter bindings

- A claim gains `"watch": true`, sent only to a bridge that lists
  `matches.watch` (the `ready` precedent), so the bridge names watchers only
  for helpers that parse them (§2.2). The game's claim request carries it from
  the same capability check.
- `accept_binding` (`rust/sf4-net/src/tournament/play.rs:78-115`) checks
  what it checks today. Its view adds `"watchers": [{"ember_id", "endpoint_id"}]`,
  so the game can name watchers.

### 5.2 No watcher binding

The watcher's helper receives no binding (review finding 10). It needs only
the room id and the invitation, over its authenticated session. Names come
from the room snapshot once it has joined.

This drops the following from the earlier draft:

- `accept_watch_binding`;
- `local_watcher`;
- the watcher path in `DecodeBinding`;
- signing a binding for a non-fighter.

It also stops handing every link holder both fighters' endpoints, and
everyone's Ember IDs, before admission.

### 5.3 IPC

New tournament ops in `Request` (`rust/sf4-net/src/tournament/mod.rs:45-209`):

```json
{ "op": "match_watch", "bridge_id": "brg_...", "match_id": "emt_...", "build": "<build>" }
```

Its reply is one of:

```json
{ "role": "waiting", "retry_after": 10, "reason": "not_bound" }
{ "role": "room", "room_id": "...", "invitation": "..." }
```

```json
{ "op": "match_unwatch", "bridge_id": "brg_...", "match_id": "emt_..." }
```

That reply is `{}`.

- `match_watch` reads the bridge's capabilities first, and caches the
  result per bridge like `reads_ready` (`play.rs:263-276`). A bridge without
  `matches.watch` fails at once with `watch_unsupported`, sending nothing.
- Refusals pass the bridge's `details.reason` through as `reason`.
- Room join gains an observer flag (`service/protocol.rs:13-17`):

```json
{ "type": "join", "epoch": 4, "invitation": "...", "build": "...", "observer": true }
```

- The flag defaults to false and is left out when false.
- The joining helper copies it into `RoomProof` (`rust/sf4-net/src/invite.rs:497-502`)
  as `#[serde(default)] observer: bool`.
- The flag is a courtesy, not a security property (§8).

### 5.4 Admission, retirement and voters in a bound room

**The bound roster.**

- `committed_roster` (`service/server_owned.rs:45-50`, `service/mod.rs:550-588`)
  also returns `BoundRoster { fighters, watchers }`, read from
  `checkpoint.room.snapshot.tournament`. It is `None` when the object is
  absent or empty.
- It rides the existing once-per-commit cache in `refresh.rs:210-219`.
- Every room change commits a full checkpoint (§2.4), so the roster follows
  each bind with no extra IPC. The earlier draft's "verify cadence, else add
  a `room_roster` command" fork is deleted (review finding 5).
- One shared fixture is the contract between the C++ writer and the Rust
  reader:
  - `src/tests/fixtures/tournament_watch_checkpoint.json`;
  - part C's `to_json` test produces it;
  - part B's parser test reads it.

**The admission gate**, in `completed_incoming` before the control task is
spawned (`service/games.rs:19-25`):

- **Bound room:** accept a control only from an endpoint in
  `fighters ∪ watchers`. Close any other with `b"not admitted"`.
- **Unbound room:** refuse a proof with `observer: true`, in
  `accept_control_policy`.
- **Staleness.** The gate reads the once-per-commit roster cache
  (`refresh.rs:210-219`), which can lag the leader's commit by up to a
  second. A watcher or returning fighter that dials just after a bind can be
  refused once, and retries on its normal 10 s or 30 s interval. That is
  acceptable.

**Retirement** (review finding 6, and round 2 finding R2-2). The rule is
"fence first, close last". It adds no path of its own: unnamed admissions go
through the existing pending-retirement machinery, in its order.

Nothing that the old coordination quorum may still need is closed before the
membership change commits. A voter's control link is what lets it acknowledge
a joint configuration. `reachable` is read from `controls` when the operation
starts (`members.rs:199`), and a voter with no control cannot acknowledge
(`members.rs:222-224`). Closing a control early can leave the leader alone in
a three-voter set, which freezes the room.

The sequence, on each committed-roster read in a bound room:

1. **Fence.** In `apply_coordination_refresh`, after the cached roster is read
   (`refresh.rs:210-219`), add to `pending_retired_incarnations` the
   incarnation of every admission whose `primary_endpoint` is outside
   `fighters ∪ watchers`. This is the set the previous-roster rule fills
   (`members.rs:139-153`), and it spawns the operation for the same reason
   (`members.rs:155`). The fence closes nothing and does not call
   `rpc.retire`, so the member still answers coordination RPCs.
2. **Remove through the old quorum.** `spawn_membership_operation`
   (`members.rs:175`) takes `voter_removals = pending ∩ applied voters`
   (`:213`). It builds the replacement from the leader plus the remaining
   voters that still have a control (`:225-264`), commits it as a joint
   change, then removes fenced learners (`:289`). The joint change needs a
   majority of the old set, and the fenced voter's control is still open, so
   it still counts.
3. **Confirm.** `confirmed_retirements` is the fenced set that is no longer
   an applied member. Only for those does the operation call `rpc.retire`
   (`:303-309`), and `completed_membership_reconciliation` then tombstones
   them (`apply_confirmed_retirements`, `:504`, `members.rs:50-70`).
4. **Close the control.** This is the one new step. In
   `completed_membership_reconciliation`, right after
   `apply_confirmed_retirements`, close the control whose session is
   `Session::Bound(incarnation)` for each confirmed incarnation, when its
   endpoint is outside the bound roster current at that moment. Do it as
   `Command::CloseControl` does (`service/mod.rs:1079-1089`):
   `remove_control(peer)`, then `Event::ControlClosed`. A control for a
   named endpoint is left to the existing rejoin handling.
5. **Failure keeps both.** An operation that fails, times out or completes
   stale leaves the fence and the control in place, and the next refresh
   retries (`members.rs:506-509`). Until a retirement is confirmed, the
   control stays open, as it does today.

Case 1: a fighter crashed and rejoined on a new endpoint before the first
bind, and the bind still names the old one.

- The room holds L, E2 (dead) and E2' (live), with three voters
  (`recovery.rs:888-894`). The bind names L and E2, so E2' is unnamed.
- Step 1 fences E2'. Step 2 finds `remaining = {L, E2}`, and only L answers,
  so the replacement is {L}, a joint change from {L, E2, E2'} to {L}.
- It commits because L and E2' are a majority of the old three, and E2'
  still has its control. Then E2' is removed, confirmed and tombstoned, and
  only then is its control closed.
- If step 4 came first, L alone could not commit, and the next bind naming
  E2' could never commit either.
- E2' is a live fighter, but its incarnation is per join (`fresh_incarnation`,
  `recovery.rs:112, 187`), so the tombstone costs it one rejoin. The fighter's
  next claim brings a bind that names E2', and the game's join retry (§5.4,
  returning fighter) dials in with a fresh incarnation through the gate.

Case 2: a stranger S holds a vote while the other fighter F2 is
mid-reconnect.

- The room holds L, F2 and S as voters. F2's control is down, S is unnamed.
- Step 1 fences S. Step 2 replaces the set with {L}, which commits through L
  and S, with S's control open. S is closed after the confirmation.
- If S had already dropped while F2 was down, L would hold one vote of three.
  That is today's two-of-three loss, and this rule adds no case to it.

A stranger admitted before the first bind who never completed a native join
is covered the same way. It loses its control slot, its learner seat and any
vote after the first refresh that sees the bind. A revoked watcher is retired
the same way at the bind that drops it.

**Voter eligibility.** `Actor::voter_eligible(endpoint)` is true when `bound`
is `None`; otherwise it is `bound.fighters.contains(endpoint)`. It is
applied at (a) to (d) of §2.4:

- (a) `start_next_admission_operation` (`members.rs:402-417`): filter
  `candidates`, and give `desired_voters` the eligible count.
- (b) The replacement set (`members.rs:225-252`): filter `answering`, and
  derive `desired` from eligible retained members.
- (c) `restore_stable_voters` (`members.rs:594-655`): derive `desired` from
  eligible retained members, and filter `answers` and `candidates`. Pass the
  predicate in as a closure, because the function is free-standing.
- (d) The learner handoff (`departure.rs:170-182`): only eligible successors.
  With none, there is no handoff, and the room ends for its watchers.

**A returning fighter on a new endpoint.**

- The endpoint is per helper run (`rust/sf4-net/src/tournament/mod.rs:291-292`,
  `service/entry.rs:26-28`). A fighter back after a crash is refused at the
  handshake until its claim, the leader's next claim and the bind land.
- That is the same window in which today's native Join refuses it. It
  retries on the same `OpenTimeoutMs` (30 s, `TournamentPlay.cxx:180-187`),
  so the design does not lengthen it.

### 5.5 Files

- `rust/sf4-net/src/tournament/{play.rs, mod.rs}`.
- `rust/sf4-net/src/service/{protocol.rs, games.rs, members.rs, departure.rs, server_owned.rs, mod.rs, refresh.rs, stall.rs}`.
  `stall.rs:107-124` matches every command exhaustively.
- `rust/sf4-net/src/{transport.rs, invite.rs}`.
- A new test module, `rust/sf4-net/src/service/tests/watch.rs`, registered
  in `service/tests.rs` with one `mod` line. That file is 7,599 lines, so
  the tests do not go in it.

## 6. Room model changes

All in `src/session/`. New bound-room rules go in `RoomModelTournament.cxx`
(277 lines) behind small predicates, so `RoomModel.cxx` (932 lines) gains
only call sites and stays under 1,000.

### 6.1 Binding type

`TournamentBinding.hxx` gains:

- `struct TournamentWatcher { std::string endpoint, emberId; }`;
- `std::vector<TournamentWatcher> watchers`, with at most `MaxWatchers = 8`;
- `bool WatcherOf(const std::string& endpoint) const`.

In `RoomModelTournament.cxx`:

- `Valid()` (`:24-32`) checks each watcher's hex and identifier, that
  watchers are distinct among themselves and from the fighters, and the
  count.
- `to_json` (`:51-62`) writes `"watchers"` only when non-empty, so a room
  without watchers keeps its snapshot bytes. `from_json` (`:64-90`) reads it
  when present.

In `TournamentAnswers.cxx`:

- `DecodeBinding` (`:152-182`) reads `watchers` for fighters.
- `IsTournamentPlayOp` (`:185-189`) adds `match_watch` and `match_unwatch`.
  Without that, their answers are dropped.

### 6.2 BindTournament (`RoomModelTournament.cxx:215-257`)

- The equality shortcut (`:225-226`) also compares `watchers`.
- A stranger is a member that is neither a fighter nor a named watcher
  (`:246-248`). So a revoked or expired watcher is evicted, and sent away as
  kicked (`sf4e__SessionServer__Tournament.cxx:46-61`).
- Capacity is not changed (review finding 3; §6.3).
- Table 0's revision moves only when the bind starts the set or changes the
  fighters, `gamesToWin`, `startingWins` or `assignmentGeneration`. An
  organizer's reassignment that keeps the same fighters must still stale a
  Ready, so a new generation counts as a change of the set. A watcher-only
  bind touches the room only (§6.5).
- **One normalization, for a modified client.**
  - A named watcher who is already a member is moved into table 0's
    spectators if it holds a seat, a queue place or no spectator place.
  - Only a client that omitted `observer` and joined while the room was
    unbound can be in that state (§8).
  - A cooperative watcher is already a spectator, so for it this is a no-op
    and touches nothing.
  - This is why bind-time placement is not deleted outright (§13, finding
    4).

### 6.3 Join (`RoomModel.cxx:405-450`)

`RoomModel.cxx` calls one predicate in `RoomModelTournament.cxx`,
`BoundJoin(connection, profile)`. It returns:

- **Refuse:** a profile with `observer` set when the room is unbound, or
  when the room does not name this endpoint as a watcher. The reason is
  `Unauthorized`.
- **Named:** a bound fighter or a named watcher. Such a member bypasses
  both the capacity check (`:411`) and the lock (`:414-415`). A hard check
  of `members.size() < MaximumMembers` remains, and
  `2 + MAX_WATCHERS = 10 <= 16`.
- **Stranger:** anyone else in a bound room, refused as today (`:428-430`).
- **Unbound:** today's rules.

Effects:

- Capacity is never mutated, so a host's SetCapacity (`RoomModelActions.cxx:16-20`,
  not refused in a bound room) cannot lock out a named member.
- A returning fighter always fits.

**Placement at Join.** After a named watcher joins, it goes into table 0's
`spectators`, or into `watchingNext` while a game is Playing. That is the
same placement `ApplyWatch` makes (`RoomModelActions.cxx:205-216`), with its
`MaxSpectators` and outstanding-receipt checks, except that it calls
`TouchRoom()` and not `Touch(table)` (§6.5). It is the only time a watcher is
placed.

**MemberProfile** gains `bool observer`. The watching game sets it, and it is
carried in the join request profile (`sf4e__SessionProtocol.*`). It is never
part of a snapshot.

### 6.4 Host powers and inheritance

- **`IsHost`** (`RoomModel.cxx:177`) is false for a watcher in a bound
  room. That single choke point removes Close, SetCapacity, Lock, Rename,
  CancelResult and TransferHost from watchers.
- **`Leave`** picks the oldest remaining fighter as the successor
  (`RoomModel.cxx:676-690`). It picks a watcher only when no fighter
  remains, and `IsHost` still denies that watcher the powers.
  - The host field must stay non-zero, because `RoomModel.cxx:416` refuses
    non-host joins when it is zero. That would lock out a returning fighter.
- **A bound fighter who joins while a watcher holds the host field takes
  host.**
- **`TransferHost`** refuses a watcher as the target.

### 6.5 Watchers have no table actions

| Action from a watcher | Result |
|---|---|
| Watch, Unwatch, Queue, Unqueue, SetRules, seat | refused (`seatingOrRules`, `RoomModelActions.cxx:477-479`, unchanged) |
| LockSpectating | refused (new, `:321-324`) |
| Chat | refused (new; §12 Q3) |
| Leave | allowed: the only way to stop watching |

Consequences:

- **No start hold.** `LockedSpectatorReturning` (`RoomModel.cxx:903-907`)
  never sees a locked watcher, so a watcher never holds a start.
- **Watcher-only changes do not move table 0's revision** (round 2 finding
  R2-3). A fighter's Ready is checked against that revision
  (`RoomModelActions.cxx:424-427`), so a watcher joining, leaving or being
  revoked must not touch it. The rule: at a bound table (`BoundTable`), a
  change confined to `spectators`, `watchingNext` or `endingWatchers` calls
  `TouchRoom()` and not `Touch(table)`. It applies in three places:
  - **Placement at Join** (§6.3): `TouchRoom()` only.
  - **Removal at Leave**, in `RemoveFromTable` (`RoomModel.cxx:585-628`). Split
    `ancillaryChanged` into the queue part, which still touches the table, and
    the spectator part, which at a bound table touches the room only. A
    watcher's eviction by a bind goes through `Leave`, so it follows the same
    rule.
  - **`BindTournament`** (`RoomModelTournament.cxx:243`). `Touch(table)` runs
    only when the bind starts the set, or changes `fighters`, `gamesToWin` or
    `startingWins`. Otherwise the room touch at `:254` is all. A bind that
    only adds, drops or revokes watchers moves no table revision.
  - Changes to seats, rules and scores still touch the table:
    `SeatBoundFighters` (`:112`), a fighter leaving, and the §6.2
    normalization of a modified client.
  - The room revision still moves on every watcher change, so snapshots
    still publish. Ready is not a room action (`RoomModelActions.cxx:417-420`),
    so the room revision does not affect it.
- **Why this is safe: every reader of table 0's revision or spectators was
  checked.**
  - The one increment is `RoomModel.cxx:391-393`. The revision is serialized
    only by `RoomModelJson.cxx:373, 385`.
  - Its readers compare it to a stamp copied from the same snapshot:
    - the `StaleTable` gate (`RoomModelActions.cxx:427`);
    - the stampers: `sf4e__SessionClient.cxx:518, 770`, `sf4e__SessionClient__Room.cxx:266`,
      `RoomPanel.cxx:131`, `sf4e__NetplayRuntime__Match.cxx:54, 364, 395, 486, 515`,
      `sf4e__NetplayRuntime__Tournament.cxx:78`, `MatchEndRules.hxx:158`,
      `MatchResultOutbox.cxx:59` and `CreatedRules.hxx:79-95`;
    - the connection check, which compares the seated pair and the revision
      (`RoomModel.hxx:386`, `sf4e__NetplayRuntime__Status.cxx:146`,
      `sf4e__NetplayRuntime__Room.cxx:26`, `rust/sf4-net/src/coordination.rs:705`).
      It reads no spectator list, and a watcher change no longer
      invalidates it.
  - Nothing keys spectator changes on it. `MatchAuthority.cxx`,
    `MatchAuthority.hxx` and `IrohMatchSession.cxx` contain no revision
    reference.
  - The spectator list that a game starts with is read from the list itself,
    at commit time:
    - `BeginMatch` freezes `table->spectators` (and the queue) into
      `activeMatchRecipients_` (`RoomModel.cxx:758-768`);
    - `MatchRoster` and `LiveMatchRoster` (`RoomModel.cxx:336-347`) feed the
      grant's participants and lobby data (`sf4e__SessionServer__Rooms.cxx:178, 339-341`);
    - clients match the grant roster to the projection for that generation
      (`IrohMatchSession.cxx:162-176`, `sf4e__SessionClient__Room.cxx:165-181`),
      and `ProjectSelectedRoomTable` re-reads the snapshot's list each time
      (`sf4e__SessionClient__Room.cxx:212`).
    - The other readers read the list directly: `MatchEndRules.hxx:213`,
      `sf4e__SessionServer.cxx:227`, `sf4e__SessionServer__Handlers.cxx:113`,
      `sf4e__SessionServer__Recovery.cxx:417` and `RoomModel.cxx:904`.
  - So a watcher placed before `BeginMatch` commits is in the stream list,
    and one removed before it is not, whatever the revision does.
- **Why not exempt Ready from `StaleTable` instead.** That also lets a Ready
  pass a real change that the check exists to catch: a rebind that changes
  the fighters or the set length (`RoomModelTournament.cxx:233-234`).
- **Nothing to undo.** There is no Unwatch, so there is no Unwatch choice
  for a bind to undo. `MatchEndRules.hxx:204` is unchanged.

### 6.6 Spectator load on both fighters (review finding 7)

What each watcher costs:

- **P1 (GGPO host):**
  - one QUIC game link and one 60 Hz input stream;
  - GGPO waits for every spectator's sync before RUNNING, for up to 5 s
    (`SpectatorPolicy.hxx:7-15, 96-101`).
- **The coordination leader** (either fighter):
  - it replicates every committed checkpoint append to each watcher, as a
    learner;
  - it sends each joining watcher a snapshot;
  - it commits each watcher's join and leave through its single proposal
    slot, competing with `PermitReady` and Bind in the Ready window.
  - `BindTournament` and `PermitReady` already retry when the slot is busy
    (`TournamentPlay.cxx:229-240, 253-265`).

Decisions:

- **Shorter sync deadline.** At a bound table, a watcher's GGPO sync
  deadline is 2 s instead of 5 s (`SpectatorPolicy` gets the deadline from
  its caller).
  - A slow or hostile watcher can then add at most 2 s to a game start.
  - A watcher dropped from one game keeps its place for the next, since
    `ApplyLeaveGame` keeps it watching.
  - Not waiting at all is not possible: GGPO itself withholds RUNNING until
    spectators sync, and dropping them is the only release.
- **Measured.** This was the precondition for the §12 Q1 answer. The numbers
  come from `SessionServerWatch` (`src/tests/session_server_watch_test.cxx`),
  which uses only synthetic traffic. For 0, 2, 4, 6 and 8 watchers it opens a
  bound room, then plays one game from both fighters' Ready to every
  recipient's acknowledgment of its end, committing each change. Every
  recipient acknowledges the game's set-up, which is the conservative path.
  "Wire" is the game's JSON of the proposal compressed with zlib at the
  fastest level, as the helper compresses an append (`coordination.rs`,
  `Compression::fast()`); the helper frames it a little differently, so it is
  close to the wire bytes and not exact. One learner receives each append, and
  the leader sends it to every learner: the other fighter and each watcher.

  | Watchers | Room open: commits | Last append, raw | Last append, wire | All open commits, wire | One game: commits | One game: wire, per learner | One game: leader upload (wire x learners) |
  |---|---|---|---|---|---|---|---|
  | 0 | 3 | 31,045 | 3,147 | 7,521 | 12 | 88,193 | 88,193 |
  | 2 | 5 | 76,900 | 5,959 | 17,992 | 18 | 261,399 | 784,197 |
  | 4 | 7 | 163,357 | 10,517 | 36,549 | 24 | 443,617 | 2,218,085 |
  | 6 | 9 | 302,126 | 17,657 | 68,971 | 30 | 573,694 | 4,015,858 |
  | 8 | 11 | 381,018 | 21,190 | 111,777 | 36 | 709,130 | 6,382,170 |

  What the numbers say:
  - **Commits are linear.** Opening takes 3 plus one per watcher. A game takes
    12 plus three per watcher (its set-up acknowledgment, its ready
    acknowledgment and its acknowledgment of the game's end).
  - **The raw append is superlinear, the wire form much less.** Each commit
    carries a room snapshot per recipient, so the raw size grows with the
    square of the room (31 KB at none, 302 KB at 6, 381 KB at 8). The
    snapshots are alike, so zlib shrinks the append 10 to 18 times: 17.7 KB at
    6 and 21.2 KB at 8.
  - **The leader's upload per game is the figure to weigh against an uplink.**
    It is 0.8 MB at 2 watchers, 4.0 MB at 6 and 6.4 MB at 8. The commits fall
    in the window between games, so spread over 10 s that is 0.6 Mbit/s at 2,
    3.2 Mbit/s at 6 and 5.1 Mbit/s at 8, on top of the fighter's GGPO link.
  - Part B has not recorded learner bytes per commit yet; these numbers are
    its input.
  - The test bounds all of this. The commit counts are the measured ones, and
    the byte sizes are the measured ones plus a quarter. A change that makes
    the traffic larger fails the test and has to be looked at.

## 7. Game UX

### 7.1 Opening a match as a watcher

- **The link.** A TO or viewer opens the match's existing link (`/m#<bridge>/<match>`
  or its `ember://` form). Admission stays with the bridge.
- **Where it lands.** Online, then Ember ID, then Tournament matches opens
  as today (`src/ui/ApplicationShell.cxx:739-747`):
  - for a fighter in the match, it shows Play;
  - for anyone else, it adds the row "Watch this match", and pressing Watch
    starts `WatchPlay`.
  - Existing rows keep their order and positions, so the protected journey
    tests pass unchanged.
- **No Ember ID.** The Ember ID screen shows its existing Make Ember ID
  action (`IdentityPanel.cxx:684-692`) with `tournament.watch.needs_id`. The
  opened match stays selected after.
- **Untrusted bridge.** The row shows `tournament.watch.untrusted_bridge`
  and points to Linked accounts.
- **Home.** No new Home row. The Home row stays limited to matches ready to
  play (`ApplicationShell.cxx:211-216`).

### 7.2 While waiting

`netplay::tournament::WatchPlay` (`src/netplay/WatchPlay.{hxx,cxx}`) is a
pure state machine like `TournamentPlay`. The runtime ticks it
(`src/sf4e/sf4e__NetplayRuntime__Tournament.cxx`).

- **Phases:** Idle, Asking, Waiting, Joining, Watching, Finished, Refused.
- **Polling.** It sends `match_watch` every 10 s, or after `retry_after`,
  and keeps polling while in the room. That keeps the watcher live, and
  picks up a replaced room or a revocation.
- **On `room`:**
  - if it is not in that room, it sends Join with the invitation and
    `observer: true`;
  - a refused join, or a handshake closed `not admitted` because the bind
    has not landed yet, is retried after 10 s, the poll interval.
- **A different `room_id`:** it leaves and joins the new room.
- **Kicked by a rebind:** it polls at once.
- **It never emits** Host, Publish, Bind, Prepare or Report.
- **Watch and Play are exclusive.**

| Phase | Line shown (key) |
|---|---|
| Asking | `tournament.watch.state.asking` |
| Waiting, `no_room` or `not_bound` | `tournament.watch.state.waiting_room` |
| Waiting, `full` | `tournament.watch.state.full` |
| Joining | `tournament.watch.state.joining` |
| Watching | `tournament.watch.state.watching` |
| Refused | the matching `tournament.watch.refused.*` |
| Finished | `tournament.watch.finished` |

### 7.3 Room view for a watcher

- Tournament controls stay hidden as for fighters (`src/ui/RoomPanel.cxx:243-251, 360-365, 400-403`).
- **Take seat is hidden.** The seat chooser (`RoomPanel.cxx:200`, `src/ui/RoomControls.cxx:78-91`)
  is hidden for everyone in a bound room, since the model refuses it.
- **The status line** is `room.tournament_watching`.
- **The chat input is hidden** (§12 Q3).
- **Members are shown by name, plus their Ember ID.** The member list shows
  each watcher's name with the tag `room.member.watcher`, followed by the
  short Ember ID the binding gives for that endpoint. A handed-over slot
  therefore still shows the accountable Ember ID (§8).

### 7.4 Leaving

- **Leave or Back** stops `WatchPlay`. It sends `match_unwatch`, then Leave.
- **A finished match:** the watcher leaves after the last battle closes, on
  the same `FinalBattleHoldMs` rule as fighters (`TournamentPlay.cxx:100-106`),
  and shows `tournament.watch.finished_notice`.
- **A revocation** shows `tournament.watch.removed_notice`.
- **A closed room** shows `tournament.watch.room_closed_notice`.
- **A Discord invite** accepted while watching
  (`PublishAndTickDiscordInvite`, `Status.cxx:590-605`) stops `WatchPlay`
  first, as Leave does. Fighters' behaviour there is unchanged.

### 7.5 What fighters see

- **HUD.** The match HUD already shows "Watching N" (`src/ui/MatchHud.cxx:232`).
- **Status line.** `room.tournament_status_watched` is shown when watchers
  are present.
- **Member list.** Watchers are tagged as in §7.3.
- **Changed text.** `room.tournament_status` now ends "Only its two players
  can play." The key is the same.

### 7.6 Discord presence

The join secret (`src/sf4e/sf4e__NetplayRuntime__Status.cxx:209-213`) is set
only when both of these hold:

- the room is not server-owned;
- the runtime has no `TournamentPlay` or `WatchPlay` in phase Claiming,
  Opening, InRoom or Watching.

Two points about this rule:

- It is keyed to the runtime's own state, not the snapshot's binding, so
  it covers the hosted but unbound window before the opponent claims
  (review finding 6).
- `PublishAndTickDiscordInvite` only accepts invites, so its one change is
  §7.4.

### 7.7 Strings

New keys go in `locales/en.po`. The other 13 catalogs (`locales/locales.json`)
get them later, in the same position.

| Key | English |
|---|---|
| `tournament.watch.row` | Watch this match |
| `tournament.watch.action` | Watch |
| `tournament.watch.needs_id` | Watching a match needs an Ember ID. Make one here; it takes a moment. |
| `tournament.watch.untrusted_bridge` | This match's tournament service is not trusted yet. Add it on Linked accounts. |
| `tournament.watch.state.asking` | Asking the tournament service. |
| `tournament.watch.state.waiting_room` | Waiting for the players to open the match room. |
| `tournament.watch.state.full` | This match has all the watchers its organizer allows. Waiting for a place. |
| `tournament.watch.state.joining` | Joining the match room. |
| `tournament.watch.state.watching` | Watching {0} vs {1}. |
| `tournament.watch.refused.not_allowed` | This match's organizer has not opened it to watchers. |
| `tournament.watch.refused.revoked` | The organizer removed you from this match's watchers. |
| `tournament.watch.refused.fighter` | You play in this match. Use Play instead. |
| `tournament.watch.refused.build` | The players run a different Ember version. Watching needs the same one. |
| `tournament.watch.refused.unsupported` | This tournament service does not offer watching. |
| `tournament.watch.finished` | The match is over. |
| `tournament.watch.finished_notice` | The match you watched is over. |
| `tournament.watch.removed_notice` | You stopped watching: the organizer removed you. |
| `tournament.watch.room_closed_notice` | The match room closed. |
| `room.tournament_watching` | Watching a tournament match. Only its two players can play. |
| `room.tournament_status_watched` | Tournament match, first to {0}. {1} watching. |
| `room.member.watcher` | Watcher |

Changed text, same key: `room.tournament_status` ("...Only its two players
can play.").

## 8. Security

**Who can get the invitation.**

- The two fighters, through claims.
- A watcher, from the watch route only after all of these:
  - a session and a proof for its own Ember ID;
  - the policy check;
  - the build check;
  - a place in the bound set.
- No link carries an invitation. Watchers get no binding, so they learn no
  fighter endpoints from the bridge.

**Revocation.**

- A denial, a staff removal, or turning `link_holders` off drops the watcher
  at the next `rebind`.
- At most one leader claim interval (10 s) later, plus the bind commit, the
  room evicts it. Its helper admission is retired at the next committed
  roster read (§5.4).
- The handshake gate refuses it afterwards, even though it still holds the
  invitation until the room renews it.
- Its own poll answers `revoked` or `not_allowed`, and `WatchPlay` stops.

**Leaked links and invitations.**

- **A leaked match link** gives nothing beyond what policy allows.
- **A leaked invitation in a bound room:**
  - a new control from an unnamed endpoint is refused;
  - one admitted before the bind is retired, losing its slot, its learner
    seat and its vote, at the first refresh after the bind;
  - the native Join refuses as today.
- **Before the first bind** the room is an ordinary private room. Only the
  fighters and the bridge hold the invitation, and Discord no longer
  advertises it (§7.6).
  - The bridge can give a watcher the invitation before the leader has
    applied the binding. A cooperative watcher's `observer` flag is then
    refused at the handshake.
  - **The `observer` flag is a courtesy, not a security property.** A
    modified watcher that leaves it out can join the unbound room as an
    ordinary member, and for that window even become a voter. The bind that
    names it moves it to spectators (§6.2), and eligibility removes its vote
    at the next refresh.
  - The security property is the post-bind gate, retirement and voter
    shrink.

**Address exposure** (review finding 8).

- **What is exposed.** Every room member's helper connects to other members
  over Iroh. When Iroh finds a direct path, members learn each other's
  public IP addresses. That is true of any Ember room today.
- **Who a watcher can reach:**
  - P1, through the GGPO game link (`sf4e__UserApp.cxx:207-215`);
  - the coordination leader, through learner replication.
- **What link holders add.** With `link_holders` on, anyone with the match
  link and a free Ember ID can learn a competing fighter's address in the
  middle of a tournament. That invites a flood from outside Ember.
- **Per-peer relay is not available.** `relay_only` is a helper-wide startup
  setting (`rust/sf4-net/src/main.rs:20-38`), so watcher links cannot be
  forced through the relay without forcing the fighters' own links too.
- **Decision.** Accept the exposure for staff (approved people), and state
  it plainly in `PUT /v1/connections/{id}/watch`, in INTEGRATIONS.md and in
  the organizer guide, as the cost of turning `link_holders` on. This is
  settled in §12 (2).
- **Ember-level floods are already bounded.** Strangers are refused at the
  handshake, and only named watchers get game links.

**The endpoint is caller-chosen** (review finding 9).

- The watch command's `endpoint_id` is not proven, and fighters' claims have
  the same property today.
- **Handing over a slot.** An allowed watcher can register another person's
  endpoint and hand that person the stream. The bridge still knows which
  Ember ID is accountable for the slot, and the room shows that Ember ID
  beside the member's name (§7.3). The organizer can deny it.
- **Squatting.** A watcher squatting another watcher's endpoint needs to be
  allowed itself, and to see endpoints, which only room members do. The
  conflict rule (§4.2 step 3) favours staff, and audits the conflict for the
  organizer.
- **The proper fix** is proof of endpoint possession, signed by the helper's
  room endpoint key. It crosses the tournament worker and the room actor, so
  it is phase 2 for watchers and fighters alike.

**Voting and authority.** Watchers never vote, lead, hold host powers, seat,
queue, set rules, hold a start or chat (§5.4, §6). A watcher cannot apply a
binding: it never receives one, and `WatchPlay` never emits Bind.

**Abuse limits.**

- Watch routes: 30 calls per minute per Ember ID.
- The 2 s sync deadline.
- `MAX_WATCHERS`.

**Spec §14.6.**

- Access is explicit and separately authorized: staff lists and denials,
  not organizer rights.
- The room code is never advertised for a match room.
- Play never depends on watchers.
- ROOM-06 holds.

## 9. Compatibility

- **Old fighter clients (Stable 1.3.0, older Nightlies).**
  - Their claims do not say `watch`, so the bridge never names a watcher for
    their matches, whatever build a watcher declares (§4.2 step 2). A
    watcher's build ID is self-declared and the Stable one is public, so the
    build alone would not be safe.
  - Their `bound_watchers` stays NULL and `named` returns at step 1.
  - Their bindings, revisions and prepares are exactly today's: the
    migration backfills `fighters_revision = binding_revision`.
- **Old bridge.** The new helper sends `match_watch`, and `watch` in a
  claim, only to a bridge listing `matches.watch` (§5.3, §5.1). An old
  bridge's claims are unchanged.
- **BluMint unchanged.**
  - Its connections can't be opened in phase 1 (§3.4), and it gets no new
    event kinds and no response changes.
  - The adapter routes (`bridge/src/routes/blumint/`) are untouched, and
    `spectators` stays ignored.
  - Protected tests are not edited: `tests/discord.rs`, `tests/blumint.rs`,
    `tests/blumint_reports.rs`, `src/tests/discord_connect_journey_test.cxx`
    and `src/tests/builtin_service_journey_test.cxx`.
  - New shared bridge test helpers are added beside the old ones in
    `bridge/tests/common`, never changed in place.
- **Oct 23 event on Stable 1.3.0.**
  - Stable clients can't watch, and their matches are untouched (above).
  - The migration is additive, NULL-safe and backfilled, so deploying before
    the event is safe.
  - The recommendation is still to deploy after the event, since nothing
    can use the feature before Nightly clients ship it.
- **Room snapshots.** Rooms are same-build only, and `watchers` is left out
  when empty.

## 10. Test plan

### Protocol

- A binding without watchers is byte-identical to today's, and round-trips
  through the old struct shape (pattern of `play.rs:628-677`).
- A binding with watchers signs and verifies, and the old struct shape
  refuses it.
- `check` rejects:
  - more than 8 watchers;
  - duplicate watchers;
  - a watcher equal to a fighter;
  - bad hex.
- `WatchAnswer` and the watch command parse strictly, and join the fuzz
  tests (`play.rs:737-913`).

### Bridge (new `bridge/tests/watch.rs`)

- **Deploy over live matches** (finding 2). Over a live BluMint match and a
  generic match with old-shape claims (NULL `bound_watchers`, backfilled
  `fighters_revision`):
  - claims and prepares give the same revision and the same binding bytes
    as before the migration;
  - there is no bump.
- **Defaults.** A non-fighter gets `not_allowed`, and fighters' bindings
  have no `watchers`.
- **Staff watcher, same build.**
  - The watcher gets `room`.
  - The fighters' next claims name it, with `binding_revision` up by
    exactly one.
  - `fighters_revision` is unchanged.
- **Prepare across watcher churn** (finding 1). With a watcher arriving and
  leaving between each of both fighters' prepares, 20 times over:
  - every prepare is accepted;
  - the two descriptors agree;
  - the permit issues on the first matching pair.
- **Prepare after a fighter-endpoint change.** An old revision below
  `fighters_revision` is still `stale_binding`.
- **Agreement uses the current `fighters_revision`** (round 2 finding R2-1).
  Fighter A prepares at revision R. Fighter B then reconnects on a new
  endpoint, which makes `fighters_revision` R+1, and prepares at R+1. The
  answer is `pending`, not a permit, because A's stored descriptor is below
  the current `fighters_revision`. The upper bound `<= binding_revision`
  stays covered by the churn test above.
- **Builds.** A watcher on another build gets `incompatible_build`. A
  fighter rebuilding drops every watcher row on the old build from the bound
  set.
- **Claims that attest.**
  - A watcher declaring the fighters' (public) build, with fighters whose
    claims lack `watch`, is never named, and the fighters' binding and
    revisions are unchanged.
  - With both claims saying `watch: true`, the watcher is named.
  - One fighter claiming again without `watch` drops the watchers.
- **Endpoints.** A watcher asking from a fighter's endpoint is not named. A
  fighter reconnecting on a bound watcher's endpoint takes it, and the
  binding still signs. A watcher who becomes a fighter after a reassignment
  is not named.
- **A returning watcher** does not push out one who took its place.
- **Audit.** Each organizer write records the Ember ID, or `on` / `off`.
- **Link holders.** With `link_holders` on, the third link holder waits with
  `full`. Staff fill to 6, pushing a link holder out at the ceiling.
- **Revocation.** A denial gives a fighter's next binding without the
  watcher, and the watcher's poll answers `revoked`.
- **Liveness.** A watcher silent for 61 s is dropped at the next claim.
- **Endpoint conflict.** Staff beats a link holder, the newest wins between
  equals, and an audit row is written.
- **Room replacement** keeps the watchers.
- **Auth and BluMint.** Writes by a provider credential, an organizer of
  another tenant, or anyone on a BluMint connection are refused. Reads work
  for the connection's provider.
- **Capabilities** list `matches.watch`.

### Helper (new `service/tests/watch.rs`)

- The bound roster parses from the shared fixture, and is `None` without a
  tournament.
- **Gate, bound room:**
  - an unnamed endpoint is closed `not admitted`;
  - a named watcher is admitted as a learner and never promoted, with 1, 4
    and 8 watchers (eligible count 2, so one voter).
- **Gate, unbound room:** an observer proof is refused.
- **Retirement.** A stranger admitted before the bind, never natively
  joined, is fenced at the first refresh after the bind, and its control
  closes only after the retirement is confirmed (`ControlClosed` follows the
  tombstone, never precedes it).
- **Retirement, fighter reconnect** (round 2 finding R2-2). A pre-bind room
  holds L, E2 (dead) and E2' (live, new endpoint), with three voters. The bind
  names L and E2. The room must still commit: the voter set shrinks to L
  through the old quorum with E2''s control open, and only then is that
  control closed. A later bind naming E2' admits a fresh incarnation.
- **Retirement, stranger voter.** L, F2 and a stranger S are voters, F2's
  control is down, and a bind names L and F2. The shrink to L commits with S's
  control open, and S is closed after the confirmation. While the operation is
  unconfirmed (forced failure or timeout), S's control stays open and the
  next refresh retries.
- **Departure.** A fighter leader leaving with only watchers does not hand
  off. With the other fighter present, it hands off to that fighter.
- **Transition window.** A pre-bind 3-voter room with a stranger shrinks to
  the leader within one refresh of the bind. A `remove` racing it returns to
  one voter at the next refresh.
- **`match_watch`** to a bridge without the feature fails with
  `watch_unsupported` and sends no request.
- **Regression.** Unbound rooms take every old path, and the existing
  coordination tests are unchanged.

### Room model and game (`src/tests`)

- **Bind.**
  - A bind naming watchers keeps them.
  - A rebind without one evicts it.
  - An equal binding with different watchers is not a no-op.
  - Capacity is unchanged.
- **Bind normalization.** A named watcher holding a queue place (the
  modified-client case) is moved to spectators, and the table is touched. A
  cooperative watcher causes no table touch, whether it joins, leaves or is
  evicted.
- **Join.**
  - An observer join to an unbound room is refused.
  - A named watcher joins through a lock and over a host-set capacity of
    2, and is placed as a spectator, or in `watchingNext` mid-game.
  - A fighter returns into a room of 8 watchers.
- **Table actions.**
  - A watcher's Watch, Unwatch, Queue, SetRules, LockSpectating and Chat are
    refused.
  - No `spectatorHold` is caused by a watcher.
- **Ready under churn** (deterministic). Between a fighter's snapshot and its
  Ready, a watcher joins, another watcher leaves, a watcher-only bind lands
  and a revoked watcher is evicted.
  - `tables[0].revision` is unchanged and `snapshot.revision` moves.
  - The Ready is accepted the first time.
  - A rebind that changes the fighters or `gamesToWin` does move the table
    revision, and a Ready stamped before it is `StaleTable`.
- **Spectator list at start.** A watcher placed before `BeginMatch` commits
  is in `LiveMatchRoster`, and one removed before it is not, with the table
  revision unchanged.
- **Host.**
  - `IsHost` is false for a watcher.
  - Leave passes host to a fighter.
  - TransferHost to a watcher is refused.
  - A fighter rejoining takes host back from a watcher.
- **Spectator policy.** The bound-table sync deadline of 2 s drops a
  non-syncing watcher, and it stays placed for the next game.
- **Fixture.** `to_json` of a bound snapshot with watchers equals the shared
  fixture.
- **`WatchPlay`** (`watch_play_test.cxx`):
  - poll cadence;
  - Join on `room`;
  - retry after `not admitted`;
  - follows a replaced room;
  - a kick leads to an immediate poll, then Refused on `revoked`;
  - Stop sends unwatch, then Leave;
  - it never emits Bind, Prepare or Report.
- **`TournamentAnswers`.** `watchers` decodes, and the new ops route.
- **Discord.**
  - No join secret while a `TournamentPlay` is hosting before the bind, or
    while watching.
  - An invite accepted while watching stops `WatchPlay`.
- **Measurement (§6.6).** Checkpoint append size and commits per game with
  0, 2, 4, 6 and 8 watchers, recorded in the test output.
- **Protected tests.** The protected journey tests run unchanged.

### Manual, after build validation and only with the maintainer's go

These are separate from build and synthetic validation:

- two fighters and one staff watcher on the staging bridge;
- revoking the watcher mid-set;
- a fighter Alt+F4 and rejoin with a watcher present.

## 11. Work split

There are four parts with no file overlap. The fixture is written only by C
and read by B.

**Order:**

1. A1 lands first.
2. A2, B and C then run in parallel. C commits the fixture early.
3. D comes last.

**A. Bridge, protocol, SDK and docs**

- **A1, protocol:**
  - `server/ember/protocol/src/play.rs`: `BoundWatcher`, `Binding.watchers`,
    `WATCH_FEATURE`, `MAX_WATCHERS`, `WatchRequest`, `WatchAnswer`;
  - `challenge.rs`: `MatchWatch`, `MatchUnwatch`.
- **A2, bridge:**
  - `bridge/migrations/028_match_watch.sql` and `bridge/src/db.rs`;
  - `bridge/src/routes/{play/mod.rs, discovery.rs, sessions.rs, mod.rs}`, where
    `mod.rs` registers the routes;
  - `bridge/src/events.rs`: `Viewer::sees`, shared with `matches::visible`;
  - new `bridge/src/routes/play/{room.rs, watch/}`, with `play.rs` now `play/mod.rs`;
  - a new `bridge/tests/watch.rs`, plus new helpers in `bridge/tests/common/`.
- **SDK:** `sdk/typescript/src/client.ts`:
  - methods `watchPolicy`, `setWatchLinkHolders`, `addWatchStaff`,
    `removeWatchStaff`, `denyWatcher`, `clearWatchDenial` and
    `matchWatchers`;
  - tests in `sdk/typescript/test/watch.test.ts`.
- **Docs:**
  - a "Match watch" section in `docs/guides/INTEGRATIONS.md`, including the
    address exposure;
  - the `/m` page text in `server/ember-short/deploy/site/assets/open.js`.
    This is held for the release that ships Watch (part D): the page is
    served to everyone, and Stable 1.3.0 has no Watch button.

**B. Helper**

- `rust/sf4-net/src/tournament/{play.rs, mod.rs}`.
- `rust/sf4-net/src/service/{protocol.rs, games.rs, members.rs, departure.rs, server_owned.rs, mod.rs, refresh.rs, stall.rs, tests.rs}`.
  `tests.rs` gets one `mod` line only.
- A new `rust/sf4-net/src/service/tests/watch.rs`.
- `rust/sf4-net/src/{transport.rs, invite.rs}`.
- The §5.4 retirement order lives in B alone: the fence in `refresh.rs`, the
  control close in `members.rs` after the confirmation. B holds no rejoin
  timer and no per-endpoint close times.
- Depends on A1, and reads C's fixture.

**C. Game: room model, UI, locales and CMake**

- `src/session/{TournamentBinding.hxx, RoomModel.hxx, RoomModel.cxx, RoomModelActions.cxx, RoomModelTournament.cxx, TournamentAnswers.cxx, IrohRoom.cxx, sf4e__SessionProtocol.hxx, sf4e__SessionProtocol.cxx}`.
- `src/common/SpectatorPolicy.hxx` and its caller in `src/sf4e/sf4e__UserApp.cxx`,
  for the bound-table deadline.
- `src/netplay/{WatchPlay.hxx, WatchPlay.cxx, TournamentStatus.hxx}`.
- `src/sf4e/{sf4e__NetplayRuntime__Tournament.cxx, sf4e__NetplayRuntime__Status.cxx, sf4e__NetplayRuntime.hxx}`.
- `src/ui/{RoomPanel.cxx, RoomControls.cxx, IdentityPanel.cxx, ApplicationShell.cxx}`.
- `CMakeLists.txt`:
  - a `sf4e_watch_play` library next to `sf4e_tournament_play` (`:149`);
  - `sf4e_add_unit_test` entries for `watch_play_test.cxx` and any new room
    test, next to `:186`.
- `src/tests/fixtures/tournament_watch_checkpoint.json` and new tests in
  `src/tests/`.
- `locales/en.po`, with the other 13 catalogs in a later pass.
- The table-revision rule of §6.5 lands in C's files: `RemoveFromTable` in
  `RoomModel.cxx`, `BindTournament` in `RoomModelTournament.cxx`, and the
  Join placement. There is no helper timer to coordinate with.
- C codes against §5.3 and can stub the helper until B lands.

**D. Notifier (later phase)**

- `server/ember/notifier/src/format.rs`: post "Watch: <play_url>" when
  `match.room.ready` carries `watch_url`.
- The payload field is a small A2 follow-up in `play.rs` (`:205-211`).
- Depends on A2.

**Later, not phase 1:**

- BluMint opt-in (§12 Q5);
- proof of endpoint possession (§8);
- per-match allow entries, a cap knob and staff labels;
- watcher catch-up into a game already under way.

## 12. Decisions on the open questions

Settled on 2026-10-19:

1. **Watcher numbers:** 2 link holders and a ceiling of 6 watchers in all,
   staff included, both bridge constants (`LINK_HOLDER_CAP`,
   `WATCHER_CEILING`). The decision was 4 and 8, lowered to 2 and 6 if the
   §6.6 measurement showed a high replication cost for the leader; it did
   (about 6.4 MB uploaded per game at 8 watchers), so the lower numbers
   apply. The wire limit `MAX_WATCHERS` stays 8.
2. **Address exposure for link holders:** accepted and documented on the
   setting, in INTEGRATIONS.md and in the organizer guide.
3. **Watcher chat:** none in phase 1. Watchers read the fighters' chat.
4. **The watch link:** the existing `/m` match link, with no new link form.
5. **BluMint:** connections stay closed to watchers in phase 1, and
   `spectators` stays ignored.
6. **Endpoint ties (for the design owner):** the later arrival wins between
   equals (§4.2 step 3), which stops two watchers swapping an endpoint on
   every poll. It lets an allowed link holder take another's endpoint and keep
   it. Earliest-wins would close that, at the cost of a stale row blocking its
   endpoint for up to 60 s. Not decided; the audit and denial are the remedy.

## 13. Review response

Each finding of the design review, and where it is resolved:

| # | Finding | Resolution |
|---|---|---|
| 1 | Watcher churn fails prepares | Taken: `fighters_revision`, with a bridge-only check and agreement (§2.1, §4.2). A test covers 20 churn cycles. |
| 2 | NULL `bound_watchers` bumps on deploy | Taken: NULL is empty, empty is stored as NULL, `named` short-circuits, and `fighters_revision` is backfilled. A deploy test is added (§4.1, §4.2, §10). |
| 3 | SetCapacity is not refused | Taken: the claim is corrected (§2.4, §6.3). Capacity is never mutated, and named members bypass the capacity check with a hard `MaximumMembers` check. |
| 4 | Watcher table actions and re-placing | Taken: watchers have no table actions and are placed once, at Join (§6.3, §6.5). Partly kept: a bind-time normalization for named watchers who joined before the bind without `observer`, which finding 6 shows is possible. It is a no-op for cooperative watchers. Join and leave churn cannot disturb a Ready, because watcher-only changes do not move table 0's revision (round 2, R2-3 below; §6.5). |
| 5 | Checkpoint cadence | Taken: confirmed and cited (§2.4). The fallback is deleted, and the shared fixture is owned by C (§5.4, §11). |
| 6 | Partial leaked-invitation closure | Taken: stranger retirement on each committed-roster read in bound rooms (§5.4); a Discord gate on runtime state (§7.6); `observer` stated as a courtesy (§8). |
| 7 | Watcher load under-counted | Taken: load on both fighters is described, the bound-table sync deadline is 2 s, and the numbers are gated on a measurement (§6.6, §12 Q1). |
| 8 | Direct path to P1 | Decided: documented, because per-peer relay is not available (`main.rs:20-38`). Settled in §12 (2) (§8). |
| 9 | Caller-chosen endpoint | Taken in part: the Ember ID is shown with the name, and there is a conflict rule plus an audit (§4.2, §7.3, §8). Proof of possession is phase 2, as it also applies to fighters. |
| 10 | Signed binding to watchers | Taken: deleted (§5.2). |
| 11 | `Claim.watch` redundant | Taken first, then reversed by the bridge review of part A: a watcher's build ID is self-declared and the Stable one is public, so the fighters' claims must vouch. `Claim.watch` and `match_claims.watch` are back, as an additive optional field like `ready` (§2.2, §4.2). |
| 12 | Policy surface | Taken: a flag column, staff, and per-match denials. Per-match allow, `max_watchers`, labels, `watchable` and the 1:1 table are removed. `full` is now a waiting reason. Writes are organizer only, and refused for BluMint (§3.4). Staff preemption is stated (§4.2). |
| 13 | Factual errors | Taken: rows persist (matches are never deleted); the `PublishAndTickDiscordInvite` role is corrected (§2.6, §7.4); the wait-for-one-voter rule is dropped (§2.4). |
| 14 | Work split | Taken: CMake, route registration and `auth.rs` are owned; helper tests go in a new module; bound-room rules sit in `RoomModelTournament.cxx` (§6, §11). |

Round 2 of the review (`review-match-watch-design.md`, section "Round 2"):

| # | Finding | Resolution |
|---|---|---|
| R2-1 | Prepare range sound; test gap (P3) | Taken: a test where fighter A prepares, fighter B reconnects on a new endpoint and prepares, and the answer is `pending` (§10, Bridge). |
| R2-2 | P1. Retirement can race the bind and freeze a room | Taken: unnamed admissions only join `pending_retired_incarnations`, go through `spawn_membership_operation`, and their control closes only after the retirement is confirmed. Both the fighter-reconnect and stranger-voter cases are walked through, and both have tests (§5.4, §10). The one-second stale roster is stated in §5.4. |
| R2-3 | P2. The 10 s cooldown does not bound the Ready risk | Taken, in the review's first form: watcher-only changes do not move table 0's revision. The cooldown, its limits row, its abuse-limit line, its tests and the helper's per-endpoint close times are deleted. Readers were checked first, and the evidence is in §6.5. Exempting Ready from `StaleTable` was rejected, because it would pass a Ready across a real fighter or set-length change. |
| R2-4 | §6.2 "touches nothing" is false while the bind touches the table | Resolved by R2-3: it is now true (§6.2, §6.5). |

Rejected outright: none. One suggestion is kept in a smaller form: the
review said bind-time placement had "nothing to do", but a modified watcher
can be a member before the bind (the review's own finding 6). So a
normalization that is a no-op for cooperative clients stays (§6.2).
