Two findings remain. The six Round 1 fixes are present in the current source, and the rewrite materially improves the matcher’s structure.

1. **P1 — The combined dictionaries still censor basic everyday chat.**  
   Locations: [assets/chat-filter/allow.txt:256](/C:/Users/Kate/Desktop/sf4/sf4-nightly/assets/chat-filter/allow.txt:256), [ChatFilter.cxx:165](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ChatFilter.cxx:165).

   The new corpus mostly exercises greetings. It misses common vocabulary collisions and ordinary Chinese sentences that contain short listed substrings. The current matching rules produce:

   | Ordinary message | Filtered result | Cause |
   | --- | --- | --- |
   | `fica na sala` — Portuguese, “stay in the room” | `**** na sala` | Italian `fica`, [it.txt:54](/C:/Users/Kate/Desktop/sf4/sf4-nightly/assets/chat-filter/it.txt:54) |
   | `今日你有空吗` — “Are you free today?” | `今**有空吗` | Chinese `日你`, [zh.txt:189](/C:/Users/Kate/Desktop/sf4/sf4-nightly/assets/chat-filter/zh.txt:189) |
   | `这是我妈妈的手机` — “This is my mother’s phone” | `这是我***手机` | Overlapping `妈妈的` and `妈的`, [zh.txt:87](/C:/Users/Kate/Desktop/sf4/sf4-nightly/assets/chat-filter/zh.txt:87) |
   | `靠背椅` — “chair with a backrest” | `**椅` | Chinese `靠背`, [zh.txt:273](/C:/Users/Kate/Desktop/sf4/sf4-nightly/assets/chat-filter/zh.txt:273) |

   These are ordinary messages corrupted by a preference enabled by default. Skipping single CJK characters does not make every remaining substring safe.

   **Fix:** extend the existing allow policy to remove these ambiguous entries, including every overlapping entry needed to preserve the examples. Audit the remaining short CJK entries and cross-language collisions against substantive everyday sentences. Keep this policy in the data rather than adding language-specific exceptions to the matcher. Add these examples to the regression corpus.

2. **P2 — The numeric exemption incorrectly includes letter-substitution symbols.**  
   Locations: [ChatFilter.cxx:15](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ChatFilter.cxx:15), [ChatFilter.cxx:96](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ChatFilter.cxx:96).

   `Digit()` includes `@` and `$`, and `Words()` uses it to decide whether a token is a number. Consequently, `@$$` and `@55` both fold to `ass` but remain unchanged: `PhraseAt()` rejects them as numeric tokens. Full-width equivalents inherit the same problem.

   **Fix:** make digit classification mean only narrowed ASCII `0`–`9`. Keep `@` and `$` explicitly in word-character classification and retain their existing folding. This separates numeric identity from letter substitution and preserves the intended exemption for `455` and `４５５`. Add symbol-only and mixed-symbol/digit regressions expecting `***`.

The surrounding integration is sound on inspection: UI ownership fits a display-only feature; the bounded transcript cache supports immediate preference switching; settings use the existing save and room-copy paths; CMake reuses the canonical embedding helper and tracks input changes. No scoped file crosses 1,000 lines. The recorded asset hashes match, the new CJK settings text has glyph coverage, and attribution/license delivery uses existing required package files.

Validation was read-only: scoped diff and whitespace checks, asset/font inspection, and an in-memory model cross-check covering all 47 literal filter cases plus the 256-byte boundary case. No files were edited, no build was performed, and the C++ tests were not executed.

VERDICT: CHANGES REQUIRED