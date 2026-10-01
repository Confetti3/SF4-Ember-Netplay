# Ember Identity and Tournament Bridge
## Provider-neutral identity, linking, SDK, and tournament automation specification

**Document:** EMBER-TB-001  
**Revision:** 1.0-draft.1  
**Date:** 2026-10-01  
**Status:** Proposed implementation contract, not a statement of shipped capabilities  
**Project:** SF4 Ember Netplay  
**Initial adapter:** BluMint  
**Scope:** Persistent local identity, account linking, bridge authentication, room admission, result reporting, provider API, webhooks, recovery, and validation

> **Core decision:** Ember owns a locally generated signing identity. A trusted, independently deployable bridge links that identity to external accounts. Tournament platforms integrate with a generic API and event contract. No tournament-platform code belongs in GGPO, the native result reader, or the game-side room implementation.

> **Identity promise:** The Ember ID remains the same while the same private key is retained or restored. Changing names, updating Ember, joining a different room, or unlinking an external account does not change it. Losing or replacing the key changes the ID. A key is an identity credential, not proof of a unique person or an unmodified game client.

---

## Contents

1. Scope, priorities, and explicit non-goals
2. Evidence baseline and unresolved external contracts
3. Terminology and ownership boundaries
4. Architecture and deployment
5. Security model and invariants
6. Persistent identity and identifier format
7. Local key storage, startup, and failure handling
8. Backup, import, replacement, and revocation
9. Bridge trust and cryptographic wire rules
10. Authentication and operation proofs
11. Code-based external account linking
12. Launcher handoff and application UX
13. Generic match model and rules
14. Room provisioning, admission, and host migration
15. Official game permits and readiness
16. Result capture, reconciliation, and set scoring
17. Match lifecycle and failure policy
18. Helper IPC and runtime isolation
19. Public API contract
20. Events and webhooks
21. Provider SDK and adapter contract
22. BluMint adapter and verification gates
23. Data model, consistency, and persistence
24. Privacy, audit, and operational security
25. Limits, performance, and observability
26. Compatibility and packaging
27. Implementation work packages
28. Acceptance and adversarial test matrix
29. Rollout, rollback, and release gates
30. Decisions and remaining sign-offs
31. Worked end-to-end scenario
32. Sources and verification notes

---

## 1. Scope, priorities, and explicit non-goals

### 1.1 Goals

The first complete release MUST support:

- A persistent Ed25519 identity generated locally by `sf4-net.exe`, with no Ember username/password registration.
- One-time-code linking to a verified external account, with explicit consent and browser confirmation.
- A provider-neutral bridge API for match assignment, room coordination, authenticated admission, status, results, and cancellation.
- Automatic hosting/joining through existing Ember session abstractions, subject to local user consent and safe game boundaries.
- Individual native-game reports and first-to-N set aggregation without confusing a USF4 game with a bracket match.
- Signed, retryable result reports, duplicate suppression, disagreement handling, and an organizer review path.
- Generic signed webhooks and a resumable client event stream.
- Windows support and an explicitly tested Wine/Proton identity-storage path.
- Continued casual/offline play without a bridge, linked account, or working identity store.
- Encrypted identity export/import so users have a way to preserve the same ID across machines or operating-system reinstalls.

### 1.2 Priorities

**P0:** Preserve game stability, identity continuity, authorization, and scoring correctness.  
**P1:** Automate linking, room entry, bracket reporting, and operator recovery.  
**P2:** Improve convenience, observer workflows, and additional adapters.

Correctness takes precedence over apparent automation. An unresolved result MUST be visible as unresolved, not converted into a guessed winner to keep a bracket moving.

### 1.3 Non-goals for v1

This work does not create a global ranked service, a public room directory, a payment system, an anti-cheat system, a global ban authority, or a universal account-recovery service. It does not replace GGPO, Iroh, existing room authority, or native match-result detection.

It does not require another player-installed executable. It does not require an Ember-operated central cloud. A platform, organizer, or Ember operator may deploy the bridge.

The first automated tournament mode is **one two-player bracket match on one reserved table in a dedicated room**. The protocol includes table identity from day one, but automated multi-table tournament scheduling is a later capability. Existing casual four-table rooms remain unchanged.

No remotely supplied scripts, DLLs, executable plugins, arbitrary commands, or provider-controlled game memory writes are permitted.

### 1.4 Requirement language

`MUST` and `MUST NOT` describe release requirements. `SHOULD` describes a default that requires an explicit documented exception. `MAY` describes an optional capability. All new behaviors below are proposals unless identified as existing behavior in Section 2.

## 2. Evidence baseline and unresolved external contracts

### 2.1 Inspected Ember baseline

Repository reads for this document were pinned to:

```text
Repository: Confetti3/SF4-Ember-Netplay
Branch inspected: release
Commit: 92aad00459e36b009951a6145b6dab19f6e7f223
```

The branch lookup also returned `feat/ui-consistency` at `99e79b72d82892e09e94b1916bf98ddba229bdad`. That older branch is not treated as the current implementation baseline. Before coding, refresh branch state and review intervening changes; this document is not a whole-repository audit. [E1]

| Existing building block | Observed behavior | Consequence for this proposal |
|---|---|---|
| Rust helper manifest | Pins Iroh `=1.1.0`; includes Tokio, Serde, SHA-256, and Windows bindings. | Extend the helper; pin any new security/storage dependencies separately. [E2] |
| `transport::bind_endpoint_with_policy` | Constructs an Iroh endpoint without supplying `.secret_key(...)`. Iroh documents random default endpoint identity. | Do not change transport-key persistence to implement tournament identity. [E3, S2] |
| `SessionController` | Defines host/join commands, room and match generations, health, recovery, and copied snapshots. | Use existing command/state boundaries; do not manipulate native state from network tasks. [E4] |
| `RoomModel` | Defines members, roles/status, table phases, permissions, and room actions. | Add an optional tournament policy, not a second room implementation. [E5] |
| `RoomRules` | Contains FT1, FT2, FT3, FT5, unlimited, rotation, round-count and timer fields. | A tournament rules translator must validate native semantics rather than invent new values. [E6] |
| `RoomLimits` | Four tables, two fighters per game, sixteen room members. | Carry table IDs and respect current capacity; v1 automation reserves one table. [E7] |
| Native result design | Separates capture from publication and publishes only after GGPO input confirmation. | Reuse this source; never scrape a HUD or infer victory from a disconnect. [E8] |
| `MatchResultOutbox` | Tracks captured results, retry identity, terminal observation, and profile persistence. | Preserve this mechanism, but add a distinct durable external-report outbox. Existing local retries are not an HTTP delivery guarantee. [E9] |

The inspected files do not establish an existing persistent tournament account-linking service. Every account-linking endpoint, identity file, admission certificate, and provider SDK in this document is new work.

### 2.2 BluMint verification status

The user supplied:

```text
https://staging.blumint.io/docs/game-guide
https://staging.blumint.io/docs
```

Both pages failed to load through the browser tool during preparation. A direct container request also failed DNS resolution. No live BluMint endpoint was exercised. Search did not recover a usable authoritative contract. [B1, B2]

Earlier discussion described player lookup, match creation/status callbacks, and a result-submission API. **Those descriptions are treated as provisional integration assumptions, not verified endpoint names, authentication requirements, or JSON schemas.**

This does not block the generic identity/bridge design. It does block declaring the BluMint adapter production-compatible. Section 22 lists the exact contract evidence needed before enabling that adapter.

### 2.3 Companion files

The accompanying JSON schemas define the **cryptographic and event core**, not a complete generated HTTP server. Fixtures exercise deterministic ID derivation, challenge/report signatures, and webhook signatures. The verifier tests those fixtures and schemas only. It does not test Ember, Windows DPAPI, Wine, Iroh networking, or BluMint.

## 3. Terminology and ownership boundaries

| Term | Meaning |
|---|---|
| Ember ID | Stable public identifier derived from one persistent local public key. |
| Identity key | Locally held Ed25519 signing seed/private key. Never a bridge or provider secret. |
| Transport endpoint | Iroh endpoint identity used for a particular helper lifetime; separate from Ember ID. |
| Bridge | Trusted web service managing links, assignments, admission policy, result reconciliation, and delivery. |
| Provider connection | One configured adapter instance, tenant, and environment. `blumint-staging` and `blumint-production` are distinct connections. |
| External subject | Stable account identifier asserted by an authenticated provider, not a user-entered display name. |
| Participant ID | Bridge-scoped opaque handle for an external participant record. May survive re-linking to a replacement Ember key. |
| Bracket match / set | A first-to-N series scheduled by a tournament platform. |
| Game attempt | One actual USF4 game, including an attempt later drawn, voided, or disputed. |
| Native generation | Ember's generation for a game at a particular room/table. Not a global match ID. |
| Assignment generation | Bridge counter identifying a particular roster/rules/room binding. |
| Provisioning lease | Temporary exclusive permission to create/publish a room before that room is established. |
| Admission certificate | Bridge-signed permission binding an Ember ID to an endpoint, match, room, role, and assignment generation. |
| Game permit | Bridge-signed agreement identifying one official game attempt and its immutable participants/rules. |
| Report | A player's signed statement about the result observed by its own client. |
| Adjudication | An organizer's explicit resolution, distinct from an automatically reconciled native result. |

**Authority split:** the provider owns bracket membership and advancement; the bridge owns accepted tournament assignments and result policy; Ember owns room and native game lifecycle; the local key authenticates the reporting identity. These are separate authorities.

## 4. Architecture and deployment

```text
Tournament platform / organizer application
       | authenticated API calls; signed outbound notifications
       v
Provider adapter (BluMint first; separately versioned)
       |
       v
Generic Ember bridge
  - linking and identity registry
  - match state and game-attempt ledger
  - admission issuer and public verification keys
  - provider delivery outbox and webhook workers
  - organizer review and audit UI
       ^
       | outbound HTTPS + authenticated event stream
       |
Existing sf4-net.exe
  - persistent identity and secure storage
  - constrained signing operations
  - bridge client, local report outbox, reconnect/retry
  - tournament admission validation and Iroh endpoint binding
       ^
       | existing protected helper IPC, extended with typed messages
       v
Existing game-side Ember runtime / Sidecar.dll
  - UI and local consent
  - existing SessionController / room authority
  - optional tournament policy and safe start gate
  - rollback-confirmed native result capture
       |
       +---------------- Iroh / GGPO ---------------- other players
```

The bridge is not a gameplay relay, dedicated USF4 instance, or simulation authority. Gameplay input traffic does not travel through the tournament API.

### 4.1 Deployment boundary

The reference deployment consists of a public HTTPS API, a relational database, and background delivery workers. They MAY run in one hosted service initially. Adapters MAY run in that service or externally against its generic API. The choice must not alter the native client contract.

The native client initiates all bridge connections. No incoming public HTTP listener, router forwarding rule, or provider webhook listener is added to players' PCs.

The service operator must be explicit in the UI. Trusting a bridge means trusting its account mapping and tournament permissions. Self-hosting does not automatically grant authority over other bridges' tournaments or links.

### 4.2 Casual mode

With tournaments disabled, no bridge traffic, linked-account query, or tournament heartbeat is generated. Identity/storage/service failure MUST NOT stop local play or ordinary Ember rooms. Any cloud dependency is confined to opted-in tournament operations.

## 5. Security model and invariants

### 5.1 What authentication establishes

A valid identity signature establishes possession of the key corresponding to an Ember ID. A completed link establishes that an authenticated external-account holder approved associating that account with that key. A validated admission certificate authorizes a particular role under a particular bridge policy.

None establishes a unique human, legal identity, ownership of a Steam license, absence of malware, absence of cheating, or correctness of arbitrary client-reported game state.

### 5.2 Threats in scope

Protect against network eavesdropping/tampering; replayed codes, proofs, and deliveries; stolen public room URLs; unauthorized fighters or spectators; cross-tenant/provider confusion; malicious callbacks; stale room generations; duplicate results; hostile or contradictory client reports; accidental identity regeneration; and privacy leakage through logs or crash exports.

### 5.3 Limits of protection

Malware running as the player, a compromised bridge operator, or an attacker with the identity private key remains a serious threat. DPAPI is not a boundary against every process running as the same Windows user. An exported backup is another usable copy of the identity when unlocked. Two colluding clients can sign a fabricated matching result. No wording or UI label may imply otherwise. [S3]

### 5.4 Mandatory invariants

| ID | Invariant |
|---|---|
| SEC-01 | Private identity material never appears in normal logs, URLs, native snapshots, diagnostic exports, or general-purpose IPC responses. |
| SEC-02 | The persistent identity key is not passed to the Iroh endpoint builder. |
| SEC-03 | Possessing a link code alone cannot finalize an external-account link. |
| SEC-04 | Possessing a room invite, match ID, or public match URL alone cannot occupy a tournament fighter seat. |
| SEC-05 | Provider API credentials and bridge signing keys never ship in a player build. |
| SEC-06 | Native thread progress never waits for HTTP, a webhook, key-store I/O, or an expensive KDF. |
| SEC-07 | A room host is not implicitly a tournament organizer. |
| SEC-08 | One native game result is never treated as completion of a multi-game bracket set. |
| SEC-09 | Duplicate observations/deliveries cannot add another game win. |
| SEC-10 | A missing, aborting, or disagreeing report does not imply a winner. |
| SEC-11 | Existing identity data that cannot be read is not silently replaced. |
| SEC-12 | All link, match, and provider operations are scoped to an explicit bridge, tenant, and provider connection. |
| SEC-13 | Native result acknowledgments and external-delivery acknowledgments are different obligations. |
| SEC-14 | A bridge outage may delay tournament progress, but never interrupts an already-started game merely because the bridge disappeared. |

