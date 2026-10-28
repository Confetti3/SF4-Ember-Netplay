# Findings: test code (src/tests, rust/sf4-net tests, server/* tests, sdk/typescript/test, CTest registration)

Reviewed at nightly b9ce0e31. Read-only; nothing was built or run, so no wall-time or flake-rate numbers are measured. Line numbers are at that head.

### F-TEST-1: service/tests.rs is a 7384-line catch-all that doubles as the prelude for its own submodules
- File: rust/sf4-net/src/service/tests.rs:1
- File: rust/sf4-net/src/service/tests.rs:7378
- File: rust/sf4-net/src/service/tests/public_support.rs:1
- Category: size
- Severity: P1
- Claim: When service.rs was split into modules (0a9a25d9, 2026-09-21), tests.rs came out at 2982 lines. In five weeks it has grown to 7384 lines: 88 tests, 9 fixture types, and about 25 helpers, all at one level. The seven submodules that do exist (join_deadline, match_watch, membership_retirement, public_admission, public_rooms, public_support, server_owned) all start with `use super::*`, so every fixture in this file (ReservationFixture, ProbeSide, GameSide and so on) is visible everywhere and nothing marks which fixture belongs to which scenario. Two of the submodules are tiny (membership_retirement.rs has 57 lines, join_deadline.rs has 94), while closely related tests stay in the giant file. Picking one scenario family to retest means grepping a 7k-line file. The seams are already visible from the fixtures:
  - support (stays in tests.rs or moves to tests/support.rs): `endpoint` 169, `address` 180, `assert_public_route` 184, `assert_no_address` 195, `phase` 206, `next` 211, `test_actor` 229, `test_relay` 4536, `joined_invite` 4638, `next_error` 4649. public_support.rs already shows the house pattern.
  - `tests/wire.rs` (event privacy and frame bounds): 55-165 and 7264-7378 (`nested_json`, the native nesting-limit tests), plus `ipc_service_rejects_malformed_and_replayed_commands` 4504.
  - `tests/tick.rs`: 296-350.
  - `tests/checkpoints.rs`: 351-820 (four-credit window, commit marker, export in flight, exact retry, bulk events), 3060-3356 (`held_checkpoint_and_admission_writes...`), 3704-3934 (`service_replays_committed_checkpoint...`).
  - `tests/invitations.rs`: 820-1012 (control replacement, invitation joins, one-week lifetime, renewal, refused epoch).
  - `tests/admission.rs` (BoundHost, `led_host`, `bound_host`, `send_admission`): 1161-1776, 2485-2929 (including the voter and native-rebound membership rules), 3356-3583. This absorbs membership_retirement.rs.
  - `tests/departure.rs` (LearnerFollower): 1012-1161, 2929-3060, 4829-5088.
  - `tests/probes.rs` (ProbeSide, `probe_completion`): 1907-2485, 3583-3704, 5088-5201.
  - `tests/games.rs` (GameSide, `game_connection_pair`, `form_game_link`): 5201-5431, 3934-4504 (control routing, a 16-member room), 1776-1907 (`incomplete_game_marker_does_not_block_actor_commands`).
  - `tests/reservations.rs` (ReservationFixture, PEER_CLOCK_OFFSETS): 5431-6148.
  - `tests/dials.rs` (`unconfirmed_dial`, the parked or replacement control, `flushed_departure_behind_more_than_one_poll_budget`): 6148-6749.
  - `tests/short_links.rs` (`short_service`, `complete_short`, `short_join`, `leads`, `led_by`, `pump_adopt`, `stored_link`): 6749-7264.
  - `tests/join_failures.rs`: 4536-4829. This absorbs join_deadline.rs.
  
  A smaller copy-paste problem rides along. `test_relay()` exists at 4536, but 20 other call sites inline `iroh::defaults::prod::default_relay_map().urls::<Vec<_>>().remove(0)` (21 occurrences in the file).
