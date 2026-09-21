# Recovery candidate validation

Target: `sf4-current`, selected by the parent `build-target.json`.
## September 10 wrap-up

The final source repair debounces forced recovery elections until the same
term and leader have remained unwritable for ten seconds. Native mutations
still freeze immediately. Transient control closure only reconnects; it no
longer forces an election. OpenRaft still requires the existing voter quorum.
This fixes the spurious leadership change seen during large relay admissions.

The release helper is SHA-256
`918b5cead3c28bd80000335e3b4b791738d8380f83b321b4496885313b8250ed`.
Focused default and forced-relay Started-match recovery passed with stable
generation, grants, roster and ports, 60 pre-loss plus 120 post-election GGPO
frames, and a newly committed chat action. Evidence:
`build/recovery-started-debounce-{default,relay}-v1.log`.
The sixteen-member/four-table forced-relay test passed five match cycles;
see `build/recovery-final-four-relay-debounce-v1/results.json` and its artifact
hashes. The spectator default case also passed; both route results are recorded
as they finish in `build/recovery-final-spectators-debounce-v1/results.json`.
The final Rust gate passed formatting, 69 locked tests and Clippy with warnings
denied; two explicit public-network tests are excluded by that local command.
Evidence: `build/recovery-wrap-rust-gates.log`.

Earlier sections below preserve development history, including failed attempts
and superseded helper hashes. They are not a claim that every older test or
performance result applies byte-for-byte to this final helper. The closing
build, package verification and acceptance limits are recorded separately in
`build/recovery-wrap-handoff.md`. Installation, physical controllers and two-PC
native SF4 play remain user-controlled acceptance.

## Preserved baseline

The September 8 package `ember-current-postmatch-ultra-20260908` was built
at 2026-09-09T03:43:21Z. Its reviewed receipt covered 2,087 source files,
ten binaries, and 28 passing local tests. Its ZIP and all ten packaged binary
hashes were rechecked during this repair and remain unchanged. Evidence:
`build/recovery-preserved-baseline-sol.json`. Feature checkouts and the
pre-repair source snapshot remain separate from the integration target.

## Repair ownership and review

The primary Astra agent now owns diagnosis, implementation, self-review,
testing, and delivery under the updated workspace preference. Earlier Sol
and Luna assignments are complete; no sub-agent or separate reviewer is active.
Source inspection, focused tests, actual helper/GGPO traffic,
and final package verification are recorded separately.

C++ v4 has passed bounded Astra review. Its focused authority, client, server,
and room-menu navigation tests pass. The repairs include exact terminal ACK
validation and retries, generation/member/incarnation snapshot confirmation,
native admission retries, and retaining a preparation grant until its exact
committed roster projection arrives. Evidence:
`build/recovery-sol-cpp-v4-build.log` and
`build/recovery-sol-cpp-v4-*.log`.

The frozen Sol Rust v1 helper has SHA-256
`fcabec6d65508fdef579b9c64741b2185a1e2d8fecd91fd33bc5d259558846f6`.
It passed 44 locked tests, formatting, and Clippy with warnings denied.
Both ignored process-level transport tests passed three generations and
150 exact 1,024-byte round trips. Their actual selected routes were direct
IPv6 and `relay:https://use1-1.relay.n0.iroh.link./`, respectively.
These two-helper results are intermediate evidence. The subsequent Rust v2
repair serializes membership removal and admission, binds learner bootstrap
to authenticated invitation authority, and separates prepared-listener waits
from the wire handshake deadline. An optimized v2 candidate has now passed
the four-participant relay test below. Later checkpoint-credit and probe
invalidation changes are included in the final frozen helper described below.

The frozen Sol Rust v2 helper has SHA-256
`3bdf88e76ba17c3abb8d550c5e8d17b7d879887607e022938752c81f88408ba6`.
It includes the checkpoint-credit fence, retained probe invalidations, current
probe request/pair completion checks, and authority grace when a leaving
follower becomes the successor. Root checked all fifteen source files in
`build/recovery-helper-sol-rust-v2-source-frozen.zip` byte-for-byte against the
working checkout. Evidence: `build/recovery-root-rust-freeze-verification.json`.
The source archive SHA-256 is
`394ad2dffac2a0639a694c4f6e207831a9162979e8818988f67c6e6d986cff7c`.
The designated build subsequently rebuilt the same reviewed Rust source into
`build/current/sf4-net.exe`, SHA-256
`eb85f2a78a73c03b32afd01e08028fa372db452e0131fc7fd1a087ec493f0be7`.
All fifteen reviewed source hashes still matched at that build. This executable
is preserved intermediate evidence; the v4 transfer source below supersedes it.
Both ignored process tests also passed against this designated executable:
three generations and 150 exact 1,024-byte round trips for each route policy.
The default run selected direct IPv4 (p95 225 microseconds); the forced-relay
run selected use1-1 at both ends (p95 68,662 microseconds). The logs record and
verify the actual executed helper hash:
`build/recovery-sol-rust-final-designated-default.log` and
`build/recovery-sol-rust-final-designated-relay.log`.
The final frozen source passes formatting, all 54 locked tests (two explicit
network tests remain excluded from that local command), and Clippy with
warnings denied. Both explicit process tests also pass three generations and
150 exact 1,024-byte round trips. The default run selected direct IPv6 at both
ends, with p95 RTT 878 microseconds; the forced-relay run selected the use1-1
relay at both ends, with p95 RTT 67,010 microseconds and clean simultaneous
departure. These are process-test measurements, not paired benchmark deltas.
Evidence: `build/recovery-sol-rust-v2-final-gates.log`,
`build/recovery-sol-rust-v2-final-helper-network-default.log`, and
`build/recovery-sol-rust-v2-final-helper-network-relay.log`.