## 6. Persistent identity and identifier format

### 6.1 Cryptographic identity

Use Ed25519 with a 32-byte secret seed and its derived 32-byte public key. Signatures are 64 bytes. Use a maintained library implementation, not hand-written curve arithmetic. The inspected helper already depends on Iroh; a private wrapper may use its `SecretKey` facilities, provided interoperability tests and memory-handling review pass. The wire contract is Ed25519, not an Iroh-specific object representation. [S1, S2]

Generation MUST use operating-system-backed cryptographic randomness through a supported library. Do not seed from a username, timestamp, hardware identifier, room code, MAC address, or user-selected password. RNG failure is an explicit error, not permission to use a weaker generator.

Create the identity on first explicit identity/tournament enablement. Do not create or upload identities merely because an ordinary room opens. Generation occurs only after suitable storage is available, and the identity is not registered with a bridge until it has been persisted and successfully read back.

### 6.2 Canonical Ember ID

Freeze the following v1 derivation:

```text
public_key  = exactly 32 raw Ed25519 public-key bytes
id_input    = ASCII("ember-id-v1") || byte(0x00) || public_key
digest      = SHA-256(id_input)
encoded     = RFC 4648 Base32(digest), lowercase, no '=' padding
ember_id    = "emb1_" || encoded
```

The full ID is 57 ASCII characters: a five-character prefix plus 52 Base32 characters. All 256 hash bits are retained. A receiver MUST recompute it from the public key. It MUST NOT accept a caller's unrelated claimed ID.

Wire public keys use unpadded Base64url of the raw 32 bytes: 43 characters. Wire signatures use unpadded Base64url of the raw 64 bytes: 86 characters. Decoders MUST enforce exact decoded length and canonical re-encoding, not only a regular expression.

### 6.3 Display identifiers

The UI MAY show a shortened fingerprint, for example the first eight and last eight characters of the encoded portion. Copy-ID always copies the full canonical ID. Short fingerprints are for human confirmation only; database keys, authorization, linking, and seat ownership always use the full ID/public key.

Display names are editable labels. They are neither unique account keys nor authentication credentials. Never match users by case-insensitive display-name comparison.

### 6.4 Stability and portability

| Event | Ember ID |
|---|---|
| Ember restart or update, same store | Unchanged |
| Network change or fresh Iroh endpoint | Unchanged |
| Name/language/settings change | Unchanged |
| Link/unlink/re-link to a provider, same identity key | Unchanged |
| Import the same seed on another machine | Unchanged |
| New Windows account or Wine prefix without import | New identity or no identity until enabled |
| Lost key followed by generating a replacement | Changed |
| Cryptographic key rotation | Changed in v1 |

The ID is persistent per stored identity, not inherently a machine ID or a universal person ID. A bridge's participant record may outlive key replacement, but it MUST NOT present the replacement cryptographic ID as the old ID.

## 7. Local key storage, startup, and failure handling

### 7.1 Storage layout

Use a per-user, non-install-directory location resolved through operating-system APIs. Proposed Windows layout:

```text
%LOCALAPPDATA%\Ember\Identity\v1\identity.bin
%LOCALAPPDATA%\Ember\Identity\v1\identity-state.json
%LOCALAPPDATA%\Ember\Tournament\v1\outbox.sqlite
%LOCALAPPDATA%\Ember\Tournament\v1\bridges.json
```

`identity-state.json` contains only public continuity metadata: format version, expected Ember ID, public key, storage backend, and creation time. It is a guard against accidental replacement, not a tamper-proof secret. `bridges.json` contains approved bridge origins/IDs and public metadata, not session tokens.

Do not place the key in the game directory, the release ZIP, a portable-default configuration folder, replay exports, or a directory swept into routine log bundles.

### 7.2 Native Windows backend

Protect the seed and inner metadata with current-user `CryptProtectData`. Use `CRYPTPROTECT_UI_FORBIDDEN`; do not use `CRYPTPROTECT_LOCAL_MACHINE`. Restrict the directory and files to the intended user and system accounts. Resolve the normal interactive user correctly when a launcher is elevated; never accidentally provision a second identity under a different account. [S3]

Encrypted inner metadata MUST include format, algorithm, seed, and the expected public-key/ID binding. After unprotecting, derive the public key again and verify the binding before use. Optional DPAPI entropy, if used, is a fixed versioned application context, not a secret substitute for Windows protection.

Free OS buffers correctly and zero temporary plaintext buffers on a best-effort basis. Do not implement `Debug`/serialization for a live identity object in a way that exposes key material. A public debug view may include only the Ember ID and locked/unlocked state.

DPAPI protection normally depends on the user profile and machine context. Copying a protected file is not a portable backup strategy. Document exceptions and limitations rather than claiming the blob can never be decrypted elsewhere. [S3]

### 7.3 Wine/Proton backend

Do not assume a Wine implementation of DPAPI provides the same at-rest security properties as native Windows. This spec makes no verified claim about a particular Wine/Proton build's protection guarantees.

The required v1 fallback is a **passphrase-encrypted identity file** using the portable envelope in Section 8. Users unlock it once per helper lifetime. The helper retains the decrypted seed only in memory. There is no plaintext fallback and no automatic storage of that passphrase next to the file.

Prefer this backend under detected Wine/Proton. Detection failure MUST NOT silently advertise native-Windows protection. Show the selected backend in diagnostics and allow an explicit encrypted-file choice. Run a protection round-trip self-test before enabling tournament identity.

A future host-keyring integration is optional and requires its own support/security review. It must not be smuggled into v1 as another required executable. Native Linux helper support, if added, uses a separately tested storage backend; it is not assumed to exist today.

### 7.4 Atomic creation

Under a per-user identity-store lock:

1. Check both final key file and continuity metadata.
2. If a valid final key exists, load it; never generate a competing identity.
3. If continuity metadata exists but the key is missing or unreadable, enter recovery-required state.
4. If both are absent, and the user has enabled identity, generate a seed.
5. Protect it and write a restricted temporary file on the same volume.
6. Flush data and atomically install the final file without overwriting another creator's successful file.
7. Read/decrypt/derive/verify the final file.
8. Atomically write the public continuity metadata.
9. Only now report `ready` or contact a bridge.

If another process wins creation, discard and zero the losing seed, then load the winner. If the key is valid but metadata is missing after a crash, reconstruct metadata from the key. A leftover incomplete temporary file is not an established identity.

### 7.5 Errors and lifecycle

Expose `disabled`, `creating`, `locked`, `ready`, `unavailable`, and `recovery_required` states. Distinguish not-found, permission denied, corruption, unsupported envelope, wrong passphrase, and decryption failure internally; user-facing text must not leak secrets.

Identity operations MUST NOT block casual startup. Unavailable identity disables only functions that need it. Never regenerate after a decryption failure, after changing storage backends, after an application update, or after a network outage.

If both identity and continuity files have been deleted, the application cannot prove that an older identity existed. The enablement screen must still offer import before creating a new identity.

## 8. Backup, import, replacement, and revocation

### 8.1 Portable encrypted envelope

Implement encrypted export/import in the first complete identity release. Export uses a user-chosen passphrase, a fresh 16-byte salt, Argon2id, and XChaCha20-Poly1305 with a fresh 24-byte nonce. Use maintained libraries. Argon2 and XChaCha properties are documented in [S4, S5]; parameter choices below are application defaults, not a claim that every value is prescribed by those sources.

Default KDF parameters:

```text
Argon2id version: 19
Memory:          65,536 KiB (64 MiB)
Iterations:      3
Parallelism:     1
Derived key:     32 bytes
```

Benchmark on low-spec supported hardware. KDF work runs on a bounded blocking worker and not during an active game. Imported files are untrusted: cap file size at 16 KiB, memory at 256 MiB, iterations at 6, and parallelism at 4 before allocating or deriving. Reject missing/unknown algorithms and unsupported versions.

Envelope structure:

```json
{
  "header": {
    "format": "ember-key-backup",
    "version": 1,
    "algorithm": "Ed25519",
    "ember_id": "<full Ember ID>",
    "public_key": "<base64url public key>",
    "kdf": {
      "algorithm": "argon2id",
      "version": 19,
      "memory_kib": 65536,
      "iterations": 3,
      "parallelism": 1,
      "salt": "<base64url 16 bytes>"
    },
    "aead": "XChaCha20-Poly1305",
    "nonce": "<base64url 24 bytes>"
  },
  "ciphertext": "<base64url ciphertext including authentication tag>"
}
```

Use `ASCII("EMBER:KEY-BACKUP:1\n") || JCS(header)` as authenticated additional data. Encrypt exactly the 32-byte seed; successful decryption yields 32 bytes, after which the header public key and ID MUST be rederived and checked. Salt and nonce are regenerated for every export, even with the same passphrase.

The default local encrypted-file backend uses the same envelope with a distinct format value `ember-key-local`, a distinct domain `EMBER:KEY-LOCAL:1\n`, and the same validation rules. Do not confuse local-store and backup envelope types.

### 8.2 Passphrase UX

Confirm a new passphrase twice, support paste and password managers, and provide a strength warning. Treat passphrases as exact UTF-8 bytes without trimming or Unicode normalization. Reject inputs over 1,024 UTF-8 bytes. Display that neither Ember nor the bridge can recover a forgotten backup passphrase.

The passphrase may pass once through the existing authenticated local IPC from an explicit unlock/export UI to the helper. That narrowly scoped input is not permission to send the private key through IPC. Avoid retaining passphrases in UI state, history, logs, or generic command tracing.

### 8.3 Import

Validate envelope bounds, decrypt, derive the ID, and show it before confirmation. Import of the same current identity is idempotent. Import of a different identity requires explicit replacement confirmation, logout from bridge sessions, review of active assignments, and atomic replacement only outside a live game.

On native Windows, imported seed material is reprotected with DPAPI for the destination user. Do not overwrite a working identity until the imported identity has passed all checks. Preserve the previous encrypted store until the replacement commit and read-back succeed; do not leave an unencrypted recovery copy.

### 8.4 Lost and compromised keys

A verified external account can authorize linking a new key, but cannot reconstruct the lost old private key. The new Ember ID is different. Preserve historical match records under the original ID.

If compromise is suspected, unlink/revoke at every trusted bridge that used the key, revoke current sessions/admission leases, generate a replacement, and re-link through verified provider authentication. Revocation is bridge-local unless an explicit future federation protocol is adopted.

Do not promise revocation of already-started P2P gameplay. Block new official attempts/admission and flag an in-progress affected match for review. Previously issued permits remain evidence of what was authorized at issuance; they do not authorize unlimited future games.

### 8.5 Multiple machines

Importing one backup on two machines produces the same ID on both. There is no cryptographic way to distinguish legitimate copies of the same key. The bridge therefore permits only one active fighter endpoint lease for a given participant in a given match. A takeover requires an explicit recovery flow and fences the old lease before the next game.

## 9. Bridge trust and cryptographic wire rules

### 9.1 Bridge registration

A bridge profile contains an immutable `bridge_id`, HTTPS origin, display name, API major version, supported capabilities, and public signing-key metadata. A configured default is acceptable; adding another bridge requires explicit user approval.

Discovery at `/.well-known/ember-bridge.json` is informational, not automatic trust. Deep links, QR codes, provider metadata, and match payloads MUST NOT silently add arbitrary signing issuers or network destinations. All examples use reserved `.example` domains, not a claimed deployed Ember service.

TLS certificates MUST be verified. Reject credential-bearing redirects to another origin. No token or proof is sent to a host merely because a server response supplied a URL.

### 9.2 Serialization profile

Use UTF-8 JSON and RFC 8785 JSON Canonicalization Scheme for Ember-signed JSON objects. Reject duplicate keys before converting into a normal map, invalid Unicode, unsupported critical fields, excessive nesting, and non-finite numbers. Unsigned display-only extensions may be tolerated only where specified. [S6]

All native 64-bit counters and revisions travel as canonical nonnegative decimal strings. No leading zeroes except `"0"`. Enforce the native maximum at the application layer. JSON number fields are confined to small exact integers such as table index, score, and Unix-second timestamps.

Do not claim that sorting keys with an ordinary JSON encoder is a complete JCS implementation. The fixture verifier intentionally exercises only a restricted ASCII/integer subset; production must use a conforming canonicalizer.

### 9.3 Signature domains

Freeze these byte prefixes:

```text
Identity derivation: "ember-id-v1" || 0x00
Client challenge:   "EMBER:CHALLENGE:1\n"
Native game report: "EMBER:GAME-REPORT:1\n"
Local key envelope: "EMBER:KEY-LOCAL:1\n"
Backup envelope:    "EMBER:KEY-BACKUP:1\n"
```

A client signs the full canonical structured object after the appropriate prefix, not a bare nonce. Bridge, purpose, target, and request digest are bound by that object. Every supported signing operation has a typed local handler. There is no `sign_arbitrary_bytes` IPC or public API.

### 9.4 Admission and permit tokens

Use an established JWS/JWT library for bridge-signed certificates and permits. The v1 profile uses Ed25519 keys with JOSE `alg=EdDSA`, explicit token `typ`, issuer, audience, and `kid` allowlisting. Reject `none`, HMAC substitutions, unapproved algorithms, embedded key URLs, unexpected token types, and untrusted issuers. Validation policy follows [S7, S8].

No client identity seed, provider API credential, or webhook secret may also be a bridge signing key. Rotation retains old public keys for outstanding permitted lifetimes, with an emergency-revocation path. A token's `kid` selects a key only within the already trusted bridge's key set.

## 10. Authentication and operation proofs

### 10.1 Generic challenge flow

