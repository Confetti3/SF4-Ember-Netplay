# Implementation and release checklist

**Status:** No product acceptance test below has been executed as part of this specification package. The separate validation report covers documentation fixtures only.

## Work packages

- [ ] **WP0: Contract and baseline.** Record implementation commit, review, and exit evidence from Section 27.
- [ ] **WP1: Local identity and encrypted backup/import.** Record implementation commit, review, and exit evidence from Section 27.
- [ ] **WP2: Bridge authentication and account linking.** Record implementation commit, review, and exit evidence from Section 27.
- [ ] **WP3: Generic SDK, event log, and mock provider.** Record implementation commit, review, and exit evidence from Section 27.
- [ ] **WP4: Protected native tournament binding and room entry.** Record implementation commit, review, and exit evidence from Section 27.
- [ ] **WP5: Game permits, signed reports, and reconciliation.** Record implementation commit, review, and exit evidence from Section 27.
- [ ] **WP6: Launcher convenience and Windows/Proton validation.** Record implementation commit, review, and exit evidence from Section 27.
- [ ] **WP7: Verified BluMint adapter.** Record implementation commit, review, and exit evidence from Section 27.
- [ ] **WP8: Hardening and release.** Record implementation commit, review, and exit evidence from Section 27.

## Release gates

- [ ] **G1: Identity continuity.** See Section 29 for required evidence.
- [ ] **G2: External account ownership and two-sided approval.** See Section 29 for required evidence.
- [ ] **G3: Key/endpoint-bound admission.** See Section 29 for required evidence.
- [ ] **G4: Native safety and regression preservation.** See Section 29 for required evidence.
- [ ] **G5: Score correctness and adjudication.** See Section 29 for required evidence.
- [ ] **G6: Durable delivery and ambiguous-write recovery.** See Section 29 for required evidence.
- [ ] **G7: Process, room, and database recovery.** See Section 29 for required evidence.
- [ ] **G8: Verified provider contract and staging integration.** See Section 29 for required evidence.
- [ ] **G9: Operator/security/privacy readiness.** See Section 29 for required evidence.

## Acceptance and adversarial tests


### ID

- [ ] **ID-01** Fixed known seed yields the exact fixture public key and Ember ID.
- [ ] **ID-02** Windows restart and update load the same key and ID.
- [ ] **ID-03** New Iroh endpoint leaves Ember ID unchanged.
- [ ] **ID-04** Display-name, language, and provider-link changes leave ID unchanged.
- [ ] **ID-05** RNG failure returns an error; no fallback or partially registered key.
- [ ] **ID-06** Concurrent helper startup installs only one identity.
- [ ] **ID-07** Crash at every creation/write/rename step does not replace an established key.
- [ ] **ID-08** Wrong user, invalid DPAPI blob, permission denial, and corruption do not regenerate a key.
- [ ] **ID-09** Missing key with existing continuity metadata enters recovery-required state.
- [ ] **ID-10** Valid key with missing metadata repairs metadata from the key.
- [ ] **ID-11** Same backup imported elsewhere yields the same ID.
- [ ] **ID-12** Wrong passphrase, modified ciphertext/header, truncated file, and unsupported KDF fail closed.
- [ ] **ID-13** Import bounds reject excessive memory/time parameters before resource allocation.
- [ ] **ID-14** Different-key import requires confirmation and preserves old store on failure.
- [ ] **ID-15** Export, logging, and support bundles contain no plaintext seed/passphrase.
- [ ] **ID-16** Windows normal/elevated launch does not unintentionally create different users' identities.
- [ ] **ID-17** Wine/Proton uses the declared, tested backend; no silent plaintext/weak fallback.
- [ ] **ID-18** Locked/unavailable identity does not block casual play.

### AUTH

- [ ] **AUTH-01** Challenge signature validates across Rust and bridge implementations.
- [ ] **AUTH-02** Modified audience, action, method, path, digest, ID, nonce, or expiry fails verification.
- [ ] **AUTH-03** Expired/consumed challenge cannot authenticate again.
- [ ] **AUTH-04** A fresh proof for one command cannot authorize another command under an idempotency key.
- [ ] **AUTH-05** Duplicate JSON keys and noncanonical binary encodings are rejected before authorization.
- [ ] **AUTH-06** Session expiry/revocation removes access; player tokens do not work on provider/admin APIs.
- [ ] **AUTH-07** Bearer session alone cannot finalize linking or claim a new fighter endpoint.
- [ ] **AUTH-08** Untrusted bridge/deep-link origin cannot obtain signatures or tokens silently.

### LINK

- [ ] **LINK-01** Verified browser plus entered code plus matching fingerprint approval completes a link.
- [ ] **LINK-02** Stolen code plus attacker's key cannot complete the link without authenticated browser approval.
- [ ] **LINK-03** Unauthenticated claimed provider ID cannot create an approvable account association.
- [ ] **LINK-04** Approval races consume the intent once and retain exactly one approved mapping.
- [ ] **LINK-05** A pending claimant cannot be swapped while approval is displayed.
- [ ] **LINK-06** Expired/cancelled code never links; retries of the same completed association are idempotent.
- [ ] **LINK-07** Cross-provider/environment/tenant code reuse is rejected.
- [ ] **LINK-08** Existing link replacement requires fresh external authentication and explicit approval.
- [ ] **LINK-09** Unlink does not change key/ID but revokes derived future permissions.
- [ ] **LINK-10** Guessed codes are rate-limited without permanent unauthenticated account lockout.
- [ ] **LINK-11** Unicode/external numeric IDs preserve the provider's exact identity semantics.
- [ ] **LINK-12** Provider lookup cannot enumerate unrelated users or reveal another tenant's links.

### ROOM

