# Adversary verdicts: server-bridge.md (F-BRIDGE-1 to 16) and server-rooms.md (F-ROOMS-1 to 10)

Read at sf4-nightly b9ce0e31 (verified with `git log -1`). Read-only. Out-of-tree file also read: C:\Users\Kate\Desktop\sf4\ember-tunnel\vps\ember-rooms-pool.py (the production `GET /rooms` path; see F-ROOMS-1).

Facts used by several verdicts:
- Bridge DB: one `Arc<Mutex<Connection>>`, `journal_mode=WAL`, `synchronous=FULL`, `busy_timeout` 5 s set on that one connection only (server/ember/bridge/src/db.rs:61-74). In WAL with FULL the WAL is fsynced on every commit that wrote frames; a `BEGIN IMMEDIATE` that dirties no page commits with no WAL write and no fsync.
- `AppState::committed()` (lib.rs:81-87): `events.notify_waiters()` (the `events` Notify has NO waiter anywhere in src or tests: dead), `delivery.notify_one()`, `stream_signal` generation bump.
- Production bridge does not talk to a supervisor directly: `rooms.supervisor_url` points at ember-rooms-pool.py (127.0.0.1:47840, out of tree), which merges the VPS and server1 supervisors, passes each room dict through unchanged (`merged.append(room)`, pool.py:203) and serves a down backend's last list for 180 s (`grace_secs`).
- `migrate` (db.rs:128-149) has no downgrade guard, and the upgrade-*.ps1 scripts rely on running the previous binary on the migrated DB for `-Rollback` (e.g. upgrade-bridge-blumint-production-20261020.ps1:26). Rollback compatibility is therefore a live constraint for any migration or stored-value change.
- `RoomSummary` is `#[serde(deny_unknown_fields)]` (server/ember/protocol/src/rooms.rs:200-202) and released clients parse it directly (rust/sf4-net/src/tournament/rooms.rs:130,195,215).
- docs/perf/ROOM_PERF_BASELINE.md covers room-host/member snapshot work only; none of these findings re-proposes its nine changes or per-table deltas.

---

## server-bridge.md

### F-BRIDGE-1: CONFIRMED  (severity: keep P1)
- Decisive invariant/evidence: poll.rs:135-138 seals the invitation with `keys.seal`, which draws a fresh 24-byte nonce (secrets.rs:110-118), and poll.rs:155-185 runs an unconditional `UPDATE rooms` with `invitation_sealed = COALESCE(?4, ...)` for every reported room; SQLite rewrites the row even when values are equal, and the new ciphertext guarantees a changed page anyway. WAL + `synchronous=FULL` (db.rs:69-70) makes that one WAL fsync per poll. poll.rs:96-98 calls `committed()` on every successful poll. `load` runs before and after (poll.rs:132,198).
- Correction to the estimate: "fsync every 5 s forever" holds only while at least one room is reported. With zero rooms the loop body never runs and the tail statements (poll.rs:243-253) match nothing, so no page is dirtied and no fsync happens. With rooms, the 10 to 50 KB/poll WAL figure is plausible (row plus index pages, 4 KB each) but unmeasured.
- Behavior check: (1) After the change, unchanged polls still run `skill::record_games` and the ban `INSERT OR IGNORE`s; ignored inserts dirty no page, so the "0 WAL pages, no fsync" claim holds. (2) Nobody depends on the 5 s `committed()` heartbeat: SSE has its own 5 s timeout (routes/events.rs:155) and re-checks `still_valid` then; delivery has its 1 s timeout (delivery.rs:108); `events` Notify has no waiter. Session revocation (sessions.rs:311-324) never calls `committed()`, so a revoked SSE stream closes within its own 5 s timeout today and after the change; the bound does not move. (3) The pure before/after function must keep `opened_at = COALESCE(opened_at, CASE WHEN opened THEN now END)` and the `WHERE closed_at IS NULL AND invitation_sealed IS NOT NULL` guard (a room still being created must stay untouched; create.rs:140 publishes it). (4) No wire change, bridge-only, one restart, no migration: rollback-safe.
- Adjustment: none required. Compare plaintexts after `keys.open` of the stored blob, as proposed (no migration).
- Duplicate of: no. CANONICAL for the "5 s rooms poll rewrites every room" cluster (F-ROOMS-1 and F-OVH-8 in the other batch point here).

