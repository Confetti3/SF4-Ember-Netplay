# Findings: server/ember protocol, brackets, notifier

Scope: `server/ember/protocol/src` (all files), `server/ember/brackets/src` (all files; `rounds.rs` and `startgg/wire.rs` outlined, not read line by line), `server/ember/notifier/src` (all files). Tests folders skipped; in-file `#[cfg(test)]` modules counted only for size. Head b9ce0e31. Read-only; nothing built or run.

Cross-references: the claim loop's helper side is F-TOUR-5 (rust-tournament.md); the bridge building responses and event data as `json!` literals is F-BRIDGE-16 (server-bridge.md). The findings below cover the protocol and consumer side of those and do not repeat them.

### F-PROTO-1: The notifier's delivery loop scans its whole, never-pruned inbox every second
- File: server/ember/notifier/src/inbox.rs:102
- File: server/ember/notifier/src/inbox.rs:107
- File: server/ember/notifier/src/lib.rs:110
- File: server/ember/notifier/src/inbox.rs:31
- Category: overhead
- Severity: P2
- Claim: `Notifier::run` runs `SELECT id, body, ... FROM inbox WHERE (discord_done = 0 OR twitch_done = 0) AND next_at <= ?1 ORDER BY received_at LIMIT 8`, and when nothing is due waits `timeout(1 s, wake.notified())` and runs it again. The table has only its primary key on `id` (lib.rs:112), so every query is a full scan plus sort, and nothing ever deletes a row: every event the bridge has ever delivered, with its full body BLOB, stays in `inbox` (and every label in `matches`). Idle cost therefore grows with the notifier's age: 86,400 full scans a day of an ever-larger table, each taking the `Mutex<Connection>` that `accept` (the webhook handler) also needs. Separately, `accept` performs the `synchronous = FULL` insert transaction (an fsync) directly on the async runtime thread, while ember-brackets does the same kind of write through `spawn_blocking` with a comment saying why (brackets/src/results.rs:149). Each event body is also strict-parsed twice: once in `accept` (inbox.rs:36) and again from the stored bytes in `deliver` (inbox.rs:141).
  - Payload: one SQLite query over all inbox rows (each row holds a 0.5 to 2 KB event body). Size: table grows by one row per bridge event, never shrinks. Interval: every 1 s when idle. Fan-out: one notifier per deployment. Transport: local SQLite, no wire.
- Proposed change: (1) `CREATE INDEX IF NOT EXISTS inbox_due ON inbox (next_at) WHERE discord_done = 0 OR twitch_done = 0` in `Notifier::new`, so the due query and a `SELECT MIN(next_at)` read only pending rows. (2) In `run`, when nothing is due, read `MIN(next_at)` over pending rows and wait `timeout_at(that time, wake.notified())`, or `wake.notified()` alone when there are none. `Notify::notify_one` stores a permit, so an `accept` that lands between the query and the wait is not lost. (3) Wrap the body of `receive` in `spawn_blocking` like brackets' `results::receive`. (4) Optional and separate, since it changes what the database keeps: delete rows that are done and older than the 24 h give-up window, or move them to an archive table.
- Behavior preserved: every event is still recorded before the 2xx, delivered in `received_at` order, retried on the same `RETRY` table and given up after `GIVE_UP_SECS`. Retries fire at their `next_at` (to the second, as now).
- Behavior at risk: none identified for (1) to (3). (4) changes the durable record an operator might read; keep it opt-in.
- Estimated saving: about 86,400 table scans a day become a handful of indexed lookups per event; idle CPU drops to near zero instead of O(rows) per second. No wire bytes change.
- Confidence: high

### F-PROTO-2: The brackets worker wakes every 60 s on a fixed tick, so reads are quantized and `poll_secs` below 60 is silently 60
- File: server/ember/brackets/src/bracket.rs:38
- File: server/ember/brackets/src/bracket.rs:185
- File: server/ember/brackets/src/bracket.rs:332
- File: server/ember/brackets/src/schedule.rs:39
- File: server/ember/brackets/src/config.rs:23
- Category: overhead
- Severity: P3
- Claim: `Bracket::run` loops `pass(false)` then `timeout(WAKE_SECS = 60 s, wake.notified())`. The real schedule lives in four deadlines (`State::schedule`, `State::retry`, each `deferred` Backoff, and `reconcile::Memory::next`), but the worker only looks at them once a minute. Consequences: (a) a scheduled read is late by up to 59 s, so a `poll_secs` of 120 reads every 120 to 179 s and the minimum `poll_secs` of 30 (config.rs:23, documented in README "at least 30") actually reads every 60 s, contradicting the README's "at most `poll_secs` later"; (b) every idle wake still runs `store().pending()` (a SQLite query on `results` filtered by `bracket, done` with no index) per tournament per minute even when nothing can be due. The overhead is small; the main cost is that the timing the configuration promises is not the timing it gets.
  - Payload: one SQLite query (`pending`) per tournament. Size: small table. Interval: every 60 s per configured tournament, forever (including after `finished`, where `pass` returns early but the task still wakes). Fan-out: tournaments configured. Transport: local SQLite, no wire.
