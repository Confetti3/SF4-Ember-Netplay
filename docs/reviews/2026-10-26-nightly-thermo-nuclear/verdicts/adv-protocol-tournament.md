# Adversary verdicts: server-protocol.md (F-PROTO-1..15) and rust-tournament.md (F-TOUR-1..14)

Checked at sf4-nightly b9ce0e31, read-only. Nothing built or run. Paths are repo-relative.

## Wire rule that decides several findings (verified)

Answer types really are `deny_unknown_fields`, and the helper parses them through those strict
protocol types: `ClaimAnswer` (protocol play.rs:111), `PrepareAnswer` (play.rs:347), `RoomSummary`
(rooms.rs:202), `RoomList` (rooms.rs:303), `ConnectionRoom`/`ConnectionRoomList` (rooms.rs:470/481),
`LookupAnswer`/`FoundPlayer` (partner.rs:42/52), `Standing`/`Board` (skill.rs:128/166), `Challenge`
(challenge.rs:101), `SigningKeys` (api.rs:132), and the event envelope `Event` (event.rs:170). Helper
readers: rust/sf4-net/src/tournament/play.rs:327, 391, 446 (`ClaimAnswer`, `PrepareAnswer`),
rooms.rs:130, 195, 215 (`RoomList`, `RoomSummary`), ranking.rs:9 (`Standing`, `Board`). Notifier
readers: notifier/src/bridge.rs:76 (`LookupAnswer`), :87/:102 (`ConnectionRoom`), format.rs:298
(room event data decoded into a strict `ConnectionRoom`). `ErrorCode` (api.rs:9) has no `#[serde(other)]`,
so an error code unknown to an older helper makes the whole `ApiError` fail to parse and the helper
reports `http_<status>` (sf4-net client.rs:350-362).

So the usual assumption flips for answers. **Adding a field to an answer breaks every reader compiled
before the change. Removing `deny_unknown_fields` from a reader is safe, but only for readers built
after the change.** Any proposal that puts a new member into an answer has to be gated by a signal
from the request side (the client asking for it), not by a capability the bridge lists. I checked
every proposal in the batch against this rule; the per-finding blocks give the result.

---

### F-PROTO-1: CONFIRMED  (severity: keep P2)
- Decisive invariant/evidence: notifier/src/lib.rs:111-117 creates `inbox` with only `id TEXT PRIMARY KEY`.
  There is no migrations folder: the schema is created inline with `CREATE TABLE IF NOT EXISTS`. The
  due query at inbox.rs:105-107 filters on `discord_done/twitch_done/next_at` and orders by
  `received_at`, so every pass is a full scan plus a sort. inbox.rs:121-123 waits `timeout(1 s,
  wake.notified())` and loops. There is no `DELETE` anywhere in notifier/src, so "never pruned" holds for
  both `inbox` and `matches`. The insert transaction with `synchronous = FULL` runs on the async thread
  (`receive` calls `accept` directly at inbox.rs:241). The scan cost is in bytes, not just rows: `body`
  (column 3) sits ahead of the filtered columns and fits in the row's page, so a full scan reads every
  stored body every second. The reviewer understated this.
- Adjustment: (1) the index is fine: `CREATE INDEX IF NOT EXISTS` in `Notifier::new` is safe on existing
  databases. SQLite can use a partial index whose `WHERE` matches the query's OR term exactly; an index
  on `received_at` with the same `WHERE` would also cover the ORDER BY. (2) The deadline wait needs a
  ceiling. `Clock` is wall time plus an offset that tests can advance (lib.rs:49-62). A sleep until
  `MIN(next_at)` would not notice `advance()` or a wall-clock step, so keep `min(deadline, 60 s)`, not
  an unbounded wait. (3) The `spawn_blocking` change is fine. (4) Pruning: keep `matches` rows
  (labels are looked up by later events). Prune only inbox rows that are done and older than both
  `GIVE_UP_SECS` and the webhook tolerance (webhook.rs:17, 300 s), so a late duplicate is still refused
  by its timestamp and not re-announced.
