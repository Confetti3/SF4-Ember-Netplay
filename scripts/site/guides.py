"""The player guides as pages of the site, /docs and /docs/<guide>.

They are the same guides the GitHub wiki is built from (scripts/wiki/build.py names them and their
sections), read from the checkout this runs in, so the site says what that release does. Pictures and
sounds a guide uses are served from /docs/files/, the large PNG screenshots as WebP.
"""
import html
import json
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from PIL import Image

from prose import Prose
from shell import Page

sys.path.append(str(Path(__file__).resolve().parents[1] / "wiki"))
import build as wiki  # noqa: E402

SOURCE_REPO = "https://github.com/Confetti3/SF4-Ember-Netplay"
# What a guide can link to that the site serves itself.
MEDIA = wiki.IMAGES | {".ogg"}
# Screenshots saved as PNG are several times the size they need to be on a page.
AS_WEBP = {".png", ".jpg", ".jpeg"}
OTHER_SECTION = "More guides"


@dataclass
class Guide:
    # The source file's path in the repository.
    source: str
    title: str
    section: str
    lines: list[str]

    @property
    def slug(self):
        return self.title.lower().replace(" ", "-")

    @property
    def url(self):
        return f"/docs/{self.slug}"


def collect(src):
    """The guides of the checkout at `src`, in the wiki's order, each without its own title line."""
    known = {path: (title, section) for path, title, section in wiki.PAGES}
    rank = {path: at for at, (path, _, _) in enumerate(wiki.PAGES)}
    paths = ["README.md"] + sorted(path.relative_to(src).as_posix() for path in (src / "docs/guides").glob("*.md"))
    guides = []
    for path in sorted((p for p in paths if p == "README.md" or wiki.is_guide(p)), key=lambda p: rank.get(p, len(rank))):
        lines = (src / path).read_text(encoding="utf-8").splitlines()
        if path == "README.md":
            # Only the README's install part is for players.
            start, end = (lines.index(heading) for heading in wiki.README_PART)
            lines = lines[start:end]
        elif lines and lines[0].startswith("# "):
            lines = lines[1:]
        title, section = known.get(path, (PurePosixPath(path).stem.replace("_", " ").title(), OTHER_SECTION))
        guides.append(Guide(path, title, section, lines))
    return guides


def sections(guides):
    """The guides by section, sections in the wiki's order."""
    order = [s for s in wiki.SECTIONS + [OTHER_SECTION] if any(g.section == s for g in guides)]
    return [(section, [g for g in guides if g.section == section]) for section in order]


def media_path(full):
    """Where the site serves the repository file `full`, under /docs/files/."""
    path = PurePosixPath(full.removeprefix("docs/"))
    return (path.with_suffix(".webp") if path.suffix.lower() in AS_WEBP else path).as_posix()


