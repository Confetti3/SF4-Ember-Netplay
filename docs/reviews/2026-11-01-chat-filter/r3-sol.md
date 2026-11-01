Two findings remain. The specific Round 2 fixes are present.

1. **P1 — The combined dictionary still hides clearly ordinary chat.** Locations: [allow.txt:9](C:/Users/Kate/Desktop/sf4/sf4-nightly/assets/chat-filter/allow.txt:9), [de.txt:6](C:/Users/Kate/Desktop/sf4/sf4-nightly/assets/chat-filter/de.txt:6), [it.txt:160](C:/Users/Kate/Desktop/sf4/sf4-nightly/assets/chat-filter/it.txt:160), [zh.txt:26](C:/Users/Kate/Desktop/sf4/sf4-nightly/assets/chat-filter/zh.txt:26).

   The existing exceptions address the reported examples, but these ordinary sentences still match the combined lists:

   | Language | Ordinary message | Filtered result | Trigger |
   |---|---|---|---|
   | Italian | `il mio bimbo dorme` — my child is sleeping | `il mio ***** dorme` | German `bimbo` |
   | Portuguese | `topa jogar mais uma?` — up for another game? | `**** jogar mais uma?` | Italian `topa` |
   | Chinese | `你全家都来吗` — is your whole family coming? | `***都来吗` | Chinese `你全家` |

   These meet the same standard as the previously fixed `bitte` and `fica` collisions. With the filter enabled by default, normal conversation loses meaningful words.

   **Fix:** Add `bimbo`, `topa`, and `你全家` to `allow.txt`, and add these sentences to the corpus exercised against all lists together. Keep independently listed longer insults, such as `操你全家`, active.

2. **P2 — German capital sharp S breaks both word recognition and lower-casing.** Locations: [ChatFilter.cxx:21](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ChatFilter.cxx:21), [ChatFilter.cxx:28](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ChatFilter.cxx:28).

   The listed word `scheiße` becomes `*******`, but its uppercase spelling `SCHEIẞE` remains completely visible. `ẞ` is U+1E9E, outside `WordCharacter`’s Latin ranges, so `Words` splits the input into `SCHEI` and `E`. `Fold` also lacks its lowercase mapping. This is ordinary uppercase text in a supported language, and it contradicts the filter’s case-insensitive matching contract.

   **Fix:** Recognize U+1E9E as a word character and fold it to U+00DF (`ß`) before matching. Add a regression asserting that `scheiße`, `Scheiße`, and `SCHEIẞE` are all masked. This preserves the existing character spans and requires no change to the masking model.

I found no additional structural blocker in the scoped integration. Keeping this display policy in `ui` and caching the filtered string once per message are reasonable; the cache is bounded by 200 lines. The CMake generation reuses the canonical embedding helper and registers its inputs for reconfiguration. No scoped implementation file crosses 1,000 lines. The documented asset hashes match, the new CJK settings text has embedded font coverage, and the preference follows the existing save and room-edit paths.

This was a read-only source review, supplemented by an in-memory JavaScript model for matching examples. No files were edited, no build was run, and native C++ tests were not executed.

VERDICT: CHANGES REQUIRED