### F-BRIDGE-2: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: `Supervisor::client()` builds per call (routes/rooms/supervisor.rs:131-133, used by create/list/delete); `BluMint::post` builds per report (delivery/results.rs:75), also used by progress.rs:186 and status.rs:244; `blumint::register` (routes/blumint/mod.rs:192) and the sign-in token exchange (routes/sign_in/provider.rs:286) likewise. Only `webhooks::post_signed` pins: `builder.resolve(host, address)` plus `https_only` per destination (delivery/webhooks.rs:211-221), and results' `results_url` path goes through it (results.rs:177). The BluMint reporter does NOT pin: its URL is `api_base(connection)` from the bridge's own config (results.rs:60-65), not a platform-chosen destination, so a shared client is safe there. The sign-in token URL is operator configuration, also safe to share.
- Severity lowered: one Client build per 5 s plus rare BluMint calls is small; reqwest 0.13 `rustls-no-provider` (bridge Cargo.toml:18) root loading cost per build is unmeasured. Real but minor.
- Adjustment: keep the per-attempt builder in `post_signed` with the comment, as proposed. Note the supervisor URL is the Python pool (HTTP/1.1, ThreadingHTTPServer, pool.py:38,307), so keep-alive works but a stale pooled connection fails once; the poll already treats a failed poll as "change nothing", so harmless.
- Duplicate of: no.

### F-BRIDGE-3: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: four workers wait on one `Notify` (lib.rs:121-137), `committed()` uses `notify_one` (lib.rs:84), idle wait is 1 s (delivery.rs:104-110). Each idle pass is a `BEGIN IMMEDIATE` lease (delivery.rs:116-119). Correction: on an idle bridge these leases dirty no page (status/progress sweep UPDATEs match nothing; results/webhooks only SELECT), so there is no WAL write and no fsync; the cost is 4 mutex holds plus queries per second, not disk I/O. Severity lowered for that reason.
- Adjustment: (1) `next_due` must use the exact lease predicate minus `<= now`, including results' `m.state IN (terminal)`, `c.enabled = 1` and the destination filter (results.rs lease, status.rs/progress.rs leases). A plain `MIN(delivery_next_at)` would return a past time for a queued row whose match is not terminal yet and turn the worker into a hot loop. Clamp the wait to at least 1 s. (2) With a `watch` generation all four workers wake per commit, so a busy bridge goes from 1 to 4 lease transactions per commit; fine once F-BRIDGE-5 limits which commits notify. (3) The 60 s cap is safe only if every writer that makes work due calls `committed()`: writers of `blumint_status`, `blumint_progress`, `matches.delivery_state = 'queued'`, the outbox, and any time-only transition (a match expiring by time is written by maintenance, which notifies). That audit was not done by the reviewer or by me; out-of-process writers (ops scripts that open the SQLite file) would also wait up to the cap.
- Duplicate of: no (interacts with F-BRIDGE-5).

### F-BRIDGE-4: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: WAL is on (db.rs:69); `read` and `write` both take the one mutex (db.rs:105-127). No `Db::read` closure writes (grep of all 37 `.read(` sites with the following lines found no execute/INSERT/UPDATE/DELETE), so `SQLITE_OPEN_READ_ONLY` readers are viable. Read-your-writes holds: a deferred read on another connection begun after `write` returns sees that commit. The poll's ordering rule (published set read before the supervisor is asked, poll.rs:73-77) is preserved because it is sequential in one task.
- Adjustment: each read connection needs its own `busy_timeout` (set only on the one connection today, db.rs:72); `routes/blumint/liveness.rs:124` opens `Db::open(":memory:")`, which cannot be shared across connections (use a temp file or `file::memory:?cache=shared`). Payoff shrinks a lot once F-BRIDGE-1 and F-BRIDGE-3 land (the periodic writes mostly disappear), so do it after them and only if request latency is measured to suffer.
- Duplicate of: no.