The helper requests a challenge with its public key, an allowed `action`, HTTP method/path, and the digest of the unsigned command body. The bridge constructs and stores the challenge. For authenticated actions, it additionally verifies the current session and the actor's basic authorization before issuing a challenge.

Normative challenge fields:

```json
{
  "version": 1,
  "bridge_id": "brg_<uuid>",
  "audience": "https://bridge.ember.example",
  "challenge_id": "chl_<uuid>",
  "ember_id": "<full Ember ID>",
  "action": "session.create",
  "method": "POST",
  "path": "/v1/sessions",
  "request_digest": "<base64url SHA-256 of JCS(command)>",
  "nonce": "<base64url 32 cryptographically random bytes>",
  "issued_at": 1790812800,
  "expires_at": 1790812860
}
```

The helper MUST validate expected bridge/origin, ID, action, target, digest, bounded lifetime, and the locally pending user operation before signing:

```text
signature = Ed25519.Sign(identity_key,
    UTF8("EMBER:CHALLENGE:1\n") || JCS(challenge))
```

Submit the command through its normal API with a proof object:

```json
{
  "command": { "requested_scopes": ["self:read", "tournament:participate"] },
  "proof": {
    "challenge_id": "chl_<uuid>",
    "signature": "<base64url 64-byte signature>"
  }
}
```

The server retrieves its stored challenge/public key, validates the digest against the actual command, verifies the signature, checks authorization again, and atomically consumes the challenge with the operation's state change. Clients cannot replace stored challenge fields by posting their own challenge object.

### 10.2 Replay and retry semantics

A challenge lives for 60 seconds by default. Its nonce is single-use. A session-creation proof that has already been consumed is rejected; the client requests another challenge rather than needing the server to retain a recoverable bearer token.

For state mutations, use a separate idempotency key and durable request digest. Repeating an already completed operation with the same actor/key/digest returns its recorded result, without performing the mutation twice. A different command under the same idempotency key returns `409 idempotency_conflict`.

Idempotency is not an authorization bypass. Retry lookups are scoped to the original actor and operation. Replaying a proof cannot substitute a different link, match, role, or endpoint.

### 10.3 Sessions

Successful key proof creates an opaque 32-byte random session token, returned only over HTTPS. Default lifetime is ten minutes. Store a keyed hash of the token server-side; keep the token only in helper memory. No long-lived refresh token is required in v1: reauthenticate with the key.

A session is bound in server state to Ember ID, bridge, tenant visibility, scopes, and expiry. It is a **bearer credential** for its permitted read/event operations. It is not magically proof-of-possession on every request. A stolen token can expose data permitted to that session until expiry/revocation.

Therefore link changes, fighter claims, room publication, endpoint replacement, and official-attempt preparation also require a fresh operation-specific signed challenge. Game reports carry their own durable identity signatures. Ordinary status snapshots never include reusable room capabilities or private admission credentials.

A key without an external link may authenticate to manage its own links. It cannot create provider assignments or claim arbitrary players. Key registration is not external-account verification.

### 10.4 Authentication separation

Browser management sessions use an authenticated external-provider channel and CSRF protections. Provider services use separately issued scoped service credentials. Organizers use an authenticated organizer role, with strong authentication and fresh confirmation for adjudication/rebinding. These credentials do not substitute for a player's private-key proof when that proof is required.

## 11. Code-based external account linking

### 11.1 Verified account prerequisite

A link intent must begin from one of two trusted routes:

1. A browser session whose external subject the bridge verified through a supported provider login flow; or
2. A backend-to-backend request from a configured provider connection allowed to assert that subject.

For the second route, the provider may proxy the verified browser user’s explicit approval through its authenticated backend. That narrowly scoped approval must identify the exact intent, claim, subject, and Ember ID; an unauthenticated browser assertion is never accepted. The adapter must document whether approval is performed by a bridge browser session or by this trusted provider proxy.

A posted `provider_user_id`, display name, email string, or screenshot does not establish account ownership. The bridge must not invent a provider verification endpoint. BluMint's ability to provide either trusted route remains a verification gate.

### 11.2 Link code

The bridge generates ten random Crockford-Base32 symbols, displayed as `ABCDE-FGHJK`. This provides 50 bits before online rate limits. Normalize case and the single display separator only; reject other syntax rather than applying surprising substitutions.

Codes expire after five minutes and are single-use at final approval. Store only a server-keyed HMAC of the normalized code; a plain short-code hash would facilitate offline guessing after a database leak. Bind the intent to tenant, provider connection, external subject, expiry, and the initiating authenticated browser/provider context.

The code is a rendezvous secret, not a password and not sufficient account authorization.

### 11.3 Approved flow

```text
Authenticated provider/bridge page      Ember helper                 Bridge
       |                                  |                           |
       | create link intent               |                           |
       |------------------------------------------------------------->|
       | display short-lived code         |                           |
       |<-------------------------------------------------------------|
       |                 user types code  |                           |
       |                                  | signed link.claim command |
       |                                  |-------------------------->|
       |                                  | pending claim + preview   |
       |                                  |<--------------------------|
       | show requested Ember fingerprint |                           |
       |<-------------------------------------------------------------|
       | user checks fingerprint and approves                         |
       |------------------------------------------------------------->|
       |                                  | linked event              |
       |                                  |<--------------------------|
```

Entering the code and confirming the named provider in Ember records client consent. The authenticated browser then approves the **specific claim ID and full Ember ID**, using a CSRF-protected POST. The browser must show that it is linking an Ember installation, not merely logging into an unrelated site.

A code stolen by another client can at most propose a different pending key; it cannot finalize the link without browser approval. The browser displays the same shortened fingerprint shown in Ember and warns against approving unexpected requests. This is a device-linking flow informed by RFC 8628 security considerations, not a claim that these custom endpoints implement OAuth Device Authorization Grant. [S9]

### 11.4 State machine

```text
created -> claim_pending -> approved
    |            |
    +-> expired  +-> denied / expired / cancelled
```

A claim is immutably bound to one Ember ID. It cannot be silently replaced by a different contender while a confirmation screen is open. Allow one live pending claim per intent; give the browser an explicit reset/reject control. Expire abandoned claims promptly and rate-limit attackers so a leaked code cannot be used for unbounded denial of service.

Final approval occurs in a database transaction that locks the intent, verifies it is current, verifies the exact pending claim, checks unique-link constraints, records the link, consumes the code, and queues the completion event. All-or-nothing behavior is mandatory.

### 11.5 Association rules

For v1, one external subject per provider connection has one active Ember ID; one Ember ID has one active subject per provider connection. The same Ember ID may link to different provider connections/platforms.

Re-linking the same association is idempotent. Replacing another active association requires a separate, explicit replacement approval in a freshly authenticated external-account session. Do not overwrite a link simply because a second client knows a code.

Provider subject IDs are opaque strings. Preserve exact bytes according to the provider's contract; do not coerce 64-bit Steam-like IDs through JavaScript numbers.

### 11.6 Unlink

Either the currently linked Ember key, with fresh signed proof, or the verified external-account owner can unlink. Revoke future assignment/admission permissions derived from that association, invalidate applicable sessions/leases, and retain minimal audit history. Unlinking does not delete the local key or change the Ember ID.

An active game's roster is immutable. An unlink or replacement during a live game marks the tournament match for review and blocks the next official attempt; it does not remap the current fighter to another identity.

## 12. Launcher handoff and application UX

### 12.1 No new player executable

Extend existing `Launcher.exe` and `sf4-net.exe` behavior. The launcher may register an `ember:` URI handler, but this is new functionality to implement and test, not an assumed existing handler.

Use a narrow route such as:

```text
ember://tournament/open?bridge=approved-bridge-id&handoff=opaque-single-use-code
```

The `handoff` is a high-entropy, 60-second, single-use server record bound to the expected Ember ID and match. It is not a provider API token, the player's session token, or an unrestricted room invitation. Redemption requires the expected identity's signed proof. Copying a URI alone must not grant access.

Do not place a long-lived credential or a bearer-only signed player ticket in a browser-visible URL. URI handlers can be intercepted or exposed in process arguments; proof-of-possession and narrow lifetime limit that risk.

### 12.2 URI validation

Limit URIs to 2 KiB. Parse once using a strict URI parser. Require the known scheme, host/action, and allowed parameters; reject duplicate security parameters, credentials, fragments, unexpected paths, malformed encodings, embedded NULs, or oversized values. Never build a shell command from a URI or execute an arbitrary URL/path supplied by it.

An unrecognized bridge opens a non-sensitive trust prompt, not an automatic token exchange. Production profiles never accept a staging issuer as an alias. Redact handoff values from launcher logs and Windows command-line diagnostics under Ember's control.

### 12.3 Running-instance behavior

Route a validated handoff to the existing running instance through the project's single-instance mechanism after reviewing its security. If no instance is running, the launcher starts the normal game/helper path with a bounded pending handoff. Do not start a second game instance silently.

If a game is live, queue a notification for after the game. Never force an immediate room change. Local confirmation is required before leaving a casual room or accepting a different tournament assignment. Readiness and character selection remain player actions; an external service cannot impersonate the player's Ready input.

### 12.4 Fallback and accessibility

A browser button is convenience, not a prerequisite. Provide a copyable match/handoff code in Ember and an assignment list fetched after authentication. This is the required fallback for Wine/Proton URI registration, browser restrictions, or unusual Steam launch configurations.

All new UI strings use the existing localization system. Support controller and keyboard navigation, pasted codes, clear account/provider names, visible identity fingerprint, and explicit `waiting for browser approval` status. Never label a pending link as linked.

Required screens: identity enable/unlock/status; copy ID; export/import/reset; approved bridges; enter link code; pending approval; linked accounts/unlink; current assignment; join/provision status; score/report synchronization; and organizer-review-required state.

## 13. Generic match model and rules

### 13.1 Provider-neutral objects

A match-create command contains:

```json
{
  "external_match_id": "tournament-provider-opaque-match-id",
  "game": "usf4",
  "participants": [
    { "participant_id": "epl_<uuid-a>", "ember_id": "<full ID A>", "slot": 0 },
    { "participant_id": "epl_<uuid-b>", "ember_id": "<full ID B>", "slot": 1 }
  ],
  "rules": {
    "games_to_win": 2,
    "draw_policy": "replay_no_score",
    "native_rules_profile": "usf4-standard-v1",
    "edition_policy": "ultra_only",
    "character_policy": "unrestricted_between_games",
    "stage_policy": "p1_selects",
    "input_delay_policy": "ember_existing_ready_policy"
  },
  "observer_policy": "authorized_only",
  "result_policy": "two_player_agreement_or_review",
  "required_build_id": "<agreed compatible Ember build identifier>",
  "metadata": { "round_label": "Winners round 2" }
}
```

Examples with angle-bracket placeholders are explanatory templates, not fixture values. Companion fixtures contain concrete, schema-valid values.

The provider connection and tenant are obtained from service authentication, not trusted from the command's own fields. The bridge MUST resolve each participant against that connection's current approved link. It rejects mismatched IDs, identical fighters, unknown identities, conflicting active assignments, and unsupported rules.

`external_match_id` is opaque and scoped to a provider connection. The bridge assigns its own `match_id`. The uniqueness constraint is `(tenant_id, provider_connection_id, external_match_id)`, not a globally unique provider string.

### 13.2 Stable identities and slot mapping

Freeze the complete two-player mapping for each assignment generation. Native P1/P2 slots, bracket slots, room host, and elected coordination leader are separate concepts. The bridge must never infer a winner from the network host's identity.

`assignment_generation` increases when a roster, required rules, or physical room binding is replaced. Ordinary leader transfer, reconnect within an unchanged binding, or display-name changes do not change it. A replacement carries forward explicitly accepted set score and historical attempts; it does not erase them.

### 13.3 Rules and first-to-N

For the first release, advertise and accept only FT1, FT2, FT3, and FT5 when the rules translator and tests support them. Unlimited is not a tournament completion condition. Reject unsupported values rather than approximating them.

`games_to_win` is the required number of game victories, not the number of rounds in a USF4 game. A draw or voided attempt does not increment either score. A valid first-to-N set finishes when one score reaches N; the other must remain below N.

Native round-count semantics require a verified translator. The current code's `roundCount = 3` default must not be interpreted blindly as three wins or three total rounds. `usf4-standard-v1` must be defined against an observed native configuration and tested before it is advertised. [E6]

Tournament rules also specify edition, allowed character/stage behavior, spectator policy, and whether winner/loser character retention is supported. v1 MUST NOT advertise winner-lock enforcement until the native layer actually enforces it at every rematch and reconnect. Unsupported policy returns `422 unsupported_rules`.

### 13.4 Rules locks

An active tournament binding restricts fighter seats, official start authorization, rules changes, and automatic queue/rotation behavior. The v1 room reserves table 0 and disables casual table rotation for the bound set; completing a set enters tournament post-set state rather than handing a seat to a queued stranger.

Existing casual-room controls remain unchanged outside tournament mode. An organizer override is a bridge action with audit history, not a hidden use of a host's normal room controls.

## 14. Room provisioning, admission, and host migration

### 14.1 Provisioning state

A provider creates a logical match; it does not thereby create a running USF4 room. An eligible assigned player who claims the match obtains the provisioning lease. The bridge uses an atomic compare-and-set, not a timing assumption that two requests cannot arrive together.

Default pre-room provisioning lease: 30 seconds, renewed every ten seconds. The lease is bound to match, assignment generation, Ember ID, Iroh endpoint, helper-instance ID, and fencing counter. An expired lease holder cannot publish a room after a replacement has been selected.

The local runtime receives a typed host request and uses its existing host path. It asynchronously publishes room identity, invitation, supported build, table binding, and the lease's fencing counter through a signed bridge operation.

