**Six findings remain: two P1 and four P2.** The main problems are permanent settlement of temporarily inadmissible replays and retention overriding durable publication state.

1. **R3-01 — P1: Disabling the archive allows retention to destroy an unfinished pack’s source.**  
   Locations: [routes/replays.rs:424](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/routes/replays.rs:424), [delivery/packs.rs:71](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/delivery/packs.rs:71).

   `prune()` replaces the entire archive protection predicate with `0` whenever the current configuration has no archive. Consider a set whose latest pack failed to upload, followed by an operator temporarily disabling the archive. Once retention expires, maintenance deletes its `match_replays` and closes the set despite `uploaded_version < version`. Re-enabling the archive leaves a permanently unfinished publication: the pack worker has no replays to build from, and the closed route refuses replacements.

   An in-memory probe using the actual predicates confirmed that the same unfinished set is protected with archival enabled and selected for deletion with it disabled.

   **Fix:** Separate existing publication obligations from current archive enablement. Always protect sets with an outstanding `archive_sets` version; gate only the backfill requirement for otherwise unarchived rows on the current configuration. Disabling a producer must not discard a durable queue’s inputs.

2. **R3-02 — P1: A valid replay sent before bridge confirmation is permanently abandoned.**  
   Locations: [routes/replays.rs:140](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/routes/replays.rs:140), [ReplaySender.hxx:59](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/platform/ReplaySender.hxx:59), [NetplayRuntime__Tournament.cxx:258](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/sf4e/sf4e__NetplayRuntime__Tournament.cxx:258).

   The native sender selects locally completed, tagged replays without waiting for the bridge’s `GameConfirmed` state. Report submission and replay submission are independent operations. If the replay arrives while the first game is still awaiting its second report—or while reports await organizer resolution—`free_game()` finds no accepted attempt. `hold()` answers `400 invalid_request`, which the sender treats as final and writes to `sent.txt`.

   When the game subsequently becomes accepted, that legitimate replay is never retried. The exact query returned no candidate while an attempt was `permitted` and returned that same attempt immediately after confirmation.

   **Fix:** Distinguish “not confirmed yet” from permanent incompatibility. Return a retryable response while a plausible attempt remains unresolved, or persistently defer the native upload until confirmation is known. Keep `set_closed` final; it is a different outcome.

3. **R3-03 — P2: The aggregate capacity check lets a voided game block its replacement’s replay.**  
   Location: [routes/replays.rs:183](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/routes/replays.rs:183).

   The numerator counts every retained replay, including those attached to attempts subsequently marked `voided`; the denominator counts only currently accepted attempts. After one uploaded game is voided and its replacement is accepted, there can be one retained replay and one accepted attempt. The `<` condition fails even though the replacement’s own replay slot is empty. Its upload receives a permanent refusal.

   An in-memory probe reproduced this. Removing the aggregate condition allowed the query to select the unoccupied replacement correctly.

   **Fix:** Let explicit attempt occupancy and the unique `attempt_id` index govern modern rows. Handle legacy rows with null `attempt_id` separately, accounting for currently accepted occupancy rather than all historical replays. The per-game model should replace the old aggregate model instead of carrying both conflicting rules.

4. **R3-04 — P2: Closed-set replays cannot take the documented online-archive fallback.**  
   Locations: [ReplaySets.hxx:33](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/platform/ReplaySets.hxx:33), [NetplayRuntime__Tournament.cxx:258](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/sf4e/sf4e__NetplayRuntime__Tournament.cxx:258).

   The design says a replay refused by a closed set can still enter the online archive through its sender’s sharing. In the native implementation, the set refusal marks the file globally sent. Independently, `NextToArchive()` excludes every tagged replay. Consequently, even with sharing enabled and the replay inside the consent cutoff, that file cannot reach `replay_archive`.

   **Fix:** Represent set-route settlement separately from archive delivery. Preserve the `set_closed` disposition through the helper and allow a consent-eligible online attempt after that final set refusal. A single `sent` bit cannot express both destinations’ outcomes.

5. **R3-05 — P2: A failed rollback deletes its own recovery backup.**  
   Locations: [deploy/setup.sh:198](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/archive-site/deploy/setup.sh:198), [deploy/setup.sh:228](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/archive-site/deploy/setup.sh:228).

   `put_back()` records restoration failures only in a local variable and prints a warning. `cleanup()` then unconditionally removes `$backup`. For example, if the target filesystem fills after deployment and copying the previous RustFS binary back fails, the script deletes the only retained predecessor while leaving the new or incomplete installation in place.

   **Fix:** Return an explicit rollback result. Delete the predecessor backup only after successful restoration; otherwise preserve it and print its location for recovery. The normal rollback path is substantially improved, but its failure path must retain the recovery material.

6. **R3-06 — P2: Multiline nginx directives bypass conflicting-vhost detection.**  
   Location: [deploy/setup.sh:147](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/archive-site/deploy/setup.sh:147).

   The conflict search requires `server_name` and the hostname on the same line. A valid directive with `server_name` on one line and the hostname on the next is missed. Nginx can then accept the duplicate with a warning and continue serving the earlier vhost.

   The post-reload checks do not reliably catch this when the earlier vhost serves an older archive installation: its page has the same title, its bucket root returns 404, and its listing contains `"games"`. The script can report success while the new site remains unused.

   **Fix:** Inspect complete nginx directives, preferably from `nginx -T`, and reject conflicting hostname ownership independently of line breaks. Strengthen the post-reload check to verify the staged page/assets so an older archive installation cannot satisfy installation acceptance.

R2-03, R2-05, R2-06, R2-07 and R2-08 hold up within this review’s static and in-memory scope. The atomic prune/closure update, nearest-window selection with earlier-game tie-breaking, and transactional attempt uniqueness also hold up. The findings above identify remaining interactions around R2-01/R2-02 and deployment gaps around R2-04/R2-09. I treated the documented save-time and online-attribution limitations as accepted.

Validation included the scoped source and diff review, exact-query SQLite probes, and controlled execution of the page’s actual load function. Stale first-page and append responses were dropped correctly. `git diff --check` was clean, and no changed file crossed from below to above 1,000 lines. No files were edited, no builds or repository test executables were run, and deployment/gameplay acceptance remains unverified.

VERDICT: CHANGES REQUIRED