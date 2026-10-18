# BluMint on the adapter model: gap analysis and migration plan

Status: analysis only, no code changed. Written 2026-10-18 against the
`feat/challonge` worktree. File references are to `server/ember/bridge/src`
unless another root is given.

## The question

The bridge is becoming service-neutral: sign-in providers are config data
(`GENERIC_SITES.md`) and bracket sites are adapters in `ember-brackets`
(`server/ember/brackets/ADAPTERS.md`) that drive the generic API
(`docs/guides/INTEGRATIONS.md`, `docs/design/INTEGRATION_PATHS.md`). BluMint is
the exception: its partner contract (API 2.0.0) is built into the bridge.

Can BluMint move onto the adapter model with no difference BluMint can see:
same URLs, credentials, JSON bytes, status codes, error codes, timing
semantics and idempotency behaviour?

Short answer: not over today's generic API. Six of BluMint's behaviours read
or write state inside one bridge transaction (presence, the void lock,
multi-player refusals, close-before-create, the lifetime cap, keyed replay),
and the generic API exposes none of them. It becomes possible once those are
added to the bridge as generic, per-connection capabilities. At that point
the BluMint-specific part is a pure format translation, which can live
either in the bridge (a thin codec) or in a separate stateless service. The
recommended order is to generalize first with the codec still in the bridge,
then move the codec out only if that step is still wanted.

## 1. Inventory of BluMint-specific behaviour

### 1.1 Inbound routes (BluMint calls the bridge)

All three authenticate with `Authorization: Bearer <emk_ provider credential>`
(`auth.rs:152`), then require that the credential's connection is configured
with kind `blumint`, otherwise `404 not_found` in the bridge envelope
(`routes/blumint/mod.rs:102-109`). Paths are fixed constants
(`routes/blumint/mod.rs:60-62`) and routed at `routes/mod.rs:78-80`.

| Route | Contract | Where |
|---|---|---|
| `POST /v1/blumint/lookup` | Body read with the general 64 KiB cap by the `Body` extractor, which runs before authentication, so an oversized body is a `400` envelope even with a bad credential. Parsed as plain JSON, not the strict profile. Only `discord` is read; non-string entries are skipped, the list is silently cut to 32, other keys (BluMint also sends emails) are ignored, `{}` is fine. Calls `find_and_link` with `discord_asks`, which links each found player on the connection (`approved_via` `discord`). Answers `200 {"discord":[<ember ids>]}`, `[]` when none. Malformed JSON is a `400` envelope, not a BluMint registry code. | `routes/blumint/mod.rs:113-145`, `linking/mod.rs` (`discord_asks`, `find_and_link`) |
| `POST /v1/blumint/matches` | Order: credential, then `Idempotency-Key` header (1 to 128 visible ASCII, `http.rs:73`), then body (cap 256 KiB, `mod.rs:79`; nginx allows 512 KiB on this path only). Two teams of exactly one player; each player's `identities[].inGameId` must contain exactly one parseable Ember ID; the two must differ; `series.gamesToWin` 1 to 10; `startingWins` on both teams or neither, each below `gamesToWin`. `matchSettings`, team names and spectators are ignored. The digest covers only the two Ember IDs, `gamesToWin` and `startingWins`. Answers `200 {"matchId","matchUrl"}` (always 200, never 201), `matchUrl` being the Play link. | `routes/blumint/create.rs:134-157` (handler), `:159-181` (`requested`), `:79-122` (field rules) |
| `POST /v1/blumint/matches/close` | Credential, then body `{idempotencyKey (1 to 128), reason (1 to 64)}`; the close's own `Idempotency-Key` header is not read. Marks the creation key refused for every later create, cancels the match made under it if still open, sets its delivery to `not_required` so no abort is sent, audits `blumint.close`. Always `200 {"closed":true}`, also for a key that made nothing or a match already over. | `routes/blumint/close.rs:51-80`, `:87-106` (`close_key`), `:110-135` (`end`) |

### 1.2 Refusals and error codes