Once a room is published, lease expiry **does not automatically create another room**. The bridge must reconcile established room state before reprovisioning, especially if a game is running. A network timeout is not proof the old room no longer exists.

### 14.2 Protected invitations

Store room invitations encrypted at rest on the bridge and return them only through authorized claim/admission operations. They never appear in generic match snapshots, outgoing provider webhooks, public pages, or routine audit metadata.

The invitation remains an existing room capability, but is insufficient for tournament participation. New tournament admission checks are layered on top of it.

### 14.3 Admission certificate

An admission JWT's protected type is `ember-admission+jwt`. Required claims include:

```text
Protected header: alg="EdDSA", typ="ember-admission+jwt", kid
Claims: iss, aud="ember-tournament-room", iat, nbf, exp, jti
bridge_id, match_id, assignment_generation
ember_id, public_key
room_id, table_id
role (fighter or spectator), slot (0/1 for fighter; null for spectator)
iroh_endpoint_id, helper_instance_id
rules_digest, roster_digest, required_build_id
lease_id
```

Default lifetime: ten minutes, renewable while the assignment remains authorized. Certificates are signed permissions, not encrypted documents; do not include provider secrets, external account details, or unnecessary personal information.

The local helper and receiving room validate the trusted issuer/key, token type, time bounds, exact room/table, current assignment generation, role/slot, rule/build binding, and the Iroh remote endpoint ID. The admission key's Ember ID must match its public key. Room-side role enforcement must reject an otherwise valid spectator attempting to queue as a fighter.

The helper verifies cryptography; C++ receives bounded validated claims associated with a specific authenticated connection. C++ must still enforce room phase, seat, role, and generation rules. A valid signature is not a substitute for application authorization.

### 14.4 Reconnect and replay

The one-use browser handoff code is not the reconnect credential. Certificate and lease reuse by the same identity/endpoint/helper within the same still-active binding is idempotent. Do not consume an admission token once and thereby make ordinary transport retries impossible.

A different endpoint after helper restart requires a fresh signed claim and bridge-approved lease replacement. The old endpoint is fenced before the next native game. One identity cannot concurrently occupy both fighter seats or operate two live fighter leases for the same match.

Certificates alone cannot promise immediate offline revocation. New official games require a current permit, which is where current bridge authorization is checked. Existing gameplay is allowed to finish rather than relying on real-time central revocation.

### 14.5 Host migration

Persist the validated tournament binding, rules/roster hashes, current permit, and identity-to-member mapping in the room recovery/checkpoint path. Do not replicate private keys, bridge session tokens, or provider credentials.

An ordinary Iroh/room leadership change retains the random room ID and current tournament binding. The new leader must enforce the same admission policy before taking further actions. An admission bypass through restored checkpoints, late joins, or migration is a release blocker.

A genuinely replaced room gets a fresh physical room ID and assignment generation. Accepted game outcomes remain in the bridge's ledger. Late reports from the previous binding can be stored as evidence but cannot silently score a new attempt.

### 14.6 Observer policy

Spectator access is explicit, independently authorized, and not equivalent to organizer privileges. A tournament may issue observer grants to approved broadcast accounts. v1 does not permit an arbitrary public lobby code to become a spectator bypass.

Observer absence must not override the existing bounded spectator-start behavior or create an indefinite web-service dependency. A spectator's report is witness evidence, not automatically a replacement for a missing fighter report.

## 15. Official game permits and readiness

### 15.1 Why a permit exists

A signed identity and a room score alone do not identify which games belong to the scheduled set. A permit gives each official attempt an immutable identity and prevents casual rematches, stale reports, or duplicated room histories from becoming extra bracket wins.

### 15.2 Safe start gate

At a between-game boundary, after both players have chosen Ready:

1. The room owner reserves a prospective native generation without starting native gameplay.
2. Both helpers submit signed preparation acknowledgments for the same match, assignment generation, room/table, prospective native generation, roster digest, rules digest, and required build.
3. The bridge verifies both acknowledgments, both current participant leases, and the absence of an unresolved earlier attempt.
4. In one transaction it creates or returns one `attempt_id` and a signed game permit.
5. Both clients validate and receive the same permit.
6. The room start gate checks it against the committed actual generation before invoking the existing native start path.

Waiting is represented in the orchestration state/UI. It never blocks a render/simulation thread on a future or a network request. If readiness, roster, generation, or settings change while approval is pending, discard the prospective start and require a matching new preparation.

### 15.3 Permit contents

Use `typ=ember-game-permit+jwt`, an audience distinct from admission tokens, and claims for bridge, match, assignment generation, attempt ID, permit ID, physical room/table, native generation, P1/P2 Ember IDs, rules/roster hashes, build, and issue/start-expiry times.

The start authorization expires after two minutes by default. Expiry prevents starting a new game; it does not invalidate an already-started game's result. Signed reports are reconciled against the durable permit record, not rejected just because a ten-minute game outlasted a two-minute start window.

A permit cannot be reused for a different generation or another native game. Admission verification and game permission must be complete before any remote peer reaches the native GGPO start path.

### 15.4 Offline policy

Default tournament policy is **finish current game, pause before next official game** during a bridge outage. Keep unsent evidence and show `Tournament service unavailable; current game unaffected`.

Do not pre-authorize unlimited future games in v1. An offline/manual continuation mode, if an organizer selects it, is explicitly unverified/manual tournament operation until reconciliation or adjudication. Casual Ember remains independent.

## 16. Result capture, reconciliation, and set scoring

### 16.1 Capture boundary

Observe the existing rollback-confirmed native result. The documented publication boundary is a saved state N whose contained inputs are confirmed through at least N-1. This is a local rollback-safety guarantee within the running implementation, not remote attestation. [E8]

Freeze native room ID, table ID, native generation, tournament assignment generation, attempt/permit ID, slot mapping, and rules/build identifiers at attempt start. Do not recover these later from mutable room seats or whichever table the UI currently displays.

Capture after native confirmation, not after a HUD transition and not during a speculative re-simulation. A cancelled prediction must never generate an official report.

### 16.2 Signed report

Each assigned fighter signs its own report using `EMBER:GAME-REPORT:1\n || JCS(report)`. Required fields are:

```text
version, bridge_id, match_id, assignment_generation
attempt_id, permit_id, observation_id
room_id, table_id, match_generation
reporter_id, opponent_id, p1_id, p2_id
rules_digest, roster_digest, build_id
result: p1_win | p2_win | draw | abort | cancel
capture_frame, confirmed_input_frame
observed_at, helper_instance_id
```

`capture_frame` and `confirmed_input_frame` are decimal strings for an observed native win/draw; they may be null for an abort/cancel observation. A native outcome must satisfy the reported confirmation bound. The server can validate internal consistency of that claim, but cannot prove a modified client reported its real memory state.

The signed-report transport object contains the report, public key, and signature. Recompute reporter ID from the public key. A host forwarding an opponent's report must forward that original signed object unchanged; the host's own signature cannot count for both players.

### 16.3 Independent agreement

The bridge accepts an automatically scored game only when it has valid reports from **both distinct assigned keys** agreeing on the immutable attempt identity and outcome. Compare normalized match/room/generation/roster/rules/build fields and P1/P2 result.

Different local observation IDs, timestamps, or qualifying confirmation frames are not by themselves a disagreement; clients may publish at different poll times. Opposite winners, different slot mappings, different permits, or inconsistent rules are disagreements.

The UI calls this `Player reports agree`, not `Cheat-proof` or `Server-verified gameplay`. A signed result identifies who reported it; it does not make the result intrinsically true.

### 16.4 Report identity and conflicts

Use `(bridge_id, attempt_id, reporter_id, observation_id)` to identify a report. Store its canonical digest and signature. Exact retries return the original receipt. The same report identity with different bytes returns a conflict and preserves an audit entry.

Maintain the selected outcome per `(attempt_id, reporter_id)` but retain every valid conflicting submission as evidence. Never overwrite a signed result with a later contradictory one. Even a new observation ID from the same player cannot count as the second player's vote.

Separately enforce the unique scoring effect for `attempt_id`. Do not rely only on a network event ID, in-memory cache, process epoch, leader term, or current table score to prevent duplicate wins.

### 16.5 Set score

The bridge derives set score from accepted, non-voided attempts under its durable ledger. The room scoreboard is useful presentation but is not the external accounting source, because room seats/score may reset or a room may be replaced.

Example FT2: A wins, B wins, A wins produces `2-1`. Only then may the bridge emit the set-level completion event and submit a final result to the provider. Individual game results still produce game-level events.

A draw produces no score increment and may authorize a new attempt under the configured replay policy. An abort/cancel produces no automatic win. A deliberate organizer forfeit is an adjudicated tournament outcome, not a forged native P1/P2 report.

### 16.6 Missing or disputed reports

After one report, wait up to 60 seconds by default for the other. If it is missing or contradictory, enter `needs_review` and block the next official attempt. Late agreement may resolve missing-report review automatically only if no organizer decision, cancellation, key-compromise flag, or competing conflict has intervened.

Require explicit organizer decisions for conflicting valid outcomes, disconnect forfeits, no-shows, forced room replacement with uncertain results, or report loss. Decisions carry actor, reason, evidence references, expected match revision, and resulting score/outcome.

After a completed provider submission, a correction is a new revision and `match.corrected` event. Do not silently mutate old accepted evidence or assume the provider supports undoing bracket advancement.

### 16.7 Local durable external outbox

Create a distinct helper-owned report spool. A runtime event is copied into a bounded queue, signed by the helper, and durably recorded before the helper claims it is safe to retry later. HTTP delivery runs from that spool with exponential backoff and idempotent receipts.

Do not make existing native terminal acknowledgments wait for a provider webhook, HTTP success, or indefinite storage retry. If the new spool cannot accept a report, expose `report not durably saved`, stop the next official attempt, and request reconciliation. A full-process crash before any durable handoff can lose an observation; this must lead to missing-report handling, not an invented guarantee of lossless delivery.

Store to SQLite with a transaction and durability settings appropriate for acknowledged records, on a dedicated worker. Signed report payloads must be immutable once queued. On restart, retry pending records after authentication. Preserve exact signed bytes or reconstruct from the same canonical object without semantic changes.

Default automatic report-submission window is 24 hours from the bridge's recorded attempt, not from a caller-controlled timestamp. Older evidence can enter organizer review. Do not apply the 60-second authentication-challenge expiry to an offline durable game report.

## 17. Match lifecycle and failure policy

### 17.1 Explicit tournament state machine

This layer needs its own small set-level state machine. It cannot be replaced by mapping every `TablePhase::Playing`/`MatchEnded` into provider `running`/`complete`.

```text
created -> awaiting_players -> provisioning -> ready
                                            |
                                            v
                                  running <-> between_games
                                      |             |
                                      +-> awaiting_reports
                                                |
                              +-----------------+----------------+
                              v                 v                v
                        between_games      needs_review       completed

Any nonterminal state may become cancelled through authorized policy.
Irrecoverable setup failure becomes failed, with an explicit reason.
```

`needs_review` is recoverable through evidence or adjudication. `completed`, `cancelled`, and `failed` are terminal for that revision. Changes after completion are explicit correction/replacement records.

Track provider delivery separately: `not_required`, `queued`, `delivering`, `delivered`, `retrying`, `failed`, or `ambiguous`. A set can be internally completed while bracket submission is delayed.

### 17.2 Failure matrix

| Situation | Required behavior |
|---|---|
| Bridge unavailable before entry | Keep casual play available; show assignment temporarily unavailable. |
| Bridge disappears mid-game | Let game finish; spool reports; block next official game. |
| HTTP response lost after storing report | Retry original report; unique scoring effect remains one. |
| Duplicate provider create request | Return existing logical match when command matches. Reject conflicting reuse. |
| Hosting lease expires before room publication | Reassign atomically; reject old lease's late publication. |
| Heartbeat missing after room established | Reconcile; never immediately spawn a second room. |
| Fighter disconnects before valid result | No automatic native win; review/forfeit policy. |
| Room host transfers to another member | Preserve tournament binding and seat authorization. |
| Helper crashes but persistent key survives | Reauthenticate same Ember ID; recover endpoint lease at a safe boundary. |
| Identity store unreadable | No automatic new key; tournament identity unavailable; casual mode intact. |
| Provider rejects final submission | Keep internal result; show operator delivery failure; retry only when safe. |
| Provider timeout after possible acceptance | Mark delivery ambiguous; reconcile using provider-supported status/idempotency before repeating. |
| Player unlinks while assigned | Revoke future permissions; freeze live-game mapping; review as needed. |
| Report arrives for old room/assignment | Store evidence; never apply to the newer attempt implicitly. |
| Organizer cancellation while game is live | Record cancellation; do not forcibly kill native gameplay by default; disallow automatic further scoring. |

### 17.3 Reconnect event synchronization

Each bridge snapshot includes an event cursor and revision from a consistent database view. A client reconnects with its last cursor. When the cursor is too old, the server returns `resync_required` and a fresh snapshot/cursor pair; events after that cursor are then applied.

Never combine a snapshot from one transaction with a watermark from a later transaction that could skip intervening events. Clients ignore older revisions and deduplicate event IDs. A missing event is recovered by replay or a snapshot, not by guessing local state.

## 18. Helper IPC and runtime isolation

### 18.1 Proposed native-facing operations

Add typed, version-negotiated operations such as:

```text
IdentityGetStatus / IdentityEnable / IdentityUnlock
IdentityExport / IdentityImport / IdentityResetConfirmed
BridgeSelect / LinkClaim / LinkCancel / LinkRemove
TournamentListAssignments / TournamentAccept / TournamentLeave
TournamentObserveState / TournamentObserveNativeResult
```

