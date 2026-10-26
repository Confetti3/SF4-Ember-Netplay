# Findings: server/ember/bridge/src (ember-bridge)

Scope: every file under server/ember/bridge/src at nightly b9ce0e31 (22,091 lines). Tests not reviewed.
Orientation: HOW_BLUMINT_AND_BRIDGES_WORK.md, docs/development/SERVER_TRAFFIC.md,
server/ember-short/deploy/nginx/ember-short-locations.conf (for the public JSON paths).

Recurring work found in this crate, for reference:

| Loop | Interval | What it does per run |
|---|---|---|
| `routes::rooms::poll_forever` | 5 s | GET supervisor /rooms (loopback HTTP, new reqwest Client each time), one read txn, one write txn that UPDATEs every reported room, then `committed()` |
| `routes::maintenance` | 10 s | one write txn (expiry sweeps, deletes), then `committed()` unconditionally |
| `delivery::run` x4 (webhooks, results, progress, status) | 1 s when idle | one `BEGIN IMMEDIATE` lease txn each, so 4 write txns/s on an idle bridge |
| SSE `/v1/events/stream` | each commit, else 5 s | per open stream: one read txn (`still_valid` + `events::list`) |
| nginx `/rooms.json` -> `/v1/rooms/public` | page polls 15 s, nginx caches 5 s | full room list, uncompressed |
| nginx `/rankings.json` -> `/v1/ranking/board/public` | page polls 30 s, bridge + nginx cache 15 s | up to 500 board entries, uncompressed |

---

### F-BRIDGE-1: The 5 s rooms poll rewrites every room row (fresh ciphertext) and fsyncs even when nothing changed
- File: server/ember/bridge/src/routes/rooms/poll.rs:131
- File: server/ember/bridge/src/routes/rooms/poll.rs:142
- File: server/ember/bridge/src/routes/rooms/poll.rs:197
- File: server/ember/bridge/src/routes/rooms/poll.rs:94
- File: server/ember/bridge/src/secrets.rs:110
- Category: overhead
- Severity: P1
- Claim: `apply` runs, for every room the supervisor reports, `load` (20-column SELECT), an unconditional `UPDATE rooms ...` and a second `load`. The UPDATE always passes `sealed = keys.seal(invitation)`, and `Keys::seal` uses a random 24-byte nonce, so `invitation_sealed = COALESCE(?4, invitation_sealed)` stores a different blob on every poll even when the invitation is byte-for-byte the same. Every published room's row therefore changes every 5 s, so every poll's `BEGIN IMMEDIATE` commits dirty pages to the WAL under `synchronous=FULL` (db.rs:245): one fsync every 5 s forever, WAL growth and checkpoints, all while holding the single `Mutex<Connection>` that every request's auth read also waits on (see F-BRIDGE-4). `poll_once` then calls `state.committed()` on every successful poll, waking every SSE stream and a delivery worker every 5 s although nothing was written that they care about (events are only written when `kind` is Some or a room closes).
- Proposed change: make the room transition a pure function: `fn reported(before: &Room, report: &Reported, details: &Details, invitation_unchanged: bool, now) -> Room` that applies today's COALESCE/CASE rules in Rust, and compare the result with `before` (derive `PartialEq` on `Room`). Decide `invitation_unchanged` by opening the stored blob (`keys.open(before.invitation_sealed)`, cheap) and comparing plaintext; seal only when it differs. Issue `UPDATE rooms SET <all listing columns> WHERE room_id = ?` only when `after != before`, and reuse `after` for the event decision instead of the second `load`. Have `apply` return `bool changed` (any UPDATE, close, ban insert, rated game or event) and call `state.committed()` only when it is true. This also deletes the 15-parameter COALESCE/CASE UPDATE, whose rules then live in one readable function.
- Behavior preserved: stored values after each poll (the same COALESCE/CASE rules, moved to Rust), events emitted (same before/after comparison), room closes, bans, rated games, creator-only latch (`opened_at`).
- Behavior at risk: the stored ciphertext of an unchanged invitation is no longer re-randomised each poll (nothing reads it except to open it); any reader relying on `committed()` firing every 5 s as a heartbeat (none found: SSE has its own 5 s timeout, delivery its own 1 s timeout).
- Estimated saving: idle or steady-state polls write 0 WAL pages and do no fsync (today about 17,280 fsyncs/day; with 45 rooms roughly 10 to 50 KB of WAL per poll, 2 to 10 KB/s of disk writes); 45 fewer AEAD seals and 45 fewer 20-column SELECTs per poll; mutex hold time per poll drops to the reads; SSE streams lose 0.2 wake-ups/s each.
- Confidence: high

