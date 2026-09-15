# v0.8.4 release audit

Reviewed by the primary agent against source commit `2a2fa203158996694f7de09cb77cef2e1f899e32` and the approved telemetry/audit plans. Review includes the previously uncommitted frame-meter, performance, installer and HUD work. Existing historical packages are preserved.

## Corrected before release

| Finding | Correction and evidence |
| --- | --- |
| Native recovery rejected uppercase hashes written by PowerShell. | Case-insensitive SHA-256 comparison and UTF-8 journal paths; native-to-PowerShell and PowerShell-to-native recovery fixtures. |
| Journal operations were not completely validated before restoring files. | Validate schema, state, root, backup location, safe unique paths, hash shapes and every required backup first. Reject traversal, duplicate operations and directory conflicts. Fixtures verify that malformed late operations and corrupt backups leave earlier destinations unchanged. |
| Predictable temporary siblings could overwrite unrelated files. | Exclusive unique siblings, verified and flushed before replacement. Collision fixtures preserve the old `.ember-new` and journal `.new` names. |
| CheckOnly created/deleted a lock file. | Inspection does not create, truncate or remove the installation lock. Pending-journal inspection reports the recovery command. |
| Lock lifetime had a gap before native rollback; deleting a closed lock file introduced a race. | Retain the exclusive handle through failure recovery. Both paths retain the inert lock file after releasing the handle. |
| Native manifest replacement preceded obsolete-file deletion, and backup bytes were not compared with live bytes before preparation. | Remove obsolete files before replacements; manifest remains last. Verify and flush backups before writing the prepared transaction. |
| The training capture worker was owned by a static DLL object. | Lazy creation on the normal game thread, explicit drain during normal shutdown, no thread join under the Windows DLL loader lock. |
| Captures omitted fields needed to replay contact detection. | Record validity, damage, combo damage and health with sufficient float precision and dropped-sample counts. |
| Multi-phase startup provenance retained the boundary-less initial action. | Latch provenance from the observed boundary; regression assertion added. |
| Lifecycle timestamps represented disk-write time and size-cap drops were silent. | Capture timestamps at producer admission, include QPC in the record, count failed/capped writes, and export losses even with optional CPU diagnostics disabled. |
| PresentMon runner requested v1 output but expected newer timestamp names and could mistake absent display data for failure. | Use current default columns, require absolute QPC, select one process/swap chain, and explicitly label presentation-only fallback. Column meanings checked against the official console documentation. |

## Release boundary

Kate reported successful gameplay for the compact `20260915-r2` candidate. The tested 520-by-62 opaque HUD and its text sizes remain unchanged by this audit. Automated acceptance is the designated Windows suite plus package preflight, native inventory validation and the package builder's real temporary upgrade from published v0.8.3.

The original audit plan is broader than this release. No recorded affected-move capture/replay acceptance or matched presentation benchmark has been supplied. Full immutable room views, pre-construction structured lifecycle comparisons, background formatting of periodic summaries, priority error history, presence-message reuse at resend deadlines, candidate projection caches, and exhaustive journal differential/fault-injection matrices remain follow-up work. The current append optimization still uses the prior full-vector fallback when limits require payload shedding.

Recovery fixtures establish process-interruption behavior for the exercised boundaries, not a guarantee against every storage/power failure. Backups and journals are retained on inconsistent evidence. Native move data remains unavailable where the existing native reader cannot establish a boundary; no new executable addresses were inferred.

PresentMon reference: [official console documentation](https://github.com/GameTechDev/PresentMon/blob/main/README-ConsoleApplication.md).
