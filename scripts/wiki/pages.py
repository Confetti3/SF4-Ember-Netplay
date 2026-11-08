#!/usr/bin/env python3
"""Turn a built wiki into the docs site: python3 scripts/wiki/pages.py --repo owner/name --base /SF4-Ember-Netplay

Reads the pages build.py wrote (build/wiki by default) with the manifest beside
them, and writes a static site in the look of embernetplay.link under build/pages,
for GitHub Pages. The wiki stays as it is; this is the same text with navigation,
search and the version in the header. --base is the path the site is served at (a
project's Pages site lives under its repository's name). Needs the markdown package.
"""
import argparse
import html
import json
import re
import shutil
import sys
from pathlib import Path

import markdown

import build

HERE = Path(__file__).parent
SITE = build.ROOT / "server/ember-short/deploy/site"
SITE_URL = "https://embernetplay.link"
ALERTS = {"NOTE": "Note", "TIP": "Tip", "IMPORTANT": "Important", "WARNING": "Warning", "CAUTION": "Caution"}


def body(text):
    """A wiki page's title and its Markdown without what the site renders itself: the banner, the contents list and the nav."""
    lines = text.splitlines()
    title = re.match(r"!\[(.+?)\]\(images/banner-", lines[0]) if lines else None
    if not title:
        sys.exit(f"not a page build.py wrote: {lines[:1]}")
    lines = lines[1:]
    if "<details>" in lines[:3]:
        lines = lines[lines.index("</details>") + 1:]
    if len(lines) >= 3 and lines[-3] == "---":
        lines = lines[:-3]
    return title[1], "\n".join(lines).strip()


def render(text, pages, base):
    """Markdown to HTML: wiki links point at the site's pages, GitHub's alerts are callouts, tables can scroll."""
    md = markdown.Markdown(extensions=["tables", "fenced_code", "md_in_html", "toc"],
                           extension_configs={"toc": {"slugify": lambda value, sep: build.anchor(value), "toc_depth": "2-3"}})
    out = md.convert(text)

    def link(match):
        attribute, target = match.groups()
        if re.match(r"[a-z][a-z0-9+.-]*:|#|/", target):
            return match[0]
        page, _, fragment = target.partition("#")
        if page in pages:
            return f'{attribute}="{base}/{page}/{"#" + fragment if fragment else ""}"'
        return f'{attribute}="{base}/{target}"' if page.startswith("images/") else match[0]

    out = re.sub(r'(href|src)="([^"]+)"', link, out)
    out = re.sub(r"<blockquote>\s*<p>\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\]\s*(?:<br />)?\s*",
                 lambda m: f'<blockquote class="callout {m[1].lower()}"><p class="callout-title">{ALERTS[m[1]]}</p><p>', out)
    return out.replace("<table>", '<div class="table"><table>').replace("</table>", "</table></div>"), md.toc_tokens


def header(base):
    """The website's own header, from its landing page, with the Docs link added and its paths made to work here."""
    text = (SITE / "index.html").read_text(encoding="utf-8")
    block = re.search(r'<header class="site-header">.*?</header>', text, re.S)
    nav = block and re.search(r'<a href="https://github\.com/[^"]+/releases/latest"', block[0])
    if not nav:
        sys.exit(f"{SITE / 'index.html'} has no site header with a Download link to put the Docs link before")
    docs = f'<a href="{base}/" aria-current="page">Docs</a>'
    return (block[0][:nav.start()].replace('src="/assets/', f'src="{base}/assets/').replace('href="/', f'href="{SITE_URL}/')
            + docs + block[0][nav.start():])


def contents(tokens):
    if not tokens:
        return ""
    return "<ul>" + "".join(f'<li><a href="#{t["id"]}">{t["name"]}</a>{contents(t["children"])}</li>' for t in tokens) + "</ul>"


