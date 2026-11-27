#!/usr/bin/env python3
"""Checks for the site build that need no network: python3 scripts/site/test_site.py

Builds the site from this checkout and a small made-up set of frame data pages, then checks what the
live site's rules need of it: no inline style or script (its Content-Security-Policy refuses them), no
link to a page or file the site lacks, the names the two setup scripts look for, and one shell. Then the
nginx locations the pages ask for, and the preview's made-up answers through the pages' own readers
(check_samples.js, which needs node).
"""
import json
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

import frames
import make
import preview
import preview_accounts
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

| Move | Startup | On hit | On block | Notes | Total |
|---|---:|---:|---:|---|---:|
| Close LP<br>`cl.LP` | 3 | +5° | -2 | Whiffs cr. | 11 |
| Sweep<br>`cr.HK` | 5 | KD | 0° | <script>x</script> | 36 |
| Throw<br>`LP+LK` | 3 | - | - |  | 24 |
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
# Whatever a guide writes that is not one of those tags comes out as text, however it is spelt: a
# tag closed off with a slash, one left open, a comment. A link goes only where a page may.
for written in ('<meta/http-equiv="refresh"/content="0;url=https://x.example">', "<script/x>alert(1)</script/x>",
                "<svg/onload=alert(1)>", '<iframe\nsrc="https://x.example">', "<!-- the rest of the page", "<base/href=//x.example"):
    shown = prose.allowed(f"<div>\n{written}\n</div>")
    assert shown.startswith("<div>") and shown.endswith("</div>") and shown.count("<") == 2, shown
for link, kept in (("https://x.example/a?b=1&amp;c=2", True), ("/docs/install#x", True), ("mailto:a@x.example", True),
                   ("javascript:alert(1)", False), (" JavaScript:alert(1)", False), ("java\tscript:alert(1)", False),
                   ("&#106;avascript:alert(1)", False), ("data:text/html,x", False)):
    assert (prose.allowed(f'<a href="{link}">x</a>') == f'<a href="{link}">x</a>') == kept, (link, prose.allowed(f'<a href="{link}">x</a>'))