- Proposed change: add `Backoff::due_at(&self) -> u64` (the private `at`) in schedule.rs and `Memory::due_at` in reconcile.rs; in `Bracket::run`, after each pass compute the earliest of `schedule.due_at()` (only while in window and not finished), `retry.due_at()` (only when it has failed), the minimum `deferred` due time, `reconcile.next`, and the window's `from`/`until` edge, then `tokio::time::timeout(until_then, wake.notified())`. A finished bracket waits on `wake` only. Delete `WAKE_SECS`.
- Behavior preserved: what a pass does, the backoff rules, results waking the worker at once, and the budget accounting.
- Behavior at risk: timing. Any configured `poll_secs` that is not a multiple of 60 (30, 45, 90) will read on its real schedule, which for Challonge means more requests counted against the 500 a month budget. The default (120 with a 600 cap) is a multiple of 60 and changes only by losing the up-to-59 s lag. State this in the README change.
- Estimated saving: 1,440 idle wakes and queries per tournament per day removed; reads land on time. Roughly 40 lines changed, one constant deleted.
- Confidence: high

### F-PROTO-3: tournament.rs is two modules: provider wire commands and a 550-line bracket engine that only the bridge uses
- File: server/ember/protocol/src/tournament.rs:1
- File: server/ember/protocol/src/tournament.rs:221
- File: server/ember/protocol/src/tournament.rs:520
- File: server/ember/protocol/src/tournament.rs:603
- File: server/ember/bridge/src/tournament/state.rs:124
- Category: size
- Severity: P2
- Claim: the file is 1,099 lines: about 150 lines of wire types (`Format`, `CreateTournament`, `RegisterEntrant`, `Side`, `Status`), about 570 lines of a pure bracket engine (`Source`, `Node`, `seed_order`, the label builders, `plan`, `round_robin`, `Slot`, `NodeState`, `resolve`, `settle`, `complete`, `reopen`/`unwind`, `eliminated`, `finished`, `placements`) and 375 lines of engine tests. The crate doc (lib.rs:1-7) says the crate is "wire rules"; the engine is not a wire rule and no client uses it: `sf4-net` imports nothing from `ember_protocol::tournament` (its `crate::tournament` is its own), only `server/ember/bridge` does. Every `load_bracket` in the bridge re-runs `plan` from scratch, which allocates a `String` label per node (up to 255 nodes for 128 double elimination entrants) although the plan is a pure function of `(format, entrants, grand_final_reset)`.
- Proposed change: split into `protocol/src/tournament/mod.rs` (wire types: `Format`, `CreateTournament`, `RegisterEntrant`, `Status`, `Side`) and move the engine to `bridge/src/tournament/engine.rs` (`plan.rs` for `Source`/`Node`/`plan`/`round_robin`/labels, `engine.rs` for `Slot`/`NodeState`/`settle`/`complete`/`reopen`/`placements`), taking its tests with it. If the owner prefers the engine in the shared crate, at minimum make it `protocol/src/tournament/{plan,engine}.rs` so the wire file is short. While moving: make `Node::label` a method computed from `(side, round, rounds)` instead of a stored `String`, so `plan` allocates nothing per node.
- Behavior preserved: every function body moves unchanged; node order, labels and placements stay identical; wire types keep their serde shapes.
- Behavior at risk: none identified (the bridge is the only caller; check `bridge/src/tournament/*` and `routes/tournaments/view.rs` imports).
- Estimated saving: protocol crate loses about 950 lines that the helper build compiles for nothing; one concept per file. Per `load_bracket`, up to 255 small allocations avoided (microseconds).
- Confidence: high

### F-PROTO-4: The "same attempt" test is hand-written three times, plus a 13-field copy, over the same field list
- File: server/ember/protocol/src/play.rs:334
- File: server/ember/protocol/src/play.rs:424
- File: server/ember/protocol/src/play.rs:461
- File: server/ember/protocol/src/report.rs:150
- Category: judo
- Severity: P2
- Claim: `Prepare::same_game` compares 8 fields, `Permit::covers` compares 13, `GameReport::agrees_with` compares 14, and `Permit::report` copies 13 fields from the permit into a `GameReport` by hand. They are all spelling out one concept: the coordinates of one official attempt (bridge, match, assignment generation, attempt, permit, room, table, match generation, P1, P2, rules digest, roster digest, build). Adding a field to `Permit` (it has happened to `Binding`: `starting_wins`, `watchers`) requires remembering three comparison sites; forgetting `covers` silently weakens which reports a permit accepts. The prior review (docs/reviews/2026-09-code-quality-review.md, section 3) notes `serde(flatten)` cannot be used with `deny_unknown_fields`, so the wire types must stay flat; that does not stop an internal view.
- Proposed change: add a non-wire `pub struct AttemptKey<'a> { bridge_id: &'a str, match_id: &'a str, assignment_generation: Counter, attempt_id: &'a str, permit_id: &'a str, room_id: &'a str, table_id: u8, match_generation: Counter, p1_id: &'a EmberId, p2_id: &'a EmberId, rules_digest: &'a str, roster_digest: &'a str, build_id: &'a str }` with `#[derive(PartialEq, Eq)]`, and `fn key(&self) -> AttemptKey<'_>` on `Permit` and `GameReport` (and a smaller `GameKey` on `Prepare`). Then `covers` is `self.key() == report.key()`, `agrees_with` is `self.key() == other.key() && self.result == other.result`, and `Permit::report` builds from the key. Wire structs, field order and signatures are untouched.
- Behavior preserved: identical comparisons (verify the field lists match one to one while converting; `agrees_with` already omits observation/reporter fields and must keep doing so).
- Behavior at risk: none identified; no bytes on the wire change.
- Estimated saving: about 40 lines and three hand-maintained field lists collapse to one.
- Confidence: high