- Proposed change: Move the bodies verbatim into the modules above as sibling `mod` files under service/tests/. Keep tests.rs as fixtures plus the `mod` list (target under 400 lines) so the existing `use super::*` imports keep compiling. Fixture structs move with their only users (ReservationFixture goes to reservations.rs, for example) and are `pub(super)` only when a second module needs them. Replace the 20 inline relay lookups with `test_relay()`.
- Behavior preserved: Every test body and assertion stays the same. Only test paths change (for example `service::tests::reservations::...`).
- Behavior at risk: none identified. Test names in any saved filter strings change prefix.
- Estimated saving: tests.rs goes from 7384 to about 350 lines. 12 modules of 200-800 lines each. About 60 lines from the relay dedup. `cargo test service::tests::probes` becomes a meaningful targeted retest.
- Confidence: high

### F-TEST-2: Test-only `impl Actor` re-implements voter selection and retirement confirmation, and the tests check that copy instead of the production code
- File: rust/sf4-net/src/service/tests.rs:4
- File: rust/sf4-net/src/service/tests.rs:2485
- File: rust/sf4-net/src/service/tests.rs:3007
- File: rust/sf4-net/src/service/members.rs:595
- File: rust/sf4-net/src/service/members.rs:439
- File: rust/sf4-net/src/service/members.rs:301
- Category: boundary
- Severity: P1
- Claim: tests.rs:4-52 adds `#[cfg(test)] fn stable_voters` and `async fn apply_pending_retirements` to `Actor`. Production never calls either. Both first appear in 5a16c0fe (0.8.0), where `stable_voters` was already `#[cfg(test)]`, and both moved here with the 0a9a25d9 module split.
  - `stable_voters_never_reselect_pending_or_retired_incarnations` (2485) checks `actor.stable_voters(...)`. The production selector is the pure function `admission_voters(current, candidates, desired, excluded, may_vote)` (members.rs:595), called at members.rs:439 with `excluded = pending_retired || retired`. The test-only copy also drops current voters that are missing from `admissions`, which production does not do, so the two are not even the same rule. Worked by hand: `admission_voters({1,2,3}, [1,2,3,4], 3, |id| id==2||id==3, |_| true)` gives `{1,4}`, the same result the test asserts. The pinned invariant can therefore be checked against production directly.
  - `apply_pending_retirements` (35) computes "confirmed = pending minus `applied_membership_provenance().applied`", then retires and applies. Production computes "pending minus `applied_member_ids()`" inside the spawned membership operation (members.rs:301-308), then applies it in `completed_membership_reconciliation` (members.rs:483-505), which also calls `close_retired_strangers`. `a_full_retirement_set_makes_room_by_dropping_an_aged_out_tombstone` (3007) uses the shortcut, so it never runs the production completion path, and it reads a different source for the applied set.
- Proposed change: Make `admission_voters` `pub(super)` and rewrite the 2485 test to call it with the actor's sets: `admission_voters(&current, actor.admission_order.clone(), 3, |id| actor.pending_retired_incarnations.contains(&id) || actor.retired_incarnations.contains(&id), |_| true)`. Drive 3007 through the real `start_membership_reconciliation` and the existing `complete_next(&mut actor)` helper (tests.rs:2919), which already pumps one completion. Then delete tests.rs:4-52.
- Behavior preserved: Both invariants stay asserted with the same values (`{1,4}`; tombstone aging with `MAX_RETIRED_INCARNATIONS`). Production code is untouched apart from one visibility change.
- Behavior at risk: none in production. If the 3007 rewrite through the real operation fails, that is a finding about production, not a weaker test.
- Estimated saving: 49 lines deleted. One duplicated statement of the voter and retirement rules removed. Two tests that currently pass whatever production does become real regression tests.
- Confidence: high

