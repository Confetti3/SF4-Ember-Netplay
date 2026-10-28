# Findings: rust/sf4-net tournament/, identity/, public_room, stdio, ipc, wire, bridge, control, main/lib

Reviewed at nightly b9ce0e31, read-only. File: lines are repo-relative.

Recurring traffic in this area, for orientation (none is large):

| What | Interval (set by the game) | Hops per send | Rough bytes per send |
|---|---|---|---|
| `match_claim` (lease renewal) | 10 s per fighter for the whole set (`src/netplay/TournamentPlay.hxx:27`) | 2 HTTPS (challenge + claim) | ~3-3.5 KB incl. headers |
| `game_prepare` while waiting for a permit | 2 s (`TournamentPlay.hxx:34`) | 2 HTTPS | ~2.5 KB |
| `match_watch` | 10 s per watcher (`src/netplay/WatchPlay.hxx:25`) | 2 HTTPS | ~2 KB |
| `assignment_list` | 60 s whenever a bridge is known, 5 s in link/restart windows (`src/ui/IdentityMatches.cxx:17-20`) | 1 HTTPS GET | 0.5-3 KB |
| Spool resend pass | 20 s (`tournament/mod.rs:116`) | blocking `read_dir`, then 1-3 HTTPS per pending report | 0 when empty |

---

### F-TOUR-1: The tournament dispatcher blocks identity and local-only requests behind the HTTP semaphore
- File: rust/sf4-net/src/tournament/mod.rs:539-562
- File: rust/sf4-net/src/tournament/mod.rs:384-397
- File: rust/sf4-net/src/tournament/mod.rs:528-538
- Category: structural
- Severity: P2
- Claim: The receive loop in `spawn_with` reads one job, and for any non-identity job awaits `shared.http.clone().acquire_owned()` before reading the next. The routing of identity jobs to the serial queue happens only when the loop reaches them, so an `identity_status`/`identity_unlock` sent while four bridge requests are in flight waits for a permit it never needs. The module doc (mod.rs:70-76) and the comment at 506-507 promise the opposite ("Identity operations run one at a time ... bridge requests run concurrently up to the HTTP bound"). The same applies to the four ops that do no HTTP at all: `BridgeList`, `BridgeForget` (blocking file write only), `MatchLeave` (mutex only) and the `bridge_request` arms that only touch the store. Permits are held for long stretches: `room_create` holds one up to 45 s (`client::CREATE_TIMEOUT`), each call can run 15 s, and the spool resend pass holds one for its whole pass (mod.rs:533-534), which with a black-holed bridge is 5 s connect timeout per pending report. A bridge outage therefore delays unrelated identity and local answers, and requests to a healthy second bridge.
- Proposed change: Route at submission, not in the loop. `Handle` gets three senders (`identity`, `local`, `bridge`) and `submit` picks by a `Request::lane()` method (an exhaustive match, see F-TOUR-11). Identity jobs go straight to the serial task; local jobs (`BridgeList`, `BridgeForget`, `MatchLeave`) run on their own small task without a permit; only bridge jobs take an HTTP permit. The loop in `spawn_with` shrinks to the bridge lane, and the re-routing block at 543-549 and the `_ => Err(invalid_request)` arms of `identity_request`/`bridge_request` become unreachable and can go once the lanes are typed.
- Behavior preserved: Identity ops stay strictly serial and in order; bridge ops stay bounded at `HTTP_CONCURRENCY`; queue-full still answers `busy`; every answer is still one `Event::Tournament`.
- Behavior at risk: Relative answer order between an identity op and a bridge op sent back to back can change (the identity answer may now come first). The native side matches answers by `request_id` (`src/session/TournamentAnswers.cxx`), so none identified beyond that.
- Estimated saving: Removes head-of-line waits of up to 45 s for identity/local ops during bridge trouble; ~10 lines and one routing concept deleted.
- Confidence: high