### F-PROTO-5: Event data is an untyped JSON map on both ends; the notifier reads about 30 string keys and re-spells protocol enums
- File: server/ember/protocol/src/event.rs:185
- File: server/ember/notifier/src/format.rs:64
- File: server/ember/notifier/src/format.rs:236
- File: server/ember/notifier/src/format.rs:358
- File: server/ember/notifier/src/inbox.rs:48
- Category: boundary
- Severity: P2
- Claim: `Event::data` is `serde_json::Map<String, Value>`. The bridge writes each kind's data with `json!` (`bridge/src/tournament/progress.rs:165` `data["winner_next"] = ...`, `progress.rs:263`, `routes/lobbies.rs:445`; F-BRIDGE-16), and the notifier reads it back with `data.get("...").and_then(Value::as_str)` chains across most of format.rs's 388 lines: `scores[].ember_id/wins`, `participants[].ember_id`, `winner_id`, `winner_next`, `loser_next`, `eliminated`, `walkover`, `placements[].placement/ember_id`, `queue`, `streak.sets`, `metadata.round_label/lobby_set/tournament_node/title`, and more. Missing or renamed keys degrade silently (`unwrap_or_default`, `unwrap_or(1)`), so a bridge-side rename produces "Unknown player" or "first to 1" announcements with no error. format.rs also re-spells protocol enums as literals: `"winner_stays"`/`"loser_stays"` (format.rs:236, already `lobby::Rotation`), `"double_elimination"`/`"round_robin"` (format.rs:358, already `tournament::Format`), `"draw"` (already `report::Outcome`). `render_room` clones the whole map, removes `reason`, and re-deserializes into `ConnectionRoom`. The precedent for a typed data shape already exists: `matches::MatchCompleted` is the typed data of `match.completed` and the bridge builds it (`bridge/src/routes/ledger.rs:388`).
- Proposed change: in `protocol::event` (or a new `protocol::event_data`), add typed data structs for the kinds the notifier announces (`MatchCreatedData`, `ScoreChangedData`, `TournamentMatchCompletedData`, `TournamentCompletedData`, `LobbySetCompletedData`, `RoomEventData { #[serde(flatten)] room: ConnectionRoom, reason: Option<String> }`, ...), each with `#[serde(default)]` on optional fields and without `deny_unknown_fields`. Add `Event::data_as<T: DeserializeOwned>(&self) -> Option<T>`. The bridge serializes these structs instead of `json!`; the notifier's `render` matches on `Kind` and reads typed fields, using `Rotation`/`Format` directly.
- Behavior preserved: the event JSON is byte-for-byte the same when the structs mirror today's keys (serde field order does not matter to receivers; the webhook signature covers the body as sent).
- Behavior at risk: wire compatibility only if a struct accidentally renames a key; keep a fixture test of each kind's current JSON. Old subscribers (third-party webhooks, the SDK) see the same JSON. The notifier becomes stricter about types it can read; tolerate unknown keys so a newer bridge does not break it.
- Estimated saving: roughly 150 lines of `get/and_then/unwrap_or` in format.rs; one contract instead of two hand-kept ones; announcement regressions become compile or test errors.
- Confidence: medium (the struct set needs care; the direction is clear)

