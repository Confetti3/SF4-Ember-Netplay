
# Thermo-Nuclear Code Quality Review

Use this skill for an unusually strict review focused on implementation quality, maintainability, abstraction quality, and codebase health.

Above all, this skill should push the reviewer to be **ambitious** about code structure. Do not merely identify local cleanup opportunities. Actively search for "code judo" moves: restructurings that preserve behavior while making the implementation dramatically simpler, smaller, more direct, and more elegant.

## Core Prompt

Start from this baseline:

> Perform a deep code quality audit of the current branch's changes.
> Rethink how to structure / implement the changes to meaningfully improve code quality without impacting behavior.
> Work to improve abstractions, modularity, reduce Spaghetti code, improve succinctness and legibility.
> Be ambitious, if there is a clear path to improving the implementation that involves restructuring some of the codebase, go for it.
> Be extremely thorough and rigorous. Measure twice, cut once.

## Non-Negotiable Additional Standards

Apply the baseline prompt above, plus these explicit review rules:

0. **Be ambitious about structural simplification.**
   - Do not stop at "this could be a bit cleaner."
   - Look for opportunities to reframe the change so that whole branches, helpers, modes, conditionals, or layers disappear entirely.
   - Prefer the solution that makes the code feel inevitable in hindsight.
   - Assume there is often a "code judo" move available: a re-organization that uses the existing architecture more effectively and makes the change dramatically simpler and more elegant.
   - If you see a path to delete complexity rather than rearrange it, push hard for that path.

1. **Do not let a PR push a file from under 1k lines to over 1k lines without a very strong reason.**
   - Treat this as a strong code-quality smell by default.
   - Prefer extracting helpers, subcomponents, modules, or local abstractions instead of letting a file sprawl past 1000 lines.
   - If the diff crosses that threshold, explicitly ask whether the code should be decomposed first.
   - Only waive this if there is a compelling structural reason and the resulting file is still clearly organized.

2. **Do not allow random spaghetti growth in existing code.**
   - Be highly suspicious of new ad-hoc conditionals, scattered special cases, or one-off branches inserted into unrelated flows.
   - If a change adds "weird if statements in random places", treat that as a design problem, not a stylistic nit.
   - Prefer pushing the logic into a dedicated abstraction, helper, state machine, policy object, or separate module instead of tangling an existing path.
   - Call out changes that make the surrounding code harder to reason about, even if they technically work.

3. **Bias toward cleaning the design, not just accepting working code.**
   - If behavior can stay the same while the structure becomes meaningfully cleaner, push for the cleaner version.
   - Do not rubber-stamp "it works" implementations that leave the codebase messier.
   - Strongly prefer simplifications that remove moving pieces altogether over refactors that merely spread the same complexity around.

4. **Prefer direct, boring, maintainable code over hacky or magical code.**
   - Treat brittle, ad-hoc, or "magic" behavior as a code-quality problem.
   - Be skeptical of generic mechanisms that hide simple data-shape assumptions.
   - Flag thin abstractions, identity wrappers, or pass-through helpers that add indirection without buying clarity.

5. **Push hard on type and boundary cleanliness when they affect maintainability.**
   - Question unnecessary optionality, `unknown`, `any`, or cast-heavy code when a clearer type boundary could exist.
   - Prefer explicit typed models or shared contracts over loosely-shaped ad-hoc objects.
   - If a branch relies on silent fallback to paper over an unclear invariant, ask whether the boundary should be made explicit instead.

6. **Keep logic in the canonical layer and reuse existing helpers.**
   - Call out feature logic leaking into shared paths or implementation details leaking through APIs.
   - Prefer existing canonical utilities/helpers over bespoke one-offs.
   - Push code toward the right package, service, or module instead of normalizing architectural drift.