### F-TOUR-2: The authenticated bridge call is hand-rolled about a dozen times in five files
- File: rust/sf4-net/src/tournament/client.rs:555-664
- File: rust/sf4-net/src/tournament/play.rs:242-298
- File: rust/sf4-net/src/tournament/play.rs:578-597
- File: rust/sf4-net/src/tournament/sign_in.rs:30-46
- File: rust/sf4-net/src/tournament/sign_in.rs:113-151
- File: rust/sf4-net/src/tournament/ranking.rs:296-324
- File: rust/sf4-net/src/tournament/rooms.rs:65-116
- File: rust/sf4-net/src/tournament/watch.rs:320-328
- Category: judo
- Severity: P2
- Claim: There is no canonical "call the bridge under a session" helper, so each module rebuilds it:
  - The proven call (`prove` then `client.call` then `answer`) exists as inline code in `client::claim`, `client::cancel_claim`, `client::remove_link`, as `play::send_proven`/`play::proven` (POST only), and as `sign_in::proven` (with its own `Method` to `reqwest::Method` mapping and an empty command).
  - The session GET (`call(GET, format!("{}{path}", origin), Some(&token), None)` then `answer`) is inline in `client::links`, `play::assignments`, `sign_in::status`, `rooms::list`, and `ranking::call`.
  - Refusal reading has four flavours: `answer`, `feature_answer`, `watch::reply` (refusal with reasons, no "missing"), and `rooms::reply`/`ranking::reply` wrappers, plus local `invalid_response()`, `parse()` and `to_json()` copies in rooms.rs and ranking.rs.
  - In `client::remove_link` the HTTP verb is passed twice (`Method::Delete` to `prove`, `reqwest::Method::DELETE` to `call`), so the proof and the request can disagree. `sign_in::proven` derives one from the other; the others do not.
  - `with_session` takes `F: Fn`, so every caller clones its command/body/claim inside the closure (play.rs:320, 386, 433-434; watch.rs:277; play.rs:582-583; ranking.rs:304-308; rooms.rs:182) for a retry that happens only on `unauthenticated`.
- Proposed change: One request type in client.rs:
  `struct BridgeCall<'a> { method: Method, path: Cow<'a, str>, body: Body<'a>, expect: &'a [u16], refusals: Refusals<'a> }` where `Body` is `None | Json(&'a [u8]) | Proven { action, command: &'a Value }`, and `Refusals { reasons: &[&str], missing: Option<&str> }` covers `answer`/`feature_answer`/`watch::reply`. A single `client::send<T: DeserializeOwned>(shared, bridge_id, call) -> Result<T, Failure>` does `with_session`, proves when asked (deriving the `reqwest` verb from the one `challenge::Method`), sends, and decodes (F-TOUR-3). `with_session` takes `FnMut(&Approved, &str)` over borrowed inputs so no caller clones. `links`, `claim`, `cancel_claim`, `remove_link`, `assignments`, `sign_in::{status, remove, cancel}`, `ranking::{standing, choose, board}`, `rooms::list`, `watch::{watch_match, unwatch_match}` and `deliver_one` each become a `BridgeCall` literal plus their checks.
- Behavior preserved: Same routes, same proofs (same action, method, path, digest), same session reuse and single retry on `unauthenticated`, same failure codes reaching the game.
- Behavior at risk: none identified; the refusal mapping must keep `feature_answer`'s 404 rule exactly (`rooms_unavailable`, `ranking_unavailable`) and `watch::reply`'s lack of one.
- Estimated saving: ~150-200 lines across client.rs, play.rs, sign_in.rs, ranking.rs, rooms.rs, watch.rs; removes the double-verb hazard; removes ~10 per-attempt clones.
- Confidence: high