The later Rust v4 transfer source passes all 57 locked tests (two explicit
network tests excluded), formatting, and Clippy with warnings denied. It
preserves a prior committed marker before emitting a newer transfer, including
when IPC capacity becomes available between pumps; prevents replacement of an
export in flight; and permits byte-validated retries of retained prefixes with
exact chunk-end credit. The completed-body retry regression spans more than
four chunks and forces sender expiration before the final acknowledgment.
Root reviewed the delta and verified all fifteen archived source files against
the checkout. Evidence: `build/recovery-sol-rust-v4-final-gates.log`,
`build/recovery-sol-rust-v4-red-green-receipt.json`, and
`build/recovery-root-rust-v4-freeze-verification.json`.
The archive `build/recovery-helper-sol-rust-v4-transfer-source-frozen.zip` has
SHA-256 `ffc6f618f0fc04b2603c4fb56402d2a6c7a104fcd10ee70c17d37ac2f42cbb50`.
The third designated build compiled this source into `build/current/sf4-net.exe`,
SHA-256 `6359a91183608f9cd3b6831a601b78cc88f92db5d5a70b47c727871db7623037`.
Both explicit process tests passed against these exact executable bytes: three
generations and 150 exact 1,024-byte round trips per route. Default routing
selected direct IPv4 at both helpers (p95 312 microseconds); forced relay
selected use1-1 at both helpers (p95 66,401 microseconds). The test runner's
embedded helper location was restored to its original hash afterward. Evidence:
`build/recovery-sol-rust-final-designated-v3/artifacts.json`, `results.json`,
`restore.json`, `default.log`, and `relay.log` in that directory. These process
measurements do not replace the combined native matrix or paired benchmark.

## Completed actual integration evidence

