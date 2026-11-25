"""Markdown to the site's HTML: the guides and the frame data pages.

The site's pages are served with a Content-Security-Policy that allows no inline style or script, and
the text comes from files many people edit. So what comes out holds only tags and attributes from the
lists below, column alignment is a class, and anything else a guide writes as HTML is shown as text.
"""
import html
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

from markdown_it import MarkdownIt

sys.path.append(str(Path(__file__).resolve().parents[1] / "wiki"))
from check import anchor  # noqa: E402  GitHub's id for a heading, which links between guides are written for

# The HTML a guide may write itself, and the attributes kept on it.
TAGS = {"a", "b", "br", "code", "details", "div", "em", "i", "img", "kbd", "p", "span", "strong", "sub", "summary", "sup"}
ATTRIBUTES = {"alt", "height", "href", "id", "src", "title", "width"}
TAG = re.compile(r"<(/?)([A-Za-z][\w-]*)((?:\s+[^<>]*?)?)\s*/?>")
ATTRIBUTE = re.compile(r"""([A-Za-z][\w-]*)\s*=\s*(?:"([^"]*)"|'([^']*)')""")
ALIGN = {"text-align:right": "num", "text-align:center": "mid", "text-align:left": ""}
# The inline tokens that are a text's words, and the ones that break its line.
WORDS = {"text", "code_inline"}
BREAKS = {"softbreak", "hardbreak"}
# Headings a page's own contents list names.
LISTED = {"h2", "h3"}


@dataclass
class Heading:
    level: int
    id: str
    text: str
    # The words under it, up to the next listed heading: what a search of the page reads.
    body: str = ""


@dataclass
class Rendered:
    html: str
    headings: list[Heading] = field(default_factory=list)
    # The first paragraph as plain text: what an index says of the page.
    summary: str = ""
    # The words before the first listed heading.
    opening: str = ""


def allowed(text):
    """`text` with the tags a guide may write kept, down to their kept attributes, and every other one shown."""
    def tag(match):
        closing, name, attributes = match.groups()
        if name.lower() not in TAGS:
            return html.escape(match.group(0))
        if closing:
            return f"</{name.lower()}>"
        kept = [(key.lower(), a if a is not None else b) for key, a, b in ATTRIBUTE.findall(attributes)]
        kept = [(key, value) for key, value in kept if key in ATTRIBUTES and not value.lower().startswith("javascript:")]
        return f"<{name.lower()}" + "".join(f' {key}="{html.escape(value)}"' for key, value in kept) + ">"

    return TAG.sub(tag, text)


def plain(inline):
    """An inline token's text without its markup, a line break in it as a space."""
    return "".join(" " if child.type in BREAKS else child.content
                   for child in inline.children or [] if child.type in WORDS | BREAKS)


class Prose:
    """One markdown reader for the whole site, with the rules above built in.

    `image(src)` gives an image's (width, height) when it is known, so the page keeps its place before it loads.
    """

    def __init__(self, image=lambda src: None):
        self.image = image
        self.md = MarkdownIt("commonmark", {"html": True}).enable(["table", "strikethrough"])
        rules = self.md.renderer.rules
        rules["html_inline"] = rules["html_block"] = lambda tokens, i, *_: allowed(tokens[i].content)
        rules["table_open"] = lambda *_: '<div class="table-wrap"><table>\n'
        rules["table_close"] = lambda *_: "</table></div>\n"
        rules["th_open"] = rules["td_open"] = self._cell
        rules["image"] = self._image
        rules["link_open"] = self._link

    def parse(self, text):
        return self.md.parse(text)

    def render(self, tokens):
        """The tokens as HTML, each heading with its id, and the headings a contents list names."""
        headings, used, summary, words = [], {}, "", [[]]
        for i, token in enumerate(tokens):
            if token.type == "heading_open":
                inline = tokens[i + 1]
                base = anchor(inline.content) or "section"
                count = used.get(base, 0)
                used[base] = count + 1
                token.attrSet("id", base if not count else f"{base}-{count}")
                if token.tag in LISTED:
                    headings.append(Heading(int(token.tag[1]), token.attrGet("id"), plain(inline)))
                    words.append([])
            elif token.type == "paragraph_open" and not summary and token.level == 0:
                summary = plain(tokens[i + 1])
            elif token.type == "inline" and tokens[i - 1].type != "heading_open":
                words[-1].append(plain(token))
        for heading, body in zip(headings, words[1:]):
            heading.body = " ".join(body)
        return Rendered(self.md.renderer.render(tokens, self.md.options, {}), headings, summary, " ".join(words[0]))

    def _cell(self, tokens, i, options, env):
        token = tokens[i]
        aligned = ALIGN.get(token.attrGet("style") or "", "")
        token.attrs.pop("style", None)
        if aligned:
            token.attrJoin("class", aligned)
        return self.md.renderer.renderToken(tokens, i, options, env)

    def _image(self, tokens, i, options, env):
        token = tokens[i]
        token.attrSet("alt", plain(token))
        token.attrSet("loading", "lazy")
        size = self.image(token.attrGet("src"))
        if size:
            token.attrSet("width", str(size[0]))
            token.attrSet("height", str(size[1]))
        return self.md.renderer.renderToken(tokens, i, options, env)

    def _link(self, tokens, i, options, env):
        token = tokens[i]
        if re.match(r"https?:", token.attrGet("href") or ""):
            token.attrSet("rel", "noopener")
        return self.md.renderer.renderToken(tokens, i, options, env)
