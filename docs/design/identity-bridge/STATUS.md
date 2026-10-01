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

Not in this branch: WP4 room admission, WP5 permits and signed reports, WP6 launcher
handoff and Proton validation, WP7 BluMint adapter, WP8 hardening.

## Beyond the package

Two additions go beyond EMBER-TB-001, which covers two-player bracket sets only and
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
- **Rules profile.** The only accepted `native_rules_profile` is
  `organizer-reported-v1`: results come from organizer adjudication and Ember enforces
  no native rule. `usf4-standard-v1` and every other profile return
  `422 unsupported_rules` until a tested rules translator exists (spec 13.3).
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
| Lobbies (extension) | bridge `lobbies.rs` (rotations, leaving and unlinking, players busy in other matches, and the lobby resuming and announcing seat changes when they are free); notifier `announces_lobby_rotations`; SDK `bridge.test.ts` | Automated, pass |
| Linux | `cargo test --locked` in `rust/ember` and `npm test` in `sdk/typescript` (Node 24 from nodejs.org) on Ubuntu 26.04, x86_64, at `efe545e` | Automated, pass |

Not run: AUTH-06 apart from stream revocation, LINK-03 to LINK-05, LINK-11, every ROOM item, RESULT items other than
RESULT-09, WEB-04 and WEB-08, PROVIDER-01 and PROVIDER-03 to 05, every OPS item, and all
release gates.

In-game run on 2026-10-01 (Windows, one PC, this branch's staged build against a local
bridge with the mock provider): turning on Ember ID created the key under
`%LOCALAPPDATA%\Ember\Identity\v1`, and approving the bridge, entering a link code from the
mock login page and confirming the fingerprint completed the link (the bridge recorded
`identity.link.pending` then `identity.link.completed`). Export, import, unlink and a
helper restart keeping the same ID were not exercised in that run.

## Open sign-offs (spec section 30.2)

| Topic | Default until resolved |
|---|---|
| Who hosts the first bridge and holds its signing keys | Local and mock only |
| Which BluMint login or subject-assertion route exists | Linking adapter disabled |
| BluMint API schemas, auth, retries, results | Mock provider only |
| Native USF4 rules mapping | No native rules profile advertised |
| Proton secure storage | Encrypted-file backend, no DPAPI claim |
| Organizer forfeit and no-show policy | No inferred wins |
| Performance budgets | Targets only |
| Security review | Draft protocol, not production-approved |