| Test | Result and limits | Evidence |
| --- | --- | --- |
| Reject, rejoin, and repeated rooms | Sol Rust v1 with the coherent C++ v1 fixture passed native-build rejection followed by a valid rejoin on the same helper, three fresh rooms, 90 committed Ready/Unready cycles, and neutral Leave snapshots. Default route policy. | `build/recovery-sol-rust-v1-room.log` |
| Majority recovery | C++ v4 / Sol Rust v1 recovered an action committed before the old native owner delivered effects, then committed new Chat, Queue, Ready, and Unready actions under the new leader. Original RoomId preserved. | `build/recovery-sol-cpp-v4-expanded-rust-v1.log` |
| Two-voter loss | The same run kept the survivor non-writable for 16 seconds, rejected room mutation, and created a usable fresh RoomId only through explicit replacement. | Same expanded log |
| Moderator and leader departure | Explicit moderator transfers and normal leader departure left the original room usable with the oldest eligible remaining moderator. | Same expanded log |
| Started gameplay recovery | Two existing GGPO sessions survived technical-leader loss with their instances, capabilities, roster, ports, and selected delays intact. They advanced 60 pre-loss frames and at least 120 frames each after recovery became writable, committed a new Chat, then matching results and explicit terminal ACKs. | Same expanded log |
| Migration during preparation | C++ v4 / Sol Rust v1 cancelled generation 1 after leader loss, retired the actual prepared listener, committed the terminal acknowledgments, and authorized generation 2 on both fighters. The new mappings selected direct IPv4. | `build/recovery-preparing-v3-rust-v1.log` |
| Migration after committed results | Real GGPO sessions produced results, then the leader committed the matching outcome while fighter native application was held. Its successor replayed the retained outcome, closed the original mappings, committed acknowledgments, and preserved the exact 0–1 score. Default route policy. | `build/recovery-committed-result-v3-rust-v1.log` |
| Forced-relay terminal recovery | Optimized Sol Rust v2 candidate 2 admitted four participants, measured 100/100 relay probe replies with p95 RTT 77,328 microseconds, and completed three generations with fighters and two spectator streams. Held recipients discarded 102 checkpoints each while 100 unrelated changes committed, then recovered and acknowledged their retained results. All gameplay mappings selected `relay:https://use1-1.relay.n0.iroh.link./`. | `build/recovery-sol-rust-v2-release-candidate2-relay.log` |
| Final helper forced-relay terminal recovery | The frozen Rust v2 helper passed the preserved four-participant fixture: 100 valid probe replies, zero lost, p95 RTT 70,246 microseconds, recommendation 1; three GGPO games with two spectators; 102 discarded checkpoints per held recipient after 100 commits; retained results and rematches. The first game selected relay at both fighters. The C++ fixture predates the final combined build. | `build/recovery-sol-rust-v2-final-terminal-relay.log` |
| Explicit replacement during incomplete preparation | The coherent replacement C++ implementation with Rust v1 ignored an actually queued grant after explicit replacement began. A prepared minority then remained frozen for 16 seconds, closed its exact listener, and created usable fresh authority despite a stale Playing table projection. No GGPO socket was owned when replacement became available. | `build/recovery-prepared-minority-v1-rust-v1.log` |
| Terminal acknowledgment queue saturation | A small actual-helper fixture reproduced the old server queue failure on retry 64 while native action consumption was held. After exact duplicate coalescing, 192 identical acknowledgments retained one queued action; a distinct generation acknowledgment and a Chat still produced their separate ordered replies, with unchanged scores. Rust v2 candidate 3; local host adapter. | `build/recovery-terminal-queue-red-v2.log`, `build/recovery-terminal-queue-green-v2.log` |
| Preparation authorization under journal byte pressure | A deterministic regression placed four private grants before sixteen unique large room snapshots. The old compactor erased the grants; the repair removes optional projection payloads anywhere in the journal before discarding lifecycle identities. All twenty identities and their order survive within 256 KiB. A separate 257-entry case still enforces the 256-entry cap. | `build/recovery-effect-journal-projection-red-test.log`, `build/recovery-effect-journal-projection-green-test.log` |
| Journal compaction cost | Independent Release audit compared the faster byte-accounting compactor with the exact previous selection algorithm across 24 seeded mixed histories. Every output was byte-identical and stayed within both limits. The reference took 3,407,541 microseconds and the candidate took 69,805 microseconds for this synthetic workload. This measures compaction only, not gameplay or the required paired benchmark. | `build/recovery-compaction-reference-audit.cxx`, `build/recovery-compaction-reference-audit.log` |
| Pending private proposal through a same-term pause | The old gate discarded a real four-recipient preparation proposal on a same-term, same-base writable pause. The repaired gate retains it, refuses effects while unhealthy, and emits the grants once after recovery. Real-helper bridge fixtures passed on default and forced-relay policies: they held the native owner while quorum committed its preparation, resumed the bridge, observed exactly one grant and a Ready gameplay mapping per fighter, then committed abort and terminal acknowledgment. The forced-relay run selected use1-1 at both fighters. | `build/recovery-same-term-pause-red-test.log`, `build/recovery-same-term-pause-green-server-test.log`, `build/recovery-same-term-bridge-v1-test.log`, `build/recovery-same-term-bridge-v1-relay-test.log` |
| Terminal recovery after journal eviction | C++ v1 / frozen v17 helper passed three games with two verified spectator streams. The first result withheld native control from a fighter and spectator, discarded 102 validated checkpoint deliveries per held recipient, and committed 100 unrelated rule edits. Both recovered the retained result, retired their mappings, and acknowledged it before rematching. | `build/recovery-sol-cpp-v1-terminal-v17.log` |
| Native draining | The terminal run advanced the injected IrohMatchSession clock beyond 31 and 121 seconds while actual GGPO sockets remained owned. Helper generations, Ready states, and exact virtual ports remained unchanged until socket retirement. | Same terminal log |

The terminal-eviction fixture executable SHA-256 was
`4247451ec43f48dbf86b898214bc7f6eb416218b60cdb7e5ba3cc19ce9e61f07`;
its frozen v17 helper SHA-256 was
`7dc6ce7e4c3e04884dfc60af8d6ff3b30bec9e51a221ea757869f60e64177d99`.
That run selected direct IPv4, measured 100/100 probe responses, p95 RTT
946 microseconds, and recommended zero frames. Its frozen delay pairs were
1/3, 2/6, and 3/9. This is intermediate evidence, not final Release acceptance.

## Current integration blocker and outstanding checks

- Optimized Rust v2 candidate 2 admitted all sixteen members and started all
  four tables, clearing the earlier preparation timeout. Generation 1 passed
  GGPO input forwarding and retired, then client 0 returned Step=-1 at native
  revision 68. A second run identified `room_receive_queue` in the host's
  C++ adapter while all helpers stayed connected and coordination remained
  writable. The exact-duplicate acknowledgment repair now passes the focused
  regression above. The sixteen-member rerun passed its first two complete
  cycles, including both terminal acknowledgment bursts. Evidence:
  `build/recovery-sol-cpp-final-four-tables-rust-v2-candidate2.log` and
  `build/recovery-sol-cpp-final-four-tables-rust-v2-candidate2-diagnostic.log`.
- The frozen Rust v2 / repaired C++ sixteen-member run then failed during
  cycle three: tables 0, 2, and 3 reached Started at generations 9, 11, and 12,
  while all four table-1 clients remained Idle at generation 6 after accepted
  Ready replies. All sixteen replicas had applied revision 123 and retained
  writable, rebound control with no reported error. The journal byte-pressure
  regression above is now repaired and passes review; the live run must still
  confirm that it resolves this preparation failure. Evidence:
  `build/recovery-sol-terminal-queue-green-four-tables-rust-v2-final.log`.