### F-TEST-3: The "real bridge on loopback" harness is hand-built five times, three of them as full `Config` struct literals
- File: server/ember/bridge/tests/common/mod.rs:71
- File: rust/sf4-net/tests/public_rooms_support/mod.rs:210
- File: rust/sf4-net/tests/tournament_support/mod.rs:68
- File: server/ember/brackets/tests/support/bridge.rs:46
- File: server/ember/notifier/tests/notifier.rs:325
- File: server/ember/bridge/tests/blumint_reports.rs:296
- File: server/ember/bridge/tests/blumint_command.rs:78
- File: server/ember/bridge/tests/deploy.rs:32
- File: server/ember/bridge/tests/partner.rs:462
- Category: structural
- Severity: P1
- Claim: Five harnesses each repeat the same steps: random dir, then `TcpListener::bind("127.0.0.1:0")`, then `Config { ... }`, then `Db::open`, `Keys::generate`, `AppState::new`, `sync_config`, `ember_bridge::start`. They are bridge/tests/common, sf4-net public_rooms_support, sf4-net tournament_support, brackets support/bridge.rs, and notifier.rs. The bridge, sf4-net and notifier copies spell out every `Config` and `config::Connection` field (`match_expiry_hours: config::DEFAULT_MATCH_EXPIRY_HOURS` appears 5 times outside src), so every new Config field means editing several crates' tests. The brackets copy dodges that by building from JSON, and its comment ("so new optional settings need no change here") states the problem. Inside the bridge crate itself, blumint_reports.rs (296, 343), blumint_command.rs, deploy.rs, partner.rs and common/blumint.rs build `AppState::new` again by hand instead of going through `common::Bridge`. The temp-dir type is repeated too: `Dirs` twice in sf4-net, `temp()` in brackets, notifier/support and notifier.rs, and `dir` plus a Drop in bridge/common. So are the random-id helpers (`rand16`, `random16`, and inline `getrandom::fill`). sf4-net, brackets and notifier all already dev-depend on ember-bridge.
- Proposed change: Add a `test-support` feature to ember-bridge that exposes `ember_bridge::testing`, owned by the crate that owns `Config`:
  - `TempDir` (removed on drop)
  - `loopback_config(dir, origin) -> Config`, with the defaults all five agree on
  - `Loopback::start_with(adjust: impl FnOnce(&mut Config), secrets) -> Loopback { running, origin, clock, dir }`. bridge/common already has exactly this shape.
  - `Loopback::state_with(adjust)` for the "same DB, changed config" rebuilds that blumint_reports and partner do by hand.
  
  EmberRust runs plain `cargo test` with no `--features`. So that the bridge's own 23 test binaries see the module, ember-bridge also needs a self-referencing dev-dependency: `[dev-dependencies] ember-bridge = { path = ".", features = ["test-support"] }`. sf4-net, brackets and notifier add `features = ["test-support"]` to their existing ember-bridge dev-dependency. bridge/tests/common/mod.rs becomes a thin layer of player and proof helpers over it. The sf4-net, brackets and notifier copies switch to `ember_bridge::testing::Loopback`.
- Behavior preserved: Each harness keeps its specific settings (mock_browser, tenants, rooms, sign-in sites, discord) through `adjust`. Each test still starts a real bridge on loopback.
- Behavior at risk: none identified. The feature is enabled only from dev-dependencies, so the shipped bridge build is unchanged.
- Estimated saving: About 350 lines across five copies, plus about 60 lines of AppState rebuilds. A new Config field touches one place instead of four.
- Confidence: high

### F-TEST-4: Six-plus copies of a wait-until helper with different budgets, plus dozens of inline poll loops that fall through silently
- File: server/ember/notifier/tests/twitch_room.rs:118
- File: server/ember/notifier/tests/discord_room.rs:173
- File: server/ember/bridge/tests/common/results.rs:71
- File: server/ember/bridge/tests/common/receiver.rs:70
- File: server/ember-rooms/tests/rooms.rs:120
- File: server/ember-rooms/tests/sigterm.rs:131
- File: server/ember-rooms/tests/rooms.rs:290
- Category: structural
- Severity: P2
- Claim: The same "poll until true or panic" helper exists at least six times, each with its own budget:
  - twitch_room `until`: 400×25 ms = 10 s
  - discord_room `until`: 200×25 ms = 5 s
  - bridge `results::until`: 250×20 ms
  - ember-rooms `rooms_settle_at`: 500×20 ms
  - sigterm `wait_for`
  - notifier/support `wait_for`
  
  On top of those, test files hold 53 `for _ in 0..N {` loops, many of them inline poll-and-sleep. Several in ember-rooms/tests/rooms.rs (290, 312, 335, 355) `break` on success but fall through on timeout with no failure. The test then asserts on a later snapshot, so a slow CI box reports a misleading `assert_eq!` on room members instead of "room never reached members==1 within 4 s".
