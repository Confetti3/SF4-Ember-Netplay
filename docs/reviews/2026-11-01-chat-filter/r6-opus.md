Review by Opus (Sol had reached its usage limit), against r6-prompt.md.

Round history: r1 to r5 findings are all still fixed (unioned matches, compiled index, narrowing, number exemption, CJK spaces, @ and $, capital sharp s, stems, per-entry classification, plain list of lists, module-owned include, run comparison, room journey row, shitti/shitted, padlo, exact-first comparison with xxx tested).

Checked: display path and raw-text echo matching; NoteUserText gets the drawn text; filtering once per message and the static load; settings wiring and copy in 14 locales; notices; CMake generation; file sizes; every remaining short Latin entry and every 2-3 character kana/Han entry against the allow list.

P2 (judgement, not blockers): German "dick" (thick) is hidden; allowing it also shows the English insult, so decide and record it. "shota" is a common Japanese given name; allow it.
P3: stems are searched only in the word as written, so stretched compounds (fuuuucked, shiiiithead) stay visible; also search squeezed stems in a stretched word's squeezed form.
P3: SameRuns lets a message run of 3+ match only an entry run of 1 or 2, so "xxxx" stays visible; allow any entry run no longer than the message run.
P3: the Hangul single-syllable test reads the raw code point (a full-width Latin letter passes); test the Hangul range.

No structural regression: decode, words, indexed phrases plus stems plus anywhere text, then mask.

VERDICT: APPROVE

Addressed after approval: shota allowed and "dick" kept hidden, recorded in allow.txt; squeezed stems for stretched words (fuuuucked, shiiiithead hidden); the run rule takes entry runs up to the message run (xxxxx hidden); the Hangul range test.
