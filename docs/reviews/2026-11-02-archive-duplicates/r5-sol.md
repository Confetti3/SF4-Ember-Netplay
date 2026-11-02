No findings requiring changes in `c4aa95a71..b1cd9603a`. I reviewed the duplicates fix and Discord gate, including the surrounding admission, set attribution, delivery, maintenance, listing, replay parsing, and S3 code.

All earlier actionable findings are resolved:

| Earlier findings | Verification |
|---|---|
| Round 1 F1–F2 | Destructive legacy folding waits for all identities. Reads require a valid replay with the requested digest; failures leave identity unset and retry. |
| Round 2 F1–F6 | A distinct set copy reaches insertion and folding; maintenance owns the one-time fold; one queue handles reads and deletes; short replays use their digest; retry documentation and naming arguments are corrected. Set-keeper and upload-deadline tests are present. |
| Round 3 F1–F6 | Duplicate-row revival preserves the lease deadline and attempt counter. Folding restricts donors to the keeper’s roster. Permanent versus temporary responsibilities are documented, ordering has a digest tiebreak, the legacy test harness is extracted, and read orchestration is flattened. |
| Round 4 F1–F3 | Both folding paths obtain the keeper’s roster through the canonical fold implementation. Retry recording checks the leased state. The design text restricts set folding to its players’ copies. |

The current structure earns approval. Identity calculation belongs in the protocol; admission and folding share the naming rule; and the single duplicates queue reuses durable leasing and retry machinery without duplicating workers. No production source file crosses 1,000 lines.

Read-only SQLite probes using SQL extracted from the current source passed:

- All six completion orders for the three-copy naming regression.
- Roster-generation filtering, stranger preservation, and protection of both set rows.
- Upload-to-duplicate transitions, delete deadlines, revival, and stale-record fencing.
- Admission that keeps a distinct set copy from being swallowed by an online copy’s game identity.

The Discord gate runs inside the admission transaction, before either insertion or late naming, and leaves the tournament route’s authorization intact.

`git diff --check c4aa95a71..HEAD` passed. I reused your reported archive **29/29**, full bridge/protocol suite, and clean clippy results; I did not rerun Rust suites or modify files. The SQL probes validate database transitions, not live bucket behavior.

VERDICT: APPROVE