assert prose.allowed("<a href='/a' onclick='x()' style='color:red'>x</a>") == '<a href="/a">x</a>'

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
    needed = ["index.html", "rooms.html", "rankings.html", "points.html", "me.html", "players.html", "open.html", "404.html",
              "docs.html", "docs/install.html", "docs/troubleshooting.html", "frame-data.html", "frame-data/ryu.html",
              "frame-data/c-viper.html", "docs/search.json", "assets/site.css", "assets/faces.css", "favicon.ico"]
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

    # Every link and file a page of the main site names from its root exists as nginx serves it: the
    # pages with a location of their own, the guides and frame data pages by the one location that
    # serves a path's own name with ".html", and every other file by its name.
    _, built = preview.site_rules()
    served = {"/": "index.html", "/rooms": "rooms.html", "/rankings": "rankings.html", "/points": "points.html", "/me": "me.html",
              "/players": "players.html", "/start": "open.html"}
    made = [file for file in files if file.endswith(".html") and file not in served.values() and file != "404.html"]
    assert not [file for file in made if not built.match("/" + file.removesuffix(".html"))], "a page nginx has no location for"
    served |= {"/" + file.removesuffix(".html"): file for file in made}
    served |= {"/" + file: file for file in files if not file.endswith(".html")}
    for file, text in pages.items():
        if file == "archive":
            continue
        for link in re.findall(r'(?:href|src)="(/[^"#?]*)', text):
            # The account routes are the bridge's, which nginx passes on: checked against it below.
            assert link in served or link.startswith("/account/"), f"{file}: {link} is not on the site"
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
    # The numbers of a row read across: the Notes column, which the wiki has among them, is last.
    assert "<th>Notes</th>\n</tr>" in ryu and '<td class="num">11</td>\n<td>Whiffs cr.</td>\n</tr>' in ryu, "Notes is not the last column"
    # The list of guides says each in a written line, never the cut-off start of its text.
    assert "Why Defender flags Sidecar.dll" in pages["docs.html"] and "..." not in pages["docs.html"]
    index = pages["frame-data.html"]
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

    # The points and card pages: kept out of search results and every mark filled. Each script a page
    # runs comes after the scripts it needs, as its first line says ("// needs: site marks"); the 404 page
    # (no scripts allowed) and the archive's page run none of the site's.
    for file in ("points.html", "me.html", "players.html", "rankings.html"):
        assert '<meta name="robots" content="noindex">' in pages[file], f"{file}: not kept out of search results"
        assert "{{" not in pages[file], f"{file}: a mark the build left"
    needs = {}
    for script in (shell.SOURCE / "assets").glob("*.js"):
        first = script.read_text(encoding="utf-8").split("\n", 1)[0]
        assert first.startswith("// needs: "), f"{script.name}: the first line does not say what it needs"
        needs[script.stem] = [name for name in first.removeprefix("// needs: ").split() if name != "nothing"]
    for file, text in pages.items():
        if file == "archive":
            continue
        loaded = re.findall(r'<script src="[^"]*?([\w-]+)\.js"', text)
        for at, name in enumerate(loaded):
            lacking = [need for need in needs[name] if need not in loaded[:at]]
            assert not lacking, f"{file}: {name}.js runs before {lacking}"
    assert "<script" not in pages["404.html"] and "account.js" not in pages["archive"]
    assert 'href="/me" data-nav="me" aria-current="page"' in pages["me.html"], "the account link is not current on /me"
    for file, ids in {"me.html": ["signed-out", "unknown-account", "check-again", "use-another", "sign-in-failed", "own-card", "rating-view",
                                  "rating-missing", "unavailable-title", "unavailable-note", "connect-title", "loading"],
                      "players.html": ["signed-out", "unknown-account", "check-again", "use-another", "connect-title", "player-card",
                                       "choices", "unavailable", "loading"],
                      "points.html": ["view-bp", "view-fp", "picker", "board", "you", "search", "country", "region", "more", "share"]}.items():
        lacking = [name for name in ids if f'id="{name}"' not in pages[file]]
        assert not lacking, (file, lacking)
    # points.html needs no stylesheet of the card or rooms pages.
    assert "card.css" not in pages["points.html"] and "rooms.css" not in pages["points.html"]

# Every account route and JSON answer the site asks for or links to, in a script, a page or a part, has a
# location in the site's nginx file: its own (location = PATH) or a prefix it falls under (location ^~
# PREFIX). The account routes take only their own method, pass the session cookie and the client's
# address, and are never cached.
conf = preview.SITE_NGINX.read_text(encoding="utf-8")
blocks = dict(re.findall(r"location (?:= |\^~ )(\S+) \{(.*?)\n\}", conf, re.S))
sources = [path.read_text(encoding="utf-8") for folder in ("assets", "pages", "parts")
           for path in (shell.SOURCE / folder).glob("*.js" if folder == "assets" else "*.html")]
asked = set()
for text in sources:
    asked |= set(re.findall(r"""['"](?:\{\{site\}\})?(/(?:account/[\w/.-]+|[\w/-]+\.json))[?'"]""", text))
assert {"/points.json", "/account/me.json", "/account/card.json", "/account/sign-out", "/account/sign-in"} <= asked, asked
for path in asked:
    assert path in blocks or any(path.startswith(prefix) for prefix in blocks if prefix.endswith("/")), f"nginx has no location for {path}"
for path, method in {"/account/sign-in": "GET", "/account/discord/callback": "GET", "/account/sign-out": "POST",
                     "/account/me.json": "GET", "/account/card.json": "POST", "/points.json": "GET"}.items():
    block = blocks[path]
    assert f"limit_except {method} " in block and "127.0.0.1:47820/v1/" in block and "X-Real-IP" in block, path
    if path.startswith("/account/"):
        assert 'Cache-Control "no-store"' in block and 'proxy_set_header Cookie ""' not in block, path
