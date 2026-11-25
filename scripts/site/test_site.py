#!/usr/bin/env python3
"""Checks for the site build that need no network: python3 scripts/site/test_site.py

Builds the site from this checkout and a small made-up set of frame data pages, then checks what the
live site's rules need of it: no inline style or script (its Content-Security-Policy refuses them), no
link to a page or file the site lacks, the names the two setup scripts look for, and one shell.
"""
import json
import re
import tempfile
from pathlib import Path

import frames
import make
import prose
import shell
from shell import ARCHIVE, MAIN, ROOT

FRAME_INDEX = """![Frame data](images/banner-Frame-Data.svg)

Ember's own frame data. Pick a fighter:

[Ryu](Frame-Data-Ryu) · [C. Viper](Frame-Data-C-Viper)

## Reading the tables

- **Startup**: frames until the move can hit. See [Ryu](Frame-Data-Ryu#normals).
"""
FRAME_PAGE = """![Ryu frame data](images/banner-Frame-Data-Ryu.svg)

Frames at 60 per second.

<details><summary>Key</summary>

| Mark | Meaning |
|---|---|
| KD | knocks down |

</details>

## Normals

| Move | Startup | On hit | On block | Notes |
|---|---:|---:|---:|---|
| Close LP<br>`cl.LP` | 3 | +5° | -2 | Whiffs cr. |
| Sweep<br>`cr.HK` | 5 | KD | 0° | <script>x</script> |
| Throw<br>`LP+LK` | 3 | - | - |  |
"""

# The markdown reader: what a guide may write as HTML stays, the rest is shown as text, and a column's
# alignment is a class, since the pages may carry no style attribute.
reader = prose.Prose()
out = reader.render(reader.parse('A <kbd>F10</kbd> <img src="a.png" onerror="x()" width="40"> <id> <script>x</script>\n\n'
                                 "| a | b |\n|---|---:|\n| 1 | 2 |\n\n## Two words\n\ntext under\n\n## Two words\n"))
assert "<kbd>F10</kbd>" in out.html and '<img src="a.png" width="40">' in out.html, out.html
assert "&lt;id&gt;" in out.html and "&lt;script&gt;" in out.html and "onerror" not in out.html, out.html
assert '<td class="num">2</td>' in out.html and "style=" not in out.html, out.html
assert [(h.id, h.body) for h in out.headings] == [("two-words", "text under"), ("two-words-1", "")], out.headings
assert out.summary.startswith("A F10") and out.opening.startswith("A F10"), (out.summary, out.opening)

# A frame data cell's class is its sign; a throw's dash has none.
assert [frames.advantage_class(text) for text in ("+5°", "-2", "0°", "KD", "-", "")] == ["plus", "minus", "even", "kd", "", ""]

# A page source says what the layout needs of it, and nothing the layout does not know.
with tempfile.TemporaryDirectory() as folder:
    source = Path(folder, "page.html")
    source.write_text("<!--\nfile: a.html\ntitle: A\nnav: rooms\nstyles: one two\nindex: no\n-->\n<p><a href=\"{{site}}/x\">x</a></p>\n", encoding="utf-8")
    page = shell.source_page(source, ARCHIVE)
    assert (page.file, page.styles, page.index) == ("a.html", ["one", "two"], False), page
    assert 'href="https://embernetplay.link/x"' in page.content, page.content
    source.write_text("<!--\nfile: a.html\ntitle: A\ncolour: red\n-->\n", encoding="utf-8")
    try:
        shell.source_page(source, MAIN)
        raise AssertionError("an unknown front matter key was taken")
    except ValueError:
        pass

