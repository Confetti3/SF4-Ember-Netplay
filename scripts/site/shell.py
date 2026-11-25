"""The shell every page of embernetplay.link shares: one layout, filled in for each page.

A page is its own content plus what the layout needs to know of it (`Page`). The layout
(server/ember-short/deploy/site/layout.html) holds the header, the navigation and the footer once, so
no page repeats them. The same layout serves two hosts (`Host`): the main site, whose links are paths
from its root, and the replay archive, which lives on its own host, links to the main site by its
full address and loads its files from beside the page.
"""
import html
import re
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "server/ember-short/deploy/site"
MARK = re.compile(r"\{\{(\w+)\}\}")


@dataclass(frozen=True)
class Host:
    """Where a page is served from, as its links see it."""
    # The main site's address before a path: empty on the main site itself.
    site: str
    # The address of the page's own files folder.
    assets: str
    # The replay archive's page.
    replays: str
    # Whether the host serves /favicon.ico (the archive's policy lets it load only its own files).
    favicon: bool = True
    # The one stylesheet the host serves, which the build joins from all its page needs; none on the
    # main site, where a page loads site.css and its own.
    bundle: str = ""


MAIN = Host(site="", assets="/assets", replays="https://archive.embernetplay.link/")
ARCHIVE = Host(site="https://embernetplay.link", assets="assets", replays="./", favicon=False, bundle="archive")


@dataclass
class Page:
    # The file it is written to, under the host's folder.
    file: str
    title: str
    content: str
    # What search results and link previews say; a page without one gets no preview tags.
    description: str = ""
    # The page's path on embernetplay.link, for its preview; none for a page on another host.
    path: str = ""
    # Which navigation entry is the current one (the layout's data-nav names).
    nav: str = ""
    # Stylesheets and scripts by name, in assets/: site.css is always first.
    styles: list[str] = field(default_factory=list)
    scripts: list[str] = field(default_factory=list)
    # Scripts that must run before the first paint.
    head_scripts: list[str] = field(default_factory=list)
    body: str = ""
    index: bool = True


def slug(name):
    """A name as the last part of a page's address: lower-case letters, digits and hyphens, which is
    all the server's location for the guides and the frame data serves."""
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


def front_matter(text):
    """A page source's leading comment of `key: value` lines, and the content after it."""
    match = re.match(r"<!--\n(.*?)\n-->\n", text, re.S)
    if not match:
        raise ValueError("a page source starts with a comment of key: value lines")
    values = dict(line.split(": ", 1) for line in match.group(1).splitlines())
    return values, text[match.end():]


def place(text, values):
    """`text` with each {{name}} mark replaced by its value; a mark with no value is a mistake in the source."""
    return MARK.sub(lambda match: values[match.group(1)], text)


def source_page(path, host, **parts):
    """A page written by hand (site/pages/*.html) as a `Page`.

    Its content may use the layout's {{site}}, {{assets}} and {{replays}}, and a mark for each of `parts`:
    content the build makes for it, such as the home page's list of fighters.
    """
    values, content = front_matter(path.read_text(encoding="utf-8"))
    content = place(content, {"site": host.site, "assets": host.assets, "replays": host.replays, **parts})
    names = lambda key: values.pop(key, "").split()
    page = Page(file=values.pop("file"), title=values.pop("title"), content=content,
                description=values.pop("description", ""), path=values.pop("path", ""), nav=values.pop("nav", ""),
                styles=names("styles"), scripts=names("scripts"), head_scripts=names("head-scripts"),
                body=values.pop("body", ""), index=values.pop("index", "yes") == "yes")
    if values:
        raise ValueError(f"{path.name}: unknown front matter {sorted(values)}")
    return page


def render(page, host, layout):
    """The whole page: the layout with this page's and this host's values in it."""
    asset = lambda name: f"{host.assets}/{name}"
    escape = lambda text: html.escape(text, quote=True)
    head = []
    if not page.index:
        head.append('<meta name="robots" content="noindex">')
    if page.description:
        head += [f'<meta name="description" content="{escape(page.description)}">',
                 '<meta property="og:type" content="website">',
                 '<meta property="og:site_name" content="SF4 Ember Netplay">',
                 f'<meta property="og:title" content="{escape(page.title)}">',
                 f'<meta property="og:description" content="{escape(page.description)}">']
        head += [f'<meta property="og:url" content="https://embernetplay.link{page.path}">'] if page.path else []
        head += ['<meta property="og:image" content="https://embernetplay.link/assets/og.jpg">',
                 '<meta property="og:image:width" content="1200">', '<meta property="og:image:height" content="630">',
                 '<meta name="twitter:card" content="summary_large_image">']
    if host.favicon:
        head.append('<link rel="icon" href="/favicon.ico" sizes="any">')
    head.append(f'<link rel="icon" href="{asset("ember-emblem.png")}" type="image/png">')
    head += [f'<link rel="preload" href="{asset(f"fonts/inter-latin-{weight}.woff2")}" as="font" type="font/woff2" crossorigin>'
             for weight in (400, 600)]
    head += [f'<link rel="stylesheet" href="{asset(name + ".css")}">' for name in stylesheets(page, host)]
    head += [f'<script src="{asset(name + ".js")}"></script>' for name in page.head_scripts]
    head += [f'<script src="{asset(name + ".js")}" defer></script>' for name in page.scripts]
    values = {"title": escape(page.title), "head": "\n".join(head), "body": page.body, "content": page.content.strip(),
              "site": host.site, "assets": host.assets, "replays": host.replays}

    text = place(layout, values)
    if page.nav:
        current = f'data-nav="{page.nav}"'
        if current not in text:
            raise ValueError(f"{page.file}: the layout has no navigation entry {page.nav}")
        text = text.replace(current, current + ' aria-current="page"')
    return text


def stylesheets(page, host):
    """The stylesheets a page loads: the host's bundle, or site.css and the page's own."""
    return [host.bundle] if host.bundle else ["site"] + page.styles
