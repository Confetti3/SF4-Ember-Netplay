# Identity and bridge implementation status

The files beside this one are the EMBER-TB-001 specification package (revision
1.0-draft.1), copied unchanged. `SHA256SUMS.txt` still verifies them.

## Baseline

The specification inspected `release` at `92aad00`. This branch, `feat/identity-bridge`,
starts from `release` at `e5b276a`, nine commits later. The spec's E1 to E9 facts were
rechecked at `e5b276a`:

- The Iroh endpoint is still built without a secret key, so it changes every helper run.
- Helper IPC commands use `deny_unknown_fields`. A new command sent to an older helper
  closes the link. Helper and game ship together, so this branch adds no negotiation.
- FT1, FT2, FT3 and FT5 exist in `RoomRules` but the room forces every table to
  Unlimited, and nothing ends a set. Native set scoring (WP5) needs that lifted first.
  This branch lifts it for casual rooms (see "Beyond the package" below).

`tools/verify_fixtures.py` was run with the pinned `cryptography` and `jsonschema`
versions on 2026-10-01: 33 of 33 fixture checks passed.

## Scope of this branch

WP0 to WP3: local identity and backup, bridge authentication and linking, the generic
match and event API, signed webhooks, a TypeScript SDK, a mock provider, and a reference
notifier that turns bridge events into Discord webhook posts and Twitch chat messages.

`feat/tournament-play` adds WP4 (a room bound to a match), WP5 (game permits, signed
reports and two-player agreement), WP6 (match links and the in-game match list) and
the automated part of WP8 (seeded fuzzing and adversarial tests).

Not done: WP7 (BluMint calls the generic API directly, so no adapter calls BluMint),
Windows/Proton link registration checks, low-spec benchmarks, canary tournaments, and
every check that needs two PCs or a real browser. No tournament match has been played
in the game yet.

## Tournament play

A match whose rules profile is `ember-room-v1` is played in an Ember room:

1. Each fighter's game claims the match through its helper. The first claim gets a
   30-second provisioning lease and hosts a room; the other waits. The host publishes
   the room with the lease and fence, and both claims then return the room and a
   bridge-signed binding naming the two fighters' Ember IDs and Iroh endpoints. Each
   helper checks the binding against the bridge key the player approved.
2. The game that leads the room applies the binding through a recovery checkpoint.
   Table 0 then seats the two named endpoints by slot, nobody else can stay or join,
   and seat, watch, rules and kick changes there are refused. Room protocol version 3.
3. When both fighters press Ready the room reserves a match generation and holds the
   start. Each game asks the bridge to prepare that game, and the bridge signs a permit
   once both described the same game. The start waits (up to 150 s) until both
   fighters hold the same permit.
4. When the game ends, each game reports what it saw, rollback-confirmed, in a report
   signed by the player's identity. Two agreeing reports score the game; disagreement,
   a lone report after 60 s, or a permitted game nobody reports goes to organizer review.
   Reports are saved to disk before sending and retried every 20 s.
5. The set ends at first to N; the next claim answers `stale_revision` and the game
   leaves the room.

Players find their matches on the Ember ID screen (Tournament matches) or open one from
a site's Play button (`ember://tournament/open`, see WP6 below). Nothing starts a match
without the player pressing Play, and nothing moves a player out of a room.

## Staging bridge

A staging bridge runs at `https://bridge.embernetplay.link` on the project VPS so a
platform can build its side before WP4 to WP7. It is this branch's bridge with
SQLite, behind nginx with the `embernetplay.link` wildcard certificate, deployed by
`rust/ember/bridge/deploy/setup.sh` (systemd unit with a dedicated account, request
budgets per address in nginx, and a daily database and secrets backup kept 14 days
on the same machine). All three development switches are off. Connections:
`blumint-staging` (tenant `blumint`) and `ember-test` (tenant `ember`) for our own
checks, both of the new `direct` kind: the platform calls the generic API with its
provider credential, and no adapter calls the platform. Partner onboarding is
`docs/guides/BLUMINT_QUICKSTART.md`. `ember-bridge credentials` lists issued
credentials without their tokens and `ember-bridge revoke` ends one at once;
`restore.sh` puts a backup set back.