class Site:
    """The guide pages of one checkout: `pages`, the `media` they use and the `search` index.

    The index has an entry for each guide and each of its headings: the guide's name, the heading, its
    address and the words under it.
    """

    def __init__(self, src, frame_data):
        self.src = src
        self.frame_data = frame_data
        self.guides = collect(src)
        self.by_source = {guide.source: guide for guide in self.guides}
        # Served path under /docs/files/ to the file it is made from.
        self.media = {}
        self.search = []
        self.prose = Prose(self._image_size)
        rendered = [(guide, self._render(guide)) for guide in self.guides]
        self.pages = [self._index(rendered)] + [self._page(guide, body) for guide, body in rendered]

    def _target(self, full, anchor):
        if full in self.by_source:
            return self.by_source[full].url + anchor
        if PurePosixPath(full).suffix.lower() in MEDIA:
            if not (self.src / full).is_file():
                raise FileNotFoundError(f"a guide uses {full}, which is not in the checkout")
            self.media[media_path(full)] = self.src / full
            return "/docs/files/" + media_path(full)
        return f"{SOURCE_REPO}/blob/HEAD/{full}{anchor}"

    def _image_size(self, src):
        source = self.media.get((src or "").removeprefix("/docs/files/"))
        if not source or source.suffix.lower() not in wiki.IMAGES - {".svg"}:
            return None
        with Image.open(source) as image:
            return image.size

    def _render(self, guide):
        body = self.prose.render(self.prose.parse("\n".join(wiki.rewrite(guide.lines, guide.source, self._target))))
        self.search.append({"page": guide.title, "heading": "", "url": guide.url, "text": body.opening})
        self.search += [{"page": guide.title, "heading": h.text, "url": f"{guide.url}#{h.id}", "text": h.body}
                        for h in body.headings]
        return body

    def _nav(self, current=None):
        """The search box and the list of every guide, by section, with the current one marked."""
        out = ['<nav class="doc-nav" aria-label="Guides">',
               '<label class="doc-search"><span class="offscreen">Search the guides</span>'
               '<input type="search" id="doc-search" placeholder="Search the guides" autocomplete="off" spellcheck="false"></label>',
               '<ul class="doc-results" id="doc-results" hidden></ul>']
        for section, guides in sections(self.guides):
            out.append(f"<h2>{html.escape(section)}</h2>\n<ul>")
            here = lambda g: ' aria-current="page"' if g is current else ""
            out += [f'<li><a href="{g.url}"{here(g)}>{html.escape(g.title)}</a></li>' for g in guides]
            out.append("</ul>")
        if self.frame_data:
            out.append('<h2>Frame data</h2>\n<ul><li><a href="/frame-data">Every fighter\'s frame data</a></li></ul>')
        return "\n".join(out + ["</nav>"])

    def _page(self, guide, body):
        at = self.guides.index(guide)
        earlier, later = (self.guides[at - 1] if at else None), (self.guides[at + 1] if at + 1 < len(self.guides) else None)
        turn = lambda other, way: (f'<a class="doc-turn {way}" href="{other.url}"><span>{"Previous" if way == "back" else "Next"}</span>'
                                   f'{html.escape(other.title)}</a>') if other else "<span></span>"
        toc = "".join(f'<li class="l{h.level}"><a href="#{h.id}">{html.escape(h.text)}</a></li>' for h in body.headings)
        content = f"""<div class="doc">
{self._nav(guide)}
<article class="prose">
<p class="crumbs"><a href="/docs">Guides</a><span>{html.escape(guide.section)}</span></p>
<h1>{html.escape(guide.title)}</h1>
{body.html}
<div class="doc-turns">{turn(earlier, "back")}{turn(later, "on")}</div>
<p class="note doc-source">Something wrong or missing? <a href="{SOURCE_REPO}/blob/HEAD/{guide.source}" rel="noopener">This guide on GitHub</a>, or ask on <a href="https://discord.gg/uPNqF5A5uq" rel="noopener">Discord</a>.</p>
</article>
{f'<aside class="doc-toc" aria-label="On this page"><h2>On this page</h2><ul>{toc}</ul></aside>' if toc else ''}
</div>"""
        return Page(file=f"docs/{guide.slug}.html", title=f"{guide.title} - Ember guides", content=content,
                    description=body.summary[:200], path=guide.url, nav="docs", styles=["docs"], scripts=["docs"])

    def _index(self, rendered):
        summaries = {guide.source: body.summary for guide, body in rendered}
        out = ['<div class="page-head"><div><h1>Guides</h1>',
               '<p class="lead">Installing Ember, playing in rooms, training, tournaments, and what to do when something goes wrong.</p></div></div>',
               '<label class="doc-search wide"><span class="offscreen">Search the guides</span>'
               '<input type="search" id="doc-search" placeholder="Search the guides, for example: input delay" autocomplete="off" spellcheck="false"></label>',
               '<ul class="doc-results" id="doc-results" hidden></ul>']
        for section, guides in sections(self.guides):
            out.append(f'<section class="doc-section"><h2>{html.escape(section)}</h2><div class="doc-cards">')
            out += [f'<a class="doc-card" href="{g.url}"><strong>{html.escape(g.title)}</strong>'
                    f'<span>{html.escape(shorten(summaries[g.source]))}</span></a>' for g in guides]
            out.append("</div></section>")
        if self.frame_data:
            out.append('<section class="doc-section"><h2>Frame data</h2><div class="doc-cards">'
                       '<a class="doc-card" href="/frame-data"><strong>Frame data</strong>'
                       "<span>Startup, active, recovery and advantage for every move of every fighter.</span></a></div></section>")
        return Page(file="docs/index.html", title="Ember guides", content="\n".join(out), nav="docs", path="/docs",
                    description="Guides for playing Ultra Street Fighter IV online with Ember: installing it, rooms, "
                                "training, tournaments and troubleshooting.", styles=["docs"], scripts=["docs"])

    def search_json(self):
        return json.dumps(self.search, ensure_ascii=False, separators=(",", ":"))


def shorten(text, most=150):
    """`text` cut at a word to at most `most` characters."""
    if len(text) <= most:
        return text
    return text[:most].rsplit(" ", 1)[0].rstrip(",.;:") + "..."