Responses contain public identity, state, safe display fields, validation errors, and action/result identifiers. They do not expose identity seeds, provider credentials, bridge bearer tokens, or a generic signing function.

Do not reuse `RecordResult` as a way for an external platform to manufacture a native result. Native result observations originate from the validated game path; organizer outcomes are a separate bridge contract.

### 18.2 Ownership

C++ remains the owner of game/session/room mutations. Rust owns key material, bridge HTTP, admission cryptography, and external-delivery persistence. The bridge owns participant linking, official attempt permits, result acceptance, and provider delivery.

Network tasks enqueue validated immutable messages. The C++ owner applies them only at its existing safe boundaries. No background task retains references to mutable room or native game memory. [E4, E5]

### 18.3 Isolation inside the existing helper

Separate tournament queues/tasks from Iroh control and gameplay queues. Bound HTTP concurrency, JSON sizes, cryptographic work, disk retries, and event backlogs. Use cancellation tokens and structured task ownership. Blocking KDF/storage work must not occupy the helper's critical network executor indefinitely.

A shared process is still a shared crash boundary. Adding this module does not provide process isolation. Every parser and worker must avoid unchecked panics and unbounded memory growth; a tournament task failure must be reported and stopped without intentionally terminating the existing helper.

No key generation, KDF, import/export, schema migration, or bulk webhook processing is permitted on a gameplay-sensitive thread. Enrollment/backup UI operations are deferred while a native game is active.

### 18.4 IPC trust limits

Keep the project's local authenticated helper IPC protections and peer-process checks. Review its existing authorization before extending it. Do not turn a new loopback HTTP server into an unauthenticated shortcut.

A compromised process running as the user may still manipulate its own client or access same-user resources. Restricting the IPC surface reduces exposure; it is not anti-cheat or a guarantee against local malware.

## 19. Public API contract

### 19.1 Common rules

Use `/v1/` for this API major. HTTPS is mandatory outside explicit local test fixtures. JSON request limit is 64 KiB except smaller route-specific limits. Reject oversized/deep bodies before expensive processing. Times are UTC Unix seconds for signed data and RFC 3339 strings in event metadata.

All state-changing service/client requests require an `Idempotency-Key` except one-use session creation and explicitly ephemeral heartbeat observations. Keys are scoped by authenticated actor, tenant, method, and canonical path. Store command digests and durable outcomes. Do not permit an attacker to select another actor's idempotency namespace.

`If-Match`/expected revision is required for cancellation, rebind, adjudication, and configuration changes that can race. A stale revision returns `409 stale_revision` or `412 precondition_failed`, consistently documented per endpoint.

### 19.2 Authentication classes

- **Public:** bounded capability discovery and challenge issuance; no private match data.
- **Player:** authenticated key session; fresh signed proof for privileged client mutations.
- **Browser:** verified external-account session, CSRF protection, recent-auth checks where specified.
- **Provider:** scoped backend service credential mapped to a tenant/provider connection.
- **Organizer:** authorized tournament operator within a tenant, not merely a room host.

### 19.3 Endpoint inventory

| Method and path | Actor | Contract |
|---|---|---|
| `GET /.well-known/ember-bridge.json` | Public | Bridge ID/origin, API version, supported discovery URLs; does not establish trust by itself. |
| `GET /v1/capabilities` | Public | Supported game/rules profiles, API/event versions, public size limits and optional features. |
| `GET /v1/signing-keys` | Public | Approved bridge public JWKs, key IDs, validity windows; fetched only from configured origin. |
| `POST /v1/auth/challenges` | Public/Player | Public key + action + target + command digest; returns stored 60-second challenge. |
| `POST /v1/sessions` | Signed proof | Verifies `session.create`; returns opaque token, scopes, and expiry. |
| `DELETE /v1/sessions/current` | Player | Revokes current session; does not unlink or reset key. |
| `GET /v1/identity` | Player | Own Ember ID and own bridge state; no global identity directory. |
| `POST /v1/link-intents` | Browser/Provider | Creates intent for an already verified external subject; returns code/expiry once. |
| `GET /v1/link-intents/{id}` | Owning Browser | Pending exact claimant, fingerprint, provider label, and expiry. |
| `POST /v1/link-claims` | Player + proof | `link.claim`, code, explicit chosen provider/bridge context; creates pending claim. |
| `POST /v1/link-intents/{id}/approve` | Owning Browser / authorized Provider proxy | Explicit verified-user claim/key approval; browser uses CSRF protection; proxy is scoped to the exact subject/connection; transactionally consumes intent. |
| `POST /v1/link-intents/{id}/cancel` | Owning Browser | Cancels unapproved intent. |
| `POST /v1/link-claims/{id}/cancel` | Owning Player + proof | Cancels that key's pending claim. |
| `GET /v1/links` | Player | Lists own approved/pending links, masked external labels, and status. |
| `DELETE /v1/links/{id}` | Linked Player + proof / Owning Browser | Unlink, revoke derived future permissions, and audit. |
| `POST /v1/players/resolve` | Provider | Resolve only that connection's verified external subjects to bridge participant handles/current Ember IDs. |
| `POST /v1/matches` | Provider | Validate roster/rules and create logical set; return match ID, public-safe match URL, and revision. |
| `GET /v1/matches/{id}` | Assigned Player/Provider/Organizer | Authorized snapshot, score, lifecycle, report and delivery state, consistent cursor; no invites/tokens. |
| `GET /v1/assignments` | Player | Own assignments, pagination/cursor; no arbitrary-player query. |
| `POST /v1/matches/{id}/claims` | Assigned Player + proof | Claim/recover endpoint lease; return host job, waiting state, or protected join/admission material. |
| `POST /v1/matches/{id}/room` | Provisioning holder + proof | Publish physical room with current fencing token and assignment generation. |
| `POST /v1/matches/{id}/leases/{lease}/renew` | Lease holder + proof | Renew pre-room provisioning or authorized endpoint lease; validates exact holder and generation. |
| `POST /v1/matches/{id}/attempts/prepare` | Assigned Player + proof | Submit own prepared descriptor; returns pending or same jointly authorized game permit. |
| `POST /v1/matches/{id}/reports` | Player session + signed report | Store own immutable report; acknowledge durable receipt, not implied scoring/completion. |
| `POST /v1/matches/{id}/observations` | Assigned Player | Bounded nonauthoritative presence/start/failure observations; cannot create wins or rewrite roster. |
| `POST /v1/matches/{id}/cancel` | Provider/Organizer | Versioned cancellation with reason and in-flight policy. |
| `POST /v1/matches/{id}/rebind` | Organizer | Controlled room/endpoint recovery outside active native gameplay; increments binding generation when needed. |
| `POST /v1/matches/{id}/adjudications` | Organizer | Versioned score/forfeit/void/replay/correction decision with reason/evidence. |
| `POST /v1/handoffs` | Owning Browser/Provider | Mint expected-player-bound one-use application handoff after authorization. |
| `POST /v1/handoffs/redeem` | Expected Player + proof | Consume handoff and return assignment reference; does not bypass match admission. |
| `GET /v1/events` | Player/Provider/Organizer | Cursor-based authorized event replay and continuation token. |
| `GET /v1/events/stream` | Player/Provider/Organizer | Authenticated SSE stream; same event IDs/replay contract as event API. |
| `POST /v1/webhook-subscriptions` | Provider/Organizer | Register allowed event types and reviewed HTTPS destination; secret returned once. |
| `DELETE /v1/webhook-subscriptions/{id}` | Subscription owner | Disable future delivery; purge pending credentials under retention policy. |
| `POST /v1/webhook-subscriptions/{id}/rotate-secret` | Subscription owner | Issue new secret and bounded overlap window. |

Provider callback routes under `/adapters/{provider}/...` are adapter-owned translations, not additions to the core native protocol. Their exact names depend on the verified provider contract.

### 19.4 Common responses

A created match response includes `match_id`, `external_match_id`, `state`, `revision`, and a public-safe `match_url`. A claim response has `kind=host|wait|join|recovery_required`, `assignment_generation`, `lease_id`, expiry, and only the credentials appropriate to that actor. A preparation response has `state=pending|authorized`, `attempt_id` when allocated, and the permit when both acknowledgments match.

A report receipt includes `report_id`, `durable=true`, `duplicate`, `attempt_state`, and `match_revision`. `durable=true` means the bridge stored that report; it does not mean the opponent agreed or the provider accepted a final result.

A player-presence record includes observed connection/readiness, observation time, and `stale`. An absent heartbeat cannot be represented as proof of a voluntary forfeit.

### 19.5 Error contract

```json
{
  "error": {
    "code": "stale_assignment",
    "message": "This assignment has been replaced. Refresh the match.",
    "retryable": false,
    "request_id": "req_<uuid>",
    "details": { "current_assignment_generation": "4" }
  }
}
```

Required stable codes include: `invalid_request`, `unsupported_version`, `unauthenticated`, `forbidden`, `challenge_expired`, `challenge_used`, `invalid_signature`, `identity_unavailable`, `link_expired`, `link_pending_approval`, `link_conflict`, `idempotency_conflict`, `stale_revision`, `stale_assignment`, `lease_conflict`, `unsupported_rules`, `incompatible_build`, `report_conflict`, `needs_review`, `rate_limited`, `resync_required`, and `service_unavailable`.

Use HTTP 400/401/403/404/409/412/422/429/503 appropriately. Hide existence where enumeration would leak private accounts/matches. Only expose current-generation details to an already authorized actor. Return `Retry-After` on rate limiting/transient service throttling.

## 20. Events and webhooks

### 20.1 Delivery direction

Provider webhooks terminate at the bridge or a provider adapter, never a player's computer. The helper maintains an outbound event connection or polls the bridge. External systems subscribe to bridge events; they do not inject native game-state events into Ember.

### 20.2 Event envelope

Use CloudEvents 1.0 structured JSON with `specversion`, stable `id`, `source`, `type`, `subject`, `time`, `datacontenttype`, `dataschema`, and typed `data`. Add an `emberseq` extension as a decimal-string stream cursor. The core event names and fields are frozen per major version. [S10]

Example event type: `io.ember.tournament.match.completed.v1`. `source` is the configured bridge origin, not a client-supplied string. `subject` identifies the bridge match. Event data contains a provider-neutral result and report/adjudication references, not provider credentials or room capabilities.

Required event families:

```text
identity.link.pending / identity.link.completed / identity.link.removed
match.created / match.assignment.changed / match.room.ready
match.player.present / match.player.ready / match.started
match.attempt.authorized / match.game.reported / match.game.confirmed
match.score.changed / match.needs_review / match.completed
match.cancelled / match.failed / match.corrected
provider.delivery.succeeded / provider.delivery.failed / provider.delivery.ambiguous
```

Apply the namespace and `.v1` suffix consistently. Player-private identity events are never broadcast to unrelated subscribers.

### 20.3 Webhook authentication

Use the Standard Webhooks HMAC-SHA256 profile and a maintained verifier. Headers are `webhook-id`, `webhook-timestamp`, and `webhook-signature`; signing input is the ASCII event ID, a dot, the ASCII Unix timestamp, a dot, and the exact raw request-body bytes. Signature header values use the profile's versioned format. [S11]

Each subscription has its own random 32-byte secret. The event body/id remain stable across retries; delivery timestamp/signature are regenerated for each attempt. Timestamp tolerance is five minutes by default. Reject invalid signatures and stale timestamps before processing JSON. Verify using constant-time comparison and the documented secret encoding, not an application-specific accidental variant.

Accept only configured event types/sources and bound payload sizes. Signature validity does not eliminate the need for tenant/subject authorization or schema validation.

### 20.4 Durable, at-least-once delivery

Commit a state transition, event, and outbox record in one database transaction. A worker sends the event and marks success only after a successful receiver response. Delivery is at least once; ordering is not guaranteed. Exactly-once delivery across independent HTTP systems is not promised.

Receivers persist/deduplicate event IDs and make their own side effects idempotent. A receiver should acknowledge with 2xx only after durable acceptance into its own queue or transaction. The request need not wait for all downstream processing.

Suggested retry schedule: 1 s, 5 s, 15 s, 1 min, 5 min, 15 min, then hourly, with jitter and a 24-hour automatic budget. Honor `Retry-After`. Pause on credential failures; disable/flag consistently gone endpoints; retain dead-letter records for review. Never block a native game or existing room receipt on webhook acknowledgment.

### 20.5 Outbound-destination security

Webhook URLs are configured by authorized service owners, not arbitrary match metadata. Allow HTTPS/443 only by default, disallow credentials and redirects, and validate destinations against an explicit policy. Block loopback, private/link-local networks, multicast, cloud metadata endpoints, and unsafe IPv6 equivalents.

Revalidate DNS/address policy at connection time and enforce egress controls; registration-time validation alone is insufficient against DNS rebinding. Bound connect/total timeouts, response size, retries, and concurrent requests. Do not allow custom headers to overwrite security-critical delivery headers.

### 20.6 Client event transport

SSE is the baseline client stream; an SDK may later add WebSocket without changing event semantics. Send keepalives every 20 seconds. Authenticate through headers in the native client, not tokens in query strings. Reconnect with jitter and the last committed cursor. A 30-second polling fallback is supported.

The event stream is notification, not sole authority. Reconcile with snapshots after gaps or contradictory revisions. Separate low-value/coalescible presence events from non-droppable game/report/assignment transitions.

## 21. Provider SDK and adapter contract

### 21.1 SDK scope

Publish a language-neutral HTTP/JSON specification plus a small TypeScript SDK for platform developers and Rust types/client helpers for `sf4-net`. JSON Schemas are the wire contract; SDK classes are conveniences. No native C++ ABI or dynamically loaded provider DLL is required.