- Duplicate of: no

### F-PROTO-2: PLAUSIBLE  (severity: keep P3)
- Decisive invariant/evidence: brackets/src/bracket.rs:38 (`WAKE_SECS = 60`) and :186-190 (fixed
  timeout); config.rs:23 `MIN_POLL_SECS = 30`; README.md:28 and :269 promise "at most `poll_secs`
  later". `Store::pending` (store.rs:335-343) has no index on `(bracket, done)`.
- Adjustment: keep `WAKE_SECS` as a ceiling (`min(earliest deadline, 60 s)`) and do not delete it.
  The deadlines are clock seconds from an advanceable/frozen `Clock` (brackets lib.rs:142), the same
  hazard as F-PROTO-1. The budget risk is named correctly: say in the README that a `poll_secs`
  below 60 now spends more of the 500-a-month Challonge budget.
- Duplicate of: no

### F-PROTO-3: PLAUSIBLE  (severity: P3, was P2)
- Decisive invariant/evidence: protocol/src/tournament.rs is 1,099 lines. Only the bridge imports
  `ember_protocol::tournament` (bridge tournament/mod.rs, standings.rs, routes/tournaments/view.rs);
  brackets, notifier and sf4-net do not. `plan` runs on every load (bridge tournament/state.rs:129,
  lifecycle.rs:247), and labels are built as `String`s (tournament.rs:328, 352, 364, 392).
- Adjustment: the move preserves behavior. Making `Node::label` computed saves microseconds per load;
  do it only as part of the move, not as an overhead item. Severity drops because nothing is gained at
  runtime: the change is file size plus helper compile time.
- Duplicate of: no

### F-PROTO-4: CONFIRMED  (severity: P3, was P2)
- Decisive invariant/evidence: `Permit::covers` (play.rs:461-475) and `GameReport::agrees_with`
  (report.rs:150-165) compare the same 13 coordinates; `Permit::report` (play.rs:424-458) copies them.
  `version` and the observation/reporter fields are correctly left out of both comparisons, and the key
  view has to keep it that way.
- Adjustment: none. Severity drops because this is maintainability, not overhead.
- Duplicate of: no

### F-PROTO-5: PLAUSIBLE  (severity: keep P2)
- Decisive invariant/evidence: `Event::data` is `serde_json::Map` (event.rs:182). The bridge stores
  `json::canonical(&envelope)` (bridge events.rs:57), which is JCS, so the order in which a struct declares
  its fields never reaches the wire, and "byte-for-byte the same" holds provided the keys match.
  format.rs:232-234 re-spells `Rotation`, and `render_room` (format.rs:295-298) clones the map and decodes a
  strict `ConnectionRoom`, so any field the bridge adds to room event data silently stops room
  announcements (the event is marked done as "not announced", inbox.rs:144-147).
- Adjustment: (a) the typed structs have to emit `null` wherever the `json!` literal emits `null` today:
  no `skip_serializing_if` on a field that is currently present as `null`. Partners and third-party
  webhooks read these bodies, so add a fixture test per kind as proposed. (b) `RoomEventData { #[serde(flatten)]
  room: ConnectionRoom, .. }` only works once `ConnectionRoom` is tolerant, because serde does not support
  flatten together with `deny_unknown_fields` (docs/reviews/2026-09-code-quality-review.md section 3). So
  this depends on F-PROTO-6. (c) Overlap with F-TOUR-2/3 is weaker than the brief suggests: this finding
  is event data read by the notifier, not the helper's bridge call path.
- Duplicate of: no (depends on F-PROTO-6; the bridge-writer half is F-BRIDGE-16)