### F-TOUR-3: Every bridge answer is converted through four or five JSON trees before it reaches the pipe
- File: rust/sf4-net/src/tournament/client.rs:318-325
- File: rust/sf4-net/src/tournament/rooms.rs:110-116
- File: rust/sf4-net/src/tournament/rooms.rs:195-215
- File: rust/sf4-net/src/tournament/ranking.rs:315, 326-330
- File: rust/sf4-net/src/tournament/play.rs:327-329, 391-393, 446-447
- File: rust/sf4-net/src/tournament/mod.rs:364, 584-614
- File: rust/sf4-net/src/service/protocol.rs:443-453
- Category: boundary
- Severity: P2
- Claim: `answer()` does `json::parse` (strict `json::Value`, a BTreeMap tree), then `json::from_value::<serde_json::Value>`, which is `value.to_serde()` (second tree) followed by `serde_json::from_value` (third tree, a full rebuild of a `Value` from a `Value`). Typed callers then `serde_json::from_value::<T>` (fourth), run `check()`, and turn `T` back into a `serde_json::Value` with `to_value`/`json!` (fifth) because `Outcome` is `Option<Value>`; the writer then serializes the event. `rooms::create` goes further: the closure returns `to_json(&room)` and the caller immediately `parse::<RoomSummary>`s it back (rooms.rs:210, 215). Pass-through answers (`link_list`, `assignment_list`, sign-in `remove`/`cancel`) build three trees only to re-emit the same JSON. `Outcome = Result<Option<Value>>` is the loose type that forces the round trips; it also hides each op's answer shape from the reader.
- Proposed change: (a) `client::read<T>(status, body, expect, refusals) -> Result<T, Failure>` = `json::parse_as::<T>` on success (strict tree, then serde tree via `to_serde()`, then `T`; the middle step goes only with (e)), used by every typed caller, deleting `rooms::parse`, `rooms::to_json`, `ranking::to_json` and the `from_value` at each call site. (b) For pass-through ops, validate with `json::parse` and forward the original bytes. (c) Change `Event::Tournament.data` to `Option<Box<serde_json::value::RawValue>>` and `Outcome` to `Result<Option<Box<RawValue>>, Failure>`, filled by `to_raw_value(&T)` (or the validated body for pass-through), so the event writer copies the bytes once. (d) `rooms::create`'s closure returns `RoomSummary`, not `Value`. (e) Longer term in `server/ember/protocol/src/json.rs`, `from_value` could deserialize from the strict tree directly instead of through `to_serde()`; that is a protocol-crate change and benefits the bridge too.
- Behavior preserved: The same strict checks (duplicate keys, fractions, unsafe integers, depth, trailing data) run on every bridge body, the same `check()` validations run, and the game gets the same JSON members.
- Behavior at risk: Member order inside `data` changes from serde_json's sorted map order to struct declaration order (no `preserve_order` feature in rust/sf4-net/Cargo.toml:32). The native side reads with nlohmann `find`/`value` (`src/session/IdentityEvents.cxx`, `TournamentAnswers.cxx`), so order is irrelevant. Helper and game ship together (docs/design/identity-bridge/STATUS.md, "Helper IPC commands use deny_unknown_fields ... Helper and game ship together, so this branch adds no negotiation"), so no IPC compatibility window exists. The bridge wire is untouched.
- Estimated saving: 2 of 5 tree builds per typed answer with (a)-(d), 3 with (e); pass-through answers drop to one strict tree. Timings below are not measured. Per 10 s claim ~1.2 KB body: a few microseconds; per room list (up to 100 rooms, ~35 KB): roughly 0.3-0.6 ms of allocation and copying saved. Small in CPU; the real gain is ~30 lines and a typed answer per op.
- Confidence: high

### F-TOUR-4: `Failure(String)` makes every error a free string literal, and four places branch on them
- File: rust/sf4-net/src/tournament/mod.rs:343-362
- File: rust/sf4-net/src/tournament/client.rs:161, 532
- File: rust/sf4-net/src/tournament/rooms.rs:58
- File: rust/sf4-net/src/tournament/sites.rs:95
- File: rust/sf4-net/src/tournament/play.rs:563-574
- Category: boundary
- Severity: P2
- Claim: The module has ~120 `Failure::new("...")` sites with 33 distinct literals (31 of them `"internal"`, 16 `"invalid_request"`, 11 `"bridge_invalid_response"`). Control flow depends on string equality in four places (`code == "bridge_mismatch"`, `code == "unauthenticated"`, `failure.0 == "invalid_request"`, `"not_a_bridge"`), and `play::settled` is a hand-kept string list of bridge codes. A typo in any of these compiles and silently changes behavior (for example a retry that never happens). `From<identity::Failure>` serializes the enum to a `serde_json::Value` just to read its string back (mod.rs:347-356), and `client::refusal` does the same for `ApiError.code` (client.rs:356-361). About 25 sites repeat `.lock().map_err(|_| Failure::new("internal"))` although `play_state` and `client::approved` exist for exactly that; `prepare_game`, `report_game`, `accept_binding`, `accept_permit` and `forget` inline it instead.
- Proposed change: `enum Failure { Local(Code), Bridge(String) }` where `Code` is a `#[serde(rename_all = "snake_case")]` enum of the helper's own codes with `as_str()`, and `Bridge` carries the bridge's code or reason as today. The four string comparisons and `settled` become matches on `Code` or on a bridge `ErrorCode` enum from `ember_protocol::api`. `From<identity::Failure>` maps variants directly. One `fn lock<T>(m: &Mutex<T>) -> Result<MutexGuard<T>, Failure>` replaces the inline poison mapping.
- Behavior preserved: The strings on the IPC wire (`reason`) stay byte-identical.
- Behavior at risk: none identified
- Estimated saving: Removes two serialize-to-read-a-string round trips and ~25 repeated closures; makes the retry and "settled" decisions checkable by the compiler.
- Confidence: high