7. **Treat unnecessary sequential orchestration and non-atomic updates as design smells when the cleaner structure is obvious.**
   - If independent work is serialized for no good reason, ask whether the flow should run in parallel instead.
   - If related updates can leave state half-applied, push for a more atomic structure.
   - Do not over-index on micro-optimizations, but do flag avoidable orchestration complexity that makes the implementation more brittle.

## Primary Review Questions

For every meaningful change, ask:

- Is there a "code judo" move that would make this dramatically simpler?
- Can this change be reframed so fewer concepts, branches, or helper layers are needed?
- Does this improve or worsen the local architecture?
- Did the diff add branching complexity where a better abstraction should exist?
- Did a previously cohesive module become more coupled, more stateful, or harder to scan?
- Is this logic living in the right file and layer?
- Did this change enlarge a file or component past a healthy size boundary?
- Are there repeated conditionals that signal a missing model or missing helper?
- Is the implementation direct and legible, or does it rely on special cases and incidental control flow?
- Is this abstraction actually earning its keep, or is it just a wrapper?
- Did the diff introduce casts, optionality, or ad-hoc object shapes that obscure the real invariant?
- Is this logic living in the canonical layer, or did the diff leak details across a boundary?
- Is this orchestration more sequential or less atomic than it needs to be?

## What to Flag Aggressively

Escalate findings when you see:

- A complicated implementation where a cleaner reframing could delete whole categories of complexity.
- Refactors that move code around but fail to reduce the number of concepts a reader must hold in their head.
- A file crossing 1000 lines due to the PR, especially if the new code could be split out.
- New conditionals bolted onto unrelated code paths.
- One-off booleans, nullable modes, or flags that complicate existing control flow.
- Feature-specific logic leaking into general-purpose modules.
- Generic "magic" handling that hides simple structure and makes the code harder to reason about.
- Thin wrappers or identity abstractions that add indirection without simplifying anything.
- Unnecessary casts, `any`, `unknown`, or optional params that muddy the real contract.
- Copy-pasted logic instead of extracted helpers.
- Narrow edge-case handling implemented in the middle of an already busy function.
- Refactors that technically pass tests but make the code less modular or less readable.
- "Temporary" branching that is likely to become permanent debt.
- Bespoke helpers where the codebase already has a canonical utility for the job.
- Logic added in the wrong layer/package when it should live somewhere more central.
- Sequential async flow where obviously independent work could stay simpler and clearer with parallel execution.
- Partial-update logic that leaves state less atomic than necessary.

## Preferred Remedies

When you identify a code-quality problem, prefer suggestions like:

- Delete a whole layer of indirection rather than polishing it.
- Reframe the state model so conditionals disappear instead of getting centralized.
- Change the ownership boundary so the feature becomes a natural extension of an existing abstraction.
- Turn special-case logic into a simpler default flow with fewer exceptions.
- Extract a helper or pure function.
- Split a large file into smaller focused modules.
- Move feature-specific logic behind a dedicated abstraction.
- Replace condition chains with a typed model or explicit dispatcher.
- Separate orchestration from business logic.
- Collapse duplicate branches into a single clearer flow.
- Delete wrappers that do not meaningfully clarify the API.
- Reuse the existing canonical helper instead of introducing a near-duplicate.
- Make type boundaries more explicit so the control flow gets simpler.
- Move the logic to the package/module/layer that already owns the concept.
- Parallelize independent work when that also simplifies the orchestration.
- Restructure related updates into a more atomic flow when partial state would be harder to reason about.

Do not be satisfied with "maybe rename this" feedback when the real issue is structural.
Do not be satisfied with a merely cleaner version of the same messy idea if there is a plausible path to a much simpler idea.

## Review Tone

Be direct, serious, and demanding about quality.
Do not be rude, but do not soften major maintainability issues into mild suggestions.
If the code is making the codebase messier, say so clearly.
If the implementation missed an opportunity for a dramatic simplification, say that clearly too.

Good phrases:

- `this pushes the file past 1k lines. can we decompose this first?`
- `this adds another special-case branch into an already busy flow. can we move this behind its own abstraction?`
- `this works, but it makes the surrounding code more spaghetti. let's keep the behavior and restructure the implementation.`
- `this feels like feature logic leaking into a shared path. can we isolate it?`
- `this abstraction seems unnecessary. can we just keep the direct flow?`
- `why does this need a cast / optional here? can we make the boundary more explicit instead?`
- `this looks like a bespoke helper for something we already have elsewhere. can we reuse the canonical one?`
- `i think there's a code-judo move here that makes this much simpler. can we reframe this so these branches disappear?`
- `this refactor moves complexity around, but doesn't really delete it. is there a way to make the model itself simpler?`

## Output Expectations

Prioritize findings in this order:

1. Structural code-quality regressions
2. Missed opportunities for dramatic simplification / code-judo restructuring
3. Spaghetti / branching complexity increases
4. Boundary / abstraction / type-contract problems that make the code harder to reason about
5. File-size and decomposition concerns
6. Modularity and abstraction issues
7. Legibility and maintainability concerns

Do not flood the review with low-value nits if there are larger structural issues.
Prefer a smaller number of high-conviction comments over a long list of cosmetic notes.

## Approval Bar

Do not approve merely because behavior seems correct.
The bar for approval is:

- no clear structural regression
- no obvious missed opportunity to make the implementation dramatically simpler when such a path is visible
- no unjustified file-size explosion
- no obvious spaghetti-growth from special-case branching
- no obviously hacky or magical abstraction that makes the code harder to reason about
- no unnecessary wrapper/cast/optionality churn obscuring the real design
- no clear architecture-boundary leak or avoidable canonical-helper duplication
- no missed opportunity for an obvious decomposition that would materially improve maintainability

Treat these as presumptive blockers unless the author can justify them clearly:

- the PR preserves a lot of incidental complexity when there is a plausible code-judo move that would delete it
- the PR pushes a file from below 1000 lines to above 1000 lines
- the PR adds ad-hoc branching that makes an existing flow more tangled
- the PR solves a local problem by scattering feature checks across shared code
- the PR adds an unnecessary abstraction, wrapper, or cast-heavy contract that makes the design more indirect
- the PR duplicates an existing helper or puts logic in the wrong layer when there is a clear canonical home

If those conditions are not met, leave explicit, actionable feedback and push for a cleaner decomposition.


## Scope