### F-PROTO-6: CONFIRMED  (severity: keep P2; canonical for answer strictness)
- Decisive invariant/evidence: see the wire rule above. partner.rs:36-39 still says readers are strict
  "as ember-brackets reads it", but brackets now keeps tolerant mirrors (brackets/src/bridge.rs:160,
  sets.rs:134). The notifier is the exposed service (notifier/src/bridge.rs:76, :87, :102; format.rs:298).
  Count correction: 57 occurrences across protocol/src, comments and tests included, not 61.
- Ruling on direction: correct, and forward-only exactly as the finding says. Helpers already in the
  wild keep refusing unknown answer fields, so the bridge must go on gating new answer members by a
  request-side signal until those builds age out. The notifier and brackets ship with the bridge, so for
  them the change takes effect on the next deploy and removes a latent outage.
- Adjustment: (1) add `Event` (event.rs:170) to group (c). The webhook HMAC covers the raw body
  (webhook.rs `verify` runs before the parse at inbox.rs:33-36) and nothing re-serializes the envelope,
  so tolerating unknown envelope members is safe. (2) Keep `Challenge` and `Proof` in group (b), because
  the helper signs the challenge. (3) Optionally put `SigningKeys`/`SigningKey` in (c), since JWK readers
  are expected to ignore unknown members, but that is a security-posture call: flag it, do not do it
  silently. (4) The `RoomList` test at rooms.rs:967 flips to asserting the extra field is ignored; the
  tests at 960, 965 and 973 stay strict.
- Duplicate of: no (canonical; F-PROTO-5 and F-TOUR-3 depend on it)

### F-PROTO-7: REJECTED  (severity: P3)
- Decisive invariant/evidence: json.rs:47-53 and :59-63 do build the extra trees. But the reviewer's own
  estimate is single-digit microseconds per claim-sized body, at about 13 requests/s bridge-wide. The
  proposal adds roughly 240 lines of hand-written `Deserializer` and `Serializer` on the strict path that
  produces signed bytes (`sign`, `verify`, `digest`, `canonical`), where a subtle difference (u64 above
  2^53, how enum or Option content is buffered, `deny_unknown_fields` through a custom Deserializer)
  changes what gets signed or refused. That risk is too high for the saving.
- Adjustment: keep only the small part: make `MatchState::parse` and `DeliveryState::parse`
  (matches.rs:212, 230) table lookups. That belongs with F-PROTO-11.
- Duplicate of: no (canonical; F-TOUR-3(e) duplicates it)

### F-PROTO-8: REJECTED for its central claim; PLAUSIBLE for the dedup  (severity: P3, was P2)
- Decisive invariant/evidence: the reproduction is false. The bridge never compares `required_build_id`
  with any build: grep finds it only in storage, views and echoes (bridge routes/matches.rs:185/297,
  lobbies.rs:567, tournaments/view.rs:80). docs/design/identity-bridge/STATUS.md:197 says it is "the
  provider's label" and is not compared. The only build check is claim against claim
  (routes/play/mod.rs:218-233, `incompatible_build`), and the binding takes `build_id` from the claims
  (routes/play/room.rs:202), which pass `check_build`. A non-ASCII label therefore leaves the match
  playable. Tightening it would be a wire break for partners: the published schema `BuildId`
  (schemas/core.schema.json:87-91) is any string of 1 to 128 characters with no ASCII pattern. Also,
  BluMint's create path inserts `required_build_id: ""` directly (routes/blumint/create.rs:264), so a
  shared validator placed at the insert layer would break BluMint match creation.
- Adjustment: drop "use `check_build` for `required_build_id`" and drop the brackets config change.
  Keep the identical-rule dedup: `is_hex` in `GameReport::check` (report.rs:87-93), `sites::is_site_id`
  for brackets' `is_bridge_site` (config.rs:362-367), `event::parse_rfc3339` for `parse_utc`, and the
  shared provider-command header check, keeping the current length-only rule for `required_build_id`.
  Tightening `GameReport::build_id` to ASCII is behavior-neutral in practice (a report has to be covered
  by a permit whose build passed `check_build`, bridge reports.rs:122), but the refusal moves from "no
  covering permit" to `invalid_request`. That code is in the helper's `settled` list (sf4-net play.rs:563-574),
  so the spooled report is dropped either way. Leave `table_id <= 3` alone: it is dead permissiveness
  today, and changing it would pre-empt any later multi-table permit.