### F-TOUR-5: Each 10 s claim, 2 s prepare and 10 s watch costs two HTTPS round trips plus a re-verified binding
- File: rust/sf4-net/src/tournament/client.rs:386-453
- File: rust/sf4-net/src/tournament/play.rs:160-194, 303-333
- File: rust/sf4-net/src/tournament/watch.rs:257-286
- Category: overhead
- Severity: P2
- Claim: Every proven operation first POSTs `/v1/auth/challenges` (request ~250 B, `Challenge` answer ~550 B: 12 fields including bridge_id, audience, challenge_id, ember_id, path, digest, nonce), signs it, then POSTs the command with the proof (~450 B), so the lease renewal is 2 requests, 1 Ed25519 sign, and on the answer (room + ~280-char invitation + signed binding ~1 KB) 1 Ed25519 verify. `prove` also serializes the command four times: the caller's `serde_json::to_value`, `json::to_value` (serde then strict), `canonical()` for the digest, and `ProvenRequest::to_json` (canonical again). `accept_binding` verifies the binding signature every time, even when the signed binding is identical to the one already held for `match_id` with the same `(assignment_generation, binding_revision)`.
  - Payload: challenge + proven claim + claim answer. Size: ~3-3.5 KB per renewal with HTTP headers and bearer token. Interval: 10 s per fighter for the whole set; prepare every 2 s while a permit is pending; watch every 10 s per watcher. Fan-out: per fighter and per watcher; the bridge sees 2 x (fighters + watchers) / 10 s requests (64 concurrent fighters = ~13 req/s). Transport: HTTPS to the bridge.
- Proposed change: Helper-only, no wire change: in `accept_binding`, skip `verify` when the incoming `SignedBinding` equals the held one byte-for-byte (keep a `HashMap<match_id, SignedBinding>` beside `bindings`), and build the claim command once per `claim_match` rather than per attempt. Wire change, feature-gated: a bridge that lists a new feature (named like `CLAIM_READY_FEATURE`, say `claims.renew`) accepts a renewal of an already-proven claim under the session token alone (same endpoint_id, helper_instance_id and build), which halves the requests. The helper sends it only when `play::features` reports the flag, so old bridges never see the route; old helpers keep sending proven claims, which a new bridge still accepts. This needs a security review against spec section 14: today the claim proof binds the endpoint to the identity on every renewal, while public room tickets already rely on the session alone (rooms.rs:5-9).
- Behavior preserved: Claim cadence and the lease/permit timing stay as the native state machine sets them; the binding checks still run on every new binding.
- Behavior at risk: The wire option weakens per-renewal proof binding; it must keep the first claim proven. The helper-only part has none identified.
- Estimated saving: (Byte sizes estimated from the struct fields, not captured.) Helper-only: one Ed25519 verify and two JSON trees per 10 s per fighter (tens of microseconds). With the feature: ~1 KB and one HTTPS round trip per renewal, about 0.1 KB/s per fighter and half the bridge's claim request rate.
- Confidence: medium