- The earlier fourth-helper forced-relay admission failure is superseded by
  the optimized v2 run above. Its causes included bootstrap Membership
  arriving before the learner applied its leader, relay address readiness,
  and Raft RPC deadlines cancelling healthy relay transfers. Preserve the
  earlier failure log `build/recovery-sol-rust-v1-terminal-relay.log`.
- The final combined binaries still need all default and forced-relay
  integration modes, including repeated four-table games and fourteen
  spectators. Intermediate helper runs do not replace that final gate.
- The first designated-build matrix attempt stopped during initial table
  setup. Tables 0 and 1 reached Started, but table 2 remained Preparing at
  generation 3 while its four native clients remained Idle. All sixteen
  replicas applied revision 74, and the 122-entry, 261,020-byte journal retained
  all four preparation digests. The next Ready actions were not sent because
  the fixture was still waiting for public views to catch up. The passing
  compaction regression therefore does not by itself resolve live dispatch.
  A same-term writable pause can discard pending proposal metadata and select
  the replica-import path, which cannot replay private grants. The focused
  regression and default/forced-relay real-helper bridge now pass after that repair;
  the full matrix still has to confirm the original failure is resolved.
  Evidence and unchanged binary hashes:
  `build/recovery-final-network-large-v1/results.json`,
  `build/recovery-final-network-large-v1/artifacts.json`, and its
  `four-tables-default.log`.
- The second designated build passed all 33 local tests. Its first large-room
  attempt stopped before admission with `host_unavailable`; an unchanged retry
  admitted all sixteen members, started all four tables, completed their GGPO
  input streams, and retired all native mappings. That retry then timed out
  during the first terminal acknowledgment phase. All helpers reported applied
  coordination revision 83; eleven native replicas activated 83 and five
  remained at 82. Public room views also differed. Public-room revision and
  coordination revision are separate timelines. Checkpoint handoff and final
  public-state delivery remain under investigation; empty final error fields
  do not rule out transient transfer errors. No later matrix cases ran.
  Evidence: `build/recovery-final-network-large-v2/` and
  `build/recovery-final-network-large-v3/`, including each directory's
  `results.json`, `artifacts.json`, and `four-tables-default.log`.
- Focused investigation reproduced native checkpoint retry and retention
  defects: an incomplete receiver rejected a sender restart using the same
  identity, and a complete validated body could expire before its separate
  commit marker arrived. Both have RED/GREEN regressions. A completed body's
  retry now validates each retained chunk and acknowledges that chunk's end,
  preserving the sender's four-chunk credit limit. The full-room rerun is still
  required to establish resolution of the observed stall. Evidence:
  `build/recovery-checkpoint-retry-{red,green}-test.log`,
  `build/recovery-checkpoint-complete-{red,green}-test.log`, and
  `build/recovery-checkpoint-token-v1-transfer-test.log`.
- A valid sixteen-member snapshot with 100 retained 256-byte chat messages
  serialized to 36,014 bytes, but embedding the entire public payload in its
  commit token expanded the control message to 72,403 bytes, above 64 KiB.
  Compact identity-only live tokens preserve the journal's replay copy and
  pass the focused wire-size and token round-trip regression. Ordinary chat
  containing the word `capability` is no longer mistaken for a private grant;
  the sensitive-field check uses JSON object keys. A real server regression
  filled all 100 chat slots with escaped text, queued sixteen simultaneous
  actions, and committed every reply without journal overflow or a control
  frame over 64 KiB. Recovery consumes one intent per candidate, leaving the
  remaining intents in the existing bounded transport queue. Evidence:
  `build/recovery-payload-bound-audit.log` and
  `build/recovery-checkpoint-token-v1-gate-test.log`, plus
  `build/recovery-checkpoint-token-burst-v2-server-test.log`.
- The third designated build's sixteen-member attempt admitted everyone and
  completed actual GGPO input streams on all four tables, then stopped at the
  fixture's one-shot assertion that the first result submission returns Queued.
  The old assertion did not log the participant or return code, so the cause
  cannot be inferred from this run. No later matrix cases ran. The fixture is
  being aligned with retained result reporting and explicit reply diagnostics;
  its result identity, score checks, and phase deadlines remain acceptance
  requirements. Evidence: `build/recovery-final-network-large-v4/results.json`,
  `artifacts.json`, and `four-tables-default.log` in that directory.

The profile result outbox now retries failed persistence even when insertion
fills the last record slot. Permanently unavailable records and an already
full history allow terminal acknowledgment without inventing a stored result.
The focused outbox and controller tests pass; replacement availability uses
the local gameplay owner, independently of the selected table's stale state.
Evidence: `build/recovery-sol-replacement-v1-MatchResultOutboxTest.log`,
`build/recovery-sol-replacement-v3-coherent-build.log`, and
`build/recovery-sol-replacement-v3-navigation.log`.