BluMint reads only a top-level `code` (plus `players`), and treats any other
400 or 409 body as final, so create and close answer in BluMint's registry
format (`routes/blumint/refusal.rs:22-81`). Bridge failures keep the bridge
envelope (`error.rs:95-119`, which carries a random `request_id`), and
BluMint retries a 503.

| Answer | When | Where |
|---|---|---|
| `400 {"code":"invalid_request"}` | bad body, missing or bad `Idempotency-Key`, no or two Ember IDs, same player twice, bad `gamesToWin` or `startingWins`, close fields out of range | `create.rs:79-122`, `close.rs:58-63`, `refusal.rs:39-46` |
| `400 {"code":"match_unsupported"}` | not two teams of one, or `gamesToWin` above 10 | `create.rs:92-103`, `:159-166` |
| `409 {"code":"key_reused"}` | key's stored answer has another digest; key closed; key made a match whose answer was purged | `refusal.rs:39-46`, `create.rs:183-203` |
| `400 {"code":"player_unlinked","players":[...]}` | every player without a current link, in request order | `create.rs:239-270` |
| `423 {"code":"player_busy","players":[...]}` | every player in another open match on the connection, checked only after all are linked | `create.rs:239-270` |
| envelope `401`, `403`, `404`, `503` | unknown credential, disabled connection, credential of a non-BluMint connection, database failure | `auth.rs:152-195`, `mod.rs:102-109` |

Refusals are returned as `Err` from the idempotent work, so they are never
stored against the key, while a close's key refusal and any audit commit
with the answer (`refusal.rs:83-90`).

### 1.3 Idempotency and replay

| Rule | Where |
|---|---|
| Stored answers are scoped to the connection (`connection:<id>`), not the credential, because each `register` issues a new credential. | `routes/blumint/keys.rs:11-13`, `create.rs:183-203` |
| A key replays its match for a day (generic purge after `RETENTION_SECS`), even after the match ended. | `routes/mod.rs:215-218`, `routes/idempotency.rs:14-45` |
| The key is the match's `external_match_id` as `blumint:<key>` for the match's whole life, so after the purge a repeat is `key_reused`, never a second match. | `keys.rs:17-33`, migration `024_blumint_keys.sql` |
| A close writes a `refused` marker under the key (status 409, digest `refused`) in the same transaction that cancels, so a creation still in flight meets `key_reused`. | `close.rs:87-106`, `idempotency.rs:85-108` |
| Body errors are checked before the stored answer, so a malformed retry gets `invalid_request`, not the replay. | `create.rs:141-152` |

### 1.4 Outbound pushes (the bridge calls BluMint)

All go to `<api_base>/tournaments/match/submit` with `x-api-key`,
`content-type: application/json` and user agent `ember-bridge/<version>`,
no redirects, 5 s connect and 15 s total timeout (`delivery/results.rs:41-97`,
`util.rs:86-93`). `api_base` defaults by environment
(`routes/blumint/mod.rs:65-71`). All three share the delivery engine
(`delivery.rs`: 4 concurrent, 60 s lease, 1/5/15/60/300/900 s then hourly
with 20% jitter, one day budget). BluMint's 401 and 403 are retried, not
refused (`results.rs:99-107`). Bodies are serialized by `serde_json` without
`preserve_order`, so keys are in sorted order (confirmed: no `indexmap` in
`Cargo.lock`).

| Push | Fires | Body | Where |
|---|---|---|---|
| Result or abort | match terminal; leased only while the connection is enabled and has an API key, so a match that ends before the key is set waits indefinitely, not one day | `result` with `teams` (P1 first, `wins` including starting wins); cancelled or failed is `abort` with `reason` from the last `match.cancelled` (`disputed`, `insufficientPlayers`, `serverFailure`) and `retryable:true`; expired is `abort`, `insufficientPlayers`, `retryable:false`. Body read at send time. 409 counts as delivered. | `delivery/results.rs:249-344`, `routes/blumint/report.rs:22-46`, `:143-168` |
| Progress | a game settles, raises the score, and leaves the match `between_games`; queued in the settling transaction keyed by games won; requeued if a void lowers then a later game restores the count | `{"mode":"progress","matchId","teams"}` read at send time; dropped (`not_required`) once the match is terminal | `delivery/progress.rs:35-59`, `:115-190`, `report.rs:104-106`, `routes/ledger.rs:338` |
| Status | every `bump` of the match revision, every claim, every build refusal, every lapsed claim found by the 10 s sweep, and at restart | `{"mode":"status","matchId","revision","observedAt","status":"pending"|"running","players":[{"inGameId","status":"absent"|"present-not-ready"|"present-ready"}]}`; one row per match, revision +1 per observed change, only sent if it differs; 404 from BluMint is retried | `delivery/status.rs:35-87`, `:135-244`, `report.rs:51-101`, `:124-131`, `routes/ledger.rs:178`, `routes/play.rs:340`, `:362` |