- Proposed change: Put one `async fn until(what: &str, limit: Duration, cond: impl FnMut() -> bool)` (plus an async-closure variant) in the `ember_bridge::testing` module from F-TEST-3, for the crates that already dev-depend on ember-bridge. ember-rooms is a separate workspace and keeps one local copy. Replace each inline `for..break` poll with it, keeping the same total budget, so a timeout panics with `what`.
- Behavior preserved: The same conditions are polled with the same or longer budgets, and every assertion stays.
- Behavior at risk: none identified. Tests that currently pass after a silent fall-through would now fail at the wait instead of at the assert, which is the same pass/fail outcome with a clearer message.
- Estimated saving: About 150 lines. Clear timeout messages.
- Confidence: high

### F-TEST-5: Negative assertions done as fixed sleeps not tied to the tuning they check
- File: server/ember-rooms/tests/rooms.rs:744
- File: server/ember-rooms/tests/rooms.rs:297
- File: server/ember/notifier/tests/twitch_room.rs:310
- File: server/ember/notifier/tests/twitch_room.rs:319
- File: src/tests/iroh_recovery_integration_test.cxx:1046
- File: rust/sf4-net/src/service/tests.rs:3821
- Category: structural
- Severity: P3
- Claim: Proving that something does not happen needs a window, but the windows here are bare literals. Each test costs its full sleep on every run, and nothing ties the sleep to the production value it brackets:
  - `a_room_with_members_is_left_open` sets `tuning.empty_close = 300ms`, then sleeps a literal 1200 ms.
  - rooms.rs adds 200-300 ms "settle" sleeps after the poll loops (297, 319, 337, 345, 369, 772).
  - twitch_room's keepalive test runs 8×500 ms of keepalives, then a literal 3500 ms sleep, against a 1 s welcome keepalive plus `KEEPALIVE_GRACE` (2 s, eventsub.rs:29). The fake server also parks a task in `sleep(30s)`.
  - iroh_recovery_integration_test spins 16 s (`GetTickCount64()+16000`) to prove the guest of two stays frozen.
  - service/tests.rs:3821 has an unexplained 500 ms pause between a received control message and the next request in `service_replays_committed_checkpoint_to_each_native_owner`.
  
  The supervisor uses `std::time::Instant` (supervisor.rs:512, 573, 664) and real child processes, so a paused tokio clock is not an option there. A derived window is.
- Proposed change: Express each negative window as the tuning value times a margin, for example `tuning.empty_close * 4` and `welcome_keepalive + KEEPALIVE_GRACE + margin`. In the notifier, make `KEEPALIVE_GRACE` (eventsub.rs:29) reachable from its tests (pub, or a config value). In ember-rooms, use the `Tuning` fields the harness already sets. At service/tests.rs:3821, either replace the pause with a wait on the condition it gives time for or add a comment that names it. In the C++ recovery test, take the frozen-window length from the production election or lease constant it brackets, not 16000. Replace "settle" sleeps that wait for a positive event with F-TEST-4's `until`.
- Behavior preserved: Every negative still waits at least as long as now relative to its threshold. No window gets shorter than its current margin.
- Behavior at risk: none identified. Do not shorten any window without deriving it.
- Estimated saving: Mostly legibility, plus 1-2 s per affected test where settle sleeps become condition waits.
- Confidence: medium

### F-TEST-6: Three hand-rolled HTTP stand-ins in sf4-net tests where the bridge tests already have an axum fake supervisor
- File: rust/sf4-net/tests/public_rooms_support/mod.rs:54
- File: rust/sf4-net/tests/public_rooms_support/mod.rs:70
- File: rust/sf4-net/tests/tournament_support/mod.rs:157
- File: server/ember/bridge/tests/common/supervisor.rs:1
- Category: structural
- Severity: P2
- Claim: public_rooms_support implements a fake room supervisor on a raw `TcpStream`, with its own HTTP/1.1 head and body parser (`read_raw`, `read_request`, `answer`). tournament_support has a second raw parser (`read_request` -> `HttpRequest`, `write_response`) for its `Front` and `foreign_origin`. bridge/tests/common/supervisor.rs is an axum stand-in for the same supervisor contract ("creates, lists and deletes rooms as the real one does, and can refuse, fail, or hold its answers"). Two fakes of one contract can drift apart. The sf4-net one also hand-parses HTTP that axum already handles.
- Proposed change: Export the axum fake supervisor from `ember_bridge::testing` (F-TEST-3) with the knobs both callers use (create_delay, report, set_details, refuse/hold). Replace public_rooms_support's `Fake`/`handle`/`read_raw`. Rebuild tournament_support's `Front` and `foreign_origin` on axum, or on one shared raw parser if byte-level control is the point (the `older_bridge_in_front` relay may need raw bytes, so keep `relay` as is).
- Behavior preserved: The same requests and answers on loopback.
- Behavior at risk: tests that depend on exact raw-socket behavior (the `older_bridge_in_front` relay). Keep those raw.
- Estimated saving: About 200 lines and one contract fake instead of two.
- Confidence: medium

