
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

Repository: this checkout (C++ Ember netplay UI, Dear ImGui, C++17). The working tree holds two separate changes; review only the chat filter. Its files: untracked `src/ui/ChatFilter.hxx`, `src/ui/ChatFilter.cxx`, `cmake/sf4e_chat_filter.cmake`, `assets/chat-filter/` (README.md, allow.txt, stems.txt, LICENSE and the 13 LDNOOBW `<lang>.txt` lists), and in `git diff`: CMakeLists.txt, scripts/collect-notices.py, ATTRIBUTION.md, src/netplay/PlayerPreferences.hxx, src/netplay/BoolPreferences.hxx, src/ui/MenuRows.hxx, src/ui/ApplicationShell__Settings.cxx, src/ui/ChatPanel.cxx, src/ui/ChatTranscript.hxx/.cxx, the `settings.chat_filter*` entries in locales/*.po, src/tests/shell_chat_journey_test.cxx, src/tests/shell_training_journey_test.cxx, src/tests/settings_store_test.cxx. Ignore the room-limit change (rust/, server/, PublicRoomsPanel.cxx, public_rooms_journey_test.cxx, the `public.failure.room_limit_*` .po entries) and docs/reviews/.

What it does: room chat messages are shown with listed words hidden as stars on the player's own screen, behind a new saved on/off preference `chatFilter` (default on), shown as a "Chat filter" row in Settings > Interface > Room layout, which a room leaves editable (CopyRoomSettings). The lists are LDNOOBW's (CC BY 4.0), unmodified, embedded by CMake as byte arrays in a generated header; `allow.txt` takes everyday words back out (the lists were made for search suggestions), `stems.txt` adds a few English stems matched inside words. Spaced languages match whole words / phrases (last word may take -s/-es), Japanese and Chinese entries match anywhere (single characters skipped), with lower-casing, look-alike digit folding and stretched-letter squeezing. The transcript computes `filtered` once per message; ChatPanel draws it when the preference is on. The licence goes into notices/THIRD_PARTY_LICENSES.txt through collect-notices.py so the package file set does not change (older installed builds accept only the existing file set from an update).

Check: correctness of FilterChat (UTF-8 handling, character spans, phrase matching, CJK substring matching, masking spaces, numbers, edge cases like empty text, text at the length limit, a word at the very end), false positives on ordinary chat in the allow list's languages and anything obviously ordinary still masked, performance (it runs once per message for up to 200 lines; lists load once), whether the layering is right (ui vs common, generated header, the transcript storing a second string), the CMake generation, the Settings placement and copy, tests, and the notices/licence attribution.

Do not edit files. Do not build. Report findings as P1 (bug / must fix), P2 (should fix), P3 (nit), each with file:line, the concrete failure scenario, and the fix. End with exactly one line: `VERDICT: APPROVE` or `VERDICT: CHANGES REQUIRED`.

## Round 2

Round 1 (docs/reviews/2026-11-01-chat-filter/r1-sol.md) found six issues; all are addressed in the rewritten src/ui/ChatFilter.cxx:

1. Every match counts: stems no longer skip phrase matching, and every candidate phrase at a position is checked (no early break); ranges are unioned. Tests: "fuck buttons", "nsfw images", "2 girls 1 cup".
2. The dictionary is compiled once with each phrase's words as written and squeezed, indexed by first word (written and squeezed); a message word looks up its candidates (itself, less -s, less -es) instead of scanning every phrase. Message words are one Word struct (span, folded, squeezed, number).
3. allow.txt takes back the cross-language collisions: bitte, bite, cu, 13点, 十三点. The tests run an everyday-chat corpus in all 14 languages against the combined lists.
4. Characters are narrowed (full-width to ASCII) once in Decode, before word and digit classification and folding. Tests: "ping ４５５" unchanged, "＠ss" hidden.
5. The number exemption applies only to one-word entries and stems; phrases may start with a number.
6. Japanese and Chinese entries keep their spaces ("g スポット" is hidden as written).

Also added: empty text and a 256-byte message ending in a listed word.

Review the current working tree for the same scope again.

## Round 3

Round 2 (r2-sol.md) found:

1. P1, everyday chat still hidden: allow.txt now takes back Portuguese "fica" and, for Chinese, the two-character entries everyday sentences contain (日你, 你妈, 他妈, 你娘, 他娘, 娘的, 妈的, 妈妈的, 他奶奶, 老二, 靠背, 靠爸, 靠母, 干你, 插你, 打炮, 吹箫, 野鸡, 你老闆, 粉腸, 硬膠); the longer insults that start the same (他妈的, 日你妈, 你妈的, 干你娘 ...) stay listed. Japanese entries that are everyday words were audited too (お尻/おしり/オシリ, 両刀, 脱衣, 挿入, 自己愛性, コカイン, エクスタシー, エロティック, エロティズム, フェティッシュ, プレイボーイ, ローター, ぶっかけ). The corpus adds your four examples plus 姑娘的手机, 他奶奶来了, お尻が痛い, カードを挿入する.
2. P2, @ and $ as digits: Digit is now ASCII 0-9 only; WordCharacter names @ and $ itself. Tests: "@$$" and "@55" are hidden.

Review the current working tree for the same scope again. The allow list is a judgement call by language; flag only entries that are clearly ordinary chat still hidden, or clearly offensive words wrongly allowed.

## Round 4

Round 3 (r3-sol.md) found:

1. P1: allow.txt now takes back Italian/German "bimbo", Portuguese/Italian "topa" and Chinese 你全家 (操你全家 stays listed); the three sentences are in the corpus.
2. P2: U+1E9E is a word character and folds to ß; scheiße, Scheiße and SCHEIẞE are all hidden in the tests.

The allow list will never be complete for 14 languages at once; it is a living judgement list that players' reports extend. Approve unless you find a structural problem or an ordinary collision of the same weight as these (a very common word in one of Ember's languages); report any such collisions as one list rather than as a blocker each, so they can all be added in one pass.

## Round 5

Round 4 was reviewed by Opus (r4-opus.md) while Sol was at its usage limit. All of it is addressed:

- Stems: the bare "shit" stem and the two slur stems are gone; stems.txt has shitt, shith, shitf, shitl, shitb, bullshit, dipshit, horseshit (romaji never puts a consonant after t), and the slurs rely on their list entries (+s). Stems are matched as written only. Corpus: arigatou gozaimashita, otsukaresama deshita, doushite?, sniggering stay; shithead, shitting, fuuuucks are hidden.
- allow.txt adds lul, debil (Czech and Polish), godverdomme, gadverdamme, 老味, 撒尿, 白痴, 白癡, 情色, 宦官.
- Each entry is classified by its own characters (kana/Han -> anywhere, more than one character; no word -> anywhere; else a phrase with a long word). The generated header is a plain list of lists; List, tag and the second CMake list are gone; the module owns its target_include_directories (removed from CMakeLists.txt).
- Stretched words compare letter run by letter run (SameRuns): a run of 3+ matches a run of 1 or 2, others must be equal. Phrase keeps only its words; the squeezed first word is just the lookup key. Corpus: "connnn Bobbb" stays.
- shell_room_journey_test lists chat-filter among the Room layout rows a room leaves open.

Same instruction as round 4: approve unless there is a structural problem or a very common ordinary collision; list any collisions together.

## Round 6

Round 5 (r5-opus.md) is addressed:

1. stems.txt drops shitt for shitti and shitted (shitter is left out, inside shitteru); the comment says why. Corpus: "shitteru? shitte shitto" stays; "shithead, shitting, fuuuucks" is still hidden.
2. allow.txt takes back padlo (Czech, a Russian entry); "spojení mi padlo" is in the corpus.
3. SameWord compares exactly first, then run by run for a stretched word, so the listed "xxx" is hidden as written (tested).

Same instruction: approve unless there is a structural problem or a very common ordinary collision; list any collisions together.