### 1.5 Presence (liveness)

A claim keeps a fighter present for `LEASE_SECS` = 136 s
(`routes/blumint/liveness.rs:27-28`, re-exported as `PRESENCE_LEASE_SECS` at
`lib.rs:32`). The sweep reads only claims that lapsed since the last pass
(`liveness.rs:63-86`, table `bridge_liveness`, migration 023). On restart the
bridge first runs the pass it missed at the last moment it was alive, then
restarts live leases from now so downtime does not read as absence
(`liveness.rs:91-115`, `delivery/status.rs:103-109`, `routes/mod.rs:159-180`,
which runs before the listener serves, `lib.rs:104-118`). Clock steps back
are tolerated up to 10 s (`status.rs:30`, `:47-63`). A fighter refused for a
different build reads present-not-ready (`match_claim_refusals`, migration
022, `routes/play.rs:333-343`). `ready` on claims (migration 025, feature
`matches.claim_ready`) exists only for this report.

### 1.6 Policy, lifetimes and branches outside the BluMint modules

| Behaviour | Where |
|---|---|
| `Policy::of(kind, disputes)`: `own_api_only`, `reports_progress` and a 24 h `max_lifetime_secs` all derive from `kind == "blumint"` | `config.rs:262-298` |
| Policy is read from the stored connection record; `sync_config` writes `kind` and `disputes` once and never updates them | `routes/policy.rs:15-26`, `lib.rs:180-200`, guarded by `tests/deploy.rs:78-80` |
| Generic match, lobby and tournament routes refuse an `own_api_only` connection | `routes/policy.rs:31-44`, called at `routes/matches.rs:110`, `routes/lobbies.rs:592`, `routes/tournaments/mod.rs:92` |
| Kind `blumint` implies `discord_lookup`, `disputes: restart`, results sent; may not set lookup, disputes or `results_url`; only kind allowed an `api_base` | `config.rs:183-226`, `:403-418` |
| Lifetime cap stored per match as `ends_by`; at the cap the match expires even with a game open | `routes/matches.rs:268-283`, `routes/expiry.rs:30-33`, `:56-68` |
| Disputes become cancellations (`needs_review` short-circuit), already generic via `disputes: restart` | `routes/matches.rs:950-962`, `routes/reports.rs:367` |
| A result is final once `delivering` (generic) | `routes/ledger.rs:289-301` |
| Void lock: once a progress report was attempted, a change that lowers a team's wins is refused (`lease_conflict`) | `delivery/progress.rs:65-90`, `routes/ledger.rs:334` |
| Startup: end open matches made before 2.0.0 keys (`external_match_id` without `blumint:`) | `routes/blumint/close.rs:146-167`, `routes/mod.rs:161-170` |
| Three delivery workers built from config at startup | `lib.rs:120-133`, `lib.rs:164-168` |

### 1.7 Register command, config and secrets