with tempfile.TemporaryDirectory() as folder:
    data, out = Path(folder, "frame-data"), Path(folder, "out")
    data.mkdir()
    (data / "Frame-Data.md").write_text(FRAME_INDEX, encoding="utf-8")
    for name in ("Ryu", "C-Viper"):
        (data / f"Frame-Data-{name}.md").write_text(FRAME_PAGE, encoding="utf-8")
    files = make.build(ROOT, out, data)
    site, archive = out / "site", out / "archive-site"

    # MANIFEST lists every file, and the pages nginx serves by name are there.
    assert files == (site / "MANIFEST").read_text(encoding="utf-8").split(), "MANIFEST is not the file list"
    assert all((site / file).is_file() for file in files)
    needed = ["index.html", "rooms.html", "rankings.html", "open.html", "404.html", "docs/index.html", "docs/install.html",
              "docs/troubleshooting.html", "frame-data/index.html", "frame-data/ryu.html", "frame-data/c-viper.html",
              "docs/search.json", "assets/site.css", "assets/faces.css", "favicon.ico"]
    assert not [file for file in needed if file not in files], [file for file in needed if file not in files]
    assert not [file for file in files if re.search(r"\s", file)], "a file name with a space"

    pages = {file: (site / file).read_text(encoding="utf-8") for file in files if file.endswith(".html")}
    pages["archive"] = (archive / "index.html").read_text(encoding="utf-8")
    for file, text in pages.items():
        # The policy the pages are served with: scripts and styles only from files.
        assert not re.search(r"<[^>]+\sstyle=", text), f"{file}: an inline style"
        assert not re.search(r"<script(?![^>]*\ssrc=)", text) and not re.search(r"<[^>]+\son\w+=", text), f"{file}: an inline script"
        assert "{{" not in text.split("<main", 1)[0], f"{file}: a mark the layout left"
        assert text.count("<header class=\"site-header\">") == 1 and text.count("</footer>") == 1, f"{file}: not one shell"
        assert text.count('aria-current="page"') >= (0 if file in ("open.html", "404.html", "index.html") else 1), file

    # Every link and file a page of the main site names from its root exists as nginx serves it.
    served = {"/": "index.html", "/rooms": "rooms.html", "/rankings": "rankings.html", "/start": "open.html"}
    served |= {"/" + file.removesuffix("/index.html").removesuffix(".html"): file for file in files
               if file.startswith(("docs/", "frame-data/")) and file.endswith(".html")}
    served |= {"/" + file: file for file in files if not file.endswith(".html")}
    for file, text in pages.items():
        if file == "archive":
            continue
        for link in re.findall(r'(?:href|src)="(/[^"#?]*)', text):
            assert link in served, f"{file}: {link} is not on the site"
        for link, heading in re.findall(r'href="(/docs/[a-z0-9-]+)#([^"]+)"', text):
            assert f'id="{heading}"' in pages[served[link]], f"{file}: no heading #{heading} on {link}"

    # The guides: their own links became the site's, and the search knows every guide and heading.
    guide = pages["docs/player-guide.html"]
    assert ".md" not in "".join(re.findall(r'href="(/[^"]*)"', guide)), "a guide link still names a file"
    assert 'href="/docs/troubleshooting' in guide or 'href="/docs/custom-rooms' in guide, "guides do not link to each other"
    search = json.loads((site / "docs/search.json").read_text(encoding="utf-8"))
    assert {"page": "Troubleshooting", "heading": "", "url": "/docs/troubleshooting"}.items() <= next(
        entry for entry in search if entry["url"] == "/docs/troubleshooting").items()
    assert all(set(entry) == {"page", "heading", "url", "text"} for entry in search)

    # Frame data: faces by native ID, links between the pages, the sign of an advantage, and no page's
    # HTML let through.
    ryu = pages["frame-data/ryu.html"]
    assert 'class="face f0 large"' in ryu and 'href="/frame-data/c-viper"' in ryu, "Ryu's page lacks his face or the roster"
    assert '<td class="plus num">+5°</td>' in ryu and '<td class="minus num">-2</td>' in ryu and '<td class="kd num">KD</td>' in ryu
    assert "&lt;script&gt;x&lt;/script&gt;" in ryu and "banner-" not in ryu
    index = pages["frame-data/index.html"]
    assert 'href="/frame-data/ryu#normals"' in index and "Pick a fighter" not in index
    assert 'class="fd-roster"' in pages["index.html"], "the home page lacks the fighters"
    css = (site / "assets/faces.css").read_text(encoding="utf-8")
    assert f".face.f{len(frames.roster()) - 1} {{ background-position: 100.0000% 0; }}" in css, css[-200:]

    # The archive's page: its host's own names, links to the main site by its address, and the strings
    # its setup.sh stamps with each file's hash.
    page = pages["archive"]
    assert '<link rel="stylesheet" href="assets/archive.css">' in page and 'href="/assets/' not in page and 'src="/assets/' not in page
    assert 'href="https://embernetplay.link/rooms"' in page and 'href="./" data-nav="replays" aria-current="page"' in page
    assert "/favicon.ico" not in page
    for file, names in make.ARCHIVE_STAMPED.items():
        text = (archive / file).read_text(encoding="utf-8")
        assert all(name in text for name in names), (file, names)
    bundle = (archive / "assets/archive.css").read_text(encoding="utf-8")
    assert ".site-header" in bundle and ".face.f43" in bundle and ".set-hero" in bundle
    assert not re.search(r"""url\(["']?/""", bundle), "the archive's sheet names a file from the host's root"

print("site checks passed")
