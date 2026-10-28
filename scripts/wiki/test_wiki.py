#!/usr/bin/env python3
"""Checks for the wiki build that need no network: python3 scripts/wiki/test_wiki.py"""
import tempfile
from pathlib import Path

import build
import check
import publish

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

# A pull request that changes one step of a list shows that step alone, with its number and indented lines.
steps = "Do this:\n\n1. One.\n2. Two.\n\n   ![two](two.png)\n\n3. Three.\n4. Four.\n5. Five."
doc = build.Doc(steps)
doc.add(steps, steps.replace("4. Four.", "4. Four, then wait."), 6)
doc.add(steps, steps.replace("2. Two.", "2. Two, twice."), 7)
doc.apply()
marked = build.mark(doc.lines, doc.owners)
assert [line for line in marked if line.startswith("> ") and line[2:3].isdigit()] == \
    ["> 2. Two, twice.", "> 4. Four, then wait."], marked
assert ">    ![two](two.png)" in marked and marked.count("3. Three.") == 1, marked
assert marked.index("4. Four.") < marked.index("> 4. Four, then wait.") < marked.index("5. Five.") - 1, marked
assert marked[marked.index("5. Five.") - 1] == "", marked

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

# The check fails on a dead page link or image and warns about steps without a screenshot.
with tempfile.TemporaryDirectory() as folder:
    Path(folder, "Home.md").write_text("[x](Missing) ![y](images/none.png)\n\n## Steps\n\n1. Do it.\n")
    errors, warnings = check.check(Path(folder))
    assert len(errors) == 2 and len(warnings) == 1, (errors, warnings)
# A build without frame data keeps the published Frame Data section after Training.
old = "**[Home](Home)**\n\n**Training**\n\n- a\n\n**Frame Data**\n\n- b\n\n**Tournaments**\n\n- c\n"
assert publish.carry(old, old.replace("**Frame Data**\n\n- b\n\n", ""), "**") == old
print("wiki checks passed")