The first SDK should expose:

```text
getCapabilities()
resolvePlayers(subjects)
createMatch(spec, idempotencyKey)
getMatch(matchId)
cancelMatch(matchId, expectedRevision, reason)
createLinkIntent(verifiedSubject)
createHandoff(authorizedMatch, expectedPlayer)
verifyWebhook(rawBody, headers, subscriptionSecret)
parseEvent(validatedPayload)
```

Provide typed errors, timeout/backoff hooks, idempotency support, and fixture-based conformance tests. Do not hide an ambiguous provider write behind an automatic infinite retry.

### 21.2 Adapter interface

A provider adapter translates a **verified provider contract** into generic commands/events. Its responsibilities include authenticating provider callbacks, mapping external subjects and IDs, translating rules/status/result shapes, final submission, and reconciling ambiguous provider responses.

Conceptual service-side interface:

```text
ProviderAdapter
  authenticateInbound(request) -> ProviderContext
  normalizePlayerLookup(payload) -> ResolvePlayersCommand
  normalizeMatchCreate(payload) -> CreateMatchCommand
  normalizeStatusQuery(payload) -> MatchReference
  projectMatchSnapshot(snapshot) -> ProviderStatusPayload
  submitFinalResult(acceptedResult, deliveryKey) -> DeliveryOutcome
  reconcileDelivery(deliveryRecord) -> DeliveryOutcome
```

`DeliveryOutcome` is `accepted`, `retryable`, `permanent_failure`, or `ambiguous`, with safe diagnostic detail. Never make the core bridge assume every provider has idempotent writes, webhooks, OAuth login, result correction, or status polling.

### 21.3 Capability negotiation

Publish capabilities including external-login/subject-assertion support, match creation, status, final score submission, result correction, idempotent writes, authorized observers, supported rules profiles, and optional scheduling features.

A generic webhook consumer can use the service without a dedicated adapter. An unsupported provider feature is explicitly rejected or handled through organizer review, not silently approximated. Future platform adapters are possibilities, not claims of compatibility already established for start.gg, Challonge, or another service.

## 22. BluMint adapter and verification gates

### 22.1 Proposed mapping, pending confirmation

| Provisional platform need | Generic bridge responsibility | Verification required |
|---|---|---|
| Resolve a game's player identity | `players/resolve` after approved external-account link | Exact subject identifiers, request/response schema, batch behavior, and privacy scope |
| Create a scheduled match | `matches` with frozen participant mapping and rules | Callback authentication, provider match key, retry behavior, and return fields |
| Retrieve match/player status | Project generic state/presence into platform states | Enumerations, polling semantics, stale/pending/review representation |
| Submit final score | Adapter consumes accepted set result | Correct endpoint/base URL, payload, idempotency/reconciliation, score/winner semantics |
| Player clicks Play | Authorized handoff to existing launcher | Whether platform supports a per-player authenticated handoff or only a shared match URL |
| Link account | Verified browser login or trusted subject assertion | Provider support; lookup callbacks alone do not establish account ownership |

Do not hard-code the previously mentioned `/api/tournaments/match/submit` path as a verified fact. Do not ship example player-lookup JSON from the earlier conversation as a production schema without the actual documentation or approved fixtures.

### 22.2 State translation rules

Whatever names BluMint ultimately requires, the adapter must preserve these meanings:

- Waiting for players, provisioning, and between-game readiness are not completed matches.
- One confirmed native game is not a completed FT2/FT3 set.
- `needs_review` cannot be mapped to a guessed winner. If the platform has no review state, hold it in a nonterminal state and surface operator attention.
- Player `ready` requires the game's actual valid readiness state for the assigned attempt, not merely a connected browser or a platform check-in flag.
- A bridge-completed result is not automatically provider-delivered. Track both statuses.

### 22.3 Required provider evidence before production

Obtain versioned docs or a provider-approved OpenAPI/JSON schema snapshot; real callback fixtures with secrets removed; exact authentication/secret-rotation rules; staging and production base URLs; match identity/idempotency behavior; supported result corrections; expected timeouts/retries; account-ownership/linking support; player launch/handoff behavior; and an authorized staging test account/tournament.

Run contract tests against those fixtures and a complete two-player staging set, then inject duplicate creation, duplicate result, response loss, invalid auth, expired link, disconnected player, and a disputed result. A provider contact must confirm how an ambiguous final submission is reconciled before automation is enabled.

The generic bridge is allowed to ship/test with a mock provider while these gates remain open. The BluMint adapter must remain disabled by default until they are closed.

## 23. Data model, consistency, and persistence

### 23.1 Required records

Use transactional storage with explicit uniqueness and foreign-key constraints. PostgreSQL is the reference database choice for the hosted service; an alternative must demonstrate equivalent concurrency/durability behavior. This is an architectural selection, not a claim that a particular hosting service is required.

| Record | Required contents and constraints |
|---|---|
| `identities` | Full Ember ID, raw public key, status, created time; unique public key; derive/check ID on insertion. |
| `provider_connections` | Tenant, provider kind/environment, scopes, encrypted provider credentials, configuration revision, enabled state. |
| `external_accounts` | Connection ID + exact external subject, bridge participant ID, safe display label; unique within connection. |
| `links` | Account, identity, approval provenance, consent timestamps, revoked time; one active link per account and per identity/connection. |
| `link_intents` | Code HMAC, owner account/context, expiry, state, pending claim, failure/rate counters; never plaintext code at rest. |
| `link_claims` | Intent, exact key/ID, proof reference, client consent, browser approval, state. |
| `auth_challenges` | Public key, canonical challenge, target/digest, expiry, consumed marker; atomically single-use. |
| `sessions` | Token HMAC, key/ID, scopes, expiry, revocation; no recoverable long-lived token in logs. |
| `matches` | Tenant/connection/external key, bridge match ID, immutable create digest, current revision/generation, lifecycle and accepted score. |
| `match_participants` | Frozen participant/Ember ID/slot mapping per assignment generation. |
| `room_bindings` | Match/generation, physical room/table, rules/roster/build binding, encrypted invitation, replacement history. |
| `leases` | Actor/endpoint/helper, match/generation, role, lease expiry and fence; one active fighter lease per assigned player. |
| `attempts` | Match/assignment, permit and attempt IDs, native generation, immutable descriptor, state, score effect. |
| `reports` | Attempt/reporter/observation key, canonical payload bytes/digest, public key/signature, received time; append-only conflicts. |
| `adjudications` | Organizer, expected revision, reason, evidence, explicit result/void/forfeit/correction, timestamp. |
| `events` | Stable ID, tenant sequence, type, subject, revision, immutable payload, creation time. |
| `delivery_outbox` | Event/subscription or provider result revision, state, attempts, next attempt, safe last error. |
| `provider_submissions` | Provider/match/result revision, stable delivery key, payload digest, remote acknowledgment, ambiguous/reconciled state. |
| `webhook_subscriptions` | Tenant/owner, validated endpoint, event filter, encrypted secrets/rotation state, enabled state. |
| `idempotency_records` | Actor/tenant/method/path/key, command digest, operation state, replayable nonsecret result, retention deadline. |
| `audit_entries` | Actor class and ID, action, target, result, reason, request ID; no secret request bodies. |

### 23.2 Transaction boundaries

The following are single transactions or equivalent serializable critical sections:

- Link approval + uniqueness checks + code consumption + completion event/outbox.
- Hosting lease acquisition/replacement + incremented fencing counter.
- Match creation + immutable participants/rules + create event.
- Two-player preparation barrier + unique official attempt/permit allocation.
- New report + agreement evaluation + unique score effect + match revision + game/set events.
- Organizer adjudication + score recalculation + correction event + delivery record.
- Provider delivery-state transition + audit/event update.

A uniqueness constraint or row lock must protect against two workers scoring the same attempt concurrently. Application-level `if not exists` followed by a separate insert is insufficient.

### 23.3 Idempotency and retention

Preserve match-create uniqueness for the lifetime of the provider match record. Preserve attempt scoring uniqueness and terminal revision records for the tournament's supported history window. A short Redis TTL alone is not adequate duplicate suppression.

A default 24-hour HTTP response-cache retention is acceptable for ordinary idempotency replies, but expiring that cache must not remove durable domain uniqueness. If large/private result payloads are purged, retain enough nonsecret tombstone state to reject duplicate score effects within the supported replay window.

Receivers never treat provider match IDs or native generation numbers as interchangeable. All database lookups include their tenant/provider/room/assignment scope.

### 23.4 Service restart and disaster recovery

After restart, resume pending outbox work, expire old challenges/link intents, reconcile leases against established-room state, and recover incomplete deliveries. Do not reset identities, match revisions, or score ledgers.

Back up encrypted provider/webhook secrets and bridge signing-key recovery material separately from application data, with access controls and tested restoration. A restored database must not blindly reuse an old provisioning fence or reissue an already consumed game permit as a new attempt. During uncertain restore, pause issuance and reconcile active tournaments before reopening writes.

Test restoration of a completed set with an ambiguous provider submission. The recovered bridge must not produce a duplicate bracket advance.

## 24. Privacy, audit, and operational security

### 24.1 Data minimization

An Ember ID is a persistent, potentially linkable pseudonym. Separating it from an ephemeral Iroh endpoint avoids making every transport identity a permanent account identity, but does not make tournament participation anonymous. The same ID disclosed to multiple bridges can be correlated.

Send persistent ID/public key only to approved bridges and peers that require tournament admission. Ordinary casual rooms do not broadcast the tournament ID by default. A bridge must not expose a public reverse-lookup graph of all linked provider accounts.

Store only the external subject identifiers and display data required for linking and tournament operation. Avoid collecting emails, real names, IP history, or hardware identifiers unless a separately justified feature requires them. Display labels do not need to include email addresses.

### 24.2 Logs and diagnostic exports

Never log private keys, backup passphrases, link codes, bearer tokens, provider credentials, admission token strings, room invitations, handoff codes, or raw webhook secrets. Redact authorization headers and query parameters before tracing. Use safe request IDs, coarse errors, and nonsecret hashes where correlation is necessary.

Default support bundles exclude the entire identity directory and tournament credential stores. Crash dumps may contain live in-memory secrets despite application logging rules; disable automatic public upload and warn users before sharing full helper/game dumps. Secret-handling design cannot guarantee that process memory never enters an OS dump.

### 24.3 Default retention policy

Suggested operational defaults, to be made visible and configurable by the operator:

- Link intents/challenges: remove expired secret-bearing state within 24 hours; retain minimal abuse counters briefly.
- Session records: remove expired token hashes after the incident/debug window, default 24 hours.
- Detailed native reports and adjudication evidence: 90 days after tournament completion.
- Outbox/dead letters: 30 days after final resolution unless operator policy requires longer.
- Active approved links: until the user or provider unlinks; retain minimal revocation/audit references as disclosed.
- Raw networking telemetry: disabled by default for identity linking; short retention only for opt-in diagnostics.

These are design defaults, not assertions of legal compliance. Public deployment needs an operator-approved privacy policy and retention/access process.

### 24.4 Service administration

Separate player, provider-service, organizer, and infrastructure-admin roles. Restrict credentials by tenant and capability. Production and staging use separate provider connections, service credentials, signing keys, databases or isolated namespaces, and webhook secrets.

Use encrypted secrets at rest, least-privilege service accounts, TLS, dependency scanning, and rate limiting. Audit secret rotation, organizer adjudication, link replacement, bridge-key changes, and report conflicts.

A provider adapter must not gain permission to operate unrelated tenants or forge a player's proof. A database entry saying a key exists is not permission to disclose its linked accounts to every adapter.

## 25. Limits, performance, and observability

### 25.1 Initial protocol limits

These are proposed defaults and acceptance targets, not measured properties of existing Ember.

| Item | Default |
|---|---|
| Link code | 10 Base32 symbols; 5-minute lifetime |
| Authentication/operation challenge | 32-byte nonce; 60-second lifetime |
| Helper session | 10 minutes; reauthenticate rather than persist refresh token |
| Browser handoff | 32 random bytes encoded Base64url; 60-second lifetime; one-use |
| Pre-room provisioning lease | 30 seconds; renew every 10 seconds |
| Fighter endpoint lease | 90 seconds; renew every 30 seconds; no mid-game forced termination on expiry |
| Admission certificate | 10 minutes; renew only while authorized |
| Game-permit start window | 2 minutes; duration of started game not limited by this window |
| Missing-opponent-report grace | 60 seconds before review |
| Automatic report replay window | 24 hours from bridge-recorded attempt |
| URI length | 2 KiB |
| Link/proof request size | 8 KiB |
| Signed native-report size | 8 KiB |
| General API/event payload | 64 KiB |
| JSON nesting | 16 levels maximum |
| External subject / metadata value | 256 / 1,024 UTF-8 bytes |
| Metadata total | 8 KiB; display-only, never authorization policy |
| Local pending report spool | 2,000 records or 32 MiB, whichever first; pause next official attempt when full |
| In-memory terminal/control queue | Bounded separately; reserve capacity for result and cancellation events |
| Helper HTTP concurrency | 4 tournament requests maximum, separate from gameplay I/O |
| SSE keepalive / polling fallback | 20 / 30 seconds |
| Automatic webhook retry budget | 24 hours, with dead-letter review afterward |

### 25.2 Rate limiting

Apply limits per key, source network, link intent, external account, and tenant rather than solely IP; shared/NAT networks must remain usable. Initial ceilings: 30 challenge requests per minute per key; ten link-code submissions per ten minutes per key; five incorrect attempts against an identifiable intent; and limited concurrent pending intents per external account.