### F-PROTO-6: Answer types are `deny_unknown_fields`, so ember-brackets keeps 11 tolerant mirror structs and the notifier is exposed
- File: server/ember/protocol/src/partner.rs:33
- File: server/ember/protocol/src/rooms.rs:200
- File: server/ember/brackets/src/bridge.rs:163
- File: server/ember/brackets/src/bridge.rs:240
- File: server/ember/brackets/src/sets.rs:137
- File: server/ember/notifier/src/bridge.rs:76
- Category: boundary
- Severity: P2
- Claim: 61 protocol types carry `deny_unknown_fields`, including answer-only types that services read: `partner::LookupAnswer`, `FoundPlayer`, `MatchResult`, `rooms::RoomSummary`, `ConnectionRoom`. ember-brackets refuses to depend on them and keeps its own tolerant copies "so a field a later bridge adds does not stop an older connector": `LookupView`, `PlayerView`, `AccountView`, `CapabilitiesView`, `SiteView`, `MatchView`, `Attempt`, `Seat`, `Wins` (bridge.rs) and `ResultView`, `SeatView` (sets.rs). The notifier, in the same workspace, does use the strict `LookupAnswer` and `ConnectionRoom` (notifier/src/bridge.rs:76, 87, 102), so any field the bridge adds to a room summary or lookup answer without the skip-when-default dance breaks `/room` and `!room`. The comment on `LookupAnswer` (partner.rs:36-39) says readers refuse unknown fields "as ember-brackets reads it", which is no longer true. This strictness is also the root of the optionality churn in the wire types: every added answer field must be `Option`/`skip_serializing_if`/`is_false` and often capability-gated (`own_delay`, `host_name`, `fighters`, `locked`, `set_format`, `rotation`, `listed_at`, `badge`, `developer`, `accounts`).
- Proposed change: classify protocol types into three groups and make the attribute follow the group: (a) requests the bridge accepts: keep `deny_unknown_fields`; (b) signed objects (`Binding`, `Permit`, `RoomTicket`, `GameReport`, `Challenge`): keep it, since verification re-serializes the struct and an ignored field would fail the signature anyway; (c) answers and deliveries read by clients and services (`LookupAnswer`, `FoundPlayer`, `FoundAccount`, `MatchResult`, `ResultParticipant`, `RoomSummary`, `RoomList`, `ConnectionRoom`, `ConnectionRoomList`, `Standing`, `Board`, `WatchAnswer`, `ClaimAnswer`, `PrepareAnswer`): drop it. Then delete brackets' `LookupView`/`PlayerView`/`AccountView`/`ResultView`/`SeatView` in favour of the protocol types (keep `MatchView` until the match snapshot is a protocol type, see F-BRIDGE-16), and fix the partner.rs comment. Update the rooms.rs tests that assert an extra field is refused (only `RoomList` at rooms.rs:967; the `CreateRoom` and `TicketRequest` tests at 960 and 965 are requests and the `RoomTicket` test at 973 is a signed object, so those stay strict) to assert it is ignored for group (c).
- Behavior preserved: every byte the bridge sends; requests and signed objects stay strict.
- Behavior at risk: forward-only. Helpers already in the wild still refuse unknown fields in answers, so the bridge must keep omitting new default fields and gating them by capability until those builds age out; the change only stops that rule from growing for builds made after it. Any test that relied on a refused answer as a version signal must be found first (grep for tests parsing answer types with extra fields).
- Estimated saving: about 120 lines of mirror structs in brackets; one place to read each answer shape; removes a latent notifier outage.
- Confidence: medium (the security posture on answers is a policy call; the duplication and the notifier exposure are facts)

### F-PROTO-7: The strict JSON path builds two extra full trees on every parse and every signature
- File: server/ember/protocol/src/json.rs:47
- File: server/ember/protocol/src/json.rs:53
- File: server/ember/protocol/src/json.rs:59
- File: server/ember/protocol/src/json.rs:139
- File: server/ember/bridge/src/http.rs:36
- Category: overhead
- Severity: P3
- Claim: `parse_as` is bytes to strict `Value` (a `BTreeMap` tree), then `to_serde()` builds a second full `serde_json::Value` tree (cloning every key and string), then `serde_json::from_value` builds `T`. `to_value` (used by every `sign`, `verify`, `digest` and `canonical`) is `T` to `serde_json::Value` to strict `Value` (cloning every string again). The bridge's request extractor `Json<T>` (bridge/src/http.rs:36) uses `parse_as` for every request body, and every bridge-signed binding, permit and room ticket goes through `to_value` twice (once in `check` via `roster_digest`/`rules_digest`, once for the signing bytes). `MatchState::parse` and `DeliveryState::parse` (matches.rs:212, 230) round-trip through `serde_json::Value::String` to parse a word.
  - Payload: any request or signed object. Size: 0.3 to 8 KB (claims about 0.7 KB, proven bodies up to `MAX_PROOF_BODY`). Interval: per request; claims every 10 s per fighter, prepare every 2 s while pending, watch every 10 s per watcher. Fan-out: per fighter and watcher. Transport: HTTPS body parsing and signing on the bridge and helper.