| Item | Detail | Where |
|---|---|---|
| `ember-bridge blumint-register <bridge.json> <connection>` | Issues a provider credential labelled `BluMint callbacks`, posts it with `{origin}/v1/blumint/...` to `setLookupPlayer`, `match/setCreate`, `match/setClose`, checking the connection is still enabled before each; revokes earlier `BluMint callbacks` credentials only after all three succeed | `main.rs:43-49`, `lib.rs:170-175`, `routes/blumint/mod.rs:148-248` |
| Connection config | `kind: "blumint"`, `environment`, optional `api_base` | `config.rs:19-20`, `:110-150` |
| Secret | BluMint API key in `integrations.json` `api_keys/<connection>` | `integrations.rs:41-43`, `deploy/set-integration-secret.sh:38-40` |
| nginx | `location = /v1/blumint/matches` with 512 KiB body, shared `ember_bridge` rate zone | `deploy/nginx/bridge.conf.in` |

### 1.8 Database objects used only for BluMint

`blumint_progress` (019), `blumint_status` (021), `bridge_liveness` (023),
`match_claim_refusals` (022), `match_claims.ready` (025),
`matches.starting_wins_p1/p2` (020, only BluMint creates non-zero starts),
`matches.ends_by` (015/policy), `idempotency_records` rows with actor
`connection:<id>` and path `/v1/blumint/matches`, the `blumint:` prefix on
`external_match_id` (024), and migration 026 (dropped table). Audit actions
`blumint.submit`, `blumint.progress`, `blumint.status`, `blumint.close` are
internal only.

### 1.9 Game client

Nothing in the game or helper branches on BluMint. Claims carry `ready`
(generic protocol field), the table starts from `starting_wins` (generic),
and the connect link `embernetplay.link/start#<bridge>/BluMint` matches the
connection's display name. The only BluMint mentions are UI strings in
`locales/*.po`, `src/ui/SignInSites.hxx` comments, the connect page text and
test fixtures. So the client is unaffected provided the connection ID and
display name stay the same. `sdk/typescript/test` has no BluMint tests;
`lookup.test.ts` covers the generic lookup.

## 2. Generic equivalents and gaps

"Exact" means an outside adapter can produce identical bytes and codes with
the generic API as it is today.

| BluMint behaviour | Generic equivalent today | Verdict | Generic capability needed |
|---|---|---|---|
| Lookup by Discord | `POST /v1/players/lookup` with `discord_lookup: true`, same `find_and_link` (`routes/lookup.rs:22-112`) | Exact with translation: adapter keeps the first 32 strings, drops other keys, sends `{"discord":[...]}`, reshapes `players[].ember_id` | G7 (whoami) to authenticate before parsing and to answer `404` for another connection's credential |
| Create by key | `POST /v1/matches` (`routes/matches.rs:103-197`) | Not exact | G2: create by Ember ID (participant resolved from the current link), `starting_wins`, refusals listing every unlinked and every busy Ember ID (generic stops at the first slot, `matches.rs:232-264`), digest over Ember IDs not participant IDs, connection-scoped idempotency (generic scopes by credential, so a re-registration mid-retry would lose the replay) |
| Key reuse after purge | external-ID replay answers `200` with the match (`matches.rs:158-172`) | Mappable but fragile: generic answers 201 new, 200 external replay; adapter maps 200 to `key_reused` | Part of G2: an explicit "replay for a day, then conflict" key mode is cleaner than inferring from 201 versus 200 |
| Close by key with tombstone | `POST /v1/matches/{id}/cancel` needs the match ID, an expected revision and its own key; no lookup by external ID; no way to refuse a future create | Not possible atomically | G3: close by `external_match_id`, which cancels an open match without queuing its result and reserves the ID against later creates, in one transaction, always 200 |
| Result | `results_url` signed push, same queue (`results.rs:205-241`, outcome `completed`, `restart`, `expired`) | Exact except the abort reason | G4: the cancel reason in the result body. `MatchResult` has `deny_unknown_fields` (`protocol/src/partner.rs:106-122`) and `ember-brackets` parses it strictly (`brackets/src/results.rs:83`), so the field ships in lockstep or behind a new `type` version |
| BluMint answer handling | generic: 401 and 403 refused | Exact with translation | none: the adapter maps BluMint's 401 and 403 (and 404 on status) to 503 with `Retry-After` passed through |
| Progress | `score.changed` event exists (`routes/ledger.rs:361`) but carries the score at event time, fires on set-ending games and corrections, is delivered unordered with no "skip once terminal" rule | Not exact | G5: per-connection progress push with the current queue semantics (keyed by games won, body read at send time, dropped when terminal) |
| Void lock | none | Cannot move out | G5 must keep it in the bridge: "once a progress report was attempted, voids that lower wins are refused" decided in the settling transaction. An adapter cannot tell the bridge this atomically |
| Status and presence | `player.present` fires once, on the first host claim (`routes/play.rs:408`); `player.ready` is defined but never emitted; no event for the second arrival, Ready changes, lapses or build refusals; the snapshot has no claims | Cannot move out | G6: per-connection status push computed in the bridge (claims, refusals, lease sweep, restart resume) with a monotonic per-match revision and `observedAt` |
| 24 h lifetime cap | bridge-wide `match_expiry_hours` only; per-connection cap is listed as a follow-up in `INTEGRATION_PATHS.md` | Not exact | G1: per-connection `max_match_hours`, stored with the record like `disputes` |
| `own_api_only` | none | Weaker if the adapter forwards BluMint's own credential to generic routes, since that credential could then make matches BluMint does not know | G1 for the flag; G8 (credential scopes) to keep it exact under pass-through |
| Disputes as restarts | `disputes: "restart"` | Exact | none |
| Policy fixed by kind | stored `kind` never changes | Needs a migration | G1: store policy columns per connection, migrate the existing `blumint` records once |
| Register | `ember-bridge credential` and `revoke` exist | Movable | G7 helps; otherwise the adapter's register takes a credential issued by the bridge CLI and the operator revokes old ones |
| Starting wins in events | `match.completed` already carries them | Exact | none |
| Results waiting for a key | generic waits for a result secret, BluMint waits for its API key | Slight difference | none in the bridge; the adapter refuses to start without its BluMint key so results do not spend their day of retries |