### F-BRIDGE-5: PLAUSIBLE  (severity: keep P3)
- Decisive invariant/evidence: 34 `committed()` call sites (grep count); maintenance notifies unconditionally (routes/mod.rs:225-252); rooms poll likewise (poll.rs:96-98).
- Adjustment: the flag must also cover commits that change what `still_valid` returns if prompt stream termination is wanted (credential revoke via lib.rs:360-380, connection disable); today session revoke does not notify, so the 5 s bound is already the behavior and nothing regresses, but say it explicitly. Delete the dead `events: Notify` (no waiter anywhere) in the same change. The rooms-poll half is subsumed by F-BRIDGE-1.
- Duplicate of: partial overlap with F-BRIDGE-1 (rooms-poll `committed()`); F-BRIDGE-1 is canonical for that part.

### F-BRIDGE-6: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: in the tree, `gzip_types` appears only inside `location ^~ /assets/` (server/ember-short/deploy/nginx/ember-short-locations.conf:173-174); there is no `gzip on` anywhere in the tree (the asset block relies on the distro nginx.conf's `gzip on`, whose default `gzip_types` is text/html only). The `/rooms.json` and `/rankings.json` locations (locations.conf:70-93, 110-133) set no gzip directives; bridge.conf.in has none either. So, per the tree, both JSON answers go out identity unless the live site file or nginx.conf adds `application/json`, which cannot be checked from the tree. Adding the three directives is idempotent if it already is on.
- Bridge-side half REJECTED as negligible for steady-state overhead: via the site (the only known caller) nginx caches `/rooms.json` 5 s and `/rankings.json` 15 s with `proxy_cache_lock on`, and the bridge's board cache is 15 s (ranking.rs:71-118), so the bridge re-serialises at most once per 5 s / 15 s. Both routes are also reachable directly on bridge.embernetplay.link, capped only by the 20 r/s per-address zone (server/ember/bridge/deploy/nginx/ember-bridge-zone.conf:4), where each direct rooms hit is a full DB read; abuse protection belongs in that zone, not a bridge cache. Caching `public_list` per poll would also shift `listed_at` from request time to poll time (up to 5 s), which rooms.js uses for ages.
- Adjustment: nginx only. Optionally `gzip_proxied any` so clients arriving through a proxy that adds `Via` are compressed too. Keep `Vary: Accept-Encoding` (`gzip_vary on`).
- Duplicate of: no. CANONICAL for "public JSON uncompressed"; F-ROOMS-9 is its duplicate.

### F-BRIDGE-7: PLAUSIBLE  (severity: keep P3)
- Decisive invariant/evidence: `to_value` then `ok(&body)` (routes/rooms/mod.rs:441-468). Dropping the Value hop is behavior-free (same serializer; `RoomList` field order equals the struct order the client already accepts).
- Adjustment: do only the `to_value` removal. The per-poll cache changes `listed_at` semantics for `detail=1` and the in-game call rate is unknown, so it is not justified yet.
- Duplicate of: no.

### F-BRIDGE-8: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: `Json<T>` exists privately (routes/play/room.rs:26-43); `serde_json::to_string(..).unwrap_or_default()` at matches.rs:296-298 and `from_str::<Value>(..).unwrap_or_default()` at matches.rs:386, 463-464, confirmed. `Rules` is `#[serde(deny_unknown_fields)]` (server/ember/protocol/src/matches.rs:33-35).
- Adjustment: reading `rules` as `Json<Rules>` is stricter than "no such rows exist": a row written by a bridge whose `Rules` had a field later removed, or by a newer bridge read after a `-Rollback`, fails today's tolerant read and would 503 `GET /v1/matches/{id}` and the whole `/v1/assignments` list. Typed round trips can also add or drop default/None fields, so "response shapes unchanged" is not guaranteed. Use `Json<serde_json::Value>` (or `Box<RawValue>` passthrough) for `rules`/`metadata` in responses, typed `Json<T>` only where the bridge reads fields; keep `json_extract` for `assignments`. A list endpoint should skip-and-log a bad row rather than fail the whole list. Severity lowered: no observed defect, a robustness/cleanup item.
- Duplicate of: no.