Live checks on 2026-10-02 against the deployed bridge: `setup.sh` probes passed directly
and through nginx; the helper's HTTP client (reqwest 0.13.4 with rustls on Windows)
accepted the certificate and still refused an expired one; `tools/walkthrough.ts` on the
`ember-test` connection linked two test players, completed a first-to-2 match 2-1 and
read its 12 events and the record; the SSE stream delivered events and keep-alives through
nginx; a credential was issued while the bridge ran; and a webhook to an `https` address
on the bridge's own host reached nginx (logged as `POST /webhook-tls-check 404`, retried
on schedule), so outbound TLS verification works from the service. A 2xx webhook receiver
on the public internet was not exercised. `restore.sh` was run in a sandbox only.

For testing without the game, the SDK has `TestPlayer` and `tools/test-player.ts`
(a throwaway key that claims a link code the way the helper does) and
`tools/walkthrough.ts` (linking, a match decided by the organizer, and its events).

## Beyond the package

These additions go beyond EMBER-TB-001, which covers two-player bracket sets only and
lists queue rotation and multi-table scheduling as future work.

- **Casual room sets and rotation.** A room table can have a set length of first to 1,
  2, 3 or 5 and a rotation: winner stays (king of the hill), loser stays, or both
  rotate. When a fighter reaches N wins the room records the set, moves the streak, and
  sends the rotated fighters to the back of the table's queue; the next queued members
  sit down. Only members who joined the queue are seated; watching never queues anyone.
  With no set length a table behaves exactly as before. The room protocol is now 2.
  Spec 13.4 (an active tournament binding disables casual rotation and reserves its
  table) still applies and is owed with WP4; nothing binds a room table yet.
- **Bridge lobbies.** `/v1/lobbies` is a provider-run queue that plays one first-to-N
  set after another with the same three rotations. Each set is an ordinary match row,
  so scoring, adjudication, idempotency and the busy check are the match code's, and
  the rotation and the next set happen in the transaction that completes a set. Lobby
  sets cannot be cancelled or reopened through the match routes. A room table does not
  report to a lobby yet; results still come from organizer adjudication.
- **Player records and lobby standings.** `GET /v1/players/{ember_id}/record` and the
  lobby snapshot's `standings` add up finished matches. They are computed from accepted
  games on every read, so corrections show at once; only a lobby player's best streak is
  stored, as sets complete. Records are scoped like events: a provider sees its
  connection, an organizer its tenant, and a player only their own. Lobbies (migration
  003) and the stored best streak (004) are both unreleased and ship together, so no
  deployed bridge has lobby history without it; a local test database that played lobby
  sets before 004 keeps a best streak of zero for those earlier sets. Snapshots a player
  reads (lobbies, tournaments) show only that player's own `participant_id`, and
  tournament events carry Ember IDs only.
- **Tournaments.** `/v1/tournaments` runs single elimination, double elimination (with
  an optional grand final reset) and round robin on a provider connection. Spec 1.3
  leaves multi-match scheduling to providers; this is a deliberate extension for
  communities without one. The layout is a pure function of format and entrant count in
  `ember_protocol::tournament`, which also holds the state machine (byes, walkovers,
  reopening a result, placements), so the bridge stores only node states. Each set is
  an ordinary match created in the transaction that decides its players. A busy player's
  set waits until their match ends, and lobbies do not seat a player whose bracket set is
  waiting. A correction reopens a set and clears what it fed while no later set has a
  game; finished tournaments are final.

## Deviations from the specification

- **Bridge database.** The spec names PostgreSQL as the reference store. This branch
  uses SQLite (WAL, `synchronous=FULL`, every write in `BEGIN IMMEDIATE`) because the
  first bridge is local and mock-only, and the tests must run inside the normal build
  without Docker. Uniqueness is enforced by constraints, not check-then-insert. The SQL
  avoids SQLite-only syntax where it can so a PostgreSQL backend can be added later.
- **Player idempotency.** Proof-carrying player routes (`/v1/sessions`, link claims,
  claim cancel, unlink) do not take an `Idempotency-Key`. Each proof's challenge is
  single use and consumed in the same transaction as the change, and retrying a
  claim or approval returns the existing record.