- Proposed change: implement `serde::Deserializer` for `&json::Value` (about 120 lines, the standard pattern) so `from_value` deserializes `T` straight from the strict tree, and implement `serde::Serializer` producing `json::Value` directly so `to_value` never builds a `serde_json::Value`. Delete `Value::to_serde` and `Value::from_serde`. The strict profile (duplicate keys, depth, safe integers, no fractions) stays in `Seed`, and the safe-integer and depth checks move into the new serializer. Make `MatchState::parse`/`DeliveryState::parse` table lookups like `Status::parse` (see F-PROTO-11).
- Behavior preserved: the same inputs accepted and refused, and the same canonical bytes; keep the existing fuzz tests (play.rs:856 onward) and add one comparing old and new `to_value` on every fixture.
- Behavior at risk: a subtle difference in integer handling (u64 above `MAX_SAFE_INTEGER`, negative zero) if the serializer is written carelessly; covered by the fixture comparison.
- Estimated saving: two tree allocations per parse and per signature. CPU is small at current load (single-digit microseconds per claim-sized body); it matters only as allocation churn. Also about 50 lines deleted.
- Confidence: medium

### F-PROTO-8: Field validation is duplicated with drifting rules (build IDs, hex IDs, table IDs, site IDs, names, dates)
- File: server/ember/protocol/src/play.rs:60
- File: server/ember/protocol/src/matches.rs:129
- File: server/ember/protocol/src/lobby.rs:77
- File: server/ember/protocol/src/tournament.rs:105
- File: server/ember/protocol/src/report.rs:87
- File: server/ember/protocol/src/report.rs:95
- File: server/ember/brackets/src/config.rs:362
- File: server/ember/brackets/src/config.rs:371
- File: server/ember/brackets/src/config.rs:426
- Category: spaghetti
- Severity: P2
- Claim: one rule, several spellings, and they disagree. A build ID is `check_build` (1 to 128 bytes, ASCII) for claims, bindings, permits, rooms and tickets, but `CreateMatch`, `CreateLobby` and `CreateTournament` check `required_build_id` with only `is_empty() || len() > 128` (no ASCII), `GameReport::check` uses `!is_empty() && len() <= 128` (no ASCII), and ember-brackets' config repeats the length check (config.rs:426). A non-ASCII `required_build_id` is therefore accepted at creation and can never match a helper's build, so the match is unplayable rather than refused. `GameReport::check` re-implements `is_hex(room_id, 32)` inline (report.rs:87) and accepts `table_id <= 3` while every permit requires `table_id == TABLE` (0). `CreateMatch`/`CreateLobby`/`CreateTournament` each repeat the same five-step header check (external ID length, `game == "usf4"`, `games_to_win_valid`, build ID, `check_metadata`). ember-brackets' `is_bridge_site` (config.rs:362) is `protocol::sites::is_site_id` copied; its `parse_utc` (config.rs:371) is the inverse of `protocol::event::rfc3339` and belongs beside it. `rooms::check_room_name` and `rooms::is_player_name` are the same single-line-text rule with different maxima.
- Proposed change: in protocol, make `check_build` public and use it for every build ID (`required_build_id`, `GameReport::build_id`, brackets config); use `is_hex` in `GameReport::check`; add `fn check_provider_command(external_id, game, games_to_win, build_id, metadata, external_field: &'static str) -> Result<()>` in matches.rs and call it from the three `Create*::check`s; add `event::parse_rfc3339(text) -> Option<u64>` and delete brackets' `parse_utc`; delete brackets' `is_bridge_site` for `sites::is_site_id`; express the two name checks as `is_single_line(text, max)`. Leave `table_id <= 3` alone only if a report for another table is meant to be accepted; otherwise check `== TABLE` like the permit (the report must match its permit via `covers` anyway, so `<= 3` is dead permissiveness).
- Behavior preserved: every value accepted today by the strict checks.
- Behavior at risk: tightening `required_build_id` to ASCII refuses non-ASCII build labels at creation that are accepted (and unplayable) today; BluMint and the brackets connector send ASCII builds, so none expected in practice, but say so in release notes.
- Estimated saving: about 60 lines; one rule per field.
- Confidence: high

### F-PROTO-9: Three bridge-signed objects each re-implement sign and verify
- File: server/ember/protocol/src/play.rs:254
- File: server/ember/protocol/src/play.rs:281
- File: server/ember/protocol/src/play.rs:413
- File: server/ember/protocol/src/play.rs:498
- File: server/ember/protocol/src/rooms.rs:152
- File: server/ember/protocol/src/rooms.rs:185
- Category: judo
- Severity: P3
- Claim: `Binding`/`SignedBinding`, `Permit`/`SignedPermit` and `RoomTicket`/`SignedRoomTicket` each have the same `sign` (check, clone, `key.sign(Domain::X, &json::to_value(self)?)`) and the same `verify` (kid match, check, `key.verify(Domain::X, &json::to_value(inner)?, &signature)`), six copies of one invariant ("a bridge object is checked, then signed over its canonical bytes under its own domain"). The signed wrappers differ only in the inner field's name (`binding`, `permit`, `ticket`), which is why a generic `Signed<T>` was not used.
- Proposed change: `trait BridgeSigned: Serialize + Clone { const DOMAIN: Domain; fn check(&self) -> Result<()>; }` implemented by the three inner types, plus `fn bridge_sign<T: BridgeSigned>(value: &T, key, kid) -> Result<(T, String, String)>` and `fn bridge_verify<'a, T: BridgeSigned>(value: &'a T, kid: &str, signature: &str, key: &PublicKey, expected_kid: &str) -> Result<&'a T>` in sign.rs. Each wrapper keeps its struct (same wire shape) and its `sign`/`verify` become one-line delegations.
- Behavior preserved: identical bytes signed and verified; same error order (kid, then check, then signature).
- Behavior at risk: none identified.
- Estimated saving: about 40 lines; the domain-to-type pairing becomes a constant on the type instead of a literal at two call sites each.
- Confidence: high

