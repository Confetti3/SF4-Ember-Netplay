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
# Every page says what it covers, and the outbound links come from the interface's own table.
assert all(page.about for page in build.PAGES), [page.path for page in build.PAGES if not page.about]
assert build.links()["discord"].startswith("https://discord.gg/"), build.links()
# A page with four sections gets a collapsed list of them, with GitHub's ids; a short page gets none.
long = ["## One", "", "## Two [x](y)!", "```", "## Not a heading", "```", "## Three", "## Four"]
assert build.contents(long)[3:7] == ["- [One](#one)", "- [Two x!](#two-x)", "- [Three](#three)", "- [Four](#four)"], \
    build.contents(long)
assert build.contents(long[:3]) == []
# A build from a README that lost a part-heading says which, instead of a traceback.
with tempfile.TemporaryDirectory() as folder:
    (Path(folder) / "docs/guides").mkdir(parents=True)
    (Path(folder) / "README.md").write_text("# Ember\n\n## Get started\n")
    try:
        build.build(Path(folder), build.UPSTREAM, Path(folder) / "out", None, overlay_prs=False)
    except SystemExit as stop:
        assert build.README_PART[0] in str(stop), stop
    else:
        raise AssertionError("a README without its part-headings built")
# Previous and Next follow the sidebar: by section, then PAGES, whatever order PAGES is written in, with
# the frame data index after Training.
with tempfile.TemporaryDirectory() as folder:
    src, frames = Path(folder, "src"), Path(folder, "frames")
    (src / "docs/guides").mkdir(parents=True), frames.mkdir()
    (src / "README.md").write_text("# Ember\n\n" + "\n\n".join(build.README_PART) + "\n")
    for name in ("TRAINING_LAB", "TROUBLESHOOTING"):
        (src / f"docs/guides/{name}.md").write_text(f"# {name}\n\nText.\n")
    (frames / "Frame-Data.md").write_text("# Frame Data\n\nMoves.\n")
    written, build.PAGES = build.PAGES, build.PAGES[::-1]
    try:
        build.build(src, build.UPSTREAM, Path(folder, "out"), None, overlay_prs=False, framedata=frames)
        # The build that publishes has no frame data: the index keeps its place, and the published page is kept.
        build.build(src, build.UPSTREAM, Path(folder, "bare"), None, overlay_prs=False)
    finally:
        build.PAGES = written
    last = lambda page: Path(folder, "out", f"{page}.md").read_text().splitlines()[-1]
    assert last("Install") == "[Home](Home) | [Next: Training Lab](Training-Lab)", last("Install")
    assert last("Training-Lab") == "[Previous: Install](Install) | [Home](Home) | [Next: Frame Data](Frame-Data)"
    assert last("Frame-Data") == "[Previous: Training Lab](Training-Lab) | [Home](Home) | [Next: Troubleshooting](Troubleshooting)"
    sidebar = Path(folder, "out", "_Sidebar.md").read_text()
    assert sidebar.index("Install") < sidebar.index("Training Lab") < sidebar.index("Frame Data") < sidebar.index("Trouble")
    bare = Path(folder, "bare")
    assert Path(bare, "Training-Lab.md").read_text().splitlines()[-1].endswith("[Next: Frame Data](Frame-Data)")
    assert not Path(bare, "Frame-Data.md").exists() and "[Frame Data](Frame-Data)" in Path(bare, "_Sidebar.md").read_text()
    assert check.check(bare)[0] == [], check.check(bare)[0]
# A page or a heading that links point to may not disappear unnoticed; an anchor kept for a moved heading counts.
assert publish.lost({"A": "## One\n## Two", "B": "x"}, {"A": "## One\n## Three"}) == (["B"], ["A#two"])
assert publish.lost({"A": "## Two"}, {"A": '<a name="two"></a>'}) == ([], [])

# The README still has both headings the wiki takes its install part between.
readme = (build.ROOT / "README.md").read_text(encoding="utf-8").splitlines()
assert all(h in readme for h in build.README_PART), [h for h in build.README_PART if h not in readme]

print("wiki checks passed")