### F-TOUR-6: Each IPC and control frame is written with four writes and a flush
- File: rust/sf4-net/src/wire.rs:77-96
- File: rust/sf4-net/src/wire.rs:40-61
- File: rust/sf4-net/src/service/entry.rs:61-79
- Category: overhead
- Severity: P2
- Claim: `write_frame` calls `write_u32`, `write_u16`, `write_u64`, `write_all(payload)` and `flush` on an unbuffered stream. On the Windows named pipe (tokio over mio), mio allows one overlapped write in flight, so each small header write is its own `WriteFile` and the next write waits for its IOCP completion; on the Linux stdio helper each is a separate `write(2)`; on QUIC control streams (transport.rs:140, 193) each is a separate quinn stream write. This applies to every event the helper sends the game (`Message`, every `CheckpointChunk` of the ~460 KB a member imports per commit (docs/perf/ROOM_PERF_BASELINE.md), `Statistics`, `MemberLatency`, `HelperLoad`, tournament answers) and to the stdio duplex in F-TOUR-7. The writer task also serializes each event into a fresh `Vec` that is then written behind a separately written header.
  - Payload: any frame. Size: 14-byte header + payload (up to 512 KB). Interval: every event. Fan-out: every frame on pipe, stdio and QUIC control. Transport: named pipe / stdout / QUIC.
- Proposed change: `write_frame` builds one buffer: reserve 14 bytes, append the payload, fill the header, one `write_all`. Better for the IPC writer: add `wire::encode_ipc(message_id, |buf| serde_json::to_writer(buf, &event))` that serializes straight after a reserved header in one `Vec`, so the event is copied once and written once. Keep the read side as is (reads are buffered by mio).
- Behavior preserved: Byte-identical frames on every transport.
- Behavior at risk: none identified
- Estimated saving: 3 of 4 write calls (and on the pipe, 3 IOCP round trips) per frame; one payload-sized copy per event if the payload is written directly. For a 16-member room commit that sends tens of checkpoint chunks per member, that is on the order of 100 fewer pipe writes per commit per member. Not measured; the IOCP part relies on mio allowing one outstanding named-pipe write.
- Confidence: medium

### F-TOUR-7: The stdio helper re-frames every command through an in-process duplex just to turn EOF into Shutdown
- File: rust/sf4-net/src/stdio.rs:99-152
- File: rust/sf4-net/src/service/entry.rs:31-58
- Category: judo
- Severity: P2
- Claim: `serve_on_port` spawns `forward`, which `read_ipc`s each frame from stdin (allocates and validates), `write_ipc`s it into a 64 KB `tokio::io::duplex` (four writes, F-TOUR-6), and the service's reader then `read_ipc`s it again (second allocation, second validation) before parsing. The only reason is EOF handling: the forwarder converts end of input into a synthetic `{"type":"shutdown"}` frame with `last_id + 1` and then parks forever so the service does not see a closed feed. On the VPS room host every command from `sf4e-room-host` (including every per-member `Send` of a room snapshot, ~164 KB per commit across members per ROOM_PERF_BASELINE.md) pays the extra copy, and the helper carries an extra task, a duplex buffer and a frame-ID fabrication rule.
- Proposed change: Give `service::run_on_port` an input policy, `enum OnEof { LostIpc, Shutdown }`. The reader loop in entry.rs, on `UnexpectedEof` with `OnEof::Shutdown`, submits `Command::Shutdown` itself (it already holds `last_id`). `stdio::serve_on_port` then passes `tokio::io::join(input, output)` directly; `forward`, `FEED_BUFFER`, `SHUTDOWN` and the duplex go.
- Behavior preserved: A clean stop on EOF (including EOF inside a frame, the `input_that_ends_inside_a_frame_also_stops_cleanly` case), failure on a malformed frame, the "first ID above 1" rule, and the Windows pipe path (`OnEof::LostIpc`) unchanged.
- Behavior at risk: The shutdown must still win over the lost-IPC path that `failed_ipc.send(true)` triggers after the loop; the reader has to submit Shutdown and not signal failure on that branch. Owned by the service area; coordinate with that reviewer.
- Estimated saving: One frame read, one payload copy and four writes per command on Linux room hosts; ~35 lines and one task deleted.
- Confidence: high

