#!/usr/bin/env python3
"""Build the docs site: python3 scripts/wiki/pages.py --repo owner/name

Collects the wiki's pages with build.py from this checkout's docs alone,
without the "Coming soon" notes from open pull requests, and writes them as a
static site in the look of embernetplay.link under build/pages, for GitHub
Pages: the same text as the wiki with navigation, search and the version in the
header. It renders build.collect()'s pages, not the wiki's finished Markdown.
--repo is the repository every link and the version list are taken from. Every
page is a file beside Home's index.html, named as the wiki names it, so the
wiki's own links and image paths work as they are, and so do the relative asset
paths, wherever the site is served. Needs the markdown package.
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


def render(text):
    """Markdown to HTML: GitHub's alerts are callouts, tables can scroll."""
    md = markdown.Markdown(extensions=["tables", "fenced_code", "md_in_html", "toc"],
                           extension_configs={"toc": {"slugify": lambda value, sep: build.anchor(value), "toc_depth": "2-3"}})
    out = re.sub(r"<blockquote>\s*<p>\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\]\s*(?:<br />)?\s*",
                 lambda m: f'<blockquote class="callout {m[1].lower()}"><p class="callout-title">{ALERTS[m[1]]}</p><p>',
                 md.convert(text))
    return out.replace("<table>", '<div class="table"><table>').replace("</table>", "</table></div>"), md.toc_tokens


def header():
    """The website's own header, from its landing page, with the Docs link added and its paths made to work here."""
    text = (SITE / "index.html").read_text(encoding="utf-8")
    block = re.search(r'<header class="site-header">.*?</header>', text, re.S)
    nav = block and re.search(r'<a href="https://github\.com/[^"]+/releases/latest"', block[0])
    if not nav:
        sys.exit(f"{SITE / 'index.html'} has no site header with a Download link to put the Docs link before")
    docs = '<a href="./" aria-current="page">Docs</a>'
    return (block[0][:nav.start()].replace('src="/assets/', 'src="assets/').replace('href="/', f'href="{SITE_URL}/')
            + docs + block[0][nav.start():])


def contents(tokens):
    if not tokens:
        return ""
    return "<ul>" + "".join(f'<li><a href="#{t["id"]}">{t["name"]}</a>{contents(t["children"])}</li>' for t in tokens) + "</ul>"


def href(built, title):
    """Where a page of the chain is: its file here, or the wiki's page for one the site has no file for (the
    Frame Data index of a build without frame data)."""
    if any(sheet.page.title == title and sheet.lines is None for sheet in built.chain):
        return f"https://github.com/{built.wiki}/wiki/{build.slug(title)}"
    return build.slug(title)