- Duplicate of: no

### F-PROTO-9: CONFIRMED  (severity: keep P3)
- Decisive invariant/evidence: play.rs:254-260 and :281-292 and rooms.rs:152-159 and :185-197 are the same
  check, clone, sign and kid, check, verify sequences, one domain each.
- Adjustment: none; keep the order kid, then check, then signature.
- Duplicate of: no

### F-PROTO-10: CONFIRMED  (severity: keep P3)
- Decisive invariant/evidence: `Claim.ready`/`watch` are `Option<bool>` with
  `skip_serializing_if = "Option::is_none"` (play.rs:91-100). The helper sends only `.then_some(true)`
  (sf4-net tournament/play.rs:359-360). The bridge reads `== Some(true)` (routes/play/mod.rs:240). The proof
  digest is computed over the received command `Value`, not a re-serialized struct (bridge
  routes/sessions.rs:353-357), so normalizing the type does not affect proofs.
- Adjustment: one edge case. `bool` with `default` refuses an explicit `"ready": null`, which
  `Option<bool>` accepts. No known sender does this and the schema does not list these fields, but note
  it. `PublishRoom` (play.rs:136-143) has no `skip_serializing_if`, so older helpers send explicit
  `null`s; the `try_from` wire struct must keep accepting them.
- Duplicate of: no

### F-PROTO-11: PLAUSIBLE  (severity: keep P3)
- Decisive invariant/evidence: `Kind::from_full` formats `full()` for each candidate (event.rs:155-157).
  brackets bridge.rs:46 goes through `serde_json::to_value(code)`. `ErrorCode` (api.rs:7-9) has no
  `as_str`.
- Adjustment: the macro must keep `ErrorCode` strict: an unknown code must still fail to deserialize,
  because the helper's `http_<status>` fallback and its `settled` decision depend on that. Adding a
  `#[serde(other)]` would change what older helpers see as settled. That is a separate decision, not
  part of this refactor.