assert "$ember_points_query" in blocks["/points.json"] and 'proxy_set_header Cookie ""' in blocks["/points.json"]
zone = (preview.SITE_NGINX.parent / "ember-short-zone.conf").read_text(encoding="utf-8")
assert "keys_zone=ember_points_cache" in zone and "zone=ember_account" in zone and "map $arg_fighter $ember_points_query" in zone
setup = (preview.SITE_NGINX.parents[1] / "setup.sh").read_text(encoding="utf-8")
assert "/var/lib/nginx/ember-points" in setup, "setup.sh does not make the points boards' cache folder"

# The preview's made-up answers in every session state go through the pages' own readers (check_samples.js
# under node): a sample a page would refuse to draw fails here. The preview's routes answer as the
# bridge does where a page tells the answers apart.
samples = {}
for state in preview_accounts.STATES:
    accounts = preview_accounts.Accounts(state)
    status, raw, _, _ = accounts.get("/account/me.json", "")
    sample = {"me": json.loads(raw), "boards": {}, "cards": {}}
    for fighter in [""] + [str(fighter) for fighter in range(preview_accounts.FIGHTERS)]:
        sample["boards"][fighter] = json.loads(accounts.get("/points.json", f"fighter={fighter}")[1])
    if state in preview_accounts.SIGNED_IN:
        for asked_for in ({"name": "Player 2"}, {"name": "Mirror Match"}, {"name": "Nobody Here"}, {"card": "c1.preview-003"}):
            status, raw, _, _ = accounts.post("/account/card.json", json.dumps(asked_for).encode(), {})
            assert status == 200, (state, asked_for, status)
            sample["cards"][json.dumps(asked_for)] = json.loads(raw)
    samples[state] = sample
with tempfile.TemporaryDirectory() as folder:
    written = Path(folder, "samples.json")
    written.write_text(json.dumps(samples), encoding="utf-8")
    node = shutil.which("node")
    assert node, "node is needed to run the pages' readers over the preview samples"
    run = subprocess.run([node, str(Path(__file__).with_name("check_samples.js")), str(shell.SOURCE / "assets"), str(written)],
                         capture_output=True, text=True)
    assert run.returncode == 0, run.stderr or run.stdout
assert all("players" in samples[state]["cards"]['{"name": "Mirror Match"}'] for state in preview_accounts.SIGNED_IN)
assert samples["not_taking_part"]["me"]["places"]["fighters"] == [], "a player who stopped taking part is still on a board"
assert any(len(board["players"]) > 50 for board in samples["signed_in"]["boards"].values()), "no board shows Show 50 more"
signed_out = preview_accounts.Accounts("signed_out")
assert signed_out.post("/account/card.json", b'{"name": "Player 2"}', {})[0] == 401
assert json.loads(preview_accounts.Accounts("no_account").post("/account/card.json", b'{"name": "Player 2"}', {})[1])["error"]["details"] == {"reason": "no_ember_id"}
accounts = preview_accounts.Accounts("signed_in")
assert accounts.get("/points.json", "fighter=44")[0] == 400 and accounts.get("/points.json", "fighter=07")[0] == 400
assert accounts.post("/account/sign-out", b"", {"Origin": "null", "Host": "localhost:18733"})[0] == 403 and accounts.state == "signed_in"
assert accounts.post("/account/sign-out", b"", {"Origin": "http://localhost:18733", "Host": "localhost:18733"})[0] == 204
assert accounts.state == "signed_out" and accounts.get("/account/sign-in", "")[0] == 302 and accounts.state == "signed_in"
assert accounts.post("/account/sign-out", b"", {"Sec-Fetch-Site": "cross-site", "Origin": "http://localhost:18733", "Host": "localhost:18733"})[0] == 403

print("site checks passed")