### F-TEST-7: `test_actor` repeats the 62-field Actor literal from entry.rs; the actor-plus-seed setup is repeated about 70 times
- File: rust/sf4-net/src/service/tests.rs:229
- File: rust/sf4-net/src/service/entry.rs:95
- Category: structural
- Severity: P2
- Claim: `test_actor` (tests.rs:229-293) is the same struct literal as the production one in entry.rs:95-156. It differs only in `endpoint` (a clone there), `coordination_port` (None vs the argument) and `latency` (`Latency::offline()` vs `Default::default()`; offline() turns `net` off, latency.rs:121). Because a struct literal must list every field, this is duplication, not a drift hole. Still, each new Actor field costs two identical edits. Separately, the pair `let (events, _rx) = mpsc::channel(IPC_QUEUE_CAPACITY); let mut actor = test_actor(endpoint, events);` appears about 70 times (80 channel calls, 68 test_actor calls). `Invite::create(id, test_relay, "test-build".into(), now().unwrap(), 3600)` appears 39 times.
- Proposed change: Add `Actor::new(endpoint, relay_only, events, coordination_port, latency)` in mod.rs, used by entry.rs and by `test_actor` (which becomes one line). Add `fn actor() -> (Actor, Receiver<Event>, Endpoint)` and `fn seed_invite(host: &Endpoint) -> Invite` fixtures to the tests support module (F-TEST-1).
- Behavior preserved: The same initial actor state on both paths.
- Behavior at risk: none identified.
- Estimated saving: About 65 lines from the literal, about 200 lines of repeated setup.
- Confidence: high

### F-TEST-8: session_server_transport_test.cxx has a 277-line unnamed scenario in `main()` and three 280-350-line test functions
- File: src/tests/session_server_transport_test.cxx:2089
- File: src/tests/session_server_transport_test.cxx:1154
- File: src/tests/session_server_transport_test.cxx:1504
- File: src/tests/session_server_transport_test.cxx:857
- Category: size
- Severity: P2
- Claim: At 2365 lines this is the largest C++ test. `main()` (2089-2365) runs 14 named tests and then an inline lobby scenario of about 260 lines: hello and join, wrong build, duplicate name, settings, stage validation, chara and costume validation, ready ordering, forwarding source spoofing. `TestCommittedSessionGate` (1154-1504, 350 lines), `TestCustomRoomDepartures` (1504-1826) and `TestTerminalAcknowledgmentBatch` (857-1136) are each one function. Because `CHECK` from test_support.hxx exits on the first failure and the file is a single CTest entry, one regression hides every later scenario. The natural clusters are:
  - lobby and selection: the inline main body
  - barriers: `TestOptionalSpectatorBarrier`, `BarrierRoom`, `TestRoomBattleBarrier`, `TestSpectatorLeavesBeforeP1Prepared`
  - terminal and results: `TestMaximumRoomResultBurst`, `TestTerminalReceiptReplay`, `TestTerminalAcknowledgmentBatch`
  - commit and recovery: `TestAtomicMatchRebind`, `TestRoomChatDelta`, `TestSameTermProposalPause`, `TestCommittedSessionGate`, `CheckIdleRecoveryWork`
  - departures: `ParticipantRoom`, `TestParticipantDepartureCheckpoint`, `ApplyAsRejoinedMember`, `TestRejoinAfterPreStartDeparture`, `TestCustomRoomDepartures`, `TestServerOwnedRoom`
