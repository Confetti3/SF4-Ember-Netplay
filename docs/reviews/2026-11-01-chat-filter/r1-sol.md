Found **six actionable issues** in the chat-filter change.

1. **P1 — Match coverage depends on dictionary order and early exits.**  
   [src/ui/ChatFilter.cxx:220](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ChatFilter.cxx:220), [src/ui/ChatFilter.cxx:229](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ChatFilter.cxx:229)  
   A stem match skips phrase matching entirely, and the first matching phrase prevents longer phrases at the same position from being considered. Consequently, listed `fuck buttons` becomes `**** buttons`, `nsfw images` becomes `**** images`, and `rosy palm and her 5 sisters` becomes `**** **** and her 5 sisters`. The latter two are shadowed by shorter entries appearing earlier in `en.txt`.

   **Fix:** Accumulate matching ranges independently of stem matches and dictionary order. Union all applicable ranges, or select the longest phrase at each start while retaining overlapping matches from other starts. Sorting the lists alone would leave the stem shortcut broken. Add regression cases covering both paths.

2. **P2 — Compile dictionary forms once instead of reconstructing them during matching.**  
   [src/ui/ChatFilter.cxx:193](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ChatFilter.cxx:193), [src/ui/ChatFilter.cxx:224](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ChatFilter.cxx:224)  
   Loading the lists once does not eliminate dictionary preprocessing from the message path: every stretched message token causes `Squeeze(phrase.words[k])` to reconstruct candidate dictionary words. There are 1,103 loaded phrases, including 869 single-word entries. A valid 256-byte message consisting of `"aaa "` repeated 64 times causes approximately **70,000 dictionary-word squeezes**; a 100-message history repeats roughly **seven million**. This work occurs synchronously during transcript updates. These are operation counts, not measured C++ latency.

   **Fix:** Build an immutable dictionary containing both normal and squeezed forms during `Loaded()`. Index candidates by their first normal/squeezed token, with direct lookup for single-word entries. This removes the full dictionary scan for most tokens and the repeated string construction. Keep each message token’s span, normalized forms, and numeric status together instead of coordinating four parallel vectors.

3. **P1 — The combined dictionary still masks basic, ordinary multilingual chat.**  
   [assets/chat-filter/fr.txt:4](C:/Users/Kate/Desktop/sf4/sf4-nightly/assets/chat-filter/fr.txt:4), [assets/chat-filter/zh.txt:2](C:/Users/Kate/Desktop/sf4/sf4-nightly/assets/chat-filter/zh.txt:2), [src/ui/ChatFilter.cxx:131](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ChatFilter.cxx:131)  
   The global dictionary produces these results with the default-on preference:

   - German `bitte warten` (“please wait”) → `***** warten`, because French lists `bitte`.
   - English `take a bite` → `take a ****`, because French lists `bite`.
   - Chinese `13点见` (“see you at 13:00”) → `***见`. The equivalent `十三点` is also listed.

   The existing allow list does not address these collisions. Reviewing each source list only within its own language misses a fundamental consequence of applying every language simultaneously.

   **Fix:** Extend the allow list for these ordinary expressions and add a multilingual negative corpus exercised against the complete combined dictionary. Preserve the upstream lists unchanged.

4. **P2 — Full-width normalization and character classification disagree.**  
   [src/ui/ChatFilter.cxx:19](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ChatFilter.cxx:19), [src/ui/ChatFilter.cxx:214](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ChatFilter.cxx:214)  
   `WordCharacter` accepts full-width digits and `Fold` converts them, but `Digit` recognizes only ASCII digits. Thus `ping 455` remains unchanged while `ping ４５５` becomes `ping ***`: the latter folds to `ass` and is incorrectly classified as containing letters. Conversely, `＠ss` remains visible because the full-width `＠` is treated as a separator before folding, although ASCII `@ss` is masked.

   **Fix:** Apply one shared width-normalization step before word and numeric classification, then perform look-alike letter folding. Preserve numeric identity separately from the folded spelling. This also removes duplicated full-width handling from the character predicates.

5. **P2 — The numeric exemption disables listed phrases beginning with a number.**  
   [src/ui/ChatFilter.cxx:219](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ChatFilter.cxx:219)  
   The blanket `if (number[at]) continue` prevents any phrase beginning at a numeric token from being checked. The shipped English entry `2 girls 1 cup` therefore passes through completely unchanged. Protecting standalone numbers should not make an explicit multiword dictionary entry unreachable.

   **Fix:** Continue evaluating phrases at numeric tokens. Apply the number exemption only to standalone word/stem matching, and retain the existing `ping 455` regression case alongside a numeric-leading phrase case.

6. **P2 — Japanese phrase compaction changes only the dictionary side.**  
   [src/ui/ChatFilter.cxx:145](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ChatFilter.cxx:145), [src/ui/ChatFilter.cxx:233](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ChatFilter.cxx:233)  
   Loading removes spaces from Japanese entries, but message matching preserves them. The listed `g スポット` is consequently not masked as written, while `gスポット` is masked. The shipped `プリンス アルバート ピアス` likewise never matches its original spelling.

   **Fix:** Preserve dictionary spaces for literal substring matching. If matching without spaces is intentional, normalize both sides consistently and retain a mapping to original character spans so output spacing remains intact.

The surrounding integration is reasonable: the UI owns this presentation-only behavior, the bounded transcript cache avoids per-frame filtering, and the preference uses the existing save, comparison, and room-edit mechanisms. CMake reuses the embedding helper and its configure dependencies. All 14 recorded asset hashes match the local files; the package inventory already requires attribution and the combined notices file. No changed C++ file crosses 1,000 lines.

No files were edited and no build or C++ tests were run. Validation consisted of source review, scoped whitespace checks, hash checks, and read-only JavaScript logic probes. The probes agreed with all 21 valid-UTF-8 filter fixtures; they do not establish production C++ execution or runtime performance. Add the failures above plus explicit empty-input and exact-256-byte boundary cases to the filter tests.

VERDICT: CHANGES REQUIRED