- **Link intents are not idempotent.** `POST /v1/link-intents` shows its code once and
  stores only an HMAC of it, so a replay could not return the code. Each call makes a
  new intent; at most three live intents per account.
- **Rules profiles.** Two `native_rules_profile` values are accepted:
  `organizer-reported-v1` (results come from organizer adjudication) and `ember-room-v1`
  (games are played in an Ember room under the room's own game settings and scored by
  the fighters' agreeing signed reports). `usf4-standard-v1` and every other profile
  return `422 unsupported_rules` until a tested rules translator exists (spec 13.3).
- **Signed grants instead of JWS (spec 9.4).** Bindings and permits are canonical JSON
  signed with the bridge's Ed25519 key under their own domains (`EMBER:BINDING:1`,
  `EMBER:PERMIT:1`), the same rules player proofs use. There is no JOSE header, so
  algorithm confusion and `none` cannot arise. Only the fighter's own helper verifies
  them; the game receives the checked fields.
- **The binding is the admission certificate (spec 9.4).** Instead of a per-fighter
  certificate presented on join, the room admits by the Iroh endpoints the bridge-signed
  binding names. The leading game applies the binding its own helper verified; every
  other game's helper verifies its own copy. A restarted helper has a new endpoint, so
  its claim moves the binding revision and the room seats the new endpoint.
- **Claiming again renews the lease.** There is no separate lease renewal route; the
  `lease.renew` proof action is reserved and unused.
- **A lone cancel closes its game (spec 16.6).** A `cancel` report says a start was
  called off before the game began, so nothing is in dispute and the attempt closes as
  aborted without waiting for the other report. A lone `abort` still goes to review.
- **`required_build_id` is the provider's label.** The bridge does not compare it with
  the game build. Both fighters' claims must carry the same build, or the second claim
  is refused with `incompatible_build`.
- **No observations route.** Observations travel only inside signed reports.
- **Match links instead of one-use handoffs (spec 12.1).** A match's link names its
  bridge and match (`ember://tournament/open?bridge=...&match=...`, opened from the page
  `https://embernetplay.link/m#<bridge>/<match>`, the match's `play_url`) and does not
  expire. Tournament sites send one join link per match to both players (BluMint's
  `matchUrl`), which a per-player, one-minute code cannot be, and a player who first has
  to install Ember comes back to the same link. Nothing is lost: the link was never what
  admitted a player. Only the two assigned Ember IDs can claim the match, and another
  player's Ember finds no such match in their list and says so. There is no
  `/v1/handoffs` route and no `handoff.redeem` proof action.
- **Adjudication kinds.** Organizers can record a game result (win or draw) and void a
  game, which reopens a completed match as a correction. Forfeits, no-shows and
  disconnect rulings wait for the organizer-policy sign-off.
- **Passphrases over IPC.** Enabling, unlocking, exporting and importing pass the
  passphrase once over the authenticated helper pipe, as spec 8.2 allows. The helper
  wipes its copy after use and never logs or returns it. On the game side the Ember ID
  screens hold what the player typed until it is sent, and wipe it when the player
  leaves those screens; every request copy, the JSON text, the IPC frame and the
  outgoing queue entry are wiped after use. Two copies are outside Ember's control:
  Dear ImGui keeps the text of the last active field in its own edit state until
  another field takes focus, and freed heap pages are not scrubbed by the CRT.
- **Backups go to a fixed folder.** An export always writes a new file in
  `%APPDATA%\sf4e\identity-backups`, named by date and time, which the helper creates.
  The game chooses the path, so the interface needs no file dialog; restoring reads any
  path the player pastes.
- **Helper routing.** Identity and bridge commands are handled by a separate worker
  that the IPC reader feeds directly. The room actor never sees them and was not
  changed apart from one defensive match arm.
- **Game requests are typed.** The interface never writes helper JSON. It sends a
  typed request (`netplay::IdentityRequest`) that the runtime turns into the helper's
  JSON with one case per operation, so a request the helper would refuse to parse, which
  would end the IPC link, cannot be built. Key changes (create, restore, start over)
  are allowed only at the main menu outside a room; bridge and key use wait for no live
  game; status and lists are always allowed.

## New dependencies

| Crate | Where | Why |
|---|---|---|
| `ember-protocol` (path) | sf4-net, bridge, notifier | Shared wire rules |
| `ed25519-dalek =3.0.0` | ember-protocol | Already in sf4-net's lock through iroh |
| `hmac 0.12` | ember-protocol | Standard Webhooks HMAC-SHA256 |
| `argon2 =0.5.3` | sf4-net | Passphrase KDF for local store and backups |
| `chacha20poly1305 =0.10.1` | sf4-net | XChaCha20-Poly1305 envelope |
| `getrandom 0.3` | sf4-net | OS RNG for seeds, salts and nonces; already in the lock |
| `zeroize 1` (serde) | sf4-net, ember-protocol | Wiping seeds, keys and passphrases; already in the lock |
| `reqwest =0.13.4`, `rustls =0.23.44` (ring) | sf4-net | Bridge HTTP; both already in the lock through iroh |
| `ember-bridge` (path, dev only) | sf4-net | End-to-end tests of the helper's bridge client; the shipped helper never compiles it |
| `axum 0.8`, `rusqlite 0.40` (bundled), `reqwest 0.13`, `url 2`, `futures-util 0.3` | ember-bridge | HTTP server, storage, webhook delivery |

DPAPI, Wine detection and the known-folder lookup use additional `windows-sys` features,
not new crates.

## Checklist evidence

`IMPLEMENTATION_CHECKLIST.md` is part of the package and stays unchanged; its items are
recorded here instead. Everything below ran on Windows 11 (10.0.26300), x64 Rust 1.98.0
and the x86 game build, on `feat/identity-bridge`. "Automated" means a test in the
build's ctest run (`EmberRust` runs `cargo test --locked` in `rust/ember`, `HelperRust`
runs the sf4-net suite). Mock fixtures are not counted as product acceptance.

| Item | Evidence | Result |
|---|---|---|
| ID-01 | protocol `identity_derivation_vectors`; SDK `fixtures.test.ts` | Automated, pass |
| ID-02 | `dpapi_identity_survives_restart` (store reopened, same ID) | Automated, pass; OS restart and update not run |
| ID-03 | Identity store is separate from the Iroh endpoint key, which is unchanged | By construction; not tested |
| ID-04 | None | Not run |
| ID-05 | None | Not run |
| ID-06 | `concurrent_enable_installs_one_identity` | Automated, pass |
| ID-07 | `crash_at_any_step_never_replaces_an_established_key`, `crash_during_replacement_needs_recovery`, `replacement_with_a_failed_continuity_write_is_repaired` | Automated, pass |
| ID-08 | `unreadable_keys_are_reported_and_kept`, `dpapi_round_trip_and_tamper` | Automated, pass; another Windows user not run |
| ID-09 | `missing_key_with_continuity_needs_recovery` | Automated, pass |
| ID-10 | `missing_continuity_is_repaired_from_the_key` | Automated, pass |
| ID-11 | `backup_restores_the_same_id_elsewhere`, `backup_replaces_a_forgotten_local_passphrase`; helper export into a new folder and preview (`tournament_bridge`) | Automated, pass |
| ID-12 | `fails_closed`, `import_fails_closed_without_touching_the_store` | Automated, pass |
| ID-13 | `bounds_cost_before_deriving`, `oversized_files_are_refused_before_reading` | Automated, pass |
| ID-14 | `different_identity_import_requires_confirmation_and_keeps_the_old_store`, `reset_confirms_against_the_store_on_disk` | Automated, pass |
| ID-15 | `no_seed_in_exports_status_or_debug`, `requests_carry_no_debug_output_of_secrets` | Automated, pass; diagnostics export not inspected |
| ID-16 | None | Not run (manual, elevated launch) |
| ID-17 | `wine_requires_a_passphrase` with the Wine probe forced | Automated, pass; real Wine and Proton not run |
| ID-18 | Identity requests never enter the room or match path | By construction; no gameplay run |
| AUTH-01 | protocol `challenge_signature_and_binding`; SDK `fixtures.test.ts` | Automated, pass |
| AUTH-02 | `challenge_rejections` | Automated, pass |
| AUTH-03 | `sessions_need_a_fresh_single_use_proof` | Automated, pass |
| AUTH-04 | `privileged_challenges_need_a_session_for_the_same_key`; proof routes take no idempotency key (deviation above) | Automated, pass |
| AUTH-05 | `strict_json_rejections`, `strict_json_is_enforced_before_authorization` | Automated, pass |
| AUTH-06 | `event_stream_resumes_and_hides_others` (a revoked session ends its event stream) | Automated, partial: stream revocation only |
| AUTH-07 | `a_stolen_code_cannot_finish_a_link` | Automated, pass |
| AUTH-08 | Helper refuses an unapproved bridge (`link_an_identity_through_the_helper`) | Automated, pass |
| LINK-01 | `provider_proxy_link_flow`, `browser_approval_and_replacement`, `link_an_identity_through_the_helper` | Automated, pass |
| LINK-02 | `a_stolen_code_cannot_finish_a_link` | Automated, pass |
| LINK-06, LINK-07 | `codes_are_scoped_and_bounded` | Automated, pass |
| LINK-08 | `browser_approval_and_replacement` | Automated, pass |
| LINK-09 | `cancel_and_unlink_follow_policy`; `players_cancel_their_own_pending_claims` (an answered claim cannot be cancelled) | Automated, pass |
| LINK-10 | `windows_slide` (limiter only) | Automated, pass; no lockout test |
| LINK-12 | `provider_proxy_link_flow` (another tenant sees nothing) | Automated, pass |
| WEB-01, WEB-02 | protocol `webhook_fixture`; notifier `accepts_the_specification_fixture_once` | Automated, pass |
| WEB-03 | `webhooks_are_signed_retried_and_rotated` | Automated, pass |
| WEB-05, WEB-06 | `event_stream_resumes_and_hides_others` | Automated, pass |
| WEB-07 | `refuses_special_addresses`; addresses rechecked at connect | Automated, pass; DNS rebinding not exercised |
| RESULT-09 | `ft2_set_scores_once_per_game`, through organizer adjudication only | Automated, pass |
| PROVIDER-02 | `match_creation_is_validated_and_idempotent` | Automated, pass |
| Game screens | ShellJourney `IdentityJourneys`; UiRender Ember ID pages in every locale and size | Automated, pass |
| Room sets (extension) | `RoomSets`: every rotation, empty queue, draws, rules changes, a rotated fighter's receipt, checkpoint and wire | Automated, pass; no two-PC game run |
| Records (extension) | bridge `records_follow_finished_matches`, `standings_rank_the_lobby`; SDK `bridge.test.ts` | Automated, pass |
| Tournaments (extension) | protocol `tournament` unit tests (seeding, byes for 3 to 17, every double elimination entrant losing twice, the first losers drop avoiding a rematch at 8, 16 and 32, resets, walkovers, reopening); bridge `tournaments.rs` (each format end to end, a busy player and a lobby, withdrawal, unlinking, corrections, cancellation); notifier `announces_tournament_progress`; SDK `bridge.test.ts` | Automated, pass |
| Lobbies (extension) | bridge `lobbies.rs` (rotations, leaving and unlinking, players busy in other matches, and the lobby resuming and announcing seat changes when they are free); notifier `announces_lobby_rotations`; SDK `bridge.test.ts` | Automated, pass |
| Linux | `cargo test --locked` in `rust/ember` and `npm test` in `sdk/typescript` (Node 24 from nodejs.org) on Ubuntu 26.04, x86_64, at `efe545e` | Automated, pass |

