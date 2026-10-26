# Findings: server rooms area (ember-rooms, roomhost, ember-reports, ember-short)

Reviewed at nightly head b9ce0e31. Read: ember-rooms/src (supervisor.rs, api.rs, protocol.rs, config.rs,
lib.rs, main.rs, bin/fake_room_host.rs), roomhost (main.cxx, RoomHost.*, RoomHostStatus.*,
RoomHostHelper*.cxx, soak/ headers), ember-reports/src (lib.rs, main.rs, bugsink.rs, limits.rs, store.rs
in part, worker.rs in part, intake.rs in part), ember-short (src/lib.rs, src/main.rs, rooms.js,
rankings.js, home.js, open.js in part, nginx/*.conf). To size the supervisor boundary I also read the
bridge's consumer of GET /rooms (server/ember/bridge/src/routes/rooms/poll.rs, supervisor.rs) and the
two public JSON routes; those files belong to the bridge reviewer and are cited only where the fix
spans both sides.

Facts the owner asked to have checked, before the findings:

- Room host status line (RoomHostStatus.cxx StatusLine, RoomHost.cxx Report): it is send-on-change, not
  periodic. Report returns early unless the snapshot revision, the applied commit revision or the
  invitation moved (or a ban/commit is in flight), and then emits only when the built Status differs
  from the last one sent (`current == reported_`). The fields it covers (member count, tables playing,
  invitation, bans, name/host/capacity/lock/main fighters/rules, won games) do not change on
  Ready/Unready, so a busy room emits a few lines per minute (joins, leaves, a table starting or
  ending, a won game, a lock, an invitation renewal). Size: about 0.5 to 1.5 KB for a fresh room
  (dominated by the invitation), about 6.5 KB once a room has 32 won games, worst case about 43 KB
  (512 bans). It does repeat unchanged data on every emission: the invitation, the full ban list and
  all 32 recent games. The games repetition is deliberate (docs/design/PUBLIC_ROOMS.md: the bridge
  counts each game once and the supervisor keeps only the latest status's games), so it is not
  proposed for removal; at a few lines per minute the stdout hop costs well under 1 KB/s per room.
- main.cxx 5 ms sleeps: they exist only in the helper connect wait (at most 15 s at start) and the
  close and leave phases (at most about 6 s at exit). The steady-state loop waits on
  HelperClient::WaitForIncoming with 16 ms or 250 ms. So the docs' "waits for work" is true, but only
  for an empty room: see F-ROOMS-2.
- ember-reports' 60 s interval: two directory listings (dumps, outbox) and a replay batch of at most 16;
  the accept path wakes it at once through `wake`. Gate::start's queue wait is per-request admission,
  not a poll. No overhead finding there.
- Site JS: rankings.js polls /rankings.json every 30 s, rooms.js polls /rooms.json every 15 s, both
  only while the tab is visible and both with `cache: 'no-store'`. home.js fetches once. open.js makes
  no requests. See F-ROOMS-6.

### F-ROOMS-1: GET /rooms is a full snapshot every 5 s and the bridge rewrites every room from it
- File: server/ember-rooms/src/supervisor.rs:369
- File: server/ember-rooms/src/supervisor.rs:445
- File: server/ember-rooms/src/api.rs:267
- File: server/ember/bridge/src/routes/rooms/poll.rs:28
- File: server/ember/bridge/src/routes/rooms/poll.rs:121
- Category: overhead
- Severity: P1
- Claim: `Supervisor::list` clones every live room's invitation, full `banned` Vec, `details` Value and
  `games` Value under the mutex and serializes them on each call; the bridge calls it every 5 s
  (POLL_SECS) and has no way to tell an unchanged room from a changed one. For every reported room,
  every poll, the bridge then runs `load` twice, re-seals the invitation (`keys.seal`), runs the big
  UPDATE, runs `INSERT OR IGNORE` for each of up to 32 repeated games (`skill::record_games`) and each
  of up to 512 bans, commits one write transaction and calls `state.committed()`, which wakes every
  event-stream subscriber, the delivery worker and the event waiters even when nothing changed. Room
  state actually changes a few times per minute (see the status-line note above), so almost all of
  this is repeated work. Payload: JSON array of RoomInfo, per room about 200 B of fields plus the
  invitation (about 0.3 to 1 KB, at most 4 KB) plus details (about 0.2 KB) plus games (up to about
  5.2 KB for 32 games) plus bans (usually 0, at most 30 KB). At 45 rooms with half of them past 32
  games: about 45 x 0.9 KB + 22 x 5.2 KB = about 155 KB per poll, about 31 KB/s, over loopback HTTP,
  fan-out 1 bridge x N rooms.
- Proposed change: give each room a change counter. In `Supervisor::set_live`, compare the incoming
  `Live` with the stored one and bump a `rev: u64` on `Room` only when any reported field differs
  (members, tables_playing, invitation, banned, details, games, opened); add `rev` to `RoomInfo`
  (seeded per supervisor process, for example from `seq` and a start-time nonce, so a restarted
  supervisor never repeats a value). In the bridge, keep a `HashMap<room_id, rev>` beside
  `state.rooms.poll`; in `apply`, a room whose `rev` equals the cached one only joins `alive` and skips
  the seal, UPDATE, games and bans; update the cache only after the write transaction commits; call
  `state.committed()` only when `apply` changed a row. The close-reconciliation loop, the
  PENDING_SECS sweep and the retention deletes keep running every poll (they are time-driven; see
  poll.rs:229-254). A whole-list ETag with 304 is the simpler cut, but it would still need the bridge
  to run the time-driven tail, so the per-room counter is the cleaner seam.
- Behavior preserved: which rooms are alive or closed and when; every field the bridge stores; games
  rated once each (INSERT OR IGNORE already makes repeats no-ops); bans recorded; room events
  announced on the same transitions.
- Behavior at risk: wire compat is additive both ways: the bridge's `Reported` has no
  `deny_unknown_fields`, so an old bridge ignores `rev`, and a new bridge treats a missing `rev` as
  "always apply". A failed bridge write must not advance the cache, or a change would be skipped
  until the room changes again. Events from `committed()` would no longer fire on unchanged polls;
  event streams already wake on their own 5 s timeout (routes/events.rs:155), so nothing that waits on
  them stalls.
- Estimated saving: at 45 rooms about 900 SQLite statements, 45 seals and one write commit per 5 s
  (about 15 M statements and 17 000 write transactions a day) fall to near zero in steady state; the
  15 to 30 KB/s of loopback JSON stays unless the supervisor also omits `banned`/`games` for rooms whose
  `rev` the caller already has (a later, optional step).
- Confidence: high (claim); medium (exact saving, which depends on room mix)

### F-ROOMS-2: an occupied but idle room host ticks at 62 Hz forever; only empty rooms wait for work
- File: server/roomhost/main.cxx:26
- File: server/roomhost/main.cxx:125
- File: server/roomhost/RoomHost.cxx:346
- File: src/session/RoomModel.cxx:840
- Category: overhead
- Severity: P2
- Claim: `RoomHost::Quiet()` requires `snapshot->members.empty()`, so any room with a member uses
  `TickMs = 16`: `WaitForIncoming(16)` times out about 62 times a second and each wakeup runs
  `RoomRecoveryRuntime::Tick` (a second `IrohRoom::Poll` plus a `Coordination()` read),
  `AdvanceCustomRoom` (which, in a recovery room, calls `RoomAuthority::AdvanceTime` and re-emplaces
  `lastActiveMs_` for every member), `SessionServer::Step` and `Report`. PUBLIC_ROOM_HOSTING.md says
  the host "sleeps until its helper has an event, 250 ms at most while the room is empty and settled",
  which is accurate, but the common state of a public room is players sitting in the lobby, chatting
  or picking seats, where every real input already arrives as a helper event that ends the wait. The
  room timers that need ticking are coarse: SpectatorStartHoldMs 10 s, ResultDisputeTimeoutMs 30 s,
  ReadyTimeoutMs 90 s, PermitHoldMs 105 s, and `RoomAuthority::HasDueTimerTransition` already
  enumerates exactly those deadlines. Also, on the 5 ms question: the 5 ms sleeps at main.cxx:117,143
  and 146 are bounded start and exit waits, not steady state.
- Proposed change: replace the `Quiet()` boolean with `RoomHost::WaitMs(nowMs)`: 16 ms while a table is
  Playing or Paused, while anything is in flight (the existing `pendingBans_`, `ProposalInFlight`,
  `CheckpointPending`, `HasRecoveryCandidate`, `PendingProposal`, `recoveryFailedSinceMs_` checks) or
  while closing; otherwise min(250 ms, time to the next room deadline). Add
  `RoomAuthority::NextDueMs(nowMs)` beside `HasDueTimerTransition`, using the same four timer rules, so
  the deadline logic stays in one place. Optionally, the three 5 ms `sleep_for` loops in main.cxx
  can use the same helper wait (connect: `WaitForIncoming(5)` until the state leaves Connecting; close
  and leave: `WaitForIncoming(TickMs)` between ticks), which deletes the separate sleep pattern; this
  part is tidiness, not overhead, since those loops are bounded to about 21 s per room lifetime.
- Behavior preserved: every helper event still ends the wait at once; timers still fire no later than
  their deadline plus one wait (at most 250 ms against 10 s to 120 s timeouts); playing tables keep
  the game-rate tick.
- Behavior at risk: anything in `SessionServer::Step` or the recovery runtime that is time-driven but
  not covered by the room timers and is not woken by a helper event (I did not find one, but did not
  audit every MatchAuthority phase; lobby-to-match transitions such as connect checks should be
  treated as "in flight" to be safe). Close detection after stdin EOF stays bounded by 250 ms as today.
- Estimated saving: about 58 of 62 wakeups per second per occupied idle room. At an assumed 30 to 50
  microseconds per idle tick that is roughly 0.2 to 0.3 percent of a core per lobby-idle room, or
  about 10 percent of a core across 45 rooms, plus the context switches; to be confirmed with the
  soak monitor, which already records per-room CPU.
- Confidence: high (claim); medium (saving and exact predicate)

### F-ROOMS-3: RoomHost::Report does five jobs, and its gate bypass rebuilds Status every tick during a commit
- File: server/roomhost/RoomHost.cxx:301
- File: server/roomhost/RoomHost.cxx:287
- File: server/roomhost/RoomHostStatus.cxx:242
- File: server/roomhost/RoomHostStatus.cxx:272
- Category: structural
- Severity: P2
- Claim: `Report` caches the invitation, emits the one-time `hosted` line (with the region wait),
  gates on revisions, forwards bans to the helper (`ForwardBans`, a side effect with its own retry
  timing) and performs a state transition (`snapshot->closed` sets `closing_`, `closeApplied_`,
  `closeReason_`), and only then builds and emits the status. Because ban forwarding and close
  detection live inside it, the cheap revision gate has to be weakened: it is bypassed whenever
  `pendingBans_` is non-empty, a recovery candidate exists or a proposal is pending, and also carries
  a `(closing_ || !snapshot->closed)` clause. While a commit is in flight (every committed action in a
  busy room) each 16 ms tick therefore copies up to 512 ban strings and 32 games out of
  `CommittedServerOwned()`, runs `DetailsOf` (a sort and `HostDisplayName`, which decodes the name
  into a `std::vector<Glyph>` holding one heap `std::string` per character), and compares the result,
  only to find it equal. A `pendingBans_` entry keeps the bypass on for at least the 1 s NoticeMs, 62
  rebuilds.
- Proposed change: split into `Tick` steps with one job each: `NoteInvitation()`, `MaybeEmitHosted(now)`,
  `ForwardBans(now)` driven by `pendingBans_` plus the committed ban list only when
  `recovery_.AppliedRevision()` moved, `DetectModelClose(now)` from the snapshot, and a pure
  `Report()` whose gate is only (snapshot revision, applied revision, invitation). Make
  `HostDisplayName` work on byte offsets (`struct Glyph { char32_t code; uint8_t offset, length; }`
  or two indices) instead of a string per character.
- Behavior preserved: identical lines on stdout in identical order; ban forwarding still waits NoticeMs
  and for no in-flight commit; model-initiated close still sets the same reason.
- Behavior at risk: none identified, provided ForwardBans still retries every tick while it has a
  pending entry (it would keep the room non-quiet, as today).
- Estimated saving: a few hundred small allocations per tick during in-flight commits in rooms with
  bans or long names; perhaps 10 to 30 lines simpler; the gate loses three of its seven clauses.
- Confidence: high

### F-ROOMS-4: the supervisor decodes each status into serde_json::Value twice and re-encodes it to measure it
- File: server/ember-rooms/src/protocol.rs:200
- File: server/ember-rooms/src/protocol.rs:292
- File: server/ember-rooms/src/supervisor.rs:52
- File: server/ember-rooms/src/supervisor.rs:385
- Category: judo
- Severity: P2
- Claim: `parse_line` deserializes the line into `Envelope { rest: Map<String, Value>, details: RawValue,
  games: RawValue }`, then converts `rest` with `serde_json::from_value` into `ChildMessage` (a second
  pass that rebuilds every string and the `banned` Vec), then parses `details` and `games` from their
  raw text into `Value`; `bounded` then walks each Value for depth and calls `serde_json::to_vec` only to
  learn its length. The values are stored as `Value`, cloned for every `GET /rooms` and serialized
  again. The supervisor never reads `details` or `games` (its own doc says so); it only bounds them.
- Proposed change: keep `details` and `games` as `Box<RawValue>` end to end: `Live` and `RoomInfo` hold
  `Option<Box<RawValue>>` (serde writes RawValue verbatim), the size check becomes
  `raw.get().len() <= max`, the kind check is the first non-space byte (`{` or `[`), and the depth
  check becomes a bracket-depth scan over the raw bytes that skips string contents (the parser has
  already validated the JSON). That deletes `bounded`, its `depth` recursion and both Value round
  trips. Optionally deserialize `rest` straight into small typed structs per `type` instead of the
  `Map` plus `from_value` hop.
- Behavior preserved: what is kept or dropped (object or array, size, nesting), the duplicate-member
  error, the tolerance of over-deep details, and the bytes the bridge receives for the compact JSON the
  room host emits.
- Behavior at risk: a details object written with insignificant whitespace would be measured with its
  whitespace (stricter) and passed on with it; the real host writes compact JSON, so none in practice.
  Dropping the `Map` hop would need care to keep "an unknown member named twice is an error"
  (protocol.rs test `a_member_named_twice_is_an_error`).
- Estimated saving: two allocating passes and one encode per status line plus one Value clone per room
  per poll; small in CPU at today's rates (lines per minute, polls per 5 s) but about 30 lines and one
  concept deleted.
- Confidence: high

### F-ROOMS-5: supervisor room task state is threaded through loose parameters and a duplicated struct
- File: server/ember-rooms/src/supervisor.rs:100
- File: server/ember-rooms/src/supervisor.rs:489
- File: server/ember-rooms/src/supervisor.rs:527
- File: server/ember-rooms/src/supervisor.rs:643
- Category: structural
- Severity: P2
- Claim: `Live` repeats `RoomInfo` field for field and `list()` copies one into the other. A room's task
  state is split across locals in `drive` (`hosted`, `empty_since`, `reader`, `hosted_deadline`), the
  `Shutdown` struct, and `hosted_tx`, and `apply` takes seven parameters, four of them `&mut` to that
  scattered state. `Shutdown.started` mostly mirrors `stdin.is_none()`. Adding the F-ROOMS-1 counter
  would add yet another field to both structs.
- Proposed change: a `RoomTask { key, shutdown, hosted: Option<oneshot::Sender>, empty_since, deadlines }`
  with `apply(&mut self, message)` and `next_deadline()`; store a `RoomInfo` (minus `room_id`) in
  `Room.live` instead of `Live` so `list()` is a clone plus the key and `set_live` is the single place
  that latches `opened` and bumps `rev`.
- Behavior preserved: every timer, kill and close path and their log reasons.
- Behavior at risk: none identified; `Shutdown.started` must stay distinct from `stdin.is_none()` for the
  spawn path where stdin was never taken, so keep the flag or model that case explicitly.
- Estimated saving: about 40 lines; one struct and the seven-parameter function removed.
- Confidence: medium

### F-ROOMS-6: rooms.json is refetched in full every 15 s and the page is rebuilt every time
- File: server/ember-short/deploy/site/assets/rooms.js:9
- File: server/ember-short/deploy/site/assets/rooms.js:243
- File: server/ember-short/deploy/site/assets/rankings.js:11
- File: server/ember-short/deploy/site/assets/rankings.js:651
- File: server/ember-short/deploy/site/assets/rankings.js:682
- Category: overhead
- Severity: P2
- Claim: rooms.js fetches `/rooms.json` with `cache: 'no-store'` every 15 s while visible and always
  calls `render()`, which empties `#rooms` and rebuilds every card, so a viewer's keyboard focus or
  text selection is lost every 15 s even when nothing changed. Payload: `{bridge_id, listed_at,
  rooms:[RoomSummary]}`, about 350 B per room (32-hex room id, 64-hex build id, name, host, fighters,
  rules), about 16 KB at 45 rooms, uncompressed (see F-ROOMS-9). Per open tab that is about 1 KB/s.
  rankings.js already does the right thing for rendering (`keyOf` skips redraws), but it builds the
  key only after `valid()` has run `fold()` (an NFKD normalize per character) on every one of up to
  500 names: per poll about 27 KB downloaded (500 x about 55 B) and 500 folds, every 30 s per tab.
  `no-store` forbids conditional requests, and neither upstream sends a validator, so every poll is a
  full body.
- Proposed change: in rooms.js, keep the response text and skip `render()` when it equals the previous
  one after removing `listed_at` (or reuse rankings.js's key approach); update only the "Open for"
  ages. In rankings.js compute `keyOf` from the raw entries before `fold()`, and fold only when the key
  changed. Optionally switch both to `cache: 'no-cache'` once the bridge emits an ETag over the body
  without `listed_at`.
- Behavior preserved: the same list, filters and counts; the same refresh cadence.
- Behavior at risk: `listed_at` drives "Updated N min ago" (rankings) and "Open for N min" (rooms); a
  304 or a skipped render must not freeze those, so either keep updating the summary text as
  rankings.js does now or compute ages from `Date.now()`.
- Estimated saving: the DOM rebuild every 15 s per rooms tab (and the focus loss); 500 folds every 30 s
  per rankings tab; with an ETag, about 1 KB/s per rooms tab and about 0.9 KB/s per rankings tab on
  unchanged polls.
- Confidence: high (claim); medium (byte figures)

### F-ROOMS-7: release data and helpers are copy-pasted across the site scripts
- File: server/ember-short/deploy/site/assets/rooms.js:22
- File: server/ember-short/deploy/site/assets/open.js:39
- File: server/ember-short/deploy/site/assets/rooms.js:85
- File: server/ember-short/deploy/site/assets/rankings.js:619
- File: server/ember-short/deploy/site/assets/open.js:137
- Category: spaghetti
- Severity: P3
- Claim: rooms.js carries a hand-edited table of 13 sidecar build ids to version labels, and open.js a
  hand-bumped `STABLE_VERSION`, both of which must be edited at each release (the memory notes say so).
  `copy()` with its textarea fallback is pasted three times, `el()` twice, and the visible-tab poll
  loop (timer, `loading` flag, `schedule`, `visibilitychange`) twice with different behaviors
  (rankings skips unchanged redraws, rooms does not).
- Proposed change: one `assets/site.js` with `el`, `copy` and `poll(url, intervalMs, onChange)` (which
  owns the visibility pause and the change check from F-ROOMS-6); have the bridge or supervisor
  configuration carry the label for each build id it hosts and add it to `RoomSummary` (optional
  field, skipped when absent) so rooms.js loses its VERSIONS table and falls back to "Other version"
  as today when the label is missing.
- Behavior preserved: the same pages; CSP `script-src 'self'` already allows a second script.
- Behavior at risk: pages cached in browsers with the old script keep working; an old bridge without the
  label gives "Other version" unless the table is kept as a fallback for one release.
- Estimated saving: about 60 duplicated lines and one per-release manual edit.
- Confidence: medium

### F-ROOMS-8: the status-line size limits disagree across the three components that enforce them
- File: server/roomhost/RoomHostStatus.hxx:163
- File: server/ember-rooms/src/protocol.rs:60
- File: server/ember-rooms/src/protocol.rs:84
- File: server/ember/bridge/src/routes/rooms/supervisor.rs:21
- Category: boundary
- Severity: P3
- Claim: RoomHostStatus.hxx says the supervisor kills a child "whose line passes 64 KiB" and that a full
  line is "about 36 KB plus the invitation"; protocol.rs says 128 KiB and about 43 000 bytes; the host
  fails itself at `MaximumStatusLineBytes = 60000`. MAX_ROOM_BANS, MAX_DETAILS_BYTES and
  MAX_GAMES_BYTES each exist in two or three crates with "keep them equal" comments (ember-rooms,
  the bridge, the C++ room model).
- Proposed change: correct the C++ comment to 128 KiB and 43 KB; put the shared numbers in one place
  (ember-rooms can take `ember_protocol::rooms` as a dependency, or a tiny shared constants crate) and
  have the C++ side reference a generated or asserted value in a test.
- Behavior preserved: all limits as enforced today.
- Behavior at risk: none identified.
- Estimated saving: one class of drift bug; no runtime effect.
- Confidence: high

### F-ROOMS-9: the two public JSON proxies are probably served uncompressed
- File: server/ember-short/deploy/nginx/ember-short-locations.conf:70
- File: server/ember-short/deploy/nginx/ember-short-locations.conf:110
- File: server/ember-short/deploy/nginx/ember-short-locations.conf:173
- File: server/ember-short/deploy/nginx/ember-short-zone.conf:12
- Category: overhead
- Severity: P3
- Claim: `gzip_types` is set only in the static-file location (css, js, text). The `/rooms.json` and
  `/rankings.json` locations set none, and nginx's default `gzip_types` is text/html only, so unless the
  server's nginx.conf (not in the repository) adds `application/json`, both answers go out
  uncompressed. Also the zone comment says the rankings answer is "kept for a minute" while the
  location caches it 15 s (`proxy_cache_valid 200 404 15s`), matching rankings.js's comment, so the
  zone comment is stale.
- Proposed change: add `gzip on; gzip_types application/json; gzip_vary on;` to both locations (or the
  server block); fix the zone comment.
- Behavior preserved: the same JSON bodies.
- Behavior at risk: none identified.
- Estimated saving: about 60 to 70 percent of each body: about 0.6 KB/s per open rooms tab and 0.6 KB/s
  per rankings tab at today's sizes.
- Confidence: medium (depends on the live nginx.conf)

### F-ROOMS-10: small deletions in ember-reports and ember-short
- File: server/ember-reports/src/bugsink.rs:116
- File: server/ember-reports/src/bugsink.rs:8
- File: server/ember-reports/src/lib.rs:227
- File: server/ember-short/src/lib.rs:364
- File: server/ember-short/src/main.rs:57
- Category: judo
- Severity: P3
- Claim: `Bugsink::deliver` is called only by tests/delivery.rs; production delivery is `replay`, which
  only distinguishes `BackendUnavailable` from the five other `Delivery` variants. `process` clones the
  whole minidump (up to 4 MiB) to hand it to `Worker::walk`, then uses the original only for
  `commit`. ember-short sweeps expired records twice over: a 30 s background interval in main.rs and
  an inline `next_sweep` check in `store`.
- Proposed change: test through `replay` (or make `deliver` `#[cfg(test)]`); have `walk` take
  `bytes::Bytes` or an `Arc<[u8]>` so the dump is shared, not copied; keep one sweep trigger (the
  background interval, plus the existing sweep-before-refusing at the record limit).
- Behavior preserved: delivery, retries and storage; record expiry.
- Behavior at risk: none identified.
- Estimated saving: one copy of up to 4 MiB per crash report; about 15 lines.
- Confidence: high

## Area summary
Health: good. This area is careful, well bounded and well commented; the status line is already
send-on-change, ember-reports is event-driven with a negligible 60 s housekeeping tick, and the site
pages pause polling in hidden tabs. The biggest overhead item is F-ROOMS-1: the bridge's 5 s poll of
GET /rooms is a full snapshot with no change signal, and the bridge re-seals, re-UPDATEs and re-inserts
every room's games and bans and wakes every event stream each time; a per-room revision counter
removes almost all of it with additive, both-ways-compatible wire changes. The second is F-ROOMS-2:
"room hosts wait for work" holds only for empty rooms; an occupied lobby ticks at 62 Hz although its
timers are 10 to 120 s. The biggest judo move is F-ROOMS-4 (pass `details` and `games` through as
RawValue, deleting `bounded` and two Value round trips), with F-ROOMS-3 (pull ban forwarding and close
detection out of Report) the biggest structural cleanup. No file in the area is over 900 lines;
rankings.js (803) is cohesive. Not assessed: the live nginx.conf (gzip), the real per-tick cost of an
idle occupied room (needs the soak monitor), invitation sizes on the wire, and whether any
MatchAuthority phase relies on the 16 ms tick outside Playing/Paused tables.