### F-BRIDGE-9: PLAUSIBLE  (severity: keep P2)
- Severity split: Role/Rotation/Format part P3; MatchState/DeliveryState part keeps P2 (it is what earns the headline).
- Decisive invariant/evidence: the code does fail open, `role == "provider" ? Provider : Organizer` (auth.rs:174-178). But `service_credentials.role` has `CHECK (role IN ('provider','organizer'))` (migrations/001_identity.sql:24), and `rotation` and `format` have CHECKs too (003_lobbies.sql:13, 005_tournaments.sql:14). No INSERT/UPDATE can store another value, so "typo, future role, corruption grants organizer" is not reachable through the bridge; anyone able to bypass CHECK could write 'organizer' directly. The only realistic path is a future migration that widens the role CHECK followed by a `-Rollback` to an older binary. So it is defense-in-depth, not a privilege-widening hole: P3, and the security framing should be dropped. If fixed, per the owner's rule it stays out of release notes.
- `matches.state` and `delivery_state` have NO CHECK (002_matches.sql:11,16), so `MatchState::parse(..).unwrap_or(Failed)` (ledger.rs:82) is reachable exactly in the rollback case: a state added by a newer bridge reads as terminal Failed on the old one and the match silently stops. That part keeps P2.
- Adjustment: the proposed `FromSql` turns rollback behavior from "silent Failed" into "503 on that match" (and on any list that loads it); state that as the chosen behavior, and have list paths skip-and-log the row. Role fail-closed is a cheap, quiet hardening change.
- Duplicate of: no.

### F-BRIDGE-10: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: matches.rs is 977 lines; `pub use` re-exports of `idempotent`, `busy`, `participants`, `release_players`, `scores` (matches.rs:40-43); `respond` (matches.rs:97-99) and `into_response` = `respond(..).into_response()` on a `Response` (matches.rs:974-977), an identity wrapper.
- Adjustment: none; pure moves. Severity P3 (no runtime effect).
- Duplicate of: no.

### F-BRIDGE-11: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: `on_set_completed` (lobbies.rs:426-455) repeats `announce` (lobbies.rs:344-383) step for step: `keep_streak`, `touch`, `load`, `queue`, primary event, then `LobbySetStarted` with `lobby_revision`/`queue` patched. Reason rules disagree: matches allows `\n` (matches.rs:642), lobbies rejects every control char (lobbies.rs:848).
- Adjustment: unify on the looser rule (allow `\n`). Tightening matches would start refusing partner/organizer requests that succeed today (an SDK-visible wire change); loosening lobbies only accepts more. Event order must stay primary-then-started.
- Duplicate of: no.

### F-BRIDGE-12: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: idempotency stores `serde_json::to_vec(&body)` (routes/idempotency.rs:41) and replays by `from_slice` then re-serialising (idempotency.rs:31); `http::json` is the same `serde_json::to_vec` (http.rs:40-45), so storing and replaying bytes is byte-identical, including for records stored before the change (24 h retention).
- Adjustment: keep `unwrap_or_default()` out of the byte path (a failed encode must fail the write, not store `""`). `ServiceCall` must carry the per-route differences as options, as stated.
- Duplicate of: no.

### F-BRIDGE-13: PLAUSIBLE  (severity: keep P3)
- Decisive invariant/evidence: `ledger::scores` re-reads starting wins and loops attempts (ledger.rs:131-152); `records::played` uses three correlated COUNTs (records.rs:66-80); `assignments` calls `scores` per row (matches.rs:463), so 1 + 2N queries.
- Adjustment: `scores` returns accepted ids for wins only (draws `continue` before the push, ledger.rs:145-150); the rewrite must keep that. A view migration is rollback-safe (an old binary ignores it).
- Duplicate of: no.

### F-BRIDGE-14: CONFIRMED  (severity: keep P3)
- Decisive invariant/evidence: `status::observe` reloads the match and queries `policy::of` before returning for non-BluMint (delivery/status.rs:36-40); 8 `policy::of(` call sites; routes/policy.rs:1-4 documents kind and disputes never change.
- Adjustment: `observe_loaded` must receive the post-bump match (state and revision after the update), not the pre-bump one.
- Duplicate of: no.

### F-BRIDGE-15: PLAUSIBLE  (severity: keep P3)
- Decisive invariant/evidence: dual table sets and `Tables`/`scope`/`bind` exist as described (linking/site.rs). The finding's "an older bridge binary cannot run on the migrated database (already true of every migration)" is wrong: prior migrations are additive and `-Rollback` runs the old binary on the migrated DB. Dropping `discord_*` for views makes the old binary's INSERT/UPDATE on those names fail, so Discord linking breaks on rollback.
- Adjustment: either accept and document "no rollback past this migration" (with a restore.sh restore as the only way back), or give the views INSTEAD OF triggers. Also needs a foreign-key/cascade test as the finding says.
- Duplicate of: no.