Behaviour that stays in the bridge whatever the topology: presence
computation, the sweep and restart resume, the void lock, the lifetime cap
enforcement, close-before-create atomicity, and keyed replay. Each can be
generic and service-neutral; none can live in an outside process without
changing what BluMint sees. What can leave: the BluMint wire format (field
names, `present-ready` strings, abort reason mapping, registry codes), the
answer mapping, the register calls and the API key.

## 3. Architecture options

### (a) External push-mode adapter

A stateless service (new crate `server/ember/blumint`, binary
`ember-blumint`) on loopback. nginx sends `/v1/blumint/*` to it. Inbound, it
authenticates BluMint's bearer through G7, validates and translates, and
calls G2, G3 or the generic lookup with BluMint's own credential, then
writes BluMint's bytes. Outbound, the bridge pushes signed generic result,
progress and status bodies to it (public https URL through nginx, as
`ember-brackets` receives results, since `results_url` refuses loopback
unless `allow_private_webhooks`, which is bridge-wide); it verifies, rewrites
to BluMint's format with the same user agent, posts to BluMint and returns
BluMint's status mapped for the bridge's queue. It keeps no database: every
piece of state stays in the bridge.

One timing difference is unavoidable: with the bridge's 15 s client
unchanged, the adapter must give BluMint less (about 12 s), so a BluMint
post that today succeeds between 12 and 15 s would instead be retried, and
BluMint would see a duplicate that it answers with 409 (which settles it).
Raising the bridge's timeout for that destination removes the difference.

Not a mode of `ember-brackets`: that crate is a pull poller built on the
`Site` trait with its own SQLite store, while BluMint pushes to us and needs
no store. Sharing a process would also share failures with Challonge and
start.gg tournaments.

### (b) Thin in-bridge codec

Add G1 to G6 to the bridge as generic features, then reduce
`routes/blumint` and the BluMint branches in `delivery/*` to a codec module
that translates BluMint's JSON to and from the generic internals, inside the
same transaction. Policy, queues, tables and lifetimes become generic; the
only BluMint code left is the format.

### (c) Others considered

- (c1) A stateful outside adapter over today's generic API, with its own
  store. Rejected: it cannot see presence, cannot enforce the void lock,
  cannot close before create atomically, cannot list every refused player
  without a racy pre-check, and loses connection-scoped replay. BluMint would
  see different refusals, missing status reports and lost progress.