### F-PROTO-10: Tri-state options that carry two states, and a three-mode command spelled as three options
- File: server/ember/protocol/src/play.rs:91
- File: server/ember/protocol/src/play.rs:100
- File: server/ember/protocol/src/play.rs:137
- File: server/ember/bridge/src/routes/play/mod.rs:240
- Category: boundary
- Severity: P3
- Claim: `Claim::ready` and `Claim::watch` are `Option<bool>`, but nothing distinguishes `None` from `Some(false)`: the bridge reads `command.ready == Some(true)` and `command.watch == Some(true)` (routes/play/mod.rs:240) and the helper only ever sends `.then_some(true)` (sf4-net tournament/play.rs:359-360). Elsewhere in the same crate the identical idea is `bool` with `skip_serializing_if = "is_false"` (`own_delay`, `skill_badge`, `developer_badge`). `PublishRoom` has `lease_id: Option<String>`, `fence: Option<Counter>`, `replaces: Option<String>` and a check that only three combinations are legal (first room with lease and fence, replacement with `replaces`, or neither); every reader re-derives which one it got.
- Proposed change: make `ready` and `watch` `bool` with `#[serde(default, skip_serializing_if = "is_false")]` (move `is_false` out of rooms.rs into encoding.rs, see F-PROTO-12). For `PublishRoom`, keep the wire struct and add `fn mode(&self) -> PublishMode` returning `First { lease_id, fence } | Replace { replaces } | Refresh`, or deserialize through `#[serde(try_from = "PublishRoomWire")]` into that enum so `check` disappears into the conversion.
- Behavior preserved: wire bytes are identical for everything the helper sends today (it never sends `false`); bridges read missing as false either way.
- Behavior at risk: a third-party client that sends `"ready": false` explicitly is still accepted (default false). None identified otherwise.
- Estimated saving: removes `== Some(true)` / `.then_some(true)` at four sites and one hand-written combination check.
- Confidence: high

### F-PROTO-11: Every wire enum hand-writes `ALL`, `as_str` and `parse` next to serde names that say the same thing
- File: server/ember/protocol/src/event.rs:63
- File: server/ember/protocol/src/event.rs:155
- File: server/ember/protocol/src/tournament.rs:35
- File: server/ember/protocol/src/tournament.rs:446
- File: server/ember/protocol/src/lobby.rs:26
- File: server/ember/protocol/src/matches.rs:195
- File: server/ember/protocol/src/challenge.rs:61
- File: server/ember/brackets/src/bridge.rs:46
- Category: spaghetti
- Severity: P3
- Claim: `Format`, `Status`, `Rotation`, `MatchState`, `Action`, `Method`, `Kind` (39 variants, listed twice: once in `ALL`, once in `name`) and brackets' `Progress` each carry a serde `rename_all`/`rename` plus a hand-written `as_str` match and, for most, a hand-written `ALL` and `parse`, so each variant name is spelled two or three times. Two parse strategies coexist: table lookup (`Status::parse`) and a serde round trip through an allocated `serde_json::Value::String` (`MatchState::parse`, `DeliveryState::parse`). `Kind::from_full` formats up to 39 `String`s (`full()`) per lookup, and the notifier calls it at least twice per event (`accept`, `render`). `ErrorCode` has no `as_str`, so brackets' `BridgeError` Display goes through `serde_json::to_value(code)` to print it (bridge.rs:46), as does the helper (F-TOUR-4).
- Proposed change: one `macro_rules! wire_enum` in protocol (variant to string list) generating the enum, `ALL`, `as_str`, `parse`, and `Serialize`/`Deserialize` via `as_str`/`parse`, used by all of the above including `ErrorCode`. `Kind::from_full` strips `NAMESPACE` + `tournament.` and `.v1` and then looks up `name()` without allocating.
- Behavior preserved: the same strings on the wire and in the databases.
- Behavior at risk: none identified if the macro emits the exact current strings (checked by the existing round-trip tests plus one per enum over `ALL`).
- Estimated saving: about 150 lines of string tables; no per-event allocations in `from_full`.
- Confidence: medium