### F-TOUR-8: The report spool resend keeps trying every report on an unreachable bridge and parses every file to count them
- File: rust/sf4-net/src/tournament/play.rs:576-620
- File: rust/sf4-net/src/tournament/spool.rs:68-115
- File: rust/sf4-net/src/tournament/mod.rs:524-538
- Category: overhead
- Severity: P2
- Claim: (a) `deliver_pending` sends every pending report in turn and ignores transport failures, so with the bridge unreachable a pass makes one failing request per report (each up to the 5 s connect timeout, plus a session attempt), while holding one of the four HTTP permits for the whole pass (F-TOUR-1). (b) `Spool::put` calls `self.pending()` to enforce `MAX_PENDING`, which reads and strictly parses every pending file (up to 256 x 16 KB) to get a count. (c) A report just written by `report_game` is sent by `deliver_one` while the 20 s pass may pick the same file up and send it again (harmless thanks to the bridge's idempotency on the observation, but a wasted request). (d) An empty pass still costs a `spawn_blocking` and a `read_dir` every 20 s.
  - Payload: signed report ~1.5-2 KB. Interval: 20 s. Fan-out: per pending report. Transport: HTTPS.
- Proposed change: In `deliver_pending`, stop sending to a bridge after its first transport failure (`bridge_unreachable`, `bridge_response_too_large`, `http_5xx`) in the pass; order within a bridge is kept since the oldest is tried first. Count with `read_dir` filtered by `PREFIX` in `put` (no parse). Keep an in-memory `HashSet` of observation IDs in flight, checked by `deliver_one`, so the pass skips a report that `report_game` is sending. Leave the empty pass as it is: the spool folder is per Windows user, so a second helper's pass is what resends reports another helper left behind, and an in-memory flag would miss those.
- Behavior preserved: Reports are still written durably before `saved: true`, retried every 20 s oldest first until the bridge answers, and dropped on the same settled codes.
- Behavior at risk: A report to a bridge that fails transiently for the first entry and would have succeeded for the second waits one more pass (20 s). The in-flight set is per helper, so two helpers under one user can still both send a report, as today.
- Estimated saving: During an outage with N pending: N-1 failing requests and up to (N-1) x 5 s of permit time per pass; `put` drops from N file reads and parses to one directory listing.
- Confidence: high

### F-TOUR-9: Every tournament answer carries the full identity status, even when it has not changed
- File: rust/sf4-net/src/tournament/mod.rs:584-614
- File: rust/sf4-net/src/identity/mod.rs:204-224
- Category: overhead
- Severity: P3
- Claim: `respond` computes `identity.status()` (including the fingerprint) and attaches it as a `serde_json::Value` to every `Event::Tournament`, including each 10 s claim, 2 s prepare and 10 s watch answer. It is ~250-300 B (state, ember_id, fingerprint, public_key, backend, created_at, passphrase_required). It also uses `try_lock`, so whether a given answer carries it depends on timing.
  - Payload: identity `Status`. Size: ~250-300 B. Interval: every tournament answer (about 0.1-0.6 per second during a set). Fan-out: 1. Transport: pipe.
- Proposed change: Keep the last status sent in `Shared` (`Mutex<Option<Status>>`) and attach `identity` only to `identity_*` answers and when it differs from the last one sent. Serialize it directly (RawValue, F-TOUR-3) rather than via `to_value`.
- Behavior preserved: The native side keeps its last identity view when an event has no `identity` member (`src/session/IdentityEvents.cxx:35-44` only updates on presence), which the current `try_lock` omission already relies on.
- Behavior at risk: none identified; helper and game ship together (STATUS.md), so no negotiation is needed.
- Estimated saving: ~25-150 B/s on the pipe during a set and one status build plus Value tree per answer. Small.
- Confidence: high

### F-TOUR-10: Four hand-written "temp file, sync, rename" writers with different durability
- File: rust/sf4-net/src/identity/mod.rs:569-588
- File: rust/sf4-net/src/identity/mod.rs:793-812
- File: rust/sf4-net/src/tournament/bridges.rs:416-419, 457-477
- File: rust/sf4-net/src/tournament/spool.rs:81-95
- Category: spaghetti
- Severity: P3
- Claim: `identity::write_temp` + `platform::install_new`, `identity::write_replacing` (uses `platform::replace` with `MOVEFILE_WRITE_THROUGH`), `bridges::write_atomic` (uses `fs::rename`, no write-through) and the closure in `Spool::put` (`File::create` + `fs::rename`, no write-through) are the same operation written four times with three different temp-name schemes and two different rename guarantees. The report spool, whose whole point is surviving a crash before the bridge answers, gets the weaker rename.
- Proposed change: One `durable::replace(dir, name, bytes)` (create_new temp, write, `sync_all`, `platform::replace`, remove temp on error) in a small crate-level module, used by bridges.rs and spool.rs and by `identity::write_replacing`. Identity's key install keeps `install_new` and its fault-injection steps.
- Behavior preserved: File names and contents; atomic replace semantics.
- Behavior at risk: bridges.json and spooled reports gain write-through on Windows (strictly more durable, slightly slower on a rare path).
- Estimated saving: ~35 lines; one durability rule instead of three.
- Confidence: high

### F-TOUR-11: The request enum restates serde's tag by hand and routes by a string prefix; sign-in carries a one-off boolean
- File: rust/sf4-net/src/tournament/mod.rs:295-341
- File: rust/sf4-net/src/tournament/mod.rs:710, 871
- File: rust/sf4-net/src/tournament/sign_in.rs:54-82
- File: rust/sf4-net/src/tournament/mod.rs:787-795, 803-806
- Category: spaghetti
- Severity: P3
- Claim: `Request::op()` is a 37-arm match that repeats the `rename_all = "snake_case"` names, and `is_identity()` is `self.op().starts_with("identity_")`, so routing depends on a naming convention. Because one flat enum serves two dispatchers, `identity_request` and `bridge_request` each need an unreachable `_ => Err(invalid_request)` arm. `sign_in::connect` takes `end_unopened: bool` that is `false` for Discord and `true` for sites; it and `is_sign_in_page` are properties of the sign-in, not arguments.
- Proposed change: Replace `is_identity` with an exhaustive `fn lane(&self) -> Lane` (the F-TOUR-1 routing), so a new variant cannot be routed by accident. Keep `op()` but generate it (for example `strum::IntoStaticStr` with `serialize_all = "snake_case"`, or a small `macro_rules!` that declares variant and op name once). Move `end_unopened` and the page check into `sign_in::Routes` (`discord::routes()` and `sites::routes()` fill them). Do not use `#[serde(untagged)]` to split the enum: it buffers the passphrase into serde's internal content outside `Zeroizing`.
- Behavior preserved: Same op strings, same parse strictness, same Discord vs site cancel behavior.
- Behavior at risk: none identified
- Estimated saving: ~40 lines; removes two unreachable arms and a string-prefix routing rule.
- Confidence: medium

### F-TOUR-12: public_room.rs has a forwarding wrapper and near-duplicate connect functions
- File: rust/sf4-net/src/public_room.rs:205-342
- File: rust/sf4-net/src/public_room.rs:390-430
- File: rust/sf4-net/src/public_room.rs:75-88
- Category: judo
- Severity: P3
- Claim: `PublicHost` holds exactly one field, `policy: AdmissionPolicy`; its `decide` and `is_banned` forward to the policy and `policy()` clones it. `connect_public`, `connect_public_at`, `connect_public_to` and the test-only `connect_public_on` each build the same `PublicRoomProof::new(invite.proof(), ticket.clone())` and differ only in the transport call. `hex()` duplicates `recovery::hex_digest` (which also allocates a `String` per byte), and the crate has five private `fn now()` (identity/mod.rs:821, public_room.rs:83, service/mod.rs:524, tournament/client.rs:51, transport.rs:56).
- Proposed change: Make the mutators (`admit_member`, `sync_members`, `ban`) methods of `AdmissionPolicy` and delete `PublicHost`, or keep the name as a type alias. Fold the connect variants into one `connect_public(endpoint, invite, ticket, Target)` with `enum Target { Invitation, Address(EndpointAddr), Direct(EndpointAddr) }`. One crate-level `hex` and `unix_now`.
- Behavior preserved: Admission decisions, copy-on-write ban and seat sets, wire proofs.
- Behavior at risk: none identified
- Estimated saving: ~40 lines; one concept (`PublicHost`) removed.
- Confidence: medium

### F-TOUR-13: Each bridge request clones the whole trusted-bridge list to find one entry
- File: rust/sf4-net/src/tournament/bridges.rs:171-195
- File: rust/sf4-net/src/tournament/client.rs:366-381
- Category: overhead
- Severity: P3
- Claim: `client::approved` runs on every bridge request (every claim, prepare, watch, list) and calls `Store::get`, which builds `list()` (cloning every approved and built-in `Approved`, each with up to 8 keys, into `Listed`) and then clones the match again.
  - Payload: in-memory `Approved` clones. Size: up to ~24 structs. Interval: every bridge request (claims every 10 s, prepares every 2 s, polls). Fan-out: 1. Transport: none (in process).
- Proposed change: `Store::get` searches `live()` then `approved` (skipping approvals a live pin covers, as `list` does) and clones only the hit.
- Behavior preserved: Same lookup result, same pin precedence.
- Behavior at risk: none identified
- Estimated saving: Up to ~24 struct clones per request; microseconds. Worth doing only alongside F-TOUR-2.
- Confidence: high

### F-TOUR-14: The 60 s assignment poll downloads the full list every time
- File: rust/sf4-net/src/tournament/play.rs:284-298
- File: src/ui/IdentityMatches.cxx:17-20, 139-144
- Category: streaming
- Severity: P3
- Claim: Whenever the player has a bridge and is not on the matches screen, the game asks for `assignment_list` every 60 s (every 5 s in link and restart windows), and the helper always GETs `/v1/assignments` in full and forwards it, even when nothing changed.
  - Payload: assignment list JSON. Size: ~0.5-3 KB typical, 64 KB cap. Interval: 60 s (5 s in windows). Fan-out: per player with a bridge. Transport: HTTPS, then pipe.
- Proposed change: Conditional GET. The helper keeps the last body and `ETag` per bridge and sends `If-None-Match`; on 304 it answers the game with the cached body. A bridge that sends no `ETag` is never sent `If-None-Match`, so old bridges see the same request; old helpers never send it, so a new bridge answers them in full. Bridge side (server/ember/bridge) needs a cheap ETag (e.g. per-player assignments revision).
- Behavior preserved: The game receives the same answer every poll.
- Behavior at risk: A stale cache if the bridge's ETag does not cover every field the list shows; the ETag must be derived from the response content or a revision bumped on every assignment change.
- Estimated saving: Per player ~10-50 B/s down; at the bridge, the list build and serialization for unchanged lists (most polls). Small per player, proportional to online players at the bridge.
- Confidence: medium

## Area summary
- Overall health is good: the tournament and identity code is careful about trust, strict JSON and crash safety, and every file is under 900 lines (largest: play.rs 869 with ~230 lines of tests, identity/mod.rs 828, mod.rs 804), so no decomposition finding is warranted. bridge.rs (GGPO forwarding) and control.rs are lean; bridge.rs allocates one small `Vec` per outgoing GGPO datagram, not worth touching.
- The weakness is plumbing: the authenticated bridge call is rebuilt per module (F-TOUR-2), answers travel as untyped `serde_json::Value` through 4-5 trees (F-TOUR-3), and errors are free strings (F-TOUR-4). Doing F-TOUR-2 + F-TOUR-3 together is the biggest judo move: one `BridgeCall`/`send::<T>` path, typed answers, RawValue on the pipe, ~200 lines gone.
- Biggest structural risk: head-of-line blocking in the dispatcher (F-TOUR-1), which delays identity and local answers during bridge trouble.
- Biggest overhead item in this area is modest: two HTTPS round trips per 10 s claim per fighter (F-TOUR-5, ~0.35 KB/s and 0.2 req/s each). Nothing here is "a large JSON every 15 s"; all recurring sends are under ~3.5 KB and 2-60 s apart. The large recurring payloads are elsewhere: room checkpoints over the pipe (~460 KB decoded per member per commit, service/recovery area; F-TOUR-6 reduces only their write syscalls) and the bridge's 5 s supervisor poll (server/ember-rooms).
- `to_vec_pretty` (identity/mod.rs:514, 705; identity/envelope.rs:173; bridges.rs:417) runs only on enable, import, export, approve or forget, a handful of times per install. Not an overhead; leave it (backup files are meant to be readable).
- IPC: per command the Windows path does one frame read, one strict parse and a zeroize of the payload (service/entry.rs:41, needed only for tournament commands; a memset, negligible). The Linux stdio path adds a second frame read/copy/write (F-TOUR-7). Events cost one `to_vec` plus four writes (F-TOUR-6). Room `Send` and `Message` payloads are JSON strings escaped inside JSON (service/protocol.rs:59-63, 320-325) and there is no multi-peer send command; that belongs to the service reviewer.
- Not assessed: actual byte captures and timings (no builds or runs allowed); estimates come from struct fields and the game's interval constants. The bridge server side of F-TOUR-5 and F-TOUR-14 was not reviewed.
