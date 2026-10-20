#!/usr/bin/env python3
"""Checks for the wiki build that need no network: python3 scripts/wiki/test_wiki.py"""
import build

# A changed paragraph keeps the current text and shows the new one below it, marked.
doc = build.Doc("Intro.\n\nOld step.\nSecond line.\n\nOutro.")
assert doc.add("Intro.\n\nOld step.\nSecond line.\n\nOutro.", "Intro.\n\nNew step.\nSecond line.\n\nOutro.", 7) == 1
# Another pull request on the same paragraph is measured against the docs alone and gets its own note.
assert doc.add("Intro.\n\nOld step.\nSecond line.\n\nOutro.", "Intro.\n\nOther step.\nSecond line.\n\nOutro.", 8) == 1
doc.apply()
marked = build.mark(doc.lines, doc.owners)
assert marked[:5] == ["Intro.", "", "Old step.", "Second line.", ""], marked
assert marked[5:7] == ["> [!NOTE]", f"> **Coming soon ([PR #7](https://github.com/{build.UPSTREAM}/pull/7))**"], marked
assert "> New step." in marked and "> Other step." in marked and marked[-1] == "Outro.", marked
assert marked.index("> New step.") < marked.index("> Other step."), marked

# A stacked pull request that repeats a paragraph shares its note.
doc = build.Doc("Old.")
doc.add("Old.", "New.", 4)
doc.add("Old.", "New.", 5)
doc.apply()
assert build.mark(doc.lines, doc.owners).count("> New.") == 1 and "PR #5" in build.mark(doc.lines, doc.owners)[3]

# Text that is already in the docs adds nothing.
assert build.Doc("Same.").add("Before.", "Same.", 3) == 0

# A page only a pull request adds gets one note at the top.
doc = build.Doc("")
doc.add("", "# New\n\nText.", 9)
doc.apply()
assert doc.lines[0] == "# New" and build.mark(doc.lines, doc.owners)[1].startswith("> **Coming soon ([PR #9]")

# Guide links become page links, images move to images/, other files point at GitHub, code stays.
images = set()
out = build.rewrite(["[a](TRAINING_LAB.md#x) ![b](../images/guides/lab/1.webp) [c](../design/X.md)", "```",
                     "[d](CUSTOM_ROOMS.md)", "```"], "docs/guides/USER_NETPLAY.md",
                    {"docs/guides/TRAINING_LAB.md": "Training Lab"}, images, build.UPSTREAM)
assert out[0] == ("[a](Training-Lab#x) ![b](images/guides/lab/1.webp) "
                  f"[c](https://github.com/{build.UPSTREAM}/blob/nightly/docs/design/X.md)"), out
assert out[2] == "[d](CUSTOM_ROOMS.md)" and images == {"docs/images/guides/lab/1.webp"}

print("wiki checks passed")