### F-BRIDGE-16: PLAUSIBLE  (severity: keep P3)
- Decisive invariant/evidence: `records::redact` removes `participant_id` from any object with another player's `ember_id`, recursively (records.rs, `fn redact`); event bodies are canonicalised (events.rs:59), so key order there is unaffected; plain responses would change key order (harmless to JSON clients).
- Adjustment: redaction is a privacy control. Typed structs must cover every nested object that carries `participant_id` (seats, queue, standings), and a test should keep running the recursive walk over serialised output as an assertion so a missed struct cannot leak. Do not add `deny_unknown_fields` to new shared response structs that older SDK/clients will parse.
- Duplicate of: no.

---

## server-rooms.md

### F-ROOMS-1: PLAUSIBLE  (severity: P2)
- Decisive invariant/evidence: `Supervisor::list` clones invitation, bans, details and games per call (ember-rooms/src/supervisor.rs:369-394); bridge side as in F-BRIDGE-1. Wire compatibility holds in both directions: old bridge + new supervisor is fine because `Reported` has no `deny_unknown_fields` (bridge routes/rooms/supervisor.rs:78-101); new bridge + old supervisor sees `rev` absent and must treat it as "always apply". The out-of-tree pool passes room dicts through verbatim (pool.py:192-214), so `rev` survives the merge, and its 180 s stale list re-serves the same `rev`, which correctly reads as unchanged. Room ids are unique across both supervisors, so a per-room-id cache is sound.
- Behavior risk the finding missed: a supervisor reports a room as soon as the host says `hosted`, which is before the bridge's create path stores `invitation_sealed` (create.rs:103-160). A poll in that window matches 0 rows (`WHERE ... invitation_sealed IS NOT NULL`, poll.rs:165). If the bridge caches `rev` there, the next poll skips the room and its host-reported name, capacity, fighters, host name, lock and `opened_at` never land until the room changes again. Adjustment: cache a room's `rev` only when its UPDATE actually matched a row (`changes() == 1`) and the transaction committed, and drop the entry on create/close.
- Ordering: F-BRIDGE-1 alone removes the fsync, the WAL churn and the needless `committed()` with no wire change and one restart; `rev` then only saves the seal/Details parse and the no-op `INSERT OR IGNORE`s for games and bans (CPU, no I/O). So `rev` is an additive follow-on, not the primary fix: P2. It needs three deploys in order (supervisor on VPS, supervisor on server1 via upgrade-server1-supervisor-*, bridge), any order being safe given the fallback. Minor correction: there are no "event waiters" to wake (the `events` Notify is dead).
- Duplicate of: F-BRIDGE-1 for the bridge write amplification (canonical F-BRIDGE-1; F-OVH-8 is the same). The supervisor `rev` is the only new part.

### F-ROOMS-2: PLAUSIBLE  (severity: keep P2)
- Decisive invariant/evidence: `Quiet()` requires `snapshot->members.empty()` (server/roomhost/RoomHost.cxx:241-248); main loop waits `Quiet() ? 250 : 16` ms (main.cxx:26, 125-131). PUBLIC_ROOM_HOSTING.md:154-157 says "250 ms at most while the room is empty and settled", which is accurate as written; the finding does not contradict the doc, it shows the doc's scope is narrow. The 5 ms sleeps (main.cxx:117,143,146) are bounded start/exit waits, confirmed.
- Player-visible delay, answered: chat, seat change, ready, kick are member messages that arrive as helper frames; `WaitForIncoming` returns at once while `incoming_` is non-empty (src/platform/HelperClient.hxx:55-58), and the reply/commit goes out in that same tick. Proposals and commits in flight keep 16 ms (`ProposalInFlight`, `CheckpointPending`, `HasRecoveryCandidate`, `PendingProposal` are in the fast set). So added delay for those is zero. Timer-originated transitions (`HasDueTimerTransition`, src/session/RoomModel.cxx:840-852: ready 90 s, spectator hold 10 s, permit hold, result dispute 30 s) fire up to 250 ms late with a plain 250 ms cap, and on time with `NextDueMs`. `IdleSeconds` (RoomModel.cxx:29-34) is computed at snapshot time from `nowMs_`, which every event-driven tick refreshes, so at most 250 ms stale against 1 s display granularity. Stdin EOF detection goes from at most 16 ms to at most 250 ms, inside the supervisor's 10 s allowance. `RoomRecoveryRuntime::Tick` takes no clock (src/session/RoomRecoveryRuntime.hxx:12), so it has no time-driven renewal of its own.
- Adjustment: (1) also keep 16 ms while any table is past Idle but not yet Playing (ready/starting handshakes), as the finding's own risk note suggests, and while any helper send is waiting on a full queue (ForwardBans already retries per tick; `pendingBans_` is in the fast set). (2) Saving (0.2 to 0.3 percent of a core per idle occupied room, about 10 percent across 45 rooms) is an assumption; measure with the soak monitor first. (3) Ships with a room-host binary per build, so it reaches production on release cadence, staged per build under ~/ember-rooms/builds and server1's lib/builds.
- Duplicate of: no.