def page(*, title, section, lead, content, toc, prev, after, source, current, site):
    """One page of the site; `site` is what every page shares."""
    base, version, repo = site["base"], site["version"], site["repo"]

    def item(name):
        here = ' aria-current="page"' if name == current else ""
        return f'<a href="{base}/{build.slug(name)}/"{here}>{html.escape(name)}</a>'

    menu = "".join(f'<div class="docs-section">{html.escape(name)}</div>' + "".join(item(p) for p in pages)
                   for name, pages in site["nav"])
    menu += '<div class="docs-rule"></div>' + "".join(f'<a href="{html.escape(url)}" rel="noopener">{html.escape(name)}</a>'
                                                       for name, url in site["links"])
    switch = (f'<a class="docs-version" href="{base}/Versions/">{html.escape(version)}</a>'
              if site["versions"] else f'<span class="docs-version">{html.escape(version)}</span>')
    turn = "".join(f'<a href="{base}/{build.slug(p)}/"><span>{label}</span>{html.escape(p)}</a>' if p else "<span></span>"
                   for label, p in (("Previous", prev), ("Next", after)))
    rail = f'<nav class="docs-toc" aria-label="On this page"><div class="docs-section">On this page</div>{contents(toc)}</nav>' \
        if toc else ""
    meta = (f'<p class="docs-meta">For {html.escape(version)}'
            f' · <a href="https://github.com/{repo}/blob/{build.BRANCH}/{source}" rel="noopener">Edit on GitHub</a></p>') if source else ""
    crumbs = (f'<a href="{base}/">Docs</a> <span>/</span> <span>{html.escape(section)}</span> <span>/</span> '
              f'<b>{html.escape(title)}</b>') if section else ""
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="referrer" content="no-referrer">
<meta name="description" content="{html.escape(lead)}">
<meta property="og:type" content="article">
<meta property="og:site_name" content="SF4 Ember Netplay">
<meta property="og:title" content="{html.escape(title)}">
<meta property="og:description" content="{html.escape(lead)}">
<meta property="og:image" content="{SITE_URL}/assets/og.jpg">
<meta name="color-scheme" content="dark">
<meta name="theme-color" content="#141312">
<title>{html.escape(title)} · Ember docs</title>
<link rel="icon" href="{base}/assets/ember-emblem.png" type="image/png">
<link rel="preload" href="{base}/assets/fonts/inter-latin-400.woff2" as="font" type="font/woff2" crossorigin>
<link rel="preload" href="{base}/assets/fonts/inter-latin-600.woff2" as="font" type="font/woff2" crossorigin>
<link rel="stylesheet" href="{base}/assets/site.css">
<link rel="stylesheet" href="{base}/assets/docs.css">
<script src="{base}/assets/docs.js" defer></script>
</head>
<body class="wide docs-page">
<div class="shell">
{site["header"]}
<div class="docs-bar">
<button type="button" class="docs-menu" aria-expanded="false" aria-controls="docs-nav">Guides</button>
<label class="docs-search"><span class="offscreen">Search the docs</span><input type="search" id="docs-search" placeholder="Search the docs" autocomplete="off" spellcheck="false" maxlength="64" aria-keyshortcuts="/" data-index="{base}/assets/search.json"><kbd>/</kbd></label>
<div class="docs-results" id="docs-results" hidden></div>
{switch}
</div>
<div class="docs">
<nav class="docs-nav" id="docs-nav" aria-label="Guides">{menu}</nav>
<main class="docs-main">
<p class="docs-crumbs">{crumbs}</p>
<span class="docs-tag">{html.escape(section or "Docs")}</span>
<h1>{html.escape(title)}</h1>
<p class="lead">{html.escape(lead)}</p>
{meta}
{content}
<nav class="docs-turn" aria-label="Previous and next">{turn}</nav>
<footer class="docs-foot">{site["footer"]} Also on the <a href="https://github.com/{repo}/wiki/{build.slug(current)}">GitHub wiki</a>.</footer>
</main>
{rail}
</div>
</div>
</body>
</html>
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--wiki", type=Path, default=build.ROOT / "build/wiki", help="the built wiki")
    parser.add_argument("--out", type=Path, default=build.ROOT / "build/pages", help="where the site goes")
    parser.add_argument("--repo", default=build.UPSTREAM, help="the repository whose wiki and docs the pages link to")
    parser.add_argument("--base", default="", help="the path the site is served at, such as /SF4-Ember-Netplay")
    args = parser.parse_args()
    wiki, base, out = args.wiki, args.base.rstrip("/"), args.out
    if not wiki.with_suffix(".json").is_file():
        sys.exit(f"no built wiki in {wiki}; run build.py first")
    manifest = json.loads(wiki.with_suffix(".json").read_text(encoding="utf-8"))
    listed = {row["slug"]: row for row in manifest["pages"]}
    # Every page build.py wrote: the listed ones in their order, then the rest (a fighter's frame data).
    slugs = list(listed) + sorted(p.stem for p in wiki.glob("[!_]*.md") if p.stem not in listed and p.stem != "Home")
    pages = {slug: listed[slug]["page"] if slug in listed else slug for slug in slugs}
    order = [row["page"] for row in manifest["pages"]]
    sections = {}
    for row in manifest["pages"]:
        sections.setdefault(row["section"], []).append(row["page"])
    site = dict(nav=list(sections.items()), links=manifest["links"], base=base, repo=args.repo, header=header(base),
                version=manifest["version"], versions=(wiki / "Versions.md").is_file(),
                footer=re.sub(r"</?p>", "", render(manifest["footer"], pages, base)[0]).strip())

    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(wiki / "images", out / "images")
    shutil.copytree(HERE / "site", out / "assets")
    # The site's own stylesheet, fonts, emblem and background, so the docs look like it wherever they are served.
    for name in ("ember-emblem.png", "ember-background.jpg", "ember-background.webp"):
        shutil.copyfile(SITE / "assets" / name, out / "assets" / name)
    shutil.copytree(SITE / "assets/fonts", out / "assets/fonts")
    css = (SITE / "assets/site.css").read_text(encoding="utf-8").replace('url("/assets/', f'url("{base}/assets/')
    (out / "assets/site.css").write_text(css, encoding="utf-8")

    index = []
    for slug in slugs:
        title, text = body((wiki / f"{slug}.md").read_text(encoding="utf-8"))
        content, toc = render(text, pages, base)
        row = listed.get(slug, {"page": title, "section": None, "source": None, "about": ""})
        at = order.index(row["page"]) if row["page"] in order else -1
        (out / slug).mkdir(parents=True)
        (out / slug / "index.html").write_text(page(
            title=title, section=row["section"], lead=row["about"], content=content, toc=toc,
            prev=order[at - 1] if at > 0 else None, after=order[at + 1] if 0 <= at < len(order) - 1 else None,
            source=row["source"], current=row["page"], site=site), encoding="utf-8")
        index.append({"title": title, "section": row["section"], "url": f"{base}/{slug}/", "about": row["about"],
                      "headings": [{"text": t["name"], "id": t["id"]} for top in toc for t in [top, *top["children"]]]})
    _, home = body((wiki / "Home.md").read_text(encoding="utf-8"))
    lead = next((line for line in home.splitlines() if line and not line.startswith(("#", "|"))), "")
    cards = "".join(f"<h2>{html.escape(section)}</h2><div class=\"docs-cards\">" + "".join(
        f'<a class="docs-card" href="{base}/{build.slug(p)}/"><b>{html.escape(p)}</b><span>{html.escape(listed[build.slug(p)]["about"])}</span></a>'
        for p in found) + "</div>" for section, found in sections.items())
    (out / "index.html").write_text(page(title="Guides", section=None, lead=lead, content=cards, toc=[], prev=None,
                                         after=order[0] if order else None, source=None, current="Home", site=site),
                                    encoding="utf-8")
    (out / "assets/search.json").write_text(json.dumps(index, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    (out / ".nojekyll").write_text("")
    print(f"{len(index) + 1} pages in {out}")


if __name__ == "__main__":
    sys.exit(main())