- Duplicate of: no (F-TOUR-4's serialize-to-read-a-string is the helper side of the same `as_str` gap)

### F-PROTO-12: CONFIRMED  (severity: keep P3)
- Decisive invariant/evidence: skill.rs:9 imports `rooms::{is_false, is_player_name}`; rooms.rs is 977
  lines.
- Adjustment: none (`pub use` keeps the paths).
- Duplicate of: no

### F-PROTO-13: PLAUSIBLE  (severity: keep P3)
- Decisive invariant/evidence: bridge routes/play/room.rs:195-224 rebuilds and re-signs the binding
  with `issued_at: ctx.now` (:216) on every claim. Nothing reads a binding's `issued_at`: sf4-net uses
  only the permit's (tournament/play.rs:460), and the native side reads it only in `DecodePrepareReply`
  (TournamentAnswers.cxx:283, also the permit). So making it stable per revision is safe. Watcher
  changes and room replacement bump `binding_revision` (room.rs:150-178; routes/play/mod.rs:386-390
  clears `bound_endpoints`, and room replacement also moves the assignment generation).
- Adjustment: (1) `binding_current` in `ClaimAnswer::Room` must have `skip_serializing_if` and must be
  sent only when the claim carried `bound_revision`. `ClaimAnswer` is strict in every helper in the wild
  (sf4-net play.rs:327, 391), so a capability listed by the bridge is not enough. (2) Make the "current"
  test compare `room_id` as well as `(assignment_generation, binding_revision)`, or have the helper send
  a digest of the binding it holds; the binding embeds `room_id`. (3) Part 2 (stable `issued_at`) is what
  makes F-TOUR-5's helper-side cache possible; do it first.
- Duplicate of: partial duplicate of F-TOUR-5 (canonical for the renewal loop). This finding is its
  answer-side half; design both under one feature flag.

### F-PROTO-14: CONFIRMED  (severity: keep P3)
- Decisive invariant/evidence: brackets http.rs:72/83 and notifier sent.rs:57/67 (`http_client`,
  `bounded_body`); `Clock` is defined twice (brackets lib.rs:142, notifier lib.rs:50).
- Adjustment: the shared `Clock` keeps brackets' `frozen` mode. The `client` feature stays off by default
  in ember-protocol so sf4-net does not pull it in.
- Duplicate of: no (parallel to F-TOUR-2 but in other crates; not the same code)

### F-PROTO-15: CONFIRMED  (severity: keep P3)
- Decisive invariant/evidence: eventsub.rs:206-210 clones `payload["event"]`, decodes `ChatEvent` and
  spawns a task before the broadcaster and command checks; service.rs:372 uses `from_value(value.clone())`.
- Adjustment: leave the keepalive at Twitch's default. A 30 s keepalive changes when a dead socket is
  noticed, and twitch_room.rs tests exercise the timeout path. Keep the `seen.first_time` dedupe ahead of
  everything.
- Duplicate of: no

---

### F-TOUR-1: CONFIRMED  (severity: keep P2)
- Decisive invariant/evidence: the cited lines are stale; the code is now at tournament/mod.rs:404-500.
  The loop (:473-482) reads one job, routes identity jobs to the serial queue, and otherwise awaits
  `shared.http.clone().acquire_owned()` before reading the next job. The spool resend holds a permit for
  its whole pass (:461-466). The comment at about :437-438 promises the opposite.
- Ordering question: no caller relies on cross-lane order. The identity and bridge lanes already
  interleave: once an identity job is routed it runs on `spawn_blocking` alongside the spawned bridge
  jobs, so today's order is only "an identity op does not start until every earlier bridge job holds a
  permit". The native side matches answers by `request_id` (TournamentAnswers.cxx:206;
  IdentityPanel.cxx:184), and `ApplyTournamentEvent` (IdentityEvents.cxx:28-50) overwrites `op/ok/reason`
  from whatever arrives last, so it never assumed an order. Within each lane order is kept: the identity
  queue is FIFO and serial, and the bridge lane starts jobs FIFO because tokio's `Semaphore` is fair.
  For local ops, `MatchLeave` (`play::forget`, play.rs:623-628) could now run before a claim that was
  sent earlier but is still waiting for a permit, and that claim would then re-insert a binding. The
  same race already exists today for a claim in flight (claims finish after a quick `forget`), so the
  change widens a harmless window and does not create a new one.
- Adjustment: the existing hazard is that `prove` does `identity.try_lock()` and fails with
  `identity_busy` (client.rs:397-400, 427-430) while a long identity op (an export or unlock KDF) holds
  the lock. Routing identity ops earlier does not change how likely that is, but say so in the change.