### F-ROOMS-3: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: the gate bypass clauses `pendingBans_.empty() && !HasRecoveryCandidate() && !PendingProposal() && (closing_ || !snapshot->closed)` (RoomHost.cxx:213-215); `ForwardBans` and the model-close transition live inside `Report` (RoomHost.cxx:228-231); `struct Glyph { char32_t code; std::string bytes; }` (RoomHostStatus.cxx:11).
- Adjustment: severity lowered. The extra rebuilds happen only during in-flight commits (short windows) and the 1 s ban notice, so the CPU saving is small; the value is clarity. Keep the stdout lines and their order identical (a test diffing the emitted lines over a ban + close scenario).
- Duplicate of: no.

### F-ROOMS-4: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: `Envelope` keeps `details`/`games` as `RawValue` (protocol.rs:236-283), then `bounded` decodes to `Value`, walks depth and re-encodes to measure (protocol.rs:199-224).
- Adjustment: none beyond what is named (whitespace measured, duplicate-member test kept). The bridge parses `details`/`games` into `Value` (bridge supervisor.rs:94-100), so verbatim bytes versus re-encoded (key-sorted) bytes make no difference there. CPU saving is small at status-line rates; P3.
- Duplicate of: no.

### F-ROOMS-5: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: `Live` mirrors `RoomInfo` (supervisor.rs:37-58 vs 99-108); `apply` takes seven parameters, four `&mut` task state (supervisor.rs:643-651).
- Adjustment: none beyond the stated `Shutdown.started` caveat. Pure refactor.
- Duplicate of: no.

### F-ROOMS-6: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: rooms.js `render()` empties `#rooms` and rebuilds every card each load (rooms.js:200-202, 255-262), with `cache: 'no-store'` (rooms.js:244). rankings.js folds every name in `valid()` (rankings.js:659) before `keyOf` decides (rankings.js:709).
- Adjustment: the focus/selection loss on the rooms page is the real item (UX). The 500 NFKD folds per 30 s are negligible CPU; skip that half. The ETag/304 path would freeze `listed_at`-based ages unless ages are recomputed from the last good body plus `Date.now()`, as the finding notes; with nginx `proxy_cache` in front, any ETag must exclude `listed_at` or it never matches.
- Duplicate of: no (separate from the gzip claim; only its byte figures overlap F-BRIDGE-6).

### F-ROOMS-7: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: `VERSIONS` table (rooms.js:22-31), `STABLE_VERSION = '1.4.1'` (open.js:39), `copy()` three times (open.js:137, rankings.js:619, rooms.js:85), `el()` twice, visibility poll loop twice: confirmed.
- Wire break the finding missed: adding a label field to `RoomSummary` breaks every released client's room browser, because `RoomSummary` is `deny_unknown_fields` (protocol/rooms.rs:201) and the client parses `RoomList` of `RoomSummary` (sf4-net tournament/rooms.rs:130). `player_rooms` builds the same struct for the player listing and `public_list` (routes/rooms/mod.rs:473-495), so the field would reach the game. "Skipped when absent" does not help when it is present.
- Adjustment: put build labels on the web-only `PublicRoomList` (for example a `versions: {build_id: label}` map beside `rooms`), never on `RoomSummary` without a client release that tolerates it. The labels still need a per-release edit somewhere (supervisor or bridge config), so the saving is moving the edit, not deleting it.
- Duplicate of: no.