- [ ] **ROOM-01** Simultaneous first claims produce one valid provisioning lease.
- [ ] **ROOM-02** A late expired host cannot publish a competing room.
- [ ] **ROOM-03** Established-room heartbeat loss does not automatically create another room.
- [ ] **ROOM-04** Public URL/invite without valid identity admission cannot occupy a fighter seat.
- [ ] **ROOM-05** Copied certificate used from another Iroh endpoint/helper is rejected.
- [ ] **ROOM-06** Valid spectator cannot queue into a reserved fighter seat.
- [ ] **ROOM-07** Unknown issuer, wrong token type, `none`, wrong algorithm/key, or stale assignment is rejected.
- [ ] **ROOM-08** Same-endpoint reconnect is idempotent; a restarted helper performs approved rebind.
- [ ] **ROOM-09** Same Ember key on two devices cannot own two active fighter leases in one match.
- [ ] **ROOM-10** Host transfer preserves the complete tournament authorization policy.
- [ ] **ROOM-11** Checkpoint restore/late join cannot strip tournament rules or admit an old client.
- [ ] **ROOM-12** Bracket slot and transport host independence cannot invert the winner.
- [ ] **ROOM-13** Rule/roster change while readiness awaits a permit invalidates the prospective start.
- [ ] **ROOM-14** One permit cannot authorize two generations or two official games.
- [ ] **ROOM-15** Expired start permission stops a new game, not an already-running one.
- [ ] **ROOM-16** Browser handoff during gameplay cannot interrupt or open another game instance.
- [ ] **ROOM-17** Manual code/assignment fallback works without URI integration under Proton.
- [ ] **ROOM-18** Casual room behavior, queue rotation, and spectator features regress neither when disabled nor enabled elsewhere.

### RESULT

- [ ] **RESULT-01** A discarded speculative native winner produces no external result.
- [ ] **RESULT-02** Confirmation that occurs during idle/final polling still produces a report.
- [ ] **RESULT-03** Signed report from one player cannot count for both participants.
- [ ] **RESULT-04** Same-outcome reports with different valid local confirmation frames may agree.
- [ ] **RESULT-05** Conflicting outcomes/slot mappings/rules produce review, not a score.
- [ ] **RESULT-06** Missing second report produces review after grace; no inferred disconnect win.
- [ ] **RESULT-07** Duplicate report/event/HTTP delivery after restart adds no extra game win.
- [ ] **RESULT-08** Same observation identity with changed bytes is a retained conflict.
- [ ] **RESULT-09** FT2 sequence A/B/A produces exactly one final `2-1` set result.
- [ ] **RESULT-10** Draw produces no increment and a distinct replay attempt.
- [ ] **RESULT-11** Abort/cancel does not produce a native victory; organizer forfeit is separately marked.
- [ ] **RESULT-12** Late old-room result cannot score a replacement binding.
- [ ] **RESULT-13** Host migration does not change report identity or duplicate an accepted attempt.
- [ ] **RESULT-14** Spool disk-full/queue-full pauses next official attempt without blocking native teardown.
- [ ] **RESULT-15** Helper crash before durable report produces visible missing evidence, never a made-up result.
- [ ] **RESULT-16** Bridge crash during two-report reconciliation has all-or-nothing score/event/outbox effect.
- [ ] **RESULT-17** Permit's start expiry does not invalidate a long game's legitimate report.
- [ ] **RESULT-18** Provider outage leaves local set complete and delivery pending, with no native-game dependency.
- [ ] **RESULT-19** Correction creates a new result revision and explicit provider reconciliation path.
- [ ] **RESULT-20** Signed/colluding fabricated reports are not described as anti-cheat proof in UI/docs.

### WEB

- [ ] **WEB-01** Raw-body webhook fixture verifies; body/header mutation fails.
- [ ] **WEB-02** Old delivery timestamp fails; legitimate retry with new timestamp and same event ID deduplicates.
- [ ] **WEB-03** Key rotation supports bounded old/new overlap and then rejects old secret.
- [ ] **WEB-04** Receiver crash after durable enqueue produces no duplicate side effect on retry.
- [ ] **WEB-05** Out-of-order/gapped events recover via cursor and consistent snapshot.
- [ ] **WEB-06** Event stream authorization hides unrelated matches and identity links.
- [ ] **WEB-07** Loopback/private/metadata targets, unsafe IPv6, redirects, and DNS rebinding are blocked.
- [ ] **WEB-08** Malicious callback response cannot exhaust memory, pin all workers, or affect Iroh work.

### PROVIDER

- [ ] **PROVIDER-01** Real provider fixtures pass request/auth/response contract tests.
- [ ] **PROVIDER-02** Retried match creation cannot schedule a second logical match.
- [ ] **PROVIDER-03** Timed-out result acceptance becomes ambiguous until verified, not blindly resubmitted.
- [ ] **PROVIDER-04** Provider without correction/review support triggers an explicit operator path.
- [ ] **PROVIDER-05** Staging credentials/keys cannot authorize production matches.

### OPS

- [ ] **OPS-01** Bridge backup/restore preserves identities, permits, accepted scores, and delivery uniqueness.
- [ ] **OPS-02** Secret scanning catches a intentionally seeded test secret in a release/support archive.
- [ ] **OPS-03** HTTP stalls/event floods/KDF attempts do not cause gameplay-thread I/O or sustained frame overruns.
- [ ] **OPS-04** Feature disablement leaves casual play and existing identity untouched.
- [ ] **OPS-05** Canary rollback disables new permits without stranding records or duplicating provider delivery.

## Evidence record for each completed item

Record build/commit, platform/version, test command or manual procedure, pass/fail, log/report reference, reviewer, and unresolved limitations. Do not mark a product test passed solely because a mock fixture passed.