Tournament play on `feat/tournament-play` (same machine and toolchain):

| Item | Evidence | Result |
|---|---|---|
| ROOM-01 | bridge `claims_and_descriptors_are_checked` (first claim hosts, the second waits); claims are written under `BEGIN IMMEDIATE` | Automated, partial: claims run one after the other |
| ROOM-02 | bridge `a_publish_retry_and_a_silent_game`, `claims_and_descriptors_are_checked` (only the lease holder publishes the first room) | Automated, partial: an expired lease is not exercised |
| ROOM-04, ROOM-06 | `RoomTournament` fighters only; `SessionServerTournament` (strangers are sent away and cannot join); `TournamentFuzz` bound room | Automated, pass |
| ROOM-07 | protocol `permits_verify_only_under_their_domain_and_key`, fuzz `signed_values_change_only_by_failing`; bridge stale binding refused | Automated, pass (no JWS, see deviations) |
| ROOM-08 | `RoomTournament` (a new endpoint takes its slot; a fighter who leaves sits down again) | Automated, pass; a restarted helper in a real game not run |
| ROOM-11 | `RoomTournament` checkpoint (binding and permit hold survive; a hold without a binding is refused) | Automated, pass |
| ROOM-13 | `RoomTournament` (taking Ready back or the hold running out burns the generation) | Automated, partial |
| ROOM-14 | `RoomTournament` permit gate; bridge "a native game is used once" | Automated, pass |
| ROOM-16 | `ShellJourney` (a match link never starts a match or moves a player out of a room); the launcher hands a link to the running game (`JoinLink`) | Automated, partial: no real browser |
| ROOM-17 | `ShellJourney` Paste match link | Automated, UI only; Proton not run |
| ROOM-18 | `RoomTournament` casual rooms need no permit; every room suite unchanged | Automated, pass |
| RESULT-03 | bridge `forged_and_misdirected_reports_are_refused` | Automated, pass |
| RESULT-05 | bridge `disagreement_holds_the_match_for_an_organizer` | Automated, pass |
| RESULT-06 | bridge `a_lone_report_or_a_silent_game_goes_to_review` | Automated, pass |
| RESULT-07, RESULT-08 | bridge `reports_are_idempotent_and_never_rewritten`; helper spool `reports_survive_reopening_until_done` | Automated, pass |
| RESULT-09 | bridge `agreeing_reports_score_a_set`; helper `play_a_set_through_two_helpers`; SDK `play.test.ts` | Automated, pass |
| RESULT-10 | SDK `play.test.ts` (a draw between wins; the set still ends 2-1) | Automated, pass |
| RESULT-11 | bridge `a_lone_cancel_closes_the_game_and_a_lone_abort_waits_for_review` | Automated, pass |
| RESULT-14, RESULT-15 | helper spool `no_folder_means_not_saved`; `TournamentPlay` (a report that could not be saved stops the match, leaves the room and starts no further game, with the reason shown) | Automated, partial: a full disk is not simulated |
| RESULT-17 | bridge `a_publish_retry_and_a_silent_game` (review only after the start window plus 30 minutes) | Automated, partial |
| Match links (WP6) | bridge `a_match_has_one_play_link_for_both_players`; helper `play_a_set_through_two_helpers`; `JoinLink`; `TournamentFuzz`; `ShellJourney`; SDK `play.test.ts` | Automated, pass; real browser and Proton not run |
| Fuzzing (WP8) | `TournamentFuzz` (helper answers, links, the state machine, a bound room; seeded, with coverage checks); protocol `fuzz` module | Automated, pass; it found one ordering bug, fixed in `707d030` |

