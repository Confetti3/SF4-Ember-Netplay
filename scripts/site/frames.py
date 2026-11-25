"""Frame data as pages of the site, /frame-data and /frame-data/<fighter>.

The pages are made on a lab PC with the game's files (docs/development/FRAME_DATA.md) and published to
the GitHub wiki as Frame-Data.md and one Frame-Data-<Fighter>.md each. This reads those pages as they
are, so the site and the wiki show the same numbers, and lays them out its own way: fighters are picked
by face, and a move's advantage is coloured by its sign.
"""
import html
import re
from dataclasses import dataclass
from pathlib import Path

from prose import Prose, plain
from shell import ROOT, Page

INDEX = "Frame-Data"
# A link from one frame data page to another, as the wiki writes it.
PAGE_LINK = re.compile(r"\]\((Frame-Data(?:-[\w-]+)?)((?:#[^)]*)?)\)")
FIGHTER_LINK = re.compile(r"\[([^\]]+)\]\(Frame-Data-([\w-]+)\)")
BANNER = re.compile(r"^!\[[^\]]*\]\(images/banner-[^)]*\)\s*$")
ADVANTAGE = {"On hit", "On block"}


@dataclass
class Fighter:
    name: str
    # The page's name on the wiki after "Frame-Data-", and its path on the site.
    wiki: str
    # The native fighter ID, which places the face in assets/faces.webp.
    id: int

    @property
    def url(self):
        return f"/frame-data/{self.wiki.lower()}"


def roster():
    """Fighter names in native ID order, as the game numbers them."""
    text = (ROOT / "src/common/FighterMetadata.inc").read_text(encoding="utf-8")
    return re.findall(r'^\s*\{"[A-Z0-9]+", "([^"]+)",', text, re.M)


def url_of(page):
    return "/frame-data" if page == INDEX else "/frame-data/" + page.removeprefix(INDEX + "-").lower()


def body_lines(path):
    """A wiki page's lines without the banner the wiki draws its title with, links pointed at the site."""
    lines = [line for line in path.read_text(encoding="utf-8").splitlines() if not BANNER.match(line)]
    return [PAGE_LINK.sub(lambda m: f"]({url_of(m.group(1))}{m.group(2)})", line) for line in lines]


def advantage_class(text):
    """How an On hit or On block cell reads: ahead, behind, even, or a knockdown."""
    match = re.match(r"([+-]?)(\d+|KD)", text)
    if not match:
        return ""
    sign, value = match.groups()
    if value == "KD":
        return "kd"
    return "even" if int(value) == 0 else "plus" if sign == "+" else "minus"


def mark_advantage(tokens):
    """Give each advantage cell of each table the class of its sign."""
    columns, column = [], 0
    for at, token in enumerate(tokens):
        if token.type == "thead_open":
            columns = []
        elif token.type == "th_open":
            columns.append(plain(tokens[at + 1]))
        elif token.type == "tr_open":
            column = 0
        elif token.type == "td_open":
            if column < len(columns) and columns[column] in ADVANTAGE:
                kind = advantage_class(plain(tokens[at + 1]))
                if kind:
                    token.attrJoin("class", kind)
            column += 1


def face(fighter, size=""):
    label = html.escape(fighter.name)
    return f'<span class="face f{fighter.id}{" " + size if size else ""}" role="img" aria-label="{label}"></span>'