The later stable lifecycle retry repair retains one original action ID and
exact serialized payload for each result and native-finish notification.
Delayed replies remain usable after a retry. Unsent attempts wait 100 ms;
queued attempts wait 500 ms. Only byte-identical retries from the same
authenticated sender coalesce; changed generation, result, or sender remains
distinct. Older lifecycle IDs pass through their exact table/generation and
fighter checks, while the generic accepted-action watermark never decreases.
The outbox, client transport, and server transport tests passed in
`build/recovery-stable-terminal-retry-v1-tests.log`. Two older RoomAuthority
assertions initially expected the generic accepted response to a duplicate;
they now require the explicit DuplicateResult reason. The corrected authority
suite passed in `build/recovery-stable-terminal-retry-v1-room-test.log`.
All four focused suites then passed together in 12.50 seconds, recorded in
`build/recovery-stable-terminal-retry-v2-tests.log`. The Application and large
fixture compiled. Root reviewed the production retry paths and fixture; combined acceptance
still requires a fresh designated build and actual large-room runs.
The relinked adapter also passed the actual-helper duplicate-ACK fixture in
3.70 seconds: 192 identical terminal retries retained one queued action while
a distinct-generation ACK and Chat preserved their separate ordered replies.
Evidence: `build/recovery-stable-terminal-retry-queue-acks-v1/queue-acks.log`
and `hashes.txt`. This used helper `6359a911...7623037` and native fixture
`a69e456a89eeed1fe7015c0fa24eceb38b1c69e698d0032fbe7512dfc42d7b74`.
The C++ freeze inventory is `build/recovery-root-stable-lifecycle-source-frozen.json`.

A subsequent bounded helper review found that native checkpoint proposals,
membership reconciliation, and periodic authority reads can await quorum from
inside the actor loop. A prolonged wait can stop control consumption while
the separate bounded connection worker keeps receiving frames. The helper
responsiveness repair now moves native proposals, coordination refreshes,
membership reconciliation, probe authorization, and serialized admissions into
bounded tasks. Exact room/incarnation/operation keys fence completions. Applied
committed state remains authoritative even if a proposal waiter returns late or
fails. Admission work retains distinct deferred messages, wakes when task
capacity becomes free, and computes the voter target after each preceding
admission. A successful learner binding survives a later promotion failure.

The final Rust v5 local gates pass all 60 tests, formatting, and Clippy with
warnings denied. Two public-network tests are explicitly ignored by the local
command and require separate execution against the designated helper. New
regressions use the real actor loop with held checkpoint/admission work, verify
overlapping admissions reach three voters, and reject an expired but otherwise
valid probe completion before permission installation or dialing. Removing only
the expiry guard made the last test fail on permission installation; restoring
it passed. Root reviewed the finished production delta. Evidence:
`build/recovery-sol-rust-v5-final-gates.log`,
`build/recovery-sol-rust-v5-expiry-red.log`, and
`build/recovery-sol-rust-v5-expiry-green.log`.
Root verified all fifteen files in
`build/recovery-helper-sol-rust-v5-actor-source-frozen.zip` against the checkout;
the archive SHA-256 is
`361ee9e9e70ea850bc8368f77de680af3245daf5e6031ba701a25bedca7e0392`.
The independent comparison is retained in
`build/recovery-root-rust-v5-freeze-verification.json`.
The new source requires a fresh designated build, process tests, combined
native tests, and benchmark artifact comparison; the earlier helper's passing
runs do not validate that delta.

The fourth designated build passed all 33 local tests in 35.29 seconds, staged
ten binaries, and recorded fingerprint
`c61138d06769f945cf890f9d02005b013289d335645e3ac60bb3a0e2e904ea2e`.
Its helper SHA-256 is
`f024d457607127503c2ae910688f82877e8e9a0c1b0e8d1eb9db0d8b74a3e01c`.
Both explicit process tests passed these exact bytes: direct IPv6 and use1-1
relay, each with three match generations and 150 exact 1 KiB round trips.
Evidence: `build/recovery-final-designated-v4-provenance.json`,
`build/recovery-final-designated-v4-build.log`, and
`build/recovery-sol-rust-final-designated-v4/`.

The corresponding `large-v5` four-table/default attempt stopped in 48.96
seconds during admission of the third member. Peer 1 remained at native applied
revision 6 while its helper and the other two native peers reached revision 9;
three checkpoints were staged behind `replicated checkpoint rebind pending`.
No game had started and all artifact hashes stayed unchanged. Root and C++ Sol
traced the missing logical endpoint to an asynchronous admission publication
race: control processing broadcast the old membership immediately after queuing
admission, but completion did not broadcast the new authenticated member.
The previously joined follower could never allocate the missing logical handle.
The Rust publication fix now publishes the roster after successful admission,
retains each unsent destination under control backpressure, and rechecks local
leadership before any retry. Its focused test verifies that an already joined
follower receives the newcomer endpoint and incarnation over the real control
connection. All 61 locked Rust tests pass in 12.63 seconds, formatting passes,
and Clippy passes with warnings denied. Root reviewed this narrow delta; the
native import/rebind checks remain intact. Evidence:
`build/recovery-sol-rust-v6-publication-focused.log` and
`build/recovery-sol-rust-v6-publication-final-gates.log`.
Root compared all fifteen files in the v6 source archive to the checkout.
Archive `build/recovery-helper-sol-rust-v6-publication-source-frozen.zip` has
SHA-256 `cf4776df6cb87c13723e755c123bf73eaa200665e79fa493abb402a04f995c52`;
the comparison is `build/recovery-root-rust-v6-freeze-verification.json`.
The preserved failed-run evidence is:
`build/recovery-final-network-large-v5/results.json`, `artifacts.json`, and
`four-tables-default.log`. No other cases ran in that attempt. A new designated
build and actual network runs are required for the publication fix.