Not run: AUTH-06 apart from stream revocation, LINK-03 to LINK-05, LINK-11, ROOM-03,
ROOM-05, ROOM-09, ROOM-10, ROOM-12, ROOM-15, RESULT-01, RESULT-02, RESULT-04, RESULT-12,
RESULT-13, RESULT-16, RESULT-18 to RESULT-20, WEB-04 and WEB-08, PROVIDER-01 and
PROVIDER-03 to 05, every OPS item, and all release gates. Every tournament item above
still needs a two-PC game.

In-game run on 2026-10-01 (Windows, one PC, this branch's staged build against a local
bridge with the mock provider): turning on Ember ID created the key under
`%LOCALAPPDATA%\Ember\Identity\v1`, and approving the bridge, entering a link code from the
mock login page and confirming the fingerprint completed the link (the bridge recorded
`identity.link.pending` then `identity.link.completed`). Export, import, unlink and a
helper restart keeping the same ID were not exercised in that run.

## Open sign-offs (spec section 30.2)

| Topic | Default until resolved |
|---|---|
| Who hosts the first bridge and holds its signing keys | Staging on the project VPS; no production bridge |
| Which BluMint login or subject-assertion route exists | Linking adapter disabled |
| BluMint API schemas, auth, retries, results | Mock provider only |
| Native USF4 rules mapping | No native rules profile advertised |
| Proton secure storage | Encrypted-file backend, no DPAPI claim |
| Organizer forfeit and no-show policy | No inferred wins |
| Performance budgets | Targets only |
| Security review | Draft protocol, not production-approved |