- Duplicate of: no (F-TOUR-11's `lane()` is how it is implemented)

### F-TOUR-2: CONFIRMED  (severity: P3, was P2)
- Decisive invariant/evidence: client.rs:647 and :655 pass the verb twice (`Method::Delete` and
  `reqwest::Method::DELETE`), while sign_in.rs:123 derives one from the other. `with_session` takes
  `F: Fn(Approved, Zeroizing<String>)` (client.rs:524-526), which forces the clones the finding lists.
- Adjustment: the borrowed `FnMut` closure has to produce a future that borrows its inputs, which needs
  an async-closure shape or a boxed future; budget for that. Keep the `rooms_unavailable`/
  `ranking_unavailable` 404 rule and `watch::reply`'s lack of one. Severity drops because nothing is
  gained at runtime.
- Duplicate of: no

### F-TOUR-3: PLAUSIBLE  (severity: P3, was P2)
- Decisive invariant/evidence: `answer` (client.rs:318-325) builds the strict tree, then the serde tree,
  then a rebuilt `serde_json::Value`, and `rooms::create` round-trips its own `RoomSummary`
  (rooms.rs:195-215). Native readers use `find`/`value`, so member order does not matter. The pipe is a
  byte stream.
- Adjustment: (1) `Event::Tournament.data: Option<Box<RawValue>>` will not survive the derived
  `Deserialize` of the internally tagged `Event` (service/protocol.rs:162-164, `tag = "type"`), because serde
  buffers internally tagged content and `RawValue` cannot be read from that buffer. Tests deserialize
  `Event` (tests/helper_network.rs:118, tests/tournament_support/mod.rs:356). Write a custom Serialize and
  Deserialize as `NativeControlMessage` already does (protocol.rs:558-600), or keep `Value` for the
  deserialize side. (2) Part (a) applies only to callers that are already typed. Do not move a
  pass-through answer (`link_list`, `assignment_list`) onto a strict protocol type; by the F-PROTO-6 rule
  that would make the helper refuse fields it forwards today. (3) Part (e) is F-PROTO-7, which is
  rejected.
- Duplicate of: part (e) duplicates F-PROTO-7 (canonical)

### F-TOUR-4: PLAUSIBLE  (severity: P3, was P2)
- Decisive invariant/evidence: string comparisons at client.rs:161, :532, rooms.rs:58 and sites.rs:96, and
  `settled` (play.rs:563-574). `refusal` serializes `ErrorCode` to read its string (client.rs:356-361).
- Adjustment: `settled` mixes local and bridge codes. `bridge_not_approved` is local (from
  `client::approved`), and `invalid_request` comes from both the bridge and local checks. With
  `enum Failure { Local(Code), Bridge(String) }`, `settled` has to match both forms, or a report that is
  settled today would be resent forever. An unknown bridge code has to keep producing `http_<status>`
  (ApiError parse failure) and stay unsettled.
- Duplicate of: no (its `ErrorCode` round trip is the helper side of F-PROTO-11)

### F-TOUR-5: PLAUSIBLE  (severity: P3, was P2; canonical for the claim-renewal loop)
- Decisive invariant/evidence: `prove` (client.rs:386-453) is a challenge POST followed by the command
  POST. The helper-only part, "skip `verify` when the incoming `SignedBinding` is byte-identical", never
  hits today: the bridge stamps `issued_at: ctx.now` on every claim (bridge routes/play/room.rs:216), so
  each 10 s answer is a new message with a new signature.
- Adjustment: the binding cache depends on F-PROTO-13 part 2 (stable `issued_at`) and should be ordered
  after it. Keep byte equality; do not skip verification on matching unsigned fields. One Ed25519 verify
  every 10 s is about 50 µs, so this is cleanup, not overhead. The feature-gated session-only renewal adds
  a route and changes no answer shape, so it is compatible. Keep the security-review caveat: renewal
  must not change `endpoint_id`, `helper_instance_id` or build, and the first claim stays proven.
- Duplicate of: no (canonical; F-PROTO-13 is its answer-side half)

### F-TOUR-6: CONFIRMED  (severity: P3, was P2)
- Decisive invariant/evidence: `write_frame` (wire.rs:77-96) does three header writes, `write_all`, then
  `flush`, on an unbuffered stream. The pipe is created byte mode (ipc.rs:217-222, `ServerOptions`
  default). The native reader (src/platform/HelperClient.cxx:96-113, `ReadFrame`) loops `Transfer` until it
  has `HeaderSize` and then `length - 10` bytes, so frame boundaries are invisible to it and a coalesced
  frame reads identically. Observable latency: the native worker peeks every 2 ms (HelperClient.cxx:171-181)
  and, once it sees bytes, blocks in `ReadFrame` until the frame is complete. A split write keeps it
  blocked for a few extra completion round trips (microseconds), during which its outgoing queue is not
  serviced. Coalescing only shortens that. None of it is on the rollback path: GGPO datagrams go over
  loopback UDP (sf4-net bridge.rs:13, :59), not this pipe.
- Adjustment: none. Not in ROOM_PERF_BASELINE's nine landed changes.
- Duplicate of: no

### F-TOUR-7: CONFIRMED  (severity: P3, was P2)
- Decisive invariant/evidence: stdio.rs:110-152 (duplex, `forward`, synthetic `{"type":"shutdown"}` at
  `last_id + 1`, then `pending()`); service/entry.rs:31-58 (the reader breaks on any read error or a
  non-increasing ID, then `failed_ipc.send(true)`). The Linux room host depends only on two things:
  closing the child's stdin stops it, and it must exit within `graceMs` or it is killed
  (server/roomhost/RoomHostHelperPosix.cxx:3, :35-43). It also relies on the unchanged stdout framing.
  It never sees the duplex.
- Adjustment: only `UnexpectedEof` maps to Shutdown. `InvalidData` (a malformed frame) and the
  `message_id <= last_id` break must still take the failed-IPC path, so the helper exits 1 as today. On
  the Shutdown branch the reader must submit `Command::Shutdown` with ID `last_id + 1` (the ID rule moves;
  it is not deleted) and then park, not fall through to `failed_ipc.send(true)`. The Windows path keeps
  `OnEof::LostIpc`. The saving is one 164 KB memcpy plus one validation per room-host command, which is
  microseconds, hence P3.
- Duplicate of: no

### F-TOUR-8: PLAUSIBLE  (severity: P3, was P2)
- Decisive invariant/evidence: `deliver_pending` (play.rs:611-620) tries each report in turn;
  `Spool::put` calls `pending()` (spool.rs:77), which reads and parses every file; the resend task holds a
  permit for the whole pass (tournament/mod.rs:461-466).
- Adjustment: (b) counting by `PREFIX` also counts unreadable files, which `pending()` skips and leaves for
  inspection (spool.rs:99-114). Corrupt files would then use up `MAX_PENDING` and block saves. Either
  accept that and say so, or count only files that open. (a) Per-bridge stop on `bridge_unreachable`
  (client.rs:58) keeps the oldest-first order. With few pending reports per player the saving matters
  only during outages, hence P3.
- Duplicate of: no (the permit-hold part is in F-TOUR-1)

### F-TOUR-9: CONFIRMED  (severity: keep P3)
- Decisive invariant/evidence: `respond` attaches `identity.status()` through `try_lock` to every answer
  (tournament/mod.rs:515-545); the native side updates identity only when the member is present
  (IdentityEvents.cxx:35-44).
- Adjustment: record "last sent" only when a status was actually sent (`try_lock` can omit it). Do
  not make `respond` block on the std `Mutex`: that goes against the design stated at
  tournament/mod.rs:522-523. Instead, the serial task's `spawn_blocking` job computes `identity.status()`
  while it already holds the lock and returns it with the outcome, so every identity answer carries it. Most
  of the saving is pipe bytes; the status is still built each time to detect a change.
- Duplicate of: no

### F-TOUR-10: PLAUSIBLE  (severity: keep P3)
- Decisive invariant/evidence: spool.rs:84-90 and bridges.rs:457-477 use `fs::rename`; identity
  `write_replacing` (identity/mod.rs:793-812) uses `platform::replace` (identity/platform.rs:171/186).
- Adjustment: the spool's put must stay no-overwrite ("rewriting the same observation is a no-op",
  spool.rs:74-76), so the shared helper needs a no-replace mode or the spool keeps its `exists()` check.
  Temp names must not start with the spool `PREFIX` (`pending()` filters on it).