def page(sheet, content, toc, built, header):
    """One page of the site, for sheet, with its rendered content and toc; built is what every page shares."""
    info, repo, version = sheet.page, built.wiki, built.version
    def item(page):
        here = ' aria-current="page"' if page.title == info.title else ""
        return f'<a href="{html.escape(href(built, page.title))}"{here}>{html.escape(page.title)}</a>'

    menu = "".join(f'<div class="docs-section">{html.escape(name)}</div>' + "".join(item(p) for p in pages)
                   for name, pages in built.sections.items())
    menu += '<div class="docs-rule"></div>' + "".join(f'<a href="{html.escape(url)}" rel="noopener">{html.escape(name)}</a>'
                                                       for name, url in built.links)
    switch = (f'<a class="docs-version" href="https://github.com/{repo}/wiki/Versions">{html.escape(version)}</a>'
              if built.tags else f'<span class="docs-version">{html.escape(version)}</span>')
    turn = "".join(f'<a href="{html.escape(href(built, p))}"><span>{label}</span>{html.escape(p)}</a>' if p else "<span></span>"
                   for label, p in (("Previous", sheet.before), ("Next", sheet.after)))
    rail = f'<nav class="docs-toc" aria-label="On this page"><div class="docs-section">On this page</div>{contents(toc)}</nav>' \
        if toc else ""
    meta = (f'<p class="docs-meta">For {html.escape(version)}'
            f' · <a href="https://github.com/{repo}/blob/{build.BRANCH}/{info.path}" rel="noopener">Edit on GitHub</a></p>'
            ) if info.path else ""
    crumbs = (f'<a href="./">Docs</a> <span>/</span> <span>{html.escape(info.section)}</span> <span>/</span> '
              f'<b>{html.escape(sheet.title)}</b>') if info.section else ""
    footer = re.sub(r"</?p>", "", render(built.footer)[0]).strip()
    title, section, lead = sheet.title, info.section, info.about
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
<link rel="icon" href="assets/ember-emblem.png" type="image/png">
<link rel="preload" href="assets/fonts/inter-latin-400.woff2" as="font" type="font/woff2" crossorigin>
<link rel="preload" href="assets/fonts/inter-latin-600.woff2" as="font" type="font/woff2" crossorigin>
<link rel="stylesheet" href="assets/site.css">
<link rel="stylesheet" href="assets/docs.css">
<script src="assets/docs.js" defer></script>
</head>
<body class="wide docs-page">
<div class="shell">
{header}
<div class="docs-bar">
<button type="button" class="docs-menu" aria-expanded="false" aria-controls="docs-nav">Guides</button>
<label class="docs-search"><span class="offscreen">Search the docs</span><input type="search" id="docs-search" placeholder="Search the docs" autocomplete="off" spellcheck="false" maxlength="64" aria-keyshortcuts="/" data-index="assets/search.json"><kbd>/</kbd></label>
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
<footer class="docs-foot">{footer} Also on the <a href="https://github.com/{repo}/wiki/{build.slug(info.title)}">GitHub wiki</a>.</footer>
</main>
{rail}
</div>
</div>
</body>
</html>
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=build.ROOT / "build/pages", help="where the site goes")
    parser.add_argument("--repo", default=build.UPSTREAM, help="the repository whose wiki and docs the pages link to")
    args = parser.parse_args()
    out = args.out
    if out.exists():
        shutil.rmtree(out)
    built = build.collect(build.ROOT, args.repo, None, overlay_prs=False, tags=build.wiki_tags(args.repo), wiki=args.repo)
    build.write_images(built.images, out / "images")
    top = header()

    shutil.copytree(HERE / "site", out / "assets")
    # The site's own stylesheet, fonts, emblem and background, so the docs look like it wherever they are served.
    for name in ("ember-emblem.png", "ember-background.jpg", "ember-background.webp"):
        shutil.copyfile(SITE / "assets" / name, out / "assets" / name)
    shutil.copytree(SITE / "assets/fonts", out / "assets/fonts")
    css = (SITE / "assets/site.css").read_text(encoding="utf-8").replace('url("/assets/', 'url("')
    (out / "assets/site.css").write_text(css, encoding="utf-8")

    # The site has no frame data pages yet: it renders the chain, not built.others, and links the Frame Data
    # index of a build without frame data to the wiki's.
    index = []
    for sheet in built.chain:
        if sheet.lines is None:
            continue
        slug = build.slug(sheet.page.title)
        content, toc = render("\n".join(sheet.lines).strip())
        (out / f"{slug}.html").write_text(page(sheet, content, toc, built, top), encoding="utf-8")
        index.append({"title": sheet.title, "section": sheet.page.section, "url": slug, "about": sheet.page.about,
                      "headings": [{"text": t["name"], "id": t["id"]} for h in toc for t in [h, *h["children"]]]})
    home = build.Sheet(build.Page("", "Home", None, build.HOME_LEAD), "Guides", [],
                       after=built.chain[0].page.title if built.chain else None)
    cards = "".join(f"<h2>{html.escape(section)}</h2><div class=\"docs-cards\">" + "".join(
        f'<a class="docs-card" href="{html.escape(href(built, p.title))}"><b>{html.escape(p.title)}</b>'
        f'<span>{html.escape(p.about)}</span></a>' for p in found) + "</div>" for section, found in built.sections.items())
    (out / "index.html").write_text(page(home, cards, [], built, top), encoding="utf-8")
    (out / "assets/search.json").write_text(json.dumps(index, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    (out / ".nojekyll").write_text("")
    print(f"{len(index) + 1} pages in {out}")


if __name__ == "__main__":
    sys.exit(main())