- Proposed change: Move the inline main body into `TestLobbyAndSelection()`. Split the file into session_server_{lobby,barriers,terminal,commit,departures}_test.cxx over the existing server_transport_support.hxx and fake_helper_pipe.hxx. Register them either as five CTest entries or as one multi-source target, the way ShellJourney and IdentityJourney already are (CMakeLists.txt:316-323). Five entries is better for targeted retests.
- Behavior preserved: Every check and its order within each scenario.
- Behavior at risk: none identified. Scenarios already build their own `SessionServer` and `MockTransport`, so splitting them shares no state.
- Estimated saving: The largest file drops to about 600 lines. A failing scenario no longer masks the others.
- Confidence: high

### F-TEST-9: Single giant `main()` bodies: room_panel_navigation_test (about 1375 lines) and ui_render_test (770 lines, with mode chosen by 6 env vars and positional argv)
- File: src/tests/room_panel_navigation_test.cxx:29
- File: src/tests/ui_render_test.cxx:354
- File: src/tests/ui_render_test.cxx:362
- File: src/tests/iroh_recovery_integration_test.cxx:77
- Category: size
- Severity: P2
- Claim:
  - room_panel_navigation_test.cxx is one `main() try { ... }` from line 29 to the end of the file (1405 lines). It holds about 60 comment-headed blocks over one shared `ShellView view` that later blocks mutate further: delay row (239-298), seat rows (298-337), host rules (423-455), terminal-receipt fences and queue/watch eligibility (524-633), opponent and Training notices (658-711), held start and ready timeout (719-847), members and recovery reorder (847-880), card queue/watch options (880-912). A failure throws a bare `runtime_error(message)` with no location, and because state carries over between blocks, a failure's cause can sit far above it.
  - ui_render_test.cxx's `main()` runs 354-1125. Its mode comes from positional `argv[1]`, `argv[2]` (`--training-shots-only` and similar) and `argv[4]` (`--ux-shots-only`, `--readme-shots`), plus env vars SF4E_UI_RENDER_HEADLESS, _ATLAS_STRESS, _LOCALES, _QUICK, _SHOTS and _GALLERY. CMake registers 4 tests by combining them (CMakeLists.txt:341-361).
  - iroh_recovery_integration_test.cxx's `RunRecovery` is about 990 lines (77-1068) switching on `Fault`, often with several `;`-joined statements per line.