- Duplicate of: no

### F-TOUR-11: CONFIRMED  (severity: keep P3)
- Decisive invariant/evidence: `is_identity` is `self.op().starts_with("identity_")` (mod.rs:269-271).
  `end_unopened` is at sign_in.rs:59/76. The warning about `untagged` buffering the passphrase is right.
- Adjustment: none.
- Duplicate of: no (it is how F-TOUR-1 gets implemented)

### F-TOUR-12: CONFIRMED  (severity: keep P3)
- Decisive invariant/evidence: `PublicHost { policy }` (public_room.rs:207-209), four `connect_public*`
  (:391, :400, :411, :423), five `now()`.
- Adjustment: the five `now()` disagree on type (`u64` vs `io::Result<u64>`), so a single `unix_now`
  must keep each caller's error behavior.
- Duplicate of: no

### F-TOUR-13: CONFIRMED  (severity: keep P3)
- Decisive invariant/evidence: `Store::get` builds `list()` and then searches it (bridges.rs:190-195).
- Adjustment: none; the saving is negligible, so do it only with F-TOUR-2.
- Duplicate of: no

### F-TOUR-14: REJECTED  (severity: P3)
- Decisive invariant/evidence: the answer body is not a function of stored state alone. It carries
  `event_cursor: events::head(tx)` (bridge routes/matches.rs:481), the head of the whole bridge's event
  log, which moves with any event. It also filters by `now - EXPIRED_LISTED_SECS` (:428), so a match drops
  off the list with no write at all. A per-player revision ETag would serve stale lists, which is a
  behavior change. A content-hash ETag is correct, but the bridge still builds the whole list to hash it,
  and the global cursor makes 304s rare on a busy bridge. What remains is 10 to 50 B/s per player.