### F-PROTO-12: rooms.rs mixes the player room API, the provider room API and helpers other modules borrow
- File: server/ember/protocol/src/rooms.rs:65
- File: server/ember/protocol/src/rooms.rs:95
- File: server/ember/protocol/src/rooms.rs:400
- File: server/ember/protocol/src/skill.rs:9
- Category: structural
- Severity: P3
- Claim: rooms.rs is 977 lines: about 500 of code and 474 of tests. The code is three things: the signed `RoomTicket` and the player routes (`RoomSummary`, `RoomList`, `CreateRoom`, `TicketRequest`, `RoomAdmission`), the provider-connection routes (`RoomCreator`, `ConnectionCreateRoom`, `RoomState`, `ConnectionRoom`, `ConnectionRoomList`, `CloseRoom`, `join_url`, `NOT_LINKED`, `CLOSED_BY_CONNECTION`, `ENDED`), and general helpers that `skill.rs` imports from it (`is_false`, `is_player_name`). play.rs (961 lines, about 520 code) is one concept (claim, binding, prepare, permit) and needs no split beyond F-PROTO-4 and F-PROTO-9; its size is its fuzz tests.
- Proposed change: `protocol/src/rooms/{mod.rs (constants, RoomSummary, RoomList, CreateRoom, TicketRequest, RoomAdmission), ticket.rs (RoomTicket, SignedRoomTicket), connection.rs (provider route types and reasons)}`, with `is_false` moved to encoding.rs and `is_player_name` next to the room-name rule (F-PROTO-8). Move each file's tests with it.
- Behavior preserved: everything; `pub use` keeps existing paths (`rooms::ConnectionRoom`, etc.).
- Behavior at risk: none identified.
- Estimated saving: no lines deleted; three files of about 200 lines with one concept each, and skill.rs stops depending on rooms.
- Confidence: medium

### F-PROTO-13: The claim answer has no "unchanged" form, and `issued_at: now` makes every binding unique
- File: server/ember/protocol/src/play.rs:112
- File: server/ember/protocol/src/play.rs:184
- File: server/ember/bridge/src/routes/play/room.rs:216
- Category: streaming
- Severity: P3
- Claim: the helper side of this loop is F-TOUR-5 (two HTTPS round trips per renewal). On the wire types: `ClaimAnswer::Room { binding: Option<Box<SignedBinding>> }` uses `None` to mean "not bound yet", so the bridge has no way to say "your binding is still current", and it rebuilds and re-signs the binding on every claim with `issued_at: ctx.now` (bridge routes/play/room.rs:216), so each answer carries a fresh ~1 KB signed binding even when `binding_revision` has not moved, and no cache on either side can short-circuit it. Per claim the bridge computes `rules_digest` and `roster_digest` (two JCS + SHA-256), `Binding::check` recomputes the roster digest (that one is the security check, keep it), and one Ed25519 signature.
  - Payload: `ClaimAnswer::Room` with invitation (about 280 chars) and `SignedBinding` (about 0.9 to 1.1 KB). Size: about 1.3 KB per answer. Interval: every 10 s per fighter while a set is live. Fan-out: 2 fighters per match. Transport: HTTPS, bridge to helper.
- Proposed change (coordinate with the bridge and helper reviewers): add an optional `bound_revision: Option<Counter>` to `Claim` (sent only to a bridge that lists a new feature, like `CLAIM_READY_FEATURE`), and a `binding_current: bool` (skip when false) to `ClaimAnswer::Room`; when the claim's `bound_revision` equals the room's `binding_revision` and the assignment generation is unchanged, the bridge answers `binding: None, binding_current: true` and skips building and signing. Independently, the bridge can keep `issued_at` at the time the revision was created rather than `now`, which makes the signed bytes stable per revision and lets it cache the `SignedBinding`.
- Behavior preserved: a helper that does not send `bound_revision` gets exactly today's answer; the binding is still delivered whenever the revision or generation changes.
- Behavior at risk: old helpers are unaffected (they never send the field, and an old bridge never sees it because of the feature gate). If any helper logic uses binding `issued_at` as freshness, keeping it stable changes that; check sf4-net's binding acceptance before doing the second part.
- Estimated saving: about 1 KB and one Ed25519 sign plus two digests per renewal, roughly 0.1 KB/s per fighter, 0.2 KB/s per live match. Small; worth doing together with F-TOUR-5, not alone.
- Confidence: medium

### F-PROTO-14: ember-brackets and the notifier each carry a private copy of the bridge client, HTTP client and clock
- File: server/ember/brackets/src/bridge.rs:519
- File: server/ember/notifier/src/bridge.rs:122
- File: server/ember/brackets/src/http.rs:83
- File: server/ember/notifier/src/sent.rs:67
- File: server/ember/brackets/src/lib.rs:142
- File: server/ember/notifier/src/lib.rs:50
- Category: spaghetti
- Severity: P3
- Claim: `BridgeClient::answer` (send, bounded body, strict parse on 2xx, parse `ApiError` otherwise) is the same function in both crates apart from which refusal fields it keeps; `bounded_body` and `http_client` are copied (only the user agent and timeout differ); `Clock` is defined twice with the same body (brackets adds `frozen`). Both `BridgeError` enums have the same two variants.
- Proposed change: a small shared module for services that call the bridge with a provider credential (a `client` module in a new `ember-service` crate, or behind a `client` feature of ember-protocol so the helper does not compile reqwest twice): `BridgeClient { http, api, credential }` with `get/post` builders and `answer<T>() -> Result<T, Refusal>` where `Refusal { code, message, details }`, plus `bounded_body`, `http_client(user_agent, timeout)` and `Clock`. Both crates then keep only their route methods.
- Behavior preserved: requests, timeouts and error mapping (each crate maps `Refusal` to its own text).
- Behavior at risk: none identified.
- Estimated saving: about 120 lines.
- Confidence: medium