The valid path must remain usable during an IP-level attack. Codes cannot be enumerated through different error texts or timing. Use backoff and server-side HMAC lookup. Avoid a permanently locked external account from unauthenticated hostile traffic.

### 25.3 Performance acceptance

No additional per-frame HTTP calls, file writes, key derivations, or signing operations are allowed. Native result handling copies bounded data into a queue. Identity startup/signing and report serialization occur outside the simulation critical path.

Measure baseline versus tournament-enabled builds on supported low-spec systems, including a four-GB-memory test machine/profile, Windows, and Wine/Proton. Exercise HTTP stalls, report retries, event floods, and storage faults during matches. Record median/p95/p99 frame times, overruns, helper CPU/RSS, queue occupancy, and native networking latency.

A release fails if bridge worker activity introduces sustained game-frame overruns, unbounded memory growth, or starvation of existing Iroh/gameplay work. Numeric budgets may be tightened after baseline measurement; this document does not assert an already demonstrated performance improvement or line-count estimate.

### 25.4 Operational metrics

Measure identity-store failure category, link expiry/approval duration, challenge failure rates, claims/provisioning latency, stale leases, report durability/delivery latency, one-sided/conflicting reports, review backlog, duplicate suppression, provider delivery ambiguity, webhook age, and resync frequency.

Metrics must not label series with raw IDs, full provider subjects, or secrets at unbounded cardinality. Use aggregate counters, bounded reason codes, and restricted trace lookups.

## 26. Compatibility and packaging

### 26.1 Independent versions

Version identity envelope, ID derivation, signed challenge/report schema, bridge API, event schema, helper IPC, tournament admission policy, and provider adapter independently. A bridge API change must not silently change a user's Ember ID.

Minor API changes are additive only where parsers allow them. Signed security objects have strict versioned schemas; unknown required/security-critical fields are rejected. Clients must negotiate capabilities before accepting a tournament assignment.

### 26.2 Mixed builds

Tournament rooms require the new admission/start-gate capability. An older client that lacks those checks must be rejected from a tournament room even if its casual room protocol would otherwise connect. Never downgrade a protected match into a normal room automatically.

Existing casual compatibility rules remain unchanged. The new feature must not implicitly upgrade Iroh, GGPO, or the room wire protocol merely to obtain a convenient identity API. Any necessary version change gets its own review and tests.

### 26.3 Packaging

Players receive the normal Ember package with updated launcher/helper/game-side components. There is no mandatory `Tournament.exe`, `BluMint.exe`, background Windows service, or browser extension.

The bridge and provider SDK are separate deployable/developer artifacts. Source may live in the same repository under distinct directories or a separate repository. Releases of provider adapters do not require a new gameplay binary unless the generic protocol actually changes.

Updates preserve identity and tournament state directories. Uninstall leaves identity data by default or asks separately before deleting it. Do not sweep it away as a cache. Diagnostics and release packaging tests must scan for accidentally included real key files or production secrets.

### 26.4 Disabled/unsupported feature behavior

Feature flags may disable enrollment, a specific bridge/provider adapter, new match issuance, automatic result acceptance, or deep links independently. Disabling tournament automation never disables casual play. Security-critical failures do not silently fall back to weaker authentication or plaintext key storage.

## 27. Implementation work packages

### WP0 — Contract and baseline

Pin an implementation branch, review current helper IPC and room recovery, document native rules translation, and capture provider-independent schemas. Record all external/provider unknowns. Deliver a mock bridge/provider and test fixtures before changing game integration.

**Exit:** baseline tests preserved, exact source ownership mapped, unsupported policies explicitly rejected.

### WP1 — Local identity

Implement identity wrapper, deterministic ID, secure RNG, Windows DPAPI backend, encrypted-file backend, store locking/atomic writes, public status, encrypted backup/import, and explicit replacement UX. Add negative/corruption tests and update-preservation tests.

Suggested new helper modules:

```text
rust/sf4-net/src/identity/
  mod.rs          # private key wrapper; public identity view
  id.rs           # frozen derivation / encoding
  store.rs        # load/create state and atomic persistence
  windows.rs      # current-user DPAPI wrapper
  encrypted.rs    # portable encrypted envelope
  backup.rs       # explicit export/import
  proof.rs        # allowed canonical message signing
```

These are proposed files, not claims that they exist now.

**Exit:** one identity survives restart/update and explicit export/import; unreadable state never triggers automatic replacement; casual play survives all injected storage failures.

### WP2 — Bridge identity and linking

Build bridge configuration/trust metadata, challenge/session service, verified external-account entry point, link intent/claim/browser approval, unlink/replacement, RBAC, rate limits, and audit.

**Exit:** two independently implemented clients derive matching IDs/signatures; a stolen link code cannot finalize a link; cross-tenant/provider tests pass.

### WP3 — Generic SDK, event log, and mock provider

Publish schema package and TypeScript/Rust bindings, core match CRUD, durable outbox, Standard Webhooks signing/verifying, replay/snapshot synchronization, provider-service scopes, and an operator review UI skeleton.

**Exit:** an external sample application creates a match and consumes a verified event without any BluMint code in the client/helper.

### WP4 — Native tournament binding and room entry

Add generic `TournamentCoordinator`/binding state on the native side, typed IPC, runtime opt-in UX, protected host/join workflow, roster/rules enforcement, signed admission validation, and checkpoint/migration persistence.

Suggested boundaries to inspect/update:

```text
src/netplay/SessionController.*
src/session/RoomModel.* and room/session authorization paths
src/session/MatchAuthority.* and pre-native start authorization
src/sf4e/sf4e__NetplayRuntime__Room.cxx
src/sf4e/sf4e__NetplayRuntime__Match.cxx
src/platform/HelperClient.*
rust/sf4-net/src/service/*
rust/sf4-net/src/transport.rs
```

Exact changes must follow an implementation review; the listing is not permission to bypass existing room-authority checks.

**Exit:** old/unauthorized clients cannot occupy reserved roles; no extra executable; host transfer/reconnect preserve policy.

### WP5 — Game permits and report reconciliation

Implement the pre-start barrier, immutable attempt binding, copied native result export, signed durable reports, two-player agreement, set-score ledger, review/adjudication, and outbox recovery.

Use the existing confirmed-result reader and preserve its tests. Extend or observe existing terminal-result handling without making it depend on bridge or provider acknowledgments.

**Exit:** FT2/FT3 scoring, rollback-corrected results, room replacement, duplicate reports, disagreement, process crashes, and service outages all behave according to this spec.

### WP6 — Launcher convenience and platform validation

Implement strict URI handling and running-instance delivery, assignment UI, code fallback, localization, Windows/Proton smoke tests, and safe backup UX.

**Exit:** a browser click or copied code reaches the assigned room; an active game cannot be interrupted by a handoff; no secret appears in exported logs.

### WP7 — BluMint adapter

Only after Section 22 gates close: implement verified callback translations, player lookup, launch/link flow, final submission/reconciliation, credential rotation, and full staging fixtures.

**Exit:** provider-approved staging tournament succeeds with fault injection, and no platform name or schema has leaked into native gameplay/session code.

### WP8 — Hardening and release

Fuzz parsers, exercise adversarial tests, validate backup/restore, benchmark low-spec systems, run canary tournaments with human review, and document operator runbooks.

**Exit:** release gates in Section 29 are met. Do not substitute a successful single match for identity/security/recovery validation.

## 28. Acceptance and adversarial test matrix

Each test below must become an automated test or an explicitly recorded manual/platform test. Requirements about unavailable production services remain unpassed until actually exercised.

### 28.1 Identity and storage

| ID | Test / required result |
|---|---|
| ID-01 | Fixed known seed yields the exact fixture public key and Ember ID. |
| ID-02 | Windows restart and update load the same key and ID. |
| ID-03 | New Iroh endpoint leaves Ember ID unchanged. |
| ID-04 | Display-name, language, and provider-link changes leave ID unchanged. |
| ID-05 | RNG failure returns an error; no fallback or partially registered key. |
| ID-06 | Concurrent helper startup installs only one identity. |
| ID-07 | Crash at every creation/write/rename step does not replace an established key. |
| ID-08 | Wrong user, invalid DPAPI blob, permission denial, and corruption do not regenerate a key. |
| ID-09 | Missing key with existing continuity metadata enters recovery-required state. |
| ID-10 | Valid key with missing metadata repairs metadata from the key. |
| ID-11 | Same backup imported elsewhere yields the same ID. |
| ID-12 | Wrong passphrase, modified ciphertext/header, truncated file, and unsupported KDF fail closed. |
| ID-13 | Import bounds reject excessive memory/time parameters before resource allocation. |
| ID-14 | Different-key import requires confirmation and preserves old store on failure. |
| ID-15 | Export, logging, and support bundles contain no plaintext seed/passphrase. |
| ID-16 | Windows normal/elevated launch does not unintentionally create different users' identities. |
| ID-17 | Wine/Proton uses the declared, tested backend; no silent plaintext/weak fallback. |
| ID-18 | Locked/unavailable identity does not block casual play. |

### 28.2 Authentication and linking

| ID | Test / required result |
|---|---|
| AUTH-01 | Challenge signature validates across Rust and bridge implementations. |
| AUTH-02 | Modified audience, action, method, path, digest, ID, nonce, or expiry fails verification. |
| AUTH-03 | Expired/consumed challenge cannot authenticate again. |
| AUTH-04 | A fresh proof for one command cannot authorize another command under an idempotency key. |
| AUTH-05 | Duplicate JSON keys and noncanonical binary encodings are rejected before authorization. |
| AUTH-06 | Session expiry/revocation removes access; player tokens do not work on provider/admin APIs. |
| AUTH-07 | Bearer session alone cannot finalize linking or claim a new fighter endpoint. |
| AUTH-08 | Untrusted bridge/deep-link origin cannot obtain signatures or tokens silently. |
| LINK-01 | Verified browser plus entered code plus matching fingerprint approval completes a link. |
| LINK-02 | Stolen code plus attacker's key cannot complete the link without authenticated browser approval. |
| LINK-03 | Unauthenticated claimed provider ID cannot create an approvable account association. |
| LINK-04 | Approval races consume the intent once and retain exactly one approved mapping. |
| LINK-05 | A pending claimant cannot be swapped while approval is displayed. |
| LINK-06 | Expired/cancelled code never links; retries of the same completed association are idempotent. |
| LINK-07 | Cross-provider/environment/tenant code reuse is rejected. |
| LINK-08 | Existing link replacement requires fresh external authentication and explicit approval. |
| LINK-09 | Unlink does not change key/ID but revokes derived future permissions. |
| LINK-10 | Guessed codes are rate-limited without permanent unauthenticated account lockout. |
| LINK-11 | Unicode/external numeric IDs preserve the provider's exact identity semantics. |
| LINK-12 | Provider lookup cannot enumerate unrelated users or reveal another tenant's links. |

### 28.3 Room, permissions, and lifecycle

| ID | Test / required result |
|---|---|
| ROOM-01 | Simultaneous first claims produce one valid provisioning lease. |
| ROOM-02 | A late expired host cannot publish a competing room. |
| ROOM-03 | Established-room heartbeat loss does not automatically create another room. |
| ROOM-04 | Public URL/invite without valid identity admission cannot occupy a fighter seat. |
| ROOM-05 | Copied certificate used from another Iroh endpoint/helper is rejected. |
| ROOM-06 | Valid spectator cannot queue into a reserved fighter seat. |
| ROOM-07 | Unknown issuer, wrong token type, `none`, wrong algorithm/key, or stale assignment is rejected. |
| ROOM-08 | Same-endpoint reconnect is idempotent; a restarted helper performs approved rebind. |
| ROOM-09 | Same Ember key on two devices cannot own two active fighter leases in one match. |
| ROOM-10 | Host transfer preserves the complete tournament authorization policy. |
| ROOM-11 | Checkpoint restore/late join cannot strip tournament rules or admit an old client. |
| ROOM-12 | Bracket slot and transport host independence cannot invert the winner. |
| ROOM-13 | Rule/roster change while readiness awaits a permit invalidates the prospective start. |
| ROOM-14 | One permit cannot authorize two generations or two official games. |
| ROOM-15 | Expired start permission stops a new game, not an already-running one. |
| ROOM-16 | Browser handoff during gameplay cannot interrupt or open another game instance. |
| ROOM-17 | Manual code/assignment fallback works without URI integration under Proton. |
| ROOM-18 | Casual room behavior, queue rotation, and spectator features regress neither when disabled nor enabled elsewhere. |

### 28.4 Results and accounting

| ID | Test / required result |
|---|---|
| RESULT-01 | A discarded speculative native winner produces no external result. |
| RESULT-02 | Confirmation that occurs during idle/final polling still produces a report. |
| RESULT-03 | Signed report from one player cannot count for both participants. |
| RESULT-04 | Same-outcome reports with different valid local confirmation frames may agree. |
| RESULT-05 | Conflicting outcomes/slot mappings/rules produce review, not a score. |
| RESULT-06 | Missing second report produces review after grace; no inferred disconnect win. |
| RESULT-07 | Duplicate report/event/HTTP delivery after restart adds no extra game win. |
| RESULT-08 | Same observation identity with changed bytes is a retained conflict. |
| RESULT-09 | FT2 sequence A/B/A produces exactly one final `2-1` set result. |
| RESULT-10 | Draw produces no increment and a distinct replay attempt. |
| RESULT-11 | Abort/cancel does not produce a native victory; organizer forfeit is separately marked. |
| RESULT-12 | Late old-room result cannot score a replacement binding. |
| RESULT-13 | Host migration does not change report identity or duplicate an accepted attempt. |
| RESULT-14 | Spool disk-full/queue-full pauses next official attempt without blocking native teardown. |
| RESULT-15 | Helper crash before durable report produces visible missing evidence, never a made-up result. |
| RESULT-16 | Bridge crash during two-report reconciliation has all-or-nothing score/event/outbox effect. |
| RESULT-17 | Permit's start expiry does not invalidate a long game's legitimate report. |
| RESULT-18 | Provider outage leaves local set complete and delivery pending, with no native-game dependency. |
| RESULT-19 | Correction creates a new result revision and explicit provider reconciliation path. |
| RESULT-20 | Signed/colluding fabricated reports are not described as anti-cheat proof in UI/docs. |