- Adjustment: if pursued at all, use a content-hash ETag only, and drop the claimed bridge CPU saving.
- Duplicate of: no

---

## Batch verdict
- Counts (29): 16 CONFIRMED, 10 PLAUSIBLE, 2 REJECTED, 1 split.
  - CONFIRMED (16): F-PROTO-1, 4, 6, 9, 10, 12, 14, 15; F-TOUR-1, 2, 6, 7, 9, 11, 12, 13.
  - PLAUSIBLE (10): F-PROTO-2, 3, 5, 11, 13; F-TOUR-3, 4, 5, 8, 10.
  - REJECTED (2): F-PROTO-7, F-TOUR-14.
  - Split (1): F-PROTO-8. Its central claim (unplayable match) is REJECTED; its dedup part is PLAUSIBLE.
- Severity corrections: down to P3: F-PROTO-3, 4, 8; F-TOUR-2, 3, 4, 5, 6, 7, 8. Nothing goes up.
- Duplicates: F-TOUR-3(e) duplicates F-PROTO-7 (canonical). F-PROTO-13 is the answer-side half of F-TOUR-5
  (canonical for the claim-renewal loop). F-TOUR-11 implements F-TOUR-1. F-TOUR-4 and F-PROTO-11 are two
  sides of one gap (no `ErrorCode::as_str`). F-PROTO-14 is parallel to F-TOUR-2, not a duplicate.
- Do first:
  1. F-PROTO-1: a real growth problem on a live service, and items (1) to (3) are behavior-neutral (cap the
     wait).
  2. F-PROTO-6: removes a silent notifier outage (strict `ConnectionRoom` in `render_room` and the bot
     commands) and stops the answer-field gating rule from applying to builds made after it. F-PROTO-5
     and F-TOUR-3 depend on it.
  3. F-TOUR-1: the head-of-line wait up to 45 s for identity and local answers during bridge trouble is
     user-visible, and no ordering anyone relies on changes.
- Could not verify by reading: the claim that mio allows one outstanding overlapped named-pipe write
  (external crate; F-TOUR-6's IOCP round-trip count rests on it); that SQLite uses the proposed partial
  index (needs `EXPLAIN QUERY PLAN`); the real inbox row counts on the deployed notifier; all byte and
  microsecond estimates (nothing was run).