The fifth designated build includes that publication fix. It passed all 33
local tests in 28.62 seconds and staged ten binaries at
2026-09-10T05:44:53.7043508Z. The receipt fingerprint is
`0ae6c66858b10650eba37f39bdd23414da1bc225f179715156e0438217de4e11`;
the helper SHA-256 is
`7d56f8657c6efebaf67b92feb5baa13bcf2224a98d011e2235d85ab40eb50ca9`.
The C++ large-room fixture is unchanged from the failed admission run.
Both exact-helper process tests passed three generations and 150 exact 1 KiB
round trips: the default run selected direct IPv4 (p95 260 microseconds), and
the forced-relay run selected use1-1 (p95 68,272 microseconds). The cached test
helper was restored byte-for-byte after the test. Evidence:
`build/recovery-final-designated-v5-build.log`,
`build/recovery-final-designated-v5-provenance.json`, and
`build/recovery-sol-rust-final-designated-v5/`.

The first run with this helper (`large-v6`, four-table/default) admitted all 16
members, completed cycle 1, and obtained all fighter result replies in cycle 2,
then timed out during terminal acknowledgment. Windows PowerShell's native
stderr handling interrupted the wrapper on the first diagnostic, leaving that
run without its complete failure dump or results file. The wrapper now keeps
the native invocation alive through stderr and enforces its recorded exit code
afterward. A native subprocess regression preserved stdout before and after
stderr, the stderr diagnostic, exit code 17, and the caller's error preference:
`build/recovery-network-capture-regression-v1/result.json`.

The unchanged-binary diagnostic rerun (`large-v7`, four-table/default) reproduced
the cycle-2 timeout in 365.45 seconds and retained its full dump. All four
terminal receipts were fully acknowledged (16/16 recipients), every public view
was at revision 120, every native replica and helper had applied revision 257,
and all matches were Idle with no local terminal pending state. The host still
held candidate/proposal 258, term 1/base 257, with 33 pending effects and 14
queued server messages. Host authority remained writable/rebound. The proposal
submission/commit path is being diagnosed; this single snapshot establishes
neither whether the proposal reached Raft nor how long that particular proposal
was pending. Continued processing of queued retries remains a possible cause of
the phase timeout. No acceptance predicate or limit was relaxed.
Evidence: `build/recovery-final-network-large-v7/results.json`, `artifacts.json`,
and `four-tables-default.log`. No other cases ran.

A focused native check serialized a sixteen-member room with maximum retained
escaped chat and 33 effects into 846,492 bytes, below the unchanged 1,048,576-byte
checkpoint cap. Both native transport tests passed; this is a conservative
fixture measurement, not the missing live proposal payload. Diagnostic fields
now expose the outgoing transfer identity, exact bytes/sent/acknowledged counts,
Begin/End state, age, timeout count, and rejection status. The next live run uses
the same designated v5 helper with a separately hashed diagnostic native fixture.
Evidence: `build/recovery-proposal-observability-v1-server-test.log` and
`build/recovery-proposal-observability-v2-client-test.log`.

The instrumented four-table/default run completed in 224.08 seconds with a
cycle-1 terminal-phase timeout. It used unchanged helper
`7d56f8657c6efebaf67b92feb5baa13bcf2224a98d011e2235d85ab40eb50ca9`
and diagnostic native fixture
`7f47f509552c96a6951f43453f0cfc20197baba04ff78307c7966313a0381bae`.
All sixteen terminal recipients were acknowledged and all views were current.
At the deadline there was no pending proposal; transfer 167 had sent and
acknowledged all 598,471 bytes and was marked committed. Another candidate and
thirteen queued messages remained. This establishes continued processing of
duplicate ACKs, not a stuck transfer. An already-acknowledged terminal action
returns accepted without changing the room revision, but the server still
generated sixteen snapshots, sixteen projections, and its reply. The focused
repair preserves the committed reply and suppresses only that unchanged
room-wide broadcast. Evidence: `build/recovery-proposal-258-diagnostic-v1/`.
No native acceptance predicate or phase deadline changed.

The focused regression failed on the old duplicate-ACK broadcast, then passed
with the narrow server repair. It checks the first ACK still broadcasts, an
exact duplicate produces only one reply effect, wrong-generation and wrong-room
replies remain rejected, and every response is withheld until commitment.
Existing dirty state and deferred lifecycle effects remain untouched. The
server, client, and room-authority tests pass; Astra reviewed this delta without
another finding. Evidence:
`build/recovery-terminal-duplicate-projection-red-v2-test.log`,
`build/recovery-terminal-duplicate-projection-green-test.log`,
`build/recovery-terminal-duplicate-projection-client-test.log`, and
`build/recovery-terminal-duplicate-projection-room-authority-test.log`.