Repository: this checkout, nightly at HEAD f0e83cbc plus the uncommitted working tree. This is round 2 of the review of the public replay archive (docs/design/REPLAY_ARCHIVE.md); round 1 is docs/reviews/2026-11-01-replay-archive/r1-sol.md (findings F01 to F13, CHANGES REQUIRED). Review the fixes: `git diff` and `git status` show them (untracked new files: server/ember/bridge/migrations/050_archive_identity.sql, server/ember/bridge/src/archive/{mod.rs,sets.rs}, server/ember/bridge/src/delivery/packs.rs, src/platform/ReplaySender.hxx, server/ember/archive-site/deploy/**). Ignore rust/sf4-net/tests/archive_local.rs (an #[ignore]d local harness that sends a folder of real replays through a loopback bridge into a real rustfs) and docs/reviews/.

How each round-1 finding was answered:
- F01: `archive_sets` row per match with a version bumped in the game's transaction; pack queue `delivery/packs.rs` records `uploaded_version`, a set that changed meanwhile stays due, no second lease while one runs, no empty pack; retention keeps a set's replays until the backfill passed them and the pack is stored at its latest version; the listing shows `pack` only once stored.
- F02: identity is the SHA-256 of the replay bytes after the 135-byte head (the head's last two bytes are the saving PC's slot number, which differs between the two players' copies of one game: of 233 real files, 15 slot values); object key `games/{sha256}.emberreplay`; the CRC is a checked column; same digest = 200 and nothing changes, same CRC with another body = 201. Migration 050 rebuilds archive_games without copying 049's rows (a stored 049 row has no bytes to digest; no bridge of ours ran 049; set games return through the backfill).
- F03: online games: the first copy says names, its side and that side's country; later copies change nothing. Set games: sides and names from the roster and desk; a player adds only their own country on their own slot; a game joins a set only through the match route, from a participant, saved inside the match window (created_at to updated_at once ended, or now while open, with one hour of slack for PC clocks); an online game already held joins a set only from its own sender.
- F04: `replays::Replay::parse` applies the native `Describes` checks (used flag, declared size, CRC, `#BRP` header of at least 0x174 bytes, at most 51200, record/body fighters agree); slot bytes are not checked (4 real files would fail); old helper-facing function names kept. All 233 real files parse.
- F05: `Provenance { Unknown, Played, Spectated }`; only Played is eligible; a re-saved game with no note is Unknown.
- F06: consent changes where preferences are loaded or accepted (`AcceptedPreferences`, `ShareReplaysSince`); a first load with sharing on and no cutoff sets now and saves it at once; the sender reads the cutoff by const reference.
- F07: facets kept in archive_events/archive_countries; indexes on (state, saved_at, digest) and (state, event_title, saved_at, digest); one listing looks through at most 5000 stored games; nginx: limit_req 2 r/s per address burst 10, limit_conn 6, 15 s proxy_cache with lock.
- F08: the daily count and the 2 GiB pending bound are inside the one admission (`archive::admit`) in the insert's transaction, both routes; set games skip only the daily count.
- F09: both branches of the set route archive the set's copy (`sets::keep`); `sets::backfill` takes 16 match_replays rows per maintenance pass, resumable via `archive_backfill`.
- F10: `src/platform/ReplaySender.hxx` keeps one request in flight; 3-minute safety timeout then failure; late answers ignored; helper `REPLAY_BUDGET` 45 s over all bridges (`within`, `first_keeping`).
- F11: `[hidden]{display:none!important}`.
- F12: order (saved_at DESC, digest DESC), cursor `{saved_at}.{digest}`, `next` in the answer, `cursor=` in the request; the page follows `next` and handles an empty page that still has `next`; a request counter drops stale answers.
- F13: `s3::origin()` parses the endpoint once (validate and a `Bucket` built at startup); each object URL built once; signed host from that URL; tests for default/non-default port, IPv6, and the Host reqwest actually sends.
- Deployment (new, from your closing paragraph): server/ember/archive-site/deploy/ (rustfs 1.0.4 as a hardened systemd unit on 127.0.0.1:9400, an nginx vhost serving the page, the rate-limited listing and a bucket proxy passing only exact game/set file names with GET/HEAD/OPTIONS, a bucket policy allowing anonymous s3:GetObject on games/* and sets/* only, setup.sh and bucket.sh, README). compose.yml and default.conf.template removed. server/ember/bridge/deploy/set-integration-secret.sh gains `archive [credentials file]`; tenants.staging.json gains the archive block.

Check that each finding is really closed (not just moved), that the fixes did not introduce new bugs (transactions, lease/version races in packs, retention vs backfill, the cursor's encoding and its use with filters, the 5000-row bound's interaction with `next`, migration 050 on a database holding 049 rows, admission accounting, the C++ sender state machine and its use in sf4e__NetplayRuntime__Tournament.cxx, the consent rule at load/accept, provenance readers in the UI), the deploy files (nginx regexes, method limits, cache, headers, systemd hardening, setup idempotence and rollback, secret handling), and the usual thermo-nuclear structure and simplicity concerns. Do not repeat round-1 findings that are fixed; do flag anything still open.

Do not edit files. Do not build. Report findings as P1 (bug / must fix), P2 (should fix), P3 (nit), each with file:line, the concrete failure scenario, and the fix. End with exactly one line: `VERDICT: APPROVE` or `VERDICT: CHANGES REQUIRED`.