class Site:
    """The frame data pages read from `folder`."""

    def __init__(self, folder):
        self.folder = Path(folder)
        index = self.folder / f"{INDEX}.md"
        if not index.is_file():
            raise FileNotFoundError(f"no {INDEX}.md in {self.folder}: pass the folder of frame data pages (a clone of the wiki has them)")
        ids = {name: at for at, name in enumerate(roster())}
        self.prose = Prose()
        self.fighters = [Fighter(name, wiki, ids[name]) for name, wiki in FIGHTER_LINK.findall(index.read_text(encoding="utf-8"))]
        if not self.fighters:
            raise ValueError(f"{index} links to no fighter's page")
        self.pages = [self._index(index)] + [self._fighter(at) for at in range(len(self.fighters))]

    def roster(self, current=None):
        """Every fighter's face and name as links to their pages, the current one marked."""
        here = lambda fighter: ' aria-current="page"' if fighter is current else ""
        return ('<nav class="fd-roster" aria-label="Fighters">'
                + "".join(f'<a href="{f.url}"{here(f)}>{face(f)}<span>{html.escape(f.name)}</span></a>' for f in self.fighters)
                + "</nav>")

    def _index(self, index):
        # The wiki opens with a line on the data and its list of fighters; here the page's head says
        # the first and the faces are the second, so the text starts at its first heading.
        lines = body_lines(index)
        lines = lines[next(at for at, line in enumerate(lines) if line.startswith("## ")):]
        body = self.prose.render(self.prose.parse("\n".join(lines)))
        content = f"""<div class="page-head"><div><h1>Frame data</h1>
<p class="lead">Startup, active, recovery and advantage for every move of every fighter in Ultra Street Fighter IV, measured by Ember and checked in the game.</p></div></div>
<label class="fd-find"><span class="offscreen">Find a fighter</span><input type="search" id="fd-find" placeholder="Find a fighter" autocomplete="off" spellcheck="false"></label>
{self.roster()}
<article class="prose fd-about">
{body.html}
</article>"""
        return Page(file="frame-data/index.html", title="USF4 frame data - Ember", content=content, nav="frame-data",
                    path="/frame-data", styles=["faces", "docs", "frames"], scripts=["frames"],
                    description="Frame data for all 44 fighters of Ultra Street Fighter IV: startup, active, recovery, "
                                "on hit and on block, measured by Ember and checked in the game.")

    def _fighter(self, at):
        fighter = self.fighters[at]
        tokens = self.prose.parse("\n".join(body_lines(self.folder / f"{INDEX}-{fighter.wiki}.md")))
        mark_advantage(tokens)
        body = self.prose.render(tokens)
        jump = "".join(f'<a href="#{h.id}">{html.escape(h.text)}</a>' for h in body.headings if h.level == 2)

        def turn(step, label):
            other = self.fighters[(at + step) % len(self.fighters)]
            return f'<a class="fd-turn" href="{other.url}">{label} {html.escape(other.name)}</a>'

        content = f"""<div class="page-head fd-head">{face(fighter, "large")}<div>
<p class="crumbs"><a href="/frame-data">Frame data</a></p>
<h1>{html.escape(fighter.name)}</h1></div>
<div class="fd-turns">{turn(-1, "Previous:")}{turn(1, "Next:")}</div></div>
<div class="fd-bar">
<label class="fd-find"><span class="offscreen">Filter moves</span><input type="search" id="fd-filter" placeholder="Filter moves, for example cr.MK or Hadoken" autocomplete="off" spellcheck="false"></label>
<nav class="fd-jump" aria-label="Sections">{jump}</nav>
</div>
<p class="note" id="fd-count" role="status" aria-live="polite"></p>
<article class="prose frames" id="frames">
{body.html}
</article>
<h2 class="fd-more">Other fighters</h2>
{self.roster(fighter)}"""
        return Page(file=f"frame-data/{fighter.wiki.lower()}.html", title=f"{fighter.name} frame data - Ember", content=content,
                    nav="frame-data", path=fighter.url, styles=["faces", "docs", "frames"], scripts=["frames"],
                    description=f"{fighter.name}'s frame data in Ultra Street Fighter IV: startup, active, recovery, "
                                "on hit and on block for every move.")


def faces_css(count):
    """Where each fighter's face lies in assets/faces.webp: one square each, side by side, in native ID order."""
    rules = [f".face {{ background-size: {count * 100}% 100%; }}"]
    rules += [f".face.f{at} {{ background-position: {at / (count - 1) * 100:.4f}% 0; }}" for at in range(count)]
    return "/* Made by scripts/site/make.py from src/common/FighterMetadata.inc. */\n" + "\n".join(rules) + "\n"