- (c2) Codecs as plugins (dynamic libraries or WASM). Too much machinery for
  one partner.
- (c3) Keep everything as it is. Zero risk, but the bridge stays BluMint-aware.

### Comparison

| | (a) external | (b) in-bridge codec | (c1) stateful outside |
|---|---|---|---|
| BluMint sees a difference | No, once G1 to G8 exist; extra loopback hop of milliseconds | No | Yes (see above) |
| Create within BluMint's 30 s | Yes: two local calls (whoami, create), each one SQLite write | Yes, unchanged | Yes |
| Transactional guarantees | Kept, because the generic endpoints do the whole operation in one bridge transaction | Kept | Lost |
| New failure modes | adapter down (nginx 502 on callbacks; results retried for a day); bridge to adapter timeout must exceed adapter to BluMint timeout | none | many |
| Operations | another unit, port, nginx location, secret file (BluMint API key and result secret move to the adapter) | none | another unit plus a database to back up |
| Testability | suite runs with a fixture change (section 4) | suite runs unchanged | needs a new suite |
| Bridge service-neutral | fully | all but one codec module | fully |
| Effort | (b) plus about a third again | baseline | not viable |

### Recommendation

Do (b) first, as the gate: build G1 to G6 as generic, documented
per-connection capabilities and serve BluMint through an in-bridge codec over
them. That alone removes BluMint from policy, schema and delivery, is
verified by the unchanged test suite plus golden bytes, and needs no data
migration beyond renaming tables in place. Then move the codec out to (a)
only if a BluMint-free binary is still wanted; that step needs G7, G8 and the
results routing, and is a routing switch that can be rolled back by an nginx
reload because both paths read the same state. Do not build (a) directly on
today's API: it would change what BluMint sees.

## 4. Verification plan

### Contract suite

`tests/blumint.rs` (13 tests), `tests/blumint_reports.rs` (32),
`tests/blumint_command.rs` (1), `tests/partner.rs` (12, generic path) and the
migration tests in `db.rs` are the contract. They are white-box: they use the
in-process `Bridge` fixture (`tests/common/mod.rs`) with a test clock,
`AppState`, `deliver_results`, `maintain` and direct SQL, and a stand-in
BluMint (`tests/common/blumint.rs`).

- For (b) they run unchanged. Only the fixture helpers that read
  `blumint_progress` and `blumint_status` (`tests/common/blumint.rs:295`,
  `:314`, `:387-390`) and one test (`tests/blumint.rs:913`) touch the tables;
  keep the old names as views or update those helpers in the same commit.
- For (a) the test bodies stay, the fixture changes: start the adapter
  in-process on a loopback listener, send `/v1/blumint/*` there, point the
  connection's push URLs at it with private destinations allowed for tests,
  and have `deliver_results` drive the bridge queues through it to the
  stand-in BluMint. `blumint_command.rs` moves with the register command.
- `sdk/typescript/test` and `tests/partner.rs` are regressions for the
  generic surfaces G2 to G6 touch.

### Golden bytes

Before any change, extend the stand-in BluMint to keep raw request bytes,
path and headers (`x-api-key`, `content-type`, `user-agent`) instead of
parsed JSON, and record, for every scenario in the suite: each inbound
response (status, content type, body bytes) and each outbound request. Mask
only values that are random by design (`request_id` in envelopes, generated
`emt_` and Ember IDs, normalized consistently). Status `revision` values and
`observedAt` are deterministic under the test clock and stay unmasked. Every
later step must reproduce the recording byte for byte. Add cases the suite
lacks: oversized lookup with a bad credential, a non-string and a 33rd
lookup entry, an `email` key, a create retried after re-registration, a
close racing a create.

### Staging QA with BluMint

