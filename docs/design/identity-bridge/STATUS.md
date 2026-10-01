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
