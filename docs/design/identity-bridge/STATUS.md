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

`tools/verify_fixtures.py` was run with the pinned `cryptography` and `jsonschema`
versions on 2026-10-01: 33 of 33 fixture checks passed.

## Scope of this branch

WP0 to WP3: local identity and backup, bridge authentication and linking, the generic
match and event API, signed webhooks, a TypeScript SDK, a mock provider, and a reference
notifier that turns bridge events into Discord webhook posts and Twitch chat messages.

Not in this branch: WP4 room admission, WP5 permits and signed reports, WP6 launcher
handoff and Proton validation, WP7 BluMint adapter, WP8 hardening.

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
  wipes its copy after use and never logs or returns it.
- **Helper routing.** Identity and bridge commands are handled by a separate worker
  that the IPC reader feeds directly. The room actor never sees them and was not
  changed apart from one defensive match arm.

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