- Proposed change:
  - room_panel_navigation: turn each comment heading into a named function that takes a freshly built `ShellView` from one `BaseRoomView()` fixture (only where a block does not depend on an earlier block's mutation; keep the sequential chains as one function each). Switch `Check` to test_support's file/line CHECK (F-TEST-10).
  - ui_render: parse argv and env once into a `RenderRunOptions` struct at the top of main, then dispatch to `RunAtlasStress`, `RunGallery`, `RunLocales`, `RunShots(filter)`.
  - RunRecovery: split per `Fault` case into functions over a shared setup.
- Behavior preserved: The same checks, renders and output files.
- Behavior at risk: room_panel blocks that silently depend on earlier mutations. Keep those chained, and do not reset state that a later assertion relies on.
- Estimated saving: No line savings. Three files go from one function each to readable units, and failures get located.
- Confidence: medium

### F-TEST-10: 27 C++ tests redefine CHECK (four different semantics) and 14 hand-roll `Check()`, although test_support.hxx exists
- File: src/tests/test_support.hxx:8
- File: src/tests/session_controller_test.cxx:11
- File: src/tests/package_installer_test.cxx:20
- File: src/tests/helper_process_test.cxx:6
- File: src/tests/room_panel_navigation_test.cxx:24
- Category: spaghetti
- Severity: P2
- Claim: test_support.hxx defines a fail-fast CHECK (file, line, expression, `exit(1)`), and 113 of 213 test .cxx files include it. 27 others `#define CHECK` themselves, with several meanings:
  - counting: `printf("FAIL line ...")` then `++failures` and continue (session_controller, created_rules, tournament_play, and others)
  - fail-fast through `cerr` with a local `error` variable (package_installer)
  - counting plus `GetLastError()` (helper_process)
  
  14 more define `void Check(bool, const char*)`, some throwing and some exiting. A reader cannot tell from `CHECK(x)` whether a failure stops the test.
- Proposed change: Extend test_support.hxx with `EXPECT(c)` (counting, file and line) and `TEST_EXIT_CODE()` (non-zero if any EXPECT failed). Add `CHECK_MSG(c, fmt, ...)` for the GetLastError and `error` variants. Convert counting files to EXPECT and fail-fast files to CHECK, keeping each file's current stop-or-continue semantics.
- Behavior preserved: Each file keeps its fail-fast or report-all behavior. No check is removed.
- Behavior at risk: none identified. Output text changes from "FAIL line N" to "file:line".
- Estimated saving: About 40 macro and helper definitions deleted, one vocabulary.
- Confidence: high

### F-TEST-11: blumint_reports.rs (1597 lines, 34 tests) mixes result delivery with presence/claim leasing, and borrows another test's support module through `#[path]`
- File: server/ember/bridge/tests/blumint_reports.rs:6
- File: server/ember/bridge/tests/blumint_reports.rs:740
- File: server/ember/bridge/tests/blumint_reports.rs:1032
- File: server/ember/bridge/tests/startgg.rs:9
- Category: size
- Severity: P2
- Claim: The file has two unrelated halves. Lines 22-740 cover results, aborts, restarts, expiry and progress reports. Lines 740-1593 cover status and presence (`sent_statuses`, `claiming_for`, `sweep`, lease, restart and clock-step cases) plus starting wins. It also pulls `#[path = "discord/support.rs"] #[allow(dead_code)] mod support;` from another test binary. startgg.rs does the same with challonge/support.rs. Cross-binary `#[path]` includes plus `allow(dead_code)` mean these support files are really shared, but they do not live in `common/`.
- Proposed change: Split into blumint_results.rs (22-740: delivery, restart, expiry, progress, void) and blumint_presence.rs (740-1593: statuses, claims, leases, ready). Move discord/support.rs and challonge/support.rs into `common/` (common/discord.rs, common/challonge.rs) and drop the `#[path]` and `allow(dead_code)` attributes.
- Behavior preserved: All 34 tests unchanged.
- Behavior at risk: none identified.
- Estimated saving: Two files of about 750 lines each. 4 cross-binary `#[path]` includes gone; startgg.rs's own `#[path = "startgg/support.rs"]` is just redundant.
- Confidence: high

### F-TEST-12: Bridge tests seed the schema with raw SQL copied across files
- File: server/ember/bridge/tests/blumint_reports.rs:285
- File: server/ember/bridge/tests/partner.rs:707
- File: server/ember/bridge/tests/common/blumint.rs:184
- File: server/ember/bridge/tests/common/blumint.rs:217
- File: server/ember/bridge/tests/common/sites.rs:134
- File: server/ember/bridge/tests/discord.rs:427
- File: server/ember/bridge/tests/partner.rs:112
- File: server/ember/bridge/tests/blumint.rs:433
- Category: boundary
- Severity: P2
- Claim: `INSERT INTO discord_accounts (user_id, ember_id, username, connected_at)` is written out 6 times across 5 files. The 9-column `INSERT INTO attempts ... VALUES ('att_silent', ?1, 1, 1, 'player_agreement', 'permitted', 0, 0, 'pmt_silent')` is copied verbatim in blumint_reports.rs:285 and partner.rs:707. These statements restate the schema and the attempt state machine's literal values a second time. A migration that renames a column or a state breaks them one by one. The shortcut itself is defensible: common/blumint.rs says "tests/discord.rs covers the sign-in itself".
- Proposed change: Add `common/seed.rs` with `link_discord(state, user_id, ember_id, name)`, `link_site(state, site, user_id, ember_id, username)` and `silent_permitted_attempt(state, match_id)`, and replace the copies. If the bridge has a store function for these inserts (db layer), call it instead of SQL.
- Behavior preserved: The same rows are seeded.
- Behavior at risk: none identified.
- Estimated saving: About 80 lines. One place to update per schema change.
- Confidence: high

### F-TEST-13: The CTest layout makes C++ targeted retests easy by name, but the Rust suites are three 30-minute opaque entries, and there are no LABELS
- File: CMakeLists.txt:47
- File: cmake/sf4e_helper.cmake:72
- File: cmake/sf4e_helper.cmake:82
- File: cmake/sf4e_helper.cmake:91
- File: cmake/sf4e_tests.cmake:30
- Category: structural
- Severity: P3
- Claim: `sf4e_add_unit_test` is a good, strict registration helper. 218 registrations, one executable per test, so `ctest -R RoomAuthority` works. None of them carry `LABELS`, though, so "the room tests" or "UI tests" can only be picked with name regexes.
  - All helper Rust tests are a single entry, HelperRust (TIMEOUT 1800, holding `loopback_udp_ports` and `ember_processes`). EmberRust and BridgeSdk are likewise single 1800 s entries.
  - A targeted Rust retest has to run cargo by hand with the same `CARGO_TARGET_DIR=<build>/rust-test-target`, `CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_RUSTFLAGS=-C target-feature=+crt-static` and `--target x86_64-pc-windows-msvc`. Otherwise it recompiles into a different target dir.
  - The registration for PublicRoomsChain (CMakeLists.txt:681-688) already shows a single cargo test registered on its own.
- Proposed change: Add a `LABELS` argument to `sf4e_add_unit_test` and label by area (room, ui, replay, training, netplay, integration, rust). Add a small `scripts/cargo-test-like-ctest.ps1 <filter>` (or a CMake `HelperRustFilter` custom target) that runs `cargo test` with the HelperRust environment and a module filter, for example `service::tests::probes` after F-TEST-1. Do not split HelperRust into per-module CTest entries: each would have to hold `ember_processes`/`loopback_udp_ports` and would serialize, rebuilding the same lock contention. The full gate stays one entry.
- Behavior preserved: The full ctest run is identical, and every test still gates.
- Behavior at risk: none identified.
- Estimated saving: Faster targeted retests with no recompiles from a mismatched target dir.
- Confidence: medium

### F-TEST-14: 22 bare `timeout(60 s, async {...})` wrappers in service/tests.rs, although a named `phase()` helper exists
- File: rust/sf4-net/src/service/tests.rs:206
- File: rust/sf4-net/src/service/tests.rs:1013
- File: rust/sf4-net/src/service/tests.rs:4544
- Category: spaghetti
- Severity: P3
- Claim: `phase(step, limit, work)` (206) makes a timeout name the unfinished step. Still, 22 tests wrap their whole body in `timeout(Duration::from_secs(60), async { ... }).await.unwrap()`. A hang there reports only `Elapsed` after 60 s, with no step, and the body is indented one extra level. Other limits in the file range from 1 s to 90 s (23×60, 17×10, 13×20, 8×30, ...).
- Proposed change: Replace each outer wrapper with `phase("<test name>", Duration::from_secs(60), async {...})`, keeping the 60 s, and use `phase` for the inner steps that already take their own timeouts.
- Behavior preserved: The same limits.
- Behavior at risk: none identified.
- Estimated saving: Diagnosable hangs. No runtime change.
- Confidence: high

## Area summary
- Overall health: the tests are thorough and behavior-named, and the C++ fakes are well centralized (MockTransport, FakeHelperPipe, FakeIrohRoom, session_client_mock), as is the CMake test helper. The weak points are test-side sprawl and duplication: one 7384-line Rust test file that also serves as its submodules' prelude (F-TEST-1), several single-`main()` C++ tests (F-TEST-8, F-TEST-9), and harness code copied across crates (F-TEST-3, F-TEST-4, F-TEST-6).
- Biggest judo move: F-TEST-3. One loopback-bridge harness in `ember_bridge::testing`, owned by the crate that owns `Config`, absorbs five harnesses, the `Dirs`/`temp` copies, the AppState rebuilds, and (with F-TEST-4 and F-TEST-6) the `until` helpers and the second fake supervisor.
- Highest-conviction correctness item: F-TEST-2. Two tests check test-only re-implementations of voter selection and retirement confirmation instead of `admission_voters` and the real reconciliation path.
- Overhead and streaming: nothing in this area. All serialization here is test-side.
- Could not assess: wall time, flake rate, and which tests dominate the ~6 min ctest, because the brief forbids running anything. On my reading, the 21 `default_relay_map()` uses in service/tests.rs are data inside invites, not dialed relays (endpoints are `clear_ip_transports().bind_addr(localhost)` with Minimal presets). The real-relay tests are behind `SF4E_NETWORK_TESTS`, which defaults to OFF. That is my reading, not a verified fact. sdk/typescript/test looks healthy (timeouts.test.ts stubs `AbortSignal.timeout` rather than waiting) and produced no findings.