### F-ROOMS-8: CONFIRMED  (severity: keep P3)
- Decisive invariant/evidence: RoomHostStatus.hxx:13-17 says 64 KiB and about 36 KB; protocol.rs:89-91 says about 43 000 bytes and `MAX_LINE_BYTES = 128 * 1024`; the host fails at 60000.
- Adjustment: none. A shared crate dependency is possible (the ember-rooms Dockerfile already copies server/ember, Dockerfile:24); it has no runtime effect.
- Duplicate of: no.

### F-ROOMS-9: PLAUSIBLE  (severity: keep P3)
- Decisive invariant/evidence: gzip as ruled under F-BRIDGE-6 (not confirmable from the tree). Zone-comment staleness is confirmed: it says the rankings answer is "kept for a minute" (ember-short-zone.conf) while the location caches `200 404 15s` (locations.conf:123): stale comment confirmed.
- Adjustment: none beyond F-BRIDGE-6's.
- Duplicate of: F-BRIDGE-6 (canonical) for the gzip change; the zone-comment fix is unique to this finding.

### F-ROOMS-10: PLAUSIBLE  (severity: keep P3)
- Decisive invariant/evidence: `Bugsink::deliver` is used only by tests/delivery.rs (84, 88, 112, 211); `process` clones `report.minidump`, an `Option<Vec<u8>>` (intake.rs:96), so `bytes.clone()` (lib.rs:227) is a real copy of up to 4 MiB per crash report; ember-short has both the 30 s background sweep (main.rs:56) and the inline `next_sweep` check (lib.rs:364).
- Adjustment: savings are negligible (crash reports are rare; the inline sweep rarely fires because the background sweep advances `next_sweep`). Tidiness only; keep the sweep-before-refusing at the limit (lib.rs:386).
- Duplicate of: no.

---

## Batch verdict

Counts (26 findings): CONFIRMED 8 (F-BRIDGE-1, 2, 10, 11, 14; F-ROOMS-4, 5, 8), PLAUSIBLE 18 (F-BRIDGE-3, 4, 5, 6, 7, 8, 9, 12, 13, 15, 16; F-ROOMS-1, 2, 3, 6, 7, 9, 10), REJECTED 0 whole findings (the bridge-side caching half of F-BRIDGE-6 is rejected as negligible).
Severity changes: F-ROOMS-1 P1 -> P2 (duplicate core, follow-on only); F-BRIDGE-2, 3, 4, 6, 8, 10, 11, 12 -> P3; F-BRIDGE-9 stays P2 for the MatchState/DeliveryState rollback case, its Role/Rotation/Format part is P3; F-ROOMS-3 P2 -> P3.
Duplicates: F-BRIDGE-1 canonical over F-ROOMS-1 (bridge part) and F-OVH-8; F-BRIDGE-6 canonical over F-ROOMS-9 (gzip part); F-BRIDGE-5 overlaps F-BRIDGE-1 on the rooms-poll notification.

Do first:
1. F-BRIDGE-1: the only P1. Bridge-only, no wire or migration change, one restart, rollback-safe; removes the per-poll fsync and WAL churn while rooms are open and the 5 s wake of every SSE stream.
2. F-BRIDGE-3: small, self-contained; fixes `notify_one` waking one of four workers and the 4 idle lease transactions/s, provided `next_due` uses the exact lease predicates and a 1 s floor.
3. F-ROOMS-2, after a soak-monitor measurement: the largest CPU item across 45 rooms, zero added delay for player actions, but it ships with a room-host build. If a quick win is wanted instead, F-BRIDGE-6's nginx gzip lines are trivial and idempotent.
Also do quietly at any time: F-BRIDGE-9's Role fail-closed (no release-note mention). Must not ship as written: F-ROOMS-7's `RoomSummary` field (breaks released clients).

Could not verify by reading:
- The live nginx.conf and site file on the VPS (whether gzip already covers application/json).
- Real WAL bytes per poll, per-tick CPU of an idle occupied room host, and the reqwest 0.13 per-build root-store cost.
- Whether every writer that makes delivery work due calls `committed()` (needed before F-BRIDGE-3's 60 s cap or F-BRIDGE-5's conditional notify), and whether any ops script writes the bridge SQLite file directly.
- Time-driven behavior in `SessionServer::Step` and match-authority phases outside `HasDueTimerTransition` (including a send that fails on a full helper queue and is retried per tick); must be audited before an F-ROOMS-2 room-host build ships.