### F-BRIDGE-2: A new reqwest::Client (TLS config, pool) is built for every outbound call, including the 5 s supervisor poll and every BluMint report
- File: server/ember/bridge/src/routes/rooms/supervisor.rs:131
- File: server/ember/bridge/src/delivery/results.rs:75
- File: server/ember/bridge/src/routes/blumint/mod.rs:192
- File: server/ember/bridge/src/routes/sign_in/provider.rs:286
- Category: overhead
- Severity: P2
- Claim: `Supervisor::client()` calls `outbound_client().build()` for each `list`, `create` and `delete`, so the rooms poll constructs a reqwest Client (rustls ClientConfig with its root store, hyper pool, resolver) every 5 s and opens a new TCP connection each time. `BluMint::post` does the same for every status, progress and result report, so each one pays a fresh TCP + TLS handshake to app.blumint.com / staging.blumint.io. A reqwest Client is an `Arc` meant to be shared and keeps connections alive.
- Proposed change: add one `http: reqwest::Client` (built once from `outbound_client()`) to `AppState`; `Supervisor::{list,create,delete}`, `BluMint::post`, `blumint::register` and the sign-in provider exchange use it (per-request `.timeout(CREATE_TIMEOUT)` already exists for the slow create). Keep the per-attempt builder only in `delivery::webhooks::post_signed`, which needs `resolve()` pinning and `https_only` per destination; that is the one place the per-call build is justified, and a comment should say so.
- Behavior preserved: timeouts, no redirects, no proxy, user agent, the webhook SSRF pinning (unchanged).
- Behavior at risk: none identified; connection reuse to BluMint means a stale pooled connection can fail once and be retried by the existing retry schedule.
- Estimated saving: 17,280 Client builds + TCP connects/day on the poll alone (each build costs on the order of a millisecond of CPU for the root store); one TLS handshake (about 1 RTT plus certificate verification) saved per BluMint report after the first in a burst.
- Confidence: high

### F-BRIDGE-3: Four delivery workers share one Notify woken with notify_one, and each polls a write transaction every second when idle
- File: server/ember/bridge/src/lib.rs:80
- File: server/ember/bridge/src/delivery.rs:104
- File: server/ember/bridge/src/delivery.rs:119
- Category: overhead
- Severity: P2
- Claim: `start` spawns four `delivery::run` workers (webhooks, results, progress, status) that all wait on the same `state.delivery: Notify`, but `committed()` calls `delivery.notify_one()`, which wakes exactly one waiter. The queue that just got work is woken only by luck; the other three find it on their 1 s timeout. So the commit-driven wake-up does not do what the doc comment says, and correctness rests on the 1 s polling. That polling is itself the cost: each idle worker runs `state.db.write(lease)` (a `BEGIN IMMEDIATE`, an UPDATE scan in status/progress, a SELECT join) every second: 4 spawn_blocking write transactions/s, 345,600/day, each taking the global DB mutex.
- Proposed change: give each queue its own `Notify` (or one `tokio::sync::watch` generation that every worker subscribes to, the pattern SSE already uses with `stream_signal`) so a commit wakes all four. Then let an idle worker sleep until the earliest due retry instead of 1 s: have `Queue::lease` also return `next_due: Option<u64>` (`SELECT MIN(next_attempt_at)` / `MIN(next_at)` in the same transaction) and wait `min(next_due - now, 60 s)` or a commit. Retries keep their schedule; new work still starts on the commit that queued it.
- Behavior preserved: at-least-once delivery, lease/attempt fencing, retry schedule and jitter, the 24 h budget.
- Behavior at risk: timing only. Work queued by a write that does not call `committed()` (none found today, but see F-BRIDGE-5) would wait for the 60 s cap instead of 1 s.
- Estimated saving: about 4 write transactions/s on an idle bridge drop to about 1/min; up to 1 s less latency on status/progress/result sends to BluMint after the commit that queued them.
- Confidence: high