### 28.5 Web, provider, and operations

| ID | Test / required result |
|---|---|
| WEB-01 | Raw-body webhook fixture verifies; body/header mutation fails. |
| WEB-02 | Old delivery timestamp fails; legitimate retry with new timestamp and same event ID deduplicates. |
| WEB-03 | Key rotation supports bounded old/new overlap and then rejects old secret. |
| WEB-04 | Receiver crash after durable enqueue produces no duplicate side effect on retry. |
| WEB-05 | Out-of-order/gapped events recover via cursor and consistent snapshot. |
| WEB-06 | Event stream authorization hides unrelated matches and identity links. |
| WEB-07 | Loopback/private/metadata targets, unsafe IPv6, redirects, and DNS rebinding are blocked. |
| WEB-08 | Malicious callback response cannot exhaust memory, pin all workers, or affect Iroh work. |
| PROVIDER-01 | Real provider fixtures pass request/auth/response contract tests. |
| PROVIDER-02 | Retried match creation cannot schedule a second logical match. |
| PROVIDER-03 | Timed-out result acceptance becomes ambiguous until verified, not blindly resubmitted. |
| PROVIDER-04 | Provider without correction/review support triggers an explicit operator path. |
| PROVIDER-05 | Staging credentials/keys cannot authorize production matches. |
| OPS-01 | Bridge backup/restore preserves identities, permits, accepted scores, and delivery uniqueness. |
| OPS-02 | Secret scanning catches a intentionally seeded test secret in a release/support archive. |
| OPS-03 | HTTP stalls/event floods/KDF attempts do not cause gameplay-thread I/O or sustained frame overruns. |
| OPS-04 | Feature disablement leaves casual play and existing identity untouched. |
| OPS-05 | Canary rollback disables new permits without stranding records or duplicating provider delivery. |

## 29. Rollout, rollback, and release gates

### 29.1 Rollout sequence

Begin with local identity/backup tests, then private bridge linking against a mock provider. Add observation-only native reporting without automatic bracket submission. Compare reports to human-reviewed sets. Enable protected rooms/permits and automatic two-player reconciliation for a small canary tournament. Only then enable a verified provider's final submission.

Keep automatic submission behind a per-provider/per-tournament flag. During early events, make organizer review readily available and preserve the ability to enter results manually without falsifying native evidence.

### 29.2 Required release gates

**G1 Identity continuity:** Windows and Proton persistence/export/import/error cases pass.  
**G2 Account ownership:** a verified provider identity channel and two-sided approval exist.  
**G3 Admission:** room invite alone cannot bypass the new role/endpoint gates.  
**G4 Native safety:** no network/storage/crypto blocking in the simulation path; baseline regressions pass.  
**G5 Score correctness:** deduplicated attempt ledger, two-player reconciliation, and organizer review survive faults.  
**G6 Delivery:** transactional outbox, authenticated webhooks, and ambiguous provider-write handling pass.  
**G7 Recovery:** helper/bridge restart, room migration/replacement, and backup restoration pass.  
**G8 Provider contract:** versioned BluMint evidence and authorized staging end-to-end test complete before enabling its adapter.  
**G9 Operator readiness:** secrets, monitoring, backups, privacy notices, and incident/runbook ownership are assigned.

### 29.3 Rollback plan

Disable new tournament issuance and automatic provider delivery first; do not delete identity stores or pending evidence. Let already-started native games finish. Preserve snapshots and outbox records for reconciliation. A native build rollback must not convert an already protected tournament room into an unprotected casual room.

Keep schema migrations forward/backward planning explicit. An older helper that cannot read the new identity envelope reports unsupported storage; it does not generate a replacement. A rollback that cannot safely consume pending reports uses an export/operator path, not silent loss.

## 30. Decisions and remaining sign-offs

### 30.1 Decisions fixed by this draft

Persistent per-user Ed25519 identity; deterministic full-length public ID; separate ephemeral transport identity; no new player executable; generic independently hosted bridge; typed SDK/API rather than injected provider plugins; verified-provider code linking with browser confirmation; Windows DPAPI plus tested encrypted-file fallback; encrypted backup/import; key-bound room admission; official game-attempt permits; independent fighter reports; explicit set-level state machine; durable at-least-once delivery; and unchanged casual/offline availability.

### 30.2 Sign-offs required before implementation is considered complete

| Topic | Needed decision/evidence | Default until resolved |
|---|---|---|
| Bridge operation | Who hosts, maintains, and supports the first bridge and controls its signing keys? | Local/mock environment; no public production claims. |
| External account ownership | Which verified BluMint login/subject-assertion route is actually supported? | Linking adapter disabled. |
| BluMint API | Exact schemas, auth, retries, result semantics, and recovery endpoints | Contract gate open; mock provider only. |
| Native rules mapping | Observed USF4 round/edition/stage/character semantics | Advertise only tested named profiles. |
| Proton secure storage | Confirmed encrypted-file implementation and launch/URI behavior on supported builds | Manual code fallback; no DPAPI-equivalence claim. |
| Organizer policy | Forfeit/no-show/disconnect rules, review authority, response process | No automatic inferred wins. |
| Performance | Measured budgets on low-spec Windows and Proton machines | Targets, not demonstrated results. |
| Security review | Crypto serialization, link hijacking, admission migration, backup and webhooks | Draft protocol not production-approved. |

### 30.3 Future work deliberately deferred

Multiple independent device keys under a recoverable human account; maintaining a stable logical account ID across key rotation; passkeys or hardware-backed signing; provider-independent global account recovery; multi-table scheduler; public room discovery; broadcast-specific observer automation; offline batch permits; and additional bracket adapters.

These must not be implemented by changing `emb1_` derivation semantics in place. A logical account identity above keys would be a separate versioned concept with explicit recovery/trust rules.

## 31. Worked end-to-end scenario

1. A player opens Tournaments for the first time. `sf4-net` generates and safely stores a local seed, derives the public key/Ember ID, and offers encrypted backup. No external account has been created yet.
2. The player authenticates on an approved provider/bridge page. The bridge receives a verified external subject and generates a five-minute link code.
3. The player enters that code in Ember. The helper proves possession of its local key and submits a pending link claim. Both sides display the same Ember fingerprint.
4. The authenticated browser approves that exact claim. The bridge atomically links the provider participant to the Ember ID. The code can no longer be used.
5. A provider service creates an FT2 match for two linked participants. The bridge resolves current identities, freezes slots/rules/build, and returns a logical match reference.
6. Players accept the assignment through a launcher handoff or Ember's assignment list. The first eligible claimant acquires a fenced provisioning lease, and the existing native host path creates a room.
7. The helper publishes that room. Both players obtain identity/endpoint-bound admission certificates. The room checks those certificates in addition to its existing invitation capability.
8. Both players choose Ready. Matching preparation acknowledgments cause the bridge to allocate official attempt 1. The native start gate accepts its permit and the normal Iroh/GGPO game begins.
9. A wins. Each client independently reaches the existing native confirmation boundary, copies a result observation, and its helper signs/spools the report. The bridge verifies both distinct identities and reconciles the same outcome. Set score becomes `1-0`; the bracket does not advance yet.
10. After the next shared permit, B wins game 2. Score becomes `1-1`.
11. A wins game 3. Exactly three accepted scoring attempts yield `2-1`. The bridge creates a set-completion event and one provider-delivery record in the same transaction.
12. The adapter submits the provider's verified final-result format. If a response is lost after possible provider acceptance, it marks delivery ambiguous and reconciles instead of risking a second advance.
13. The player restarts Ember the next day. The same stored seed produces the same Ember ID, regardless of the new Iroh endpoint. Existing bridge links still refer to that ID.
14. If the player moves to another PC, importing the encrypted backup preserves the same ID. Re-linking a newly generated replacement key instead would create a new ID and requires explicit association replacement.

This is the intended meaning of “generate a key once, link it with a code, then reuse that identity”: durable local identity, explicit external-account consent, and reusable provider-neutral automation without putting a tournament platform in the rollback implementation.

## 32. Sources and verification notes

Repository references below are pinned to the inspected commit. They ground statements about existing code; they do not imply the proposed features have been implemented. Standards references ground selected primitives/formats. Application policies, timeouts, file paths, limits, and module names are design choices in this document.

### Ember primary-source references

**[E1] Repository branch lookup, inspected 2026-10-01.**  
`https://api.github.com/repos/Confetti3/SF4-Ember-Netplay/branches?per_page=100`

**[E2] Helper dependency manifest.**  
`https://github.com/Confetti3/SF4-Ember-Netplay/blob/92aad00459e36b009951a6145b6dab19f6e7f223/rust/sf4-net/Cargo.toml`

**[E3] Iroh transport construction and admission code.**  
`https://github.com/Confetti3/SF4-Ember-Netplay/blob/92aad00459e36b009951a6145b6dab19f6e7f223/rust/sf4-net/src/transport.rs`

**[E4] Native session-controller ownership and commands.**  
`https://github.com/Confetti3/SF4-Ember-Netplay/blob/92aad00459e36b009951a6145b6dab19f6e7f223/src/netplay/SessionController.hxx`

**[E5] Room model, actions, permissions, and member state.**  
`https://github.com/Confetti3/SF4-Ember-Netplay/blob/92aad00459e36b009951a6145b6dab19f6e7f223/src/session/RoomModel.hxx`

**[E6] Native room rules.**  
`https://github.com/Confetti3/SF4-Ember-Netplay/blob/92aad00459e36b009951a6145b6dab19f6e7f223/src/common/RoomRules.hxx`

**[E7] Room/table/fighter limits.**  
`https://github.com/Confetti3/SF4-Ember-Netplay/blob/92aad00459e36b009951a6145b6dab19f6e7f223/src/common/RoomLimits.hxx`

**[E8] Native rollback-confirmed match-result design.**  
`https://github.com/Confetti3/SF4-Ember-Netplay/blob/92aad00459e36b009951a6145b6dab19f6e7f223/docs/design/NATIVE_MATCH_RESULT.md`

**[E9] Existing result outbox interface and persistence boundary.**  
`https://github.com/Confetti3/SF4-Ember-Netplay/blob/92aad00459e36b009951a6145b6dab19f6e7f223/src/netplay/MatchResultOutbox.hxx`

### Standards and platform references

**[S1] RFC 8032, Edwards-Curve Digital Signature Algorithm (EdDSA).** Ed25519 primitive, keys/signatures, and test vectors.  
`https://www.rfc-editor.org/rfc/rfc8032`

**[S2] Iroh SecretKey and endpoint-builder documentation.** The retrieved SecretKey page described `generate`, `public`, `sign`, and byte conversion; builder documentation described default random endpoint identity. The exact pinned dependency remains the implementation source of truth, not the moving `latest` URL.  
`https://docs.rs/iroh/latest/iroh/struct.SecretKey.html`  
`https://docs.rs/iroh/latest/iroh/endpoint/struct.Builder.html`

**[S3] Microsoft CryptProtectData documentation.** Current-user/machine scope, roaming-profile exceptions, and machine-scope flag behavior.  
`https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata`

**[S4] RFC 9106, Argon2.** KDF algorithm and parameter/security discussion; application defaults are specified separately in Section 8.  
`https://www.rfc-editor.org/rfc/rfc9106`

**[S5] Libsodium XChaCha20-Poly1305 documentation.** AEAD construction and nonce/key use.  
`https://doc.libsodium.org/secret-key_cryptography/aead/chacha20-poly1305/xchacha20-poly1305_construction`

**[S6] RFC 8785, JSON Canonicalization Scheme.** Canonical signed serialization, duplicate-property rejection, number/string constraints.  
`https://www.rfc-editor.org/rfc/rfc8785`

**[S7] RFC 8725, JSON Web Token Best Current Practices.** Algorithm, audience, issuer, and cross-token confusion safeguards.  
`https://www.rfc-editor.org/rfc/rfc8725`

**[S8] RFC 8037, EdDSA in JOSE.** Ed25519 key/signature representation for the selected JWS profile.  
`https://www.rfc-editor.org/rfc/rfc8037`

**[S9] RFC 8628, OAuth 2.0 Device Authorization Grant.** Code-flow threat considerations; Ember's custom linking API is not represented as an OAuth grant implementation.  
`https://www.rfc-editor.org/rfc/rfc8628`

**[S10] CloudEvents specification v1.0.2.** Structured event envelope and context attributes.  
`https://github.com/cloudevents/spec/blob/v1.0.2/cloudevents/spec.md`

**[S11] Standard Webhooks specification.** Signed raw-body delivery profile and header semantics.  
`https://github.com/standard-webhooks/standard-webhooks/blob/main/spec/standard-webhooks.md`

### Unverified provider references

**[B1] BluMint game guide — supplied by user; unavailable during this run.**  
`https://staging.blumint.io/docs/game-guide`

**[B2] BluMint documentation index — supplied by user; unavailable during this run.**  
`https://staging.blumint.io/docs`

No provider API schema, live endpoint success, production service domain, security audit completion, Windows/Proton execution, or game test result is asserted on the basis of these unavailable pages.

---

**End of specification.**