`HOW_BLUMINT_AND_BRIDGES_WORK.md` has no seeded-fighter section; use the
"Testing on staging" steps in `docs/guides/BLUMINT_QUICKSTART.md` with two
tester PCs and Discord accounts on `bridge-staging`. Script with BluMint
(Joe): lookup, create, busy then free, a restart from a score, a dispute,
a close mid-set, an expiry, presence leave and return, a void before and
after progress. Ask BluMint how it treats a 502 from nginx (adapter down)
before choosing (a).

### Live data and cutover

Production has no BluMint connection yet (`blumint-partner-production` waits
for BluMint's production release; the old `blumint-partner-staging` on
production is no longer called). The live data to move is the staging
bridge's. Doing the change before BluMint's production launch is the
cheapest window.

| Data | Plan |
|---|---|
| Connection record | migration sets the new stored policy columns for each `kind = 'blumint'` record (own API only, 24 h cap, progress and status on, restart disputes); keep the connection ID, so links, revoked-link state and `busy()` scope carry over. A new connection ID would lose all of that. |
| `blumint_progress`, `blumint_status` | rename in place to the generic tables with every column kept, so in-flight revisions continue upward and BluMint never sees a lower one |
| `idempotency_records` | keep actor `connection:<id>` and path; the generic keyed create uses the same scheme for that connection |
| `external_match_id` `blumint:<key>` | unchanged; in (a) the adapter adds the prefix |
| Results queue, `ends_by`, starting wins | per match, untouched |
| `end_unkeyed` | confirm no open unkeyed match remains on either bridge (all are past their 24 h cap), then delete it |

Downtime is the usual few-second bridge restart that every upgrade already
has; restart resume keeps presence correct across it. For (a), deploy the
adapter idle, switch nginx `location /v1/blumint/` to it (graceful reload),
then switch the push destinations (config read at startup, so one restart).
Queued rows are sent to the new destination with the same bodies.

### Rollback

For (b), note that `Db::open` (`db.rs`, `migrate`) only applies the
migrations a binary knows and never refuses a newer schema, so an older
binary starts against a renamed schema and then fails on its first write to
`blumint_status` or `blumint_progress`. Plain views would not help, since the
old code updates those tables. Either ship the rename with views under the
old names plus `INSTEAD OF` triggers for one release, or treat the rename as
forward only and roll back by restoring the pre-upgrade backup (losing
whatever happened since). The first is the real rollback. For (a),
revert the nginx location and the push destinations; no data moves, because
the adapter holds none. BluMint's callback credential never changes, so no
re-registration is needed either way.

## 5. Size and order

| Package | Content | Size |
|---|---|---|
| WP0 | Golden recorder and the extra cases above, on current code | S (1 day) |
| WP1 | G1: policy as stored per-connection data, migration of existing records, `max_match_hours`, `own_api_only` setting | M (2 days) |
| WP2 | G2: keyed create by Ember ID with starting wins, full refusal lists, connection-scoped replay then conflict | L (3 days) |
| WP3 | G3: close by external ID with tombstone and no result | S to M (1 to 2 days) |
| WP4 | G4: cancel reason in results, lockstep with `ember-brackets` and the SDK | S (1 day) |
| WP5 | G5 and G6: generic progress and status pushes, tables renamed, void lock, sweep and resume kept | L (3 to 4 days) |
| WP6 | (b) codec: rewrite `routes/blumint` and delivery branches over WP1 to WP5; delete kind branches; docs | M (2 days) |
| Gate | staging deploy, BluMint QA, golden and suite green, production | 2 to 3 days elapsed |
| WP7 | (a) only: G7 whoami, G8 credential scopes, adapter crate (inbound, outbound relay, register) | L (3 to 4 days) |
| WP8 | (a) only: test fixture topology, nginx, systemd unit, secrets script, setup and test-setup | M (2 days) |

About two weeks of work for (b), plus about one more for (a).

What must happen first:

1. WP0, on the current code, before anything changes. Without it "exactly the
   same" cannot be checked.
2. Ask BluMint about 502 handling and whether user agent or latency matter.
3. Agree the generic shapes of G2, G3, G5 and G6 in `INTEGRATION_PATHS.md`
   first, since they become public contract for every connection.
4. Schedule the cutover before BluMint's production launch, so only staging
   data moves.