### F-PROTO-15: Site and EventSub answers are parsed into `serde_json::Value`, cloned, then parsed again
- File: server/ember/brackets/src/challonge.rs:130
- File: server/ember/brackets/src/challonge.rs:378
- File: server/ember/brackets/src/service.rs:372
- File: server/ember/notifier/src/eventsub.rs:207
- File: server/ember/notifier/src/eventsub.rs:212
- Category: overhead
- Severity: P3
- Claim: `Challonge::call` parses every answer (up to 1 MB, 25 matches a page) into a `Value`, then `parse` runs `serde_json::from_value` into the typed page; `Service::read_open_sets` clones each set's `Value` before deserializing it (service.rs:372); start.gg's `Envelope.data` is a `Value` re-parsed by `from_value`. The EventSub reader parses every frame (including keepalives) into `Frame { payload: Value }`, then for each chat notification clones `payload["event"]` and deserializes `ChatEvent`, then spawns a task, and only inside that task checks `broadcaster_user_id`, `is_room_command` and `is_from_host`. On a busy channel every chat line (hundreds a minute, each 1 to 2 KB with fragments and badges) costs a parse, a subtree clone, a typed parse and a task spawn, to be dropped by a string compare.
  - Payload: Twitch `channel.chat.message` notifications. Size: 1 to 2 KB each. Interval: per chat line on the channel, plus a keepalive every 10 s (Twitch's default; the URL does not ask for a longer one). Fan-out: one notifier. Transport: WebSocket from Twitch (bytes not reducible: Twitch has no server-side filter for this type).
- Proposed change: Challonge `call` returns the body bytes and `list`/`read_set` deserialize `Data<...>` with `serde_json::from_slice` (keep `Value` only for `error_detail`); `Service::read_open_sets` uses `WireSet::deserialize(&value)` instead of `from_value(value.clone())`. EventSub: give `Frame` a typed payload (`#[serde(default)] payload: Payload` with `session: Option<SessionInfo>` and `event: Option<Box<RawValue>>`), deserialize `ChatEvent` from the raw value, and run the three cheap checks before `tokio::spawn`. Optionally append `?keepalive_timeout_seconds=30` to the EventSub URL.
- Behavior preserved: the same frames handled, the same sets read, the same commands answered.
- Behavior at risk: a longer keepalive delays noticing a dead socket from about 12 s to about 32 s, so chat commands in that window wait for the reconnect; leave the keepalive at default if that matters.
- Estimated saving: per chat line one tree and one subtree clone and one task spawn avoided (tens of microseconds); per Challonge page one 25-match tree avoided. Small CPU; no wire change apart from the optional keepalive (6 of every 10 keepalive frames removed).
- Confidence: medium

## Area summary
- Health: good. These crates are small, pure where they claim to be, and carefully commented; the problems are duplication and a few contracts that are untyped or stricter than they need to be, not tangled control flow. Nothing here is a P1.
- Biggest judo move: give the event data and the answer types one typed, tolerant home in protocol (F-PROTO-5, F-PROTO-6). That deletes the notifier's string-key reading and brackets' 11 mirror structs together, and stops the optionality churn on answer types from growing. Close second: one `AttemptKey` view (F-PROTO-4).
- Biggest overhead item: the notifier's one-second full scan of an unindexed, never-pruned inbox (F-PROTO-1). The brackets `WAKE_SECS` tick (F-PROTO-2) is cheap; its real defect is that reads do not happen when the configuration says, and fixing it makes sub-60 s `poll_secs` spend more Challonge budget.
- Owner's named items: `WAKE_SECS` is F-PROTO-2; the EventSub keepalive and frame handling is F-PROTO-15; the inbox 1 s loop is F-PROTO-1; the interactions retry sleeps are no finding (at most three bounded sleeps inside a task spawned per `/room` command, nothing recurring); repeated serialization of the same bracket state is F-PROTO-3 (re-planning per load) and F-PROTO-13 (re-signing the same binding per claim). No "large JSON every 15 s" originates in these crates; F-BRIDGE-6 found it (`/rooms.json`).
- Decomposition: tournament.rs is two concepts and should split (F-PROTO-3); rooms.rs is three small ones (F-PROTO-12); play.rs is one concept whose size is tests.
- Could not assess: runtime numbers (nothing was run), `brackets/src/rounds.rs` and `startgg/wire.rs` beyond their outlines, and the bridge's event-data writers beyond the lines cited.