### F-BRIDGE-4: One Mutex<Connection> serialises every read behind every write (and its fsync)
- File: server/ember/bridge/src/db.rs:238
- File: server/ember/bridge/src/db.rs:281
- Category: structural
- Severity: P2
- Claim: `Db` is `Arc<Mutex<Connection>>`; `read` and `write` both lock it. The database is in WAL mode, whose whole point is that readers do not wait for the writer, but here every request's authentication (`auth::player`, `auth::service`), every room list, every SSE page waits behind the rooms poll's write txn (F-BRIDGE-1), the maintenance pass, the four lease transactions/s (F-BRIDGE-3) and each `synchronous=FULL` fsync. With claims every 10 s per fighter each needing a challenge write plus a claim write, the request path is fully serialised on one blocking thread at a time.
- Proposed change: keep the single writer connection behind its mutex, and open a small pool (2 to 4) of read-only connections (`SQLITE_OPEN_READ_ONLY`, same pragmas) used by `Db::read`. `write`/`write_with` stay as they are, so BEGIN IMMEDIATE still serialises writers and the uniqueness constraints still decide races.
- Behavior preserved: transactional semantics: a deferred read txn in WAL sees the last committed snapshot, which is what it sees today; writers stay serialised.
- Behavior at risk: a route that reads on `Db::read` immediately after its own `Db::write` returns still sees its commit (WAL readers see committed data at BEGIN), so none expected; in-memory test databases (`:memory:`) cannot share across connections, so tests need a temp file or a shared-cache URI.
- Estimated saving: request latency no longer includes queued writes and fsyncs; CPU unchanged. Removes the hidden coupling that makes every background loop a latency cost for players.
- Confidence: medium

### F-BRIDGE-5: committed() is called by hand at 34 sites, and unconditionally by the periodic loops
- File: server/ember/bridge/src/lib.rs:76
- File: server/ember/bridge/src/routes/mod.rs:243
- File: server/ember/bridge/src/routes/rooms/poll.rs:115
- File: server/ember/bridge/src/routes/events.rs:385
- Category: judo
- Severity: P3
- Claim: whether event streams and delivery workers wake depends on each route remembering `state.committed()` after `db.write` (34 call sites). Forgetting it is silent (streams catch up on their 5 s timeout, workers on 1 s). Meanwhile `maintenance_once` (every 10 s) and the rooms poll (every 5 s) call it on every successful pass whether or not they wrote an event, so every open SSE stream re-runs `still_valid` + `events::list` about 0.3 extra times/s for nothing.
- Proposed change: make the notification a property of the write. `events::emit` and the queue writers (`status::observe`, `progress::game_settled`, outbox inserts in `fan_out`, the results `delivery_state = 'queued'` path) set a flag on the transaction (a small `Tx` wrapper around `rusqlite::Transaction` with a `Cell<bool>`, or an `AppState::write` that compares `events::head` and a queue counter before/after). `AppState::write` calls `committed()` only when the flag is set. Delete the 34 manual calls.
- Behavior preserved: streams and workers wake after every commit that wrote an event or queued a delivery.
- Behavior at risk: any commit that queued work through a path not marked would lose its immediate wake-up (mitigated by the existing timeouts); needs a careful pass over writers of `blumint_status`, `blumint_progress`, `matches.delivery_state`.
- Estimated saving: 34 call sites deleted; idle SSE streams wake on their 5 s timeout only.
- Confidence: medium

### F-BRIDGE-6: The public room list and rankings JSON go to every browser uncompressed; the bridge re-serialises its cached board per request
- File: server/ember/bridge/src/routes/rooms/mod.rs:506
- File: server/ember/bridge/src/routes/ranking.rs:92
- File: server/ember/bridge/src/routes/ranking.rs:102
- File: server/ember-short/deploy/nginx/ember-short-locations.conf:70
- File: server/ember-short/deploy/nginx/ember-short-locations.conf:110
- Category: overhead
- Severity: P2
- Claim: this is the "large JSON every 15 seconds". `rooms.js` refreshes `/rooms.json` every 15 s (REFRESH_MS 15000) and `rankings.js` refreshes `/rankings.json` every 30 s; the bridge caches the board 15 s and nginx caches 5 s / 15 s, so the bridge itself is hit at most every 5 s / 15 s (fine). The cost is the browser hop: `gzip_types` is only set inside `location ^~ /assets/`, so both JSON locations go out as identity. Payloads: up to 100 detailed `RoomSummary` (about 350 to 450 B each, so roughly 15 to 40 KB with a full listing) every 15 s per open rooms tab, and up to `MAX_BOARD` = 500 `BoardEntry` (about 60 to 80 B each, 30 to 40 KB) every 30 s per rankings tab. Both are repetitive JSON that gzips 5 to 8 times. Bridge side, `PublicCache` keeps `Arc<PublicBoard>` and `ok(&**board)` re-serialises up to 500 entries on every cache hit; `public_list` re-reads and re-serialises the room list on every nginx miss.
- Proposed change: (nginx, cross-area) add `gzip on; gzip_types application/json; gzip_vary on;` (and optionally `gzip_comp_level 5`) to the `/rooms.json` and `/rankings.json` locations; proxy_cache keeps one copy and nginx compresses per response. (bridge) cache the serialised body: `PublicCache(Mutex<Option<(u64 listed_at, bool shown, Bytes body)>>)` and answer the `Bytes` clone; do the same for `public_list` keyed by the rooms poll counter (`state.rooms.polls`) plus room create/close, so a body is built once per poll.
- Behavior preserved: body bytes identical (same struct, same serializer); `listed_at` keeps its meaning (time of the read the body came from); cache windows unchanged.
- Behavior at risk: none for old pages (any browser accepts gzip with `Vary: Accept-Encoding`); clients that do not send Accept-Encoding still get identity.
- Estimated saving: per open rooms tab about 1 to 2.5 KB/s down to about 0.2 to 0.4 KB/s; per rankings tab about 1 to 1.3 KB/s down to about 0.2 KB/s; bridge CPU for 500-entry serialisation per request goes to a refcount bump.
- Confidence: high (sizes are estimates from the struct shapes, not measured)