A separate held-quorum hypothesis check returned the proposal worker after
5.27 seconds and accepted an exact retry against unchanged Rust production.
No proposed timeout workaround was implemented and the temporary test does not
increase the frozen 61-test count. Evidence:
`build/recovery-sol-rust-v7-held-proposal-hypothesis.md` and
`build/recovery-sol-rust-v7-pending-proposal-red.log`.

Designated build v6 passed all 33 local tests in 35.24 seconds and staged ten
binaries at `2026-09-10T06:37:00.3819514Z`. Its source fingerprint is
`d84209d6122f969d508aca047badf7caf4e806959d858b48e34a02433a977a4e`.
The helper remains the exact v5 process-tested binary above. All fifteen Rust
inputs and both helper receipts were independently checked in
`build/recovery-sol-rust-v6-designated-build-verification.json`.
The new native fixture hash is
`2ff6779056543d1fbbf5b49c14e4e179dfc1636d67564ce28718d9dec2a0a797`.
Evidence: `build/recovery-final-designated-v6-build.log` and
`build/recovery-final-designated-v6-provenance.json`.

The following four-table/default run (`large-v8`) failed its cycle-1 terminal
deadline after 294.50 seconds overall. It had acknowledged nine of sixteen
recipients. Transfer 158 had committed all 598,271 bytes, no proposal was pending,
another candidate existed, and fifteen messages remained queued. One follower
was still importing that checkpoint; the others had applied it. Gameplay had
retired normally. Thus suppressing duplicate broadcasts is insufficient to meet
the existing deadline. Receipt queue throughput and native checkpoint costs
are being measured before another change. Actual default paths included both
direct IPv4/IPv6 and the use1-1 relay. Artifacts remained unchanged throughout.
Evidence: `build/recovery-final-network-large-v8/`. No other matrix cases ran.

Bounded profiles did not find a multi-second local operation. The retained
sixteen-member/max-chat native candidate measured 7.125 ms for checkpoint
construction, 19.450 ms for proposal creation, and 21.805 ms for applying its
commit. Eight-repeat averages were 2.503 ms for checkpoint construction,
5.138 ms for an empty timer poll, 5.000 ms for an empty server poll, and 4.034 ms
for passive checkpoint restore. Evidence:
`build/recovery-terminal-ack-profile-v1-test.log`; temporary timing code is
separate from the production repair.

An optimized local Rust copy-path probe measured a median 11.415 microseconds
to clone a 598,271-byte committed body and 10.4 ns for an unchanged-revision
check under the same lock. This mirror measurement identifies avoidable copies
but does not explain the deadline failure; no Rust production change was made.
Evidence: `build/recovery-sol-rust-v8-committed-clone-probe.rs` and `.log`.

The next bounded repair groups only consecutive terminal ACKs into one private
candidate, retaining FIFO order and stopping before ordinary or malformed work.
It keeps each response's exact committed digest and full local reply, with one
coalesced final room publication. Ordinary actions remain limited to one per
candidate. Maximum-size, rejected-ACK, and interrupted-delivery regressions must
pass without changing the checkpoint, journal, IPC, or phase limits before
another live matrix run.

Checkpoint/IPC payload limits, identity checks, quorum requirements, and native
setup bounds remain intact. Preserve failed logs alongside successful reruns.

## Menu and local validation

The acceptance fixtures cover the following requested failure windows. This
maps implemented tests to requirements; it does not mark an unrun final-binary
case as passed.

| Required window | Existing acceptance entry point |
| --- | --- |
| Proposal held before commitment, including same-term preparation | `SessionServerTransportTest::TestSameTermProposalPause`; `IrohRecoveryIntegrationTest --scenario same-term-preparing` |
| Commit applied before effect delivery; majority migration | `IrohRecoveryIntegrationTest --scenario majority` |
| Leadership change during preparation; two-voter loss and explicit replacement | `IrohRecoveryIntegrationTest --scenario preparing`, `preparing-minority`, and `minority` |
| Active gameplay and results already committed before migration | `IrohRecoveryIntegrationTest --scenario started` and `committed-result` |
| Graceful moderator and coordination-leader departure | `IrohRecoveryIntegrationTest --scenario departure` |
| Fresh helper admission and rejection of retired incarnations | `IrohRoomIntegrationTest`, `RoomAuthorityTest`, and Rust admission/history regressions |
| Result deadline suspension/rebasing and native ownership beyond 30/120 seconds | `SessionServerTransportTest`, `NativeRepairTimingTest`, and `IrohAuthorizedMatchTest` |
| Chunk credit, retained completed transfers, and duplicate terminal backpressure | `CheckpointTransferTest`, Rust transfer regressions, and `IrohRoomIntegrationTest --queue-acks` |
| Measured probe connection upgraded for gameplay; explicit Apply and manual Ready | `IrohAuthorizedMatchTest`, `RoomPanelNavigationTest`, and `UiRenderTest` |
| Four tables, spectators, and repeated rematches | `CustomRoomGameTest` and `CustomRoomGameTest --single-table` |