### F-BRIDGE-7: The player room listing rebuilds and double-serialises the same answer on every request between polls
- File: server/ember/bridge/src/routes/rooms/mod.rs:423
- File: server/ember/bridge/src/routes/rooms/mod.rs:443
- Category: overhead
- Severity: P3
- Claim: `GET /v1/rooms?build_id=&detail=1` (every helper's room browser refresh) runs auth, a SELECT over open rooms with a sort, builds `Vec<RoomSummary>`, converts it to `serde_json::Value` (`to_value`) and then `ok(&body)` serialises the Value again. The data can only change on a rooms poll (5 s) or a room create/close, so between those every caller with the same `(build_id, detail)` gets the same rows.
- Proposed change: drop the `to_value` hop (serialise the typed `RoomList` / `ConnectionRoomList` directly; the match arms can return `Response`). Optionally cache the player listing bytes per `(build_id, detail)` stamped with `polls_started(state)` and a create/close generation, invalidated when either moves.
- Behavior preserved: list contents, order and filter; connection listings untouched by the cache.
- Behavior at risk: with the cache, `listed_at` for `detail=1` would be the time the cached body was built (at most one poll, 5 s, older) instead of the request time; if the client shows "listed N s ago" it would read up to 5 s older. The `to_value` removal alone has no risk.
- Estimated saving: one full Value tree allocation per listing (about 100 objects) always; with the cache, the SELECT + sort + serialisation per refresh.
- Confidence: medium

### F-BRIDGE-8: A typed `Json<T>` column helper exists in play/room.rs, but the rest of the crate stores JSON with `to_string(...).unwrap_or_default()` and reads it with silent fallbacks
- File: server/ember/bridge/src/routes/play/room.rs:26
- File: server/ember/bridge/src/routes/matches.rs:296
- File: server/ember/bridge/src/routes/matches.rs:298
- File: server/ember/bridge/src/routes/matches.rs:386
- File: server/ember/bridge/src/routes/matches.rs:463
- File: server/ember/bridge/src/routes/lobbies.rs:86
- File: server/ember/bridge/src/routes/lobbies.rs:635
- File: server/ember/bridge/src/tournament/mod.rs:78
- File: server/ember/bridge/src/routes/play/mod.rs:88
- File: server/ember/bridge/src/routes/play/mod.rs:500
- File: server/ember/bridge/src/routes/play/mod.rs:626
- File: server/ember/bridge/src/routes/reports.rs:163
- File: server/ember/bridge/src/events.rs:105
- File: server/ember/bridge/src/routes/webhooks.rs:178
- Category: judo
- Severity: P2
- Claim: `play::room::Json<T>` implements `FromSql`/`ToSql` for a JSON TEXT column with real errors. Twelve other sites hand-roll the same thing with different failure policies: writes use `serde_json::to_string(x).unwrap_or_default()` (a failure stores `""`, which no reader can parse), reads use `from_str(...).unwrap_or_default()` (rules and metadata silently become `null`/empty in snapshots and assignments; lobby and tournament metadata become an empty map), `map_err(unavailable)` (permits, rules in play), or `.ok()` (prepare descriptors). `snapshot` and `assignments` also parse `rules`/`metadata` into `serde_json::Value` only to re-serialise them, and `assignments` parses both whole documents per row just to pick `native_rules_profile` and `round_label`. `events::fan_out` parses each subscription's `event_types` text for every event emitted.
- Proposed change: move `Json<T>` to `db.rs` (pub(crate)) and use it for every JSON column: `Json<Rules>`, `Json<BTreeMap<String,String>>` metadata, `Json<SignedPermit>`, `Json<Prepare>`, `Json<Vec<String>>` evidence/event types. Reads then fail as `rusqlite::Error::FromSqlConversionFailure` (which becomes `unavailable` through the existing `From`), never a silent default. In `assignments`, select `json_extract(m.rules, '$.native_rules_profile')` and `json_extract(m.metadata, '$.round_label')` instead of parsing. In `fan_out`, filter in SQL with `EXISTS (SELECT 1 FROM json_each(s.event_types) WHERE value = ?kind)`.
- Behavior preserved: stored text is the same `serde_json::to_string` output; response shapes unchanged (`rules` and `metadata` still serialise as objects).
- Behavior at risk: a row whose JSON is unreadable now answers `service_unavailable` where it answered `null`/`{}` (no such rows should exist; that is the point). Key order inside `rules` in `GET /v1/matches/{id}` may change if the typed struct's field order differs from the sorted `Value` map; JSON consumers are order-insensitive.
- Estimated saving: about 20 ad-hoc encode/decode sites collapse to one type; per `/v1/assignments` (polled every 60 s per player, up to 50 rows) 100 full JSON parses removed.
- Confidence: high

### F-BRIDGE-9: Unknown stored enum values silently become a different, meaningful value (Role -> Organizer, state -> Failed)
- File: server/ember/bridge/src/auth.rs:174
- File: server/ember/bridge/src/routes/ledger.rs:82
- File: server/ember/bridge/src/routes/ledger.rs:86
- File: server/ember/bridge/src/routes/lobbies.rs:84
- File: server/ember/bridge/src/tournament/mod.rs:73
- Category: boundary
- Severity: P2
- Claim: `auth::service` maps `role` with `if == "provider" { Provider } else { Organizer }`: any other stored text (typo, future role, corruption) grants the broader organizer role (adjudication, tenant-wide reads). `ledger::load` maps an unknown match state to `MatchState::Failed` (terminal, so the match silently stops accepting play) and an unknown delivery state to `Ambiguous`; lobbies default an unknown rotation to `WinnerStays`; tournaments default an unknown format to `SingleElimination`. These hide invariant violations instead of surfacing them.
- Proposed change: implement `FromSql` for `Role`, `MatchState`, `DeliveryState`, `Rotation`, `Format` (each already has `parse`) returning `FromSqlError::InvalidType` on unknown text, and read them with `row.get::<_, MatchState>(n)?`. Unknown values then fail the request as `unavailable` and are logged by `From<rusqlite::Error>`.
- Behavior preserved: every value the bridge writes parses as today.
- Behavior at risk: a database with an unexpected value now fails loudly on that row instead of continuing with a guessed value (intended).
- Estimated saving: removes five fallback branches; closes a privilege-widening default.
- Confidence: high

### F-BRIDGE-10: matches.rs (977 lines) mixes four route groups with match-domain operations that lobbies, tournaments, reports and linking import from a route module
- File: server/ember/bridge/src/routes/matches.rs:1
- File: server/ember/bridge/src/routes/matches.rs:70
- File: server/ember/bridge/src/routes/matches.rs:231
- File: server/ember/bridge/src/routes/matches.rs:556
- File: server/ember/bridge/src/routes/matches.rs:680
- File: server/ember/bridge/src/routes/matches.rs:975
- Category: size
- Severity: P2
- Claim: the file holds (a) read models and viewer resolution: `viewer_of`, `service_viewer`, `visible`, `snapshot`, `get`, `assignments` (about 200 lines); (b) creation: `create`, `create_match`, `NewMatch`, `linked`, `insert_match`, `created_body` (about 200); (c) lifecycle used across the crate: `cancel`, `cancel_and_release`, `cancel_match`, `cancel_lobby_set`, `cancel_tournament_set`, `has_games`, `on_unlink`, `needs_review` (about 230); (d) adjudication: `AdjudicationKind`, `AdjudicationCommand`, `adjudicate`, `apply_adjudication`, `record_adjudication`, `check_reason` (about 280). Groups (b) and (c) are domain operations called by `lobbies`, `tournament`, `reports`, `linking`, so route modules depend on each other (`routes::events` and `routes::records` import `viewer_of` from `routes::matches`; `lobbies` imports `idempotent`, `into_response`, `service_viewer`, `viewer_of` through `matches`' re-exports). It also has two identical wrappers: `respond` and `into_response` (which calls `respond(..).into_response()`, an identity) over `http::json`.
- Proposed change: split along those seams. Move `viewer_of`/`service_viewer` to `events.rs` as `Viewer::of(&Actor)` / `Viewer::of_service(&Service)` (the type lives there). Move `NewMatch`, `linked`, `insert_match`, `cancel_and_release`, `cancel_match`, `cancel_lobby_set`, `cancel_tournament_set`, `has_games`, `on_unlink`, `needs_review` into the existing domain module `routes::ledger` (or a `ledger/` folder: `ledger/create.rs`, `ledger/cancel.rs`) beside `busy`, `bump`, `settle`, `release`. Leave `routes/matches/{mod.rs (create/get/assignments/cancel routes), adjudicate.rs}`. Delete `respond`/`into_response` and the `pub use` re-exports of `idempotent`, `busy`, `participants`, `release_players`, `scores`; callers import from their real homes.
- Behavior preserved: everything; pure moves plus deleting two wrappers.
- Behavior at risk: none identified.
- Estimated saving: no file over about 350 lines; removes the route-to-route import web (lobbies, events, records, reports, tournament, linking stop depending on `routes::matches`).
- Confidence: high

### F-BRIDGE-11: lobbies.rs (908 lines): rotation engine, read model and routes in one file; on_set_completed re-implements announce
- File: server/ember/bridge/src/routes/lobbies.rs:232
- File: server/ember/bridge/src/routes/lobbies.rs:344
- File: server/ember/bridge/src/routes/lobbies.rs:386
- File: server/ember/bridge/src/routes/lobbies.rs:426
- File: server/ember/bridge/src/routes/lobbies.rs:836
- Category: size
- Severity: P2
- Claim: seams: (a) model and reads: `Lobby`, `load`, `Entry`, `entries`/`queue`/`seats`/`seated_ids`/`ids`, `visible`, `touch`, `snapshot` (about 200 lines); (b) the rotation engine called from the ledger and tournament code: `set_entry`, `unavailable`, `advance`, `keep_streak`, `announce`, `on_set_completed`, `remove`, `on_players_free`, `on_unlink` (about 330); (c) five idempotent routes with their commands (about 370). Inside (b), `on_set_completed` (lines 426 to 454) repeats `announce` step for step: `keep_streak`, `touch`, `load`, `queue`, emit the primary event, then emit `LobbySetStarted` with `lobby_revision` and `queue` patched into a `json!` Value. `advance` calls `queue(tx, ...)` again inside the per-slot loop. `close` has its own reason check (rejects `\n`) that disagrees with `matches::check_reason` (allows `\n`), and builds the stale-revision error inline twice although `ledger::stale` exists.
- Proposed change: split into `lobbies/{model.rs, rotation.rs, routes.rs}`. Generalise `announce(tx, ctx, lobby_id, primary: impl FnOnce(&Lobby, u64 revision, &[Entry] queue) -> (Kind, Value), started)` so `on_set_completed` passes its `LobbySetCompleted` builder and loses about 25 duplicated lines. Read the queue once in `advance`. Use one shared `Reason` check (one rule for `\n`, decided once) for matches, lobbies and tournaments, and one `stale(current)` constructor for match and lobby revisions.
- Behavior preserved: event order and contents (queue-changed or set-completed first, then set-started), revisions, rotation and streak rules.
- Behavior at risk: unifying the reason rule changes one of the two routes' acceptance of a `\n` in `reason`; pick the stricter or the looser explicitly.
- Estimated saving: about 40 lines deleted, one duplicated sequence gone, three files under 400 lines.
- Confidence: high

### F-BRIDGE-12: Every provider/organizer write route repeats the same 15-line service-command pipeline; sessions::Call already shows the shape
- File: server/ember/bridge/src/routes/matches.rs:102
- File: server/ember/bridge/src/routes/matches.rs:497
- File: server/ember/bridge/src/routes/matches.rs:680
- File: server/ember/bridge/src/routes/lobbies.rs:585
- File: server/ember/bridge/src/routes/lobbies.rs:690
- File: server/ember/bridge/src/routes/lobbies.rs:773
- File: server/ember/bridge/src/routes/lobbies.rs:836
- File: server/ember/bridge/src/routes/sessions.rs:419
- File: server/ember/bridge/src/routes/idempotency.rs:14
- Category: judo
- Severity: P2
- Claim: 11 handlers (matches create/cancel/adjudicate, lobbies create/join/leave/close, tournaments create/register/withdraw/start/cancel, BluMint create) each do: `auth::service` -> `provider_connection()` (sometimes `policy::generic_connection`, sometimes `service_viewer`) -> `idempotency_key` -> `body.parse` -> `check` -> `json::digest` -> `Ctx::of` -> `format!` the path -> `db.write(|tx| idempotent(tx, &service.id, &path, &key, &digest, ctx.now, |tx| ...))` -> `state.committed()` -> `into_response`. Player-proof routes already got a canonical runner, `sessions::Call::run`. Separately, `idempotent` serialises each new answer twice (`to_vec` for storage, then `http::json` for the response) and parses every replayed answer back into a `Value` only to serialise it again.
- Proposed change: add `ServiceCall { path, role: Option<Role>, generic_only: bool }` with `run<C, F>(state, headers, body, check, work: FnOnce(&Tx, &Ctx, &Service, C) -> Result<(StatusCode, impl Serialize)>) -> Result<Response>`, doing the whole pipeline once (and the commit notification of F-BRIDGE-5). Make `idempotent` work in bytes: serialise the answer once, store those bytes, return `(StatusCode, Vec<u8>)`, and replay the stored bytes as the response body without parsing.
- Behavior preserved: idempotency scope (actor, path, key), digests, status codes and bodies byte-for-byte (the stored bytes are what `http::json` would have produced from the same Value).
- Behavior at risk: none identified, provided `ServiceCall` keeps the per-route differences (organizer-only adjudication, generic-connection check, body-optional leave) as explicit options.
- Estimated saving: roughly 120 to 150 lines across four files; one JSON serialisation per write and one parse plus serialisation per replay removed.
- Confidence: medium

### F-BRIDGE-13: A match's score is computed four different ways, one of them N+1 per row
- File: server/ember/bridge/src/routes/ledger.rs:133
- File: server/ember/bridge/src/routes/records.rs:66
- File: server/ember/bridge/src/routes/records.rs:252
- File: server/ember/bridge/src/routes/matches.rs:461
- Category: spaghetti
- Severity: P3
- Claim: `ledger::scores` re-reads `starting_wins_p1/p2` (already on `Match`) and loops over accepted attempts in Rust; `records::played` and `records::lobby_standings` use three correlated `COUNT(*)` subqueries per row; `assignments` calls `scores(tx, id)` once per listed match (up to 50 extra SELECT pairs per request, polled every 60 s per player and every few seconds while a restart is watched). Four codings of "wins per slot" can drift (draws, voided games, starting wins).
- Proposed change: one SQL view (new migration) `match_wins(match_id, p1, p2, draws)` = `SELECT match_id, SUM(outcome='p1_win'), SUM(outcome='p2_win'), SUM(outcome='draw') FROM attempts WHERE state='accepted' GROUP BY match_id`, LEFT JOINed by `assignments`, `records::played`, `lobby_standings`; `ledger::scores` takes `&Match` and uses its `starting_wins` plus the view (still returning the accepted ids where settle needs them).
- Behavior preserved: counts (same predicates), response shapes.
- Behavior at risk: none identified; add a test that the four callers agree on a match with a void and a draw.
- Estimated saving: `/v1/assignments` drops from 1 + 2N queries to 1; one scoring definition instead of four.
- Confidence: medium

### F-BRIDGE-14: BluMint status observation is wired into the shared ledger bump; the immutable connection policy is re-queried 8 times
- File: server/ember/bridge/src/routes/ledger.rs:166
- File: server/ember/bridge/src/delivery/status.rs:35
- File: server/ember/bridge/src/routes/policy.rs:15
- Category: spaghetti
- Severity: P3
- Claim: `ledger::bump`, used by every match state change on every platform, calls `status::observe`, which reloads the match it was just handed (`load`), queries `provider_connections` (`policy::of`) and only then returns for non-BluMint connections. The play claim path calls `observe` again. `policy::of` is called at 8 sites, each a query, although `routes/policy.rs` itself documents that kind and disputes never change for a connection id.
- Proposed change: load the policy with the match: `ledger::load` joins `provider_connections` and fills `Match.policy: Policy`. `bump` then reads `if found.policy.reports_progress { status::observe_loaded(tx, found_after_bump, now) }`, and `progress`, `needs_review`, `reports`, `watch::access`, `watch::organizer`, `insert_match` read `found.policy` instead of querying.
- Behavior preserved: same policy values (they cannot change); BluMint status is observed on the same events.
- Behavior at risk: none identified.
- Estimated saving: 2 queries per bump on every non-BluMint match, 1 query per remaining policy check; feature logic leaves the shared path's control flow.
- Confidence: medium

### F-BRIDGE-15: Discord and configured sign-in sites keep parallel table sets, dispatched by format!-built SQL
- File: server/ember/bridge/src/linking/site.rs:70
- File: server/ember/bridge/src/linking/site.rs:113
- File: server/ember/bridge/src/linking/site.rs:122
- File: server/ember/bridge/src/linking/sign_ins.rs:17
- Category: judo
- Severity: P3
- Claim: migration 027 created `site_accounts`, `site_sign_ins`, `site_withdrawals` with Discord's columns plus `site` (the db.rs test asserts this). Instead of moving Discord's rows in with `site = 'discord'`, the code keeps both sets and routes every statement through `Tables`, `Site::scope` (table name plus a `"site = :site AND "` fragment spliced into the WHERE) and `Site::bind` (conditionally appended `:site`), across about 35 statements in `linking/site.rs`, `linking/sign_ins.rs`, `skill/`. The sign-in flow is otherwise already unified (`routes::sign_in` serves both), so the dual tables are the remaining fork.
- Proposed change: a migration that copies `discord_*` rows into `site_*` with `site = 'discord'`, drops the old tables and recreates `discord_accounts` / `discord_withdrawals` as views (`SELECT ... FROM site_accounts WHERE site = 'discord'`) for the readers that join them by name (`skill::ranking::board`, BluMint lookup). Then delete `Tables`, `scope`, `bind`; every statement is static SQL with `site = :site`.
- Behavior preserved: lookups, withdrawals, link revocation on move, the ranking join (through the views).
- Behavior at risk: migration correctness (foreign keys from `links`, cascades on `site_withdrawals`); an older bridge binary cannot run on the migrated database (already true of every migration).
- Estimated saving: one string-spliced SQL dispatch layer removed; about 35 statements become static and checkable by `EXPLAIN` tests.
- Confidence: low

### F-BRIDGE-16: Responses are built as json! Values and then patched or walked, instead of typed protocol structs
- File: server/ember/bridge/src/routes/matches.rs:350
- File: server/ember/bridge/src/routes/lobbies.rs:559
- File: server/ember/bridge/src/routes/lobbies.rs:664
- File: server/ember/bridge/src/routes/records.rs:217
- File: server/ember/bridge/src/routes/rooms/mod.rs:381
- Category: boundary
- Severity: P3
- Claim: the match snapshot, assignments, created body, lobby snapshot, lobby/match event payloads and adjudication answers are `serde_json::json!` literals; their shapes exist nowhere as types (the SDK and protocol crate have to mirror them by hand). `lobbies::get` then hides other players' `participant_id` by recursively walking the whole Value (`records::redact`), matching any object that happens to have an `ember_id` key. Room events convert a typed `ConnectionRoom` to `Value` and splice `reason` in. Lobby set-started events patch `lobby_revision` and `queue` into an existing Value.
- Proposed change: define the response structs (`MatchSnapshot`, `Assignment`, `LobbySnapshot` with `participant_id: Option<String>` and `skip_serializing_if`) in `ember_protocol` next to `RoomSummary`, build them directly, and set `participant_id: None` for other players when the viewer is a player, instead of redacting after the fact. Room events use a `#[serde(flatten)]` wrapper `{ room, reason }`.
- Behavior preserved: field names and values (each struct mirrors today's literal).
- Behavior at risk: key order in responses (Value maps sort keys; structs serialise in field order). JSON clients are order-insensitive; the event bodies are canonicalised by `json::canonical` anyway.
- Estimated saving: removes the recursive redaction pass and per-response Value trees; gives clients a shared contract.
- Confidence: medium

## Area summary

The bridge is carefully written at the function level (good comments, transactional discipline, lease fencing, SSRF pinning), but its periodic machinery does far more work than the state changes it tracks, and its two largest route files hold domain logic other modules depend on.
Biggest judo move: make room transitions a pure `before -> after` function in the rooms poll (F-BRIDGE-1), which deletes the 15-parameter COALESCE/CASE UPDATE and the second load while turning an every-5-seconds fsync into a write only when a room actually changed; close behind are promoting the existing `Json<T>` column type crate-wide (F-BRIDGE-8) and a `ServiceCall` runner (F-BRIDGE-12).
Biggest overhead item: the rooms poll write amplification (F-BRIDGE-1), then idle delivery polling at 4 write transactions/s with a Notify that wakes only one of four workers (F-BRIDGE-3), per-call reqwest Client builds (F-BRIDGE-2) and the single connection that serialises reads behind all of it (F-BRIDGE-4).
On the "large JSON every 15 s": the bridge side is already capped by nginx caches; the bytes that recur are the uncompressed /rooms.json (15 s) and /rankings.json (30 s) to each open browser tab, fixed in nginx config (F-BRIDGE-6).
SSE keepalive (a comment frame every 20 s) is negligible; the stream's cost is wake-ups on commits that wrote nothing (F-BRIDGE-5).
Not assessed: sizes were estimated from struct shapes, not measured on the VPS; the in-game room browser's refresh cadence (C++ side) was not traced, so F-BRIDGE-7's per-second rate is unknown; tests, the TypeScript SDK and ember-rooms were out of scope (a conditional or delta `GET /rooms` from the supervisor would cut the 5 s loopback payload further but needs an ember-rooms change).