The current focused renderer passed 5,904 DX9 frames across all ten existing
viewport/DPI configurations and generated 140 images, including terminal
waiting, recovery status, replacement availability, host transfer, and its
Cancel-default confirmation, replacement during preparation, and locally
retired replacement with a stale Playing table. Evidence:
`build/recovery-sol-ui-v6-render.log` and `build/recovery-sol-ui-v6-shots*.bmp`.
The waiting/replacement and final host-transfer images were visually checked
at narrow and 200% layouts, including the Cancel-default confirmation.

The current RoomPanelNavigation test passed disabled Ready and selection behavior, focus
preservation through terminal waiting, reopening controls after committed
completion, member-identity focus after snapshot reorder, and owner-based
replacement availability with Cancel as the default confirmation.

Build-provenance regressions passed: a matching receipt is accepted, while
changed source, a different CMake source, and changed staged binaries are
rejected. Evidence: `build/recovery-provenance-regression.log`.

The coherent C++ local-suite run passed 32 tests; CheckpointTransfer initially
could not run because its executable had not been built. Building that target
and running it separately passed the remaining regression, for 33 passing
local tests across the two runs. Evidence:
`build/recovery-sol-cpp-coherent-local-suite.log` and
`build/recovery-sol-checkpoint-local-test.log`.

The designated `scripts/build-current.ps1` run then built and staged all ten
product binaries and passed all 33 local tests together in 32.21 seconds.
Evidence: `build/recovery-final-designated-v1-build.log` and the preserved
`build/recovery-final-designated-v1-provenance.json`. Its source fingerprint is
`24ec9660d882488865f88d5eec024ac03e02ec839dc8e8b1f0678d783ab431b5`.
The subsequent network failure above prevents package acceptance. Source or
documentation changes after that build require a refreshed designated receipt.

The next designated build also passed all 33 local tests together, in 20.09
seconds, and staged all ten product binaries. Evidence:
`build/recovery-final-designated-v2-build.log` and
`build/recovery-final-designated-v2-provenance.json`. Its source fingerprint is
`053f14bb191606f1825bc44cec03417b83d1df3594dd91eb667dc676c94a2871`.
Its large-room retry reached the terminal handoff failure recorded above.

The third designated build passed all 33 local tests in 31.95 seconds and
staged all ten product binaries. Evidence:
`build/recovery-final-designated-v3-build.log` and
`build/recovery-final-designated-v3-provenance.json`. Its source fingerprint is
`5e9bff076fe9e26dc7067fb112205e25e9e0b491af5f47d03b6608529b75295f`.
Subsequent result-fixture and documentation edits require another fresh receipt.

## Performance and final acceptance

The audited benchmark validates an independent deterministic state oracle,
confirmed final inputs, exact packet/byte accounting, selected routes, helper
CPU, prediction stalls, rollback depth, and resource measurements.
An intermediate v15 240-frame forced-relay pair passed. Its default-route
pair selected different IPv4/IPv6 paths, so the wrapper correctly withheld
comparative deltas. Neither run is final performance acceptance.
See [the benchmark procedure](RECOVERY_BENCHMARK.md).

The later 1,800-frame paired default and forced-relay workloads passed every
measurement gate against the designated v3 helper (`6359a911...7623037`). Both
builds confirmed frame 1,799 and saved state 1,800 on both fighters with exact
oracle agreement. The direct pair selected the same IPv4 address class and the
relay pair selected use1-1 at both ends. Helper CPU per forwarding operation was
111.83 / 104.94 microseconds for direct baseline / candidate, and 197.98 /
184.85 for relay. Maximum rollback depth stayed at seven direct and eight
relay; prediction stalls were 8 / 12 and 282 / 278 respectively. The candidate
had higher sampled memory peaks by 9,187,328 and 7,290,880 bytes. One pair per
route does not establish statistical performance or long-run memory behavior.
The benchmark document records all metrics, exact artifact hashes, and raw
paths under `build/recovery-final-benchmark-{default,relay}-v1/`. Final build
hashes must match before this evidence is reused.

After the remaining failures and review findings are resolved, run the
designated `scripts/build-current.ps1`, full local suite, locked Rust tests,
formatting, and Clippy. Repeat actual default/forced-relay helper integration,
four simultaneous tables, spectators, rematches, and recovery windows on the
final binaries. Run quiet paired baseline/candidate workloads with identical
seeds and selected route classes. Package locally with
`scripts/package-team.ps1`, preserve provenance, and run the parent
`verify-current-package.ps1`.

`scripts/test-current-network.ps1 -IncludeLargeRooms` runs all sixteen explicit
default/relay cases. Each invocation creates a fresh directory under the
designated build, records helper, fixture, and DLL hashes in `artifacts.json`,
and retains per-case logs and timed results without replacing earlier runs.
Use `-Cases` and `-Routes` for a bounded retry after a diagnosed failure; combine
only passing results whose artifact hashes match the final build.

Installation, SF4 launch, physical-controller acceptance, and two-PC native
gameplay remain separate user-controlled checks. Synthetic callbacks and
helper traffic do not establish those results.
