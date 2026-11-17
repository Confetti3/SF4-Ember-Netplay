#!/usr/bin/env python3
"""Build the GitHub wiki from the player docs.

The wiki is generated, never edited by hand. Every page comes from a guide in
this checkout, or with --nightly-repo in the Nightly repository's nightly
branch. Open pull requests on the main repository with the wiki label that
change a guide add the paragraphs they add or change below the current text,
marked "Coming soon" until they merge.

The result goes to build/wiki; check.py checks it and publish.py publishes it.
"""
import argparse
import base64
import difflib
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path, PurePosixPath
from typing import NamedTuple

ROOT = Path(__file__).resolve().parents[2]
UPSTREAM = "Confetti3/SF4-Ember-Netplay"
NIGHTLY_REPO = "Confetti3/SF4-Ember-Netplay-Nightly"
BRANCH = "nightly"
# Only open pull requests with this label get "Coming soon" notes, so the owner chooses what the wiki shows.
PR_LABEL = "wiki"

SECTIONS = ["Getting started", "Playing online", "Training", "Frame Data", "Tournaments", "Integrations", "Troubleshooting"]


class Page(NamedTuple):
    """A wiki page: its source file, its name, its sidebar section, what Home says it covers, and the
    files whose change calls for reading the guide again (stale.py)."""
    path: str
    title: str
    section: str
    about: str = ""
    sources: tuple = ()


class Sheet(NamedTuple):
    """A page as collect() reads it, before the wiki's banner, contents list and nav: what the docs site renders.
    lines is None for the Frame Data index of a build without frame data, which writes no page for it."""
    page: Page
    title: str
    lines: list
    before: str = None
    after: str = None


class Built(NamedTuple):
    """What collect() read and write_wiki() writes, and the docs site renders: the pages in reading order (chain)
    and those outside the sidebar (a fighter's frame data), the chain's pages by section, the pictures they show
    by their path under images/, the sidebar's links, the release tag, the release tags of the wiki (in wiki) and
    the footer."""
    sha: str
    tag: str
    included: dict
    report: list
    version: str
    chain: list
    others: list
    sections: dict
    images: dict
    links: list
    wiki: str
    tags: dict
    footer: str


# Every guide's page. A guide missing here still gets a page, under "More guides", and the build warns about it.
# The sidebar, Home and Previous/Next read the pages by section, then in this order.
PAGES = [
    Page("README.md", "Install", "Getting started", "Which download to take, installing Ember and your first launch",
         ("src/launcher", "scripts/installer")),
    Page("docs/guides/STEAM_DECK.md", "Steam Deck", "Getting started",
         "Installing through Proton, updating, uninstalling and where the logs are",
         ("scripts/steamdeck",)),
    Page("docs/guides/LINUX.md", "Linux", "Getting started",
         "Installing through Proton on a Linux PC that is not a Steam Deck",
         ("scripts/wine-dxvk",)),
    Page("docs/guides/SCOPE_AND_LIMITATIONS.md", "Scope and Limitations", "Getting started",
         "What is experimental, and what to expect"),
    Page("docs/guides/USER_NETPLAY.md", "Player Guide", "Playing online",
         "Hosting, joining, updates and day-to-day play",
         ("src/ui/ApplicationShell.cxx", "src/ui/RoomPanel.cxx", "src/ui/ReplaysPanel.cxx",
          "src/ui/UpdatePrompt.cxx", "src/launcher/update")),
    Page("docs/guides/CUSTOM_ROOMS.md", "Custom Rooms", "Playing online", "Tables, queues, spectators and room rules",
         ("src/ui/RoomPanelTables.cxx", "src/ui/RoomActions.cxx", "src/ui/RoomControls.cxx",
          "src/ui/PublicRoomsPanel.cxx")),
    Page("docs/guides/CONTROLLER_MENUS.md", "Controllers and Menus", "Playing online",
         "Moving around Ember with a pad, stick or keyboard",
         ("src/ui/ControllersScreen.cxx", "src/ui/ButtonLayoutScreen.cxx", "src/ui/PadSetupScreen.cxx",
          "src/ui/ControllerNavigation.cxx")),
    Page("docs/guides/V2_MENUS.md", "Ember Menus", "Playing online",
         "Home, Settings, the room board and how to move around them",
         ("src/ui/V2.cxx", "src/ui/ApplicationShell__Settings.cxx", "src/ui/ApplicationShell__Screens.cxx")),
    Page("docs/guides/ACCESSIBILITY.md", "Accessibility", "Playing online",
         "Speech, TV mode and the accessibility button",
         ("src/ui/ApplicationShell__Accessibility.cxx",)),
    Page("docs/guides/GAME_THEMES.md", "In-game Themes", "Playing online",
         "Changing how the menus and battle HUD look and sound",
         ("src/sf4e/sf4e__GameThemes.cxx",)),
    Page("docs/guides/DISCORD.md", "Discord", "Playing online", "Invitations, your Discord status and privacy",
         ("src/discord",)),
    Page("docs/guides/TRAINING_LAB.md", "Training Lab", "Training",
         "Frame meter, dummy recording and dummy replies, with a notation cheat sheet",
         ("src/training", "src/ui/TrainingPanel.cxx", "src/ui/TrainingMeter.cxx")),
    Page("docs/guides/TOURNAMENT_MODE.md", "Tournament Mode", "Tournaments",
         "Ember ID and playing organized tournaments",
         ("src/ui/IdentityPanel.cxx", "src/ui/IdentityMatches.cxx")),
    Page("docs/guides/TOURNAMENT_TEST.md", "Tournament Test", "Tournaments", "Trying a tournament on two PCs"),
    Page("docs/guides/INTEGRATIONS.md", "Integrations", "Integrations",
         "Ember's tournament bridge for platforms, organizers and bots",
         ("server/ember/bridge/src/routes",)),
    Page("docs/guides/BLUMINT_QUICKSTART.md", "BluMint Quickstart", "Integrations",
         "Getting the Ember bridge working with BluMint",
         ("server/ember/bridge/src/routes/blumint",)),
    Page("docs/guides/TROUBLESHOOTING.md", "Troubleshooting", "Troubleshooting",
         "Startup, connections and diagnostics",
         ("src/launcher/launcher.cxx",)),
    Page("docs/guides/INPUT_LAG.md", "Input Lag", "Troubleshooting", "Input delay and the game's own settings",
         ("src/sf4e/sf4e__DirectInputFilter.cxx",)),
    Page("docs/guides/SAVING_LOGS.md", "Saving Logs", "Troubleshooting",
         "Saving logs from both players for a bug report",
         ("src/ui/ProblemReportPanel.cxx",)),
    Page("docs/guides/WINDOWS_DEFENDER.md", "Windows Defender", "Troubleshooting", "When antivirus flags Ember",
         ("scripts/prepare-defender-submission.ps1",)),
]
# The frame data index, after Training. Its pages are generated on a lab PC, so a build without them still lists
# it in the reading order, and the publisher keeps the published pages (KEPT, which check.py accepts as links).
FRAME_DATA = Page("Frame-Data.md", "Frame Data", "Frame Data", "Move data for every fighter")
KEPT = {"Frame-Data"}
# The README's player part, from the downloads to its guides table; the table, community and licenses are
# for the repository page.
README_PART = ("## Which download do I need?", "## Guides")
HOME_LEAD = ("Guides for playing Ultra Street Fighter IV online with Ember: installing it, playing in rooms, "
             "training, running tournaments and fixing problems.")
IMAGES = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".svg"}
ITEM = re.compile(r"(\d+\.|[-*+]) ")
LINK_TEXT = re.compile(r"\[([^\]]*)\]\([^)]*\)")
LINK = re.compile(r'(\]\(|(?:src|href)=")([^)"\s]+)')


def is_guide(path):
    return re.fullmatch(r"docs/guides/(?!README\.md)[^/]+\.md", path) is not None


def fenced(lines):
    marks, inside = [], False
    for line in lines:
        fence = line.lstrip().lstrip("> ").startswith(("```", "~~~"))
        marks.append(inside or fence)
        inside ^= fence
    return marks


def slug(page):
    return page.replace(" ", "-")


class Doc:
    """A source file's lines, each with the open pull requests that added it."""

    def __init__(self, text):
        self.lines = text.splitlines()
        self.owners = [frozenset()] * len(self.lines)
        self.pending = []

    def add(self, ancestor, theirs, pr):
        """Queue the paragraphs a pull request adds or changes, each after the paragraph it belongs with.

        The current text stays as it is: a changed paragraph shows its new version below the old one.
        Paragraphs already in the current text are left out. Returns how many were queued.
        """
        current, ancestor, theirs = self.lines, ancestor.splitlines(), theirs.splitlines()
        # For each line of theirs, how many ancestor lines come up to it; then the same into the current text.
        before, changed = [], set()
        for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, ancestor, theirs, autojunk=False).get_opcodes():
            before += [i1 + k + 1 for k in range(j2 - j1)] if tag == "equal" else [i2] * (j2 - j1)
            changed |= set(range(j1, j2)) if tag != "equal" else set()
        into = difflib.SequenceMatcher(None, ancestor, current, autojunk=False).get_opcodes()

        def position(count):
            for tag, i1, i2, j1, j2 in into:
                if i1 < count <= i2 or count == i1 == 0:
                    count = j1 + (count - i1) if tag == "equal" else j2
                    break
            return next((end for start, end in blocks(current) if start < count < end), count)

        text, queued = "\n".join(current), 0
        for start, end in blocks(theirs):
            block = theirs[start:end]
            if changed.isdisjoint(range(start, end)) or "\n".join(block) in text:
                continue
            at = position(before[end - 1])
            # A stacked pull request repeats the one below it: the same paragraph gets one note naming both.
            same = next((p for p in self.pending if p[0] == at and p[2] == block), None)
            if same:
                same[1].add(pr)
            else:
                self.pending.append((at, {pr}, block))
            queued += 1
        return queued

    def apply(self):
        """Insert the queued paragraphs, in pull request order where several share a place."""
        for _, (at, prs, block) in sorted(enumerate(self.pending), key=lambda p: (-p[1][0], -min(p[1][1]), -p[0])):
            lines = ([""] if at else []) + block + ([""] if at < len(self.lines) and self.lines[at] else [])
            self.lines[at:at] = lines
            self.owners[at:at] = [frozenset(prs)] * len(lines)
        self.pending = []


def token():
    found = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if not found and shutil.which("gh"):
        found = subprocess.run(["gh", "auth", "token"], capture_output=True, text=True).stdout.strip()
    return found


def fetch(url, auth, missing_ok=False):
    request = urllib.request.Request(url, headers={"Authorization": f"Bearer {auth}"} if auth else {})
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            return response.read()
    except urllib.error.HTTPError as error:
        if missing_ok and error.code == 404:
            return None
        raise


def get(path, auth):
    return json.loads(fetch(f"https://api.github.com/{path}", auth))


def get_all(path, auth):
    items, page = [], 1
    while True:
        batch = get(f"{path}{'&' if '?' in path else '?'}per_page=100&page={page}", auth)
        items += batch
        if len(batch) < 100:
            return items
        page += 1


def raw(repo, sha, path, auth):
    return fetch(f"https://raw.githubusercontent.com/{repo}/{sha}/{path}", auth, missing_ok=True)


def overlay(docs, auth, report, label):
    """Add each open pull request's guide changes to the docs, each measured against the docs alone.

    Returns the pull requests that added something, and the images they add,
    by path, so a page can show an image before it is in the source.
    """
    included, images = {}, {}
    for pr in sorted(get_all(f"repos/{UPSTREAM}/pulls?state=open", auth), key=lambda p: p["number"]):
        if label and label not in {l["name"] for l in pr["labels"]}:
            continue
        number, head = pr["number"], pr["head"]["sha"]
        files = [f for f in get_all(f"repos/{UPSTREAM}/pulls/{number}/files", auth) if f["status"] != "removed"]
        for f in files:
            if PurePosixPath(f["filename"]).suffix.lower() in IMAGES:
                images[f["filename"]] = head
        changed = [f for f in files if f["filename"] in docs or is_guide(f["filename"])]
        if not changed:
            continue
        compare = get(f"repos/{UPSTREAM}/compare/{pr['base']['sha']}...{head}", auth)
        base = compare["merge_base_commit"]["sha"]
        for f in changed:
            path = f["filename"]
            theirs = raw(UPSTREAM, head, path, auth)
            if theirs is None:
                report.append(f"#{number}: {path} could not be read at {head[:9]}")
                continue
            theirs = theirs.decode()
            ancestor = (raw(UPSTREAM, base, f.get("previous_filename", path), auth) or b"").decode()
            if docs.setdefault(path, Doc("")).add(ancestor, theirs, number):
                included[number] = pr
        if number not in included:
            report.append(f"#{number} changes nothing that is not in the source yet")
    for doc in docs.values():
        doc.apply()
    return included, images


def blocks(lines):
    """Paragraphs, with each top-level list item and its indented lines as one of its own.

    So a pull request that changes one step of a list shows that step, not the whole list.
    """
    spans, start = [], None
    for i, (line, code) in enumerate(zip(lines + [""], fenced(lines) + [False])):
        text = bool(line.strip()) or code
        if start is not None and (not text or (not code and ITEM.match(line))):
            spans.append((start, i))
            start = None
        if text and start is None:
            joins = spans and line[:1].isspace() and ITEM.match(lines[spans[-1][0]])
            start = spans.pop()[0] if joins else i
    return spans


def pr_links(numbers):
    return ", ".join(f"[PR #{n}](https://github.com/{UPSTREAM}/pull/{n})" for n in sorted(numbers))


def mark(lines, owners):
    """Wrap the paragraphs open pull requests changed in a "Coming soon" note.

    A page that only exists in pull requests gets one note at the top instead.
    """
    spans = [(a, b, frozenset().union(*owners[a:b])) for a, b in blocks(lines)]
    if spans and all(own for _, _, own in spans):
        return ["> [!NOTE]", f"> **Coming soon ({pr_links(frozenset().union(*(o for _, _, o in spans)))})**: "
                "this page describes a change that is not in the Nightly build yet.", ""] + lines
    out, done = [], 0
    i = 0
    while i < len(spans):
        start, end, own = spans[i]
        if not own:
            i += 1
            continue
        while i + 1 < len(spans) and spans[i + 1][2] == own:
            i += 1
            end = spans[i][1]
        out += lines[done:start] + ["> [!NOTE]", f"> **Coming soon ({pr_links(own)})**", ">"]
        out += ["> " + line if line else ">" for line in lines[start:end]]
        done = end
        i += 1
    return out + lines[done:]


def rewrite(lines, path, pages, images, repo):
    def target(match):
        link = match.group(2)
        if re.match(r"[a-z][a-z0-9+.-]*:|#|/", link):
            return match.group(0)
        file, _, anchor = link.partition("#")
        full = os.path.normpath(PurePosixPath(path).parent / file).replace(os.sep, "/")
        anchor = "#" + anchor if anchor else ""
        if full in pages:
            new = slug(pages[full]) + anchor
        elif PurePosixPath(full).suffix.lower() in IMAGES:
            images.add(full)
            new = "images/" + full.removeprefix("docs/images/")
        else:
            new = f"https://github.com/{repo}/blob/{BRANCH}/{full}{anchor}"
        return match.group(1) + new

    return [line if code else LINK.sub(target, line) for line, code in zip(lines, fenced(lines))]


def theme():
    """The palette from the game's own theme, so the wiki never gets colours of its own."""
    text = (ROOT / "src/ui/Theme.cxx").read_text(encoding="utf-8")
    return {name: "#" + value for name, value in re.findall(r"(\w+) = 0x([0-9A-F]{6})\b", text)}


def links():
    """Ember's outbound links by id, from the table the interface reads, so the wiki never copies one."""
    text = (ROOT / "src/platform/Links.hxx").read_text(encoding="utf-8")
    return {name: "https://" + address for name, address in re.findall(r'\{"(\w+)", "([^"]+)"', text)}


def banner(title, section, colors):
    """The page header as an image: GitHub wikis drop styles, so the brand block is drawn.

    The font, emblem and artwork are the website's, embedded so the image needs nothing else.
    """
    site = ROOT / "server/ember-short/deploy/site/assets"
    data = lambda name: base64.b64encode((site / name).read_bytes()).decode()
    escape = lambda text: text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    ink, ivory, ember = colors["Ink"], colors["Ivory"], colors["Ember"]
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="200" viewBox="0 0 1200 200">
<title>{escape(title)}</title>
<defs><style>@font-face{{font-family:Inter;font-weight:600;src:url(data:font/woff2;base64,{data("fonts/inter-latin-600.woff2")}) format("woff2")}}
text{{font-family:Inter,sans-serif;font-weight:600}}</style>
<linearGradient id="tint" x2="1"><stop offset="0" stop-color="{ink}" stop-opacity=".96"/><stop offset=".42" stop-color="{ink}" stop-opacity=".88"/><stop offset="1" stop-color="{ink}" stop-opacity=".5"/></linearGradient>
<clipPath id="frame"><rect width="1200" height="200" rx="4"/></clipPath></defs>
<g clip-path="url(#frame)"><rect width="1200" height="200" fill="{ink}"/>
<image href="data:image/webp;base64,{data("ember-background.webp")}" width="1200" height="200" preserveAspectRatio="xMidYMid slice"/>
<rect width="1200" height="200" fill="url(#tint)"/></g>
<image href="data:image/png;base64,{data("ember-emblem.png")}" x="40" y="32" width="44" height="44"/>
<text x="98" y="56" font-size="26" letter-spacing="1" fill="{ivory}">EMBER</text>
<text x="98" y="74" font-size="12" letter-spacing="1.5" fill="{ember}">{escape(section.upper())}</text>
<text x="40" y="140" font-size="44" fill="{ivory}">{escape(title)}</text>
<rect x="40" y="156" width="72" height="3" fill="{ember}"/>
</svg>
"""


def nightly_repo():
    """A sparse clone of the Nightly repository's docs, which players' daily builds come from."""
    checkout = ROOT / "build/wiki-src"
    if checkout.exists():
        subprocess.run(["git", "-C", checkout, "pull", "-q", "--ff-only"], check=True)
    else:
        subprocess.run(["git", "clone", "-q", "--depth", "1", "--filter=blob:none", "--sparse", "-b", BRANCH,
                        f"https://github.com/{NIGHTLY_REPO}.git", checkout], check=True)
        subprocess.run(["git", "-C", checkout, "sparse-checkout", "set", "--no-cone", "/README.md", "/docs/"], check=True)
    return checkout


def anchor(heading):
    """GitHub's id for a heading, near enough to catch a renamed one."""
    text = LINK_TEXT.sub(r"\1", heading).strip().lower()
    return re.sub(r"[^\w\- ]", "", text).replace(" ", "-")


def headings(text):
    """The ids of a page's headings, for links to check; an `<a name>` kept for a heading that moved counts."""
    return {anchor(h) for h in re.findall(r"^(?:> )*#{1,6} (.+)$", text, re.M)} | set(re.findall(r'<a name="([^"]+)">', text))


def release_tag(src):
    """The release tag at HEAD, such as v1.5.4, or None: nightly builds are tagged v1.5.1-nightly20261102."""
    tag = subprocess.run(["git", "-C", src, "describe", "--tags", "--exact-match", "--match", "v*", "--exclude", "v*-*"],
                         capture_output=True, text=True)
    return tag.stdout.strip() if tag.returncode == 0 else None


def version(src, tag):
    """The Ember version the docs describe: CMakeLists' project version, "nightly" unless HEAD is the release `tag`."""
    number = re.search(r"^\s*VERSION\s+([\d.]+)", (src / "CMakeLists.txt").read_text(encoding="utf-8"), re.M)[1]
    return f"Ember {number}" + ("" if tag else " nightly")


def versions(repo, tags):
    """The Versions page: each release's Home as it was when it came out, newest first; tags maps a ref to its commit."""
    key = lambda ref: [int(n) for n in ref.removeprefix("refs/tags/v").split(".")]
    lines = ["Each release's guides as they were when it came out. Links inside an old page lead to the current pages.", ""]
    lines += [f"- [{ref.removeprefix('refs/tags/')}](https://github.com/{repo}/wiki/Home/{tags[ref]})"
              for ref in sorted(tags, key=key, reverse=True)]
    return "\n".join(lines) + "\n"


def wiki_tags(repo):
    """The release tags of the repository's wiki, each with its commit: each release's wiki is a tag there."""
    listed = subprocess.run(["git", "ls-remote", "--refs", "--tags", f"https://github.com/{repo}.wiki.git", "v*"],
                            capture_output=True, text=True, env={**os.environ, "GIT_TERMINAL_PROMPT": "0"})
    # A repository whose wiki was never created has no tags; any other failure would drop the Versions page.
    if listed.returncode and "Repository not found" not in listed.stderr:
        sys.exit(f"could not list the release tags of {repo}'s wiki: {listed.stderr.strip()}")
    found = listed.stdout.split()
    return dict(zip(found[1::2], found[::2]))


def contents(lines):
    """A collapsed "On this page" list of a page's sections, for a page long enough to want one."""
    titles = [LINK_TEXT.sub(r"\1", line[3:]).strip() for line, code in zip(lines, fenced(lines))
              if line.startswith("## ") and not code]
    if len(titles) < 4:
        return []
    items = [f"- [{title}](#{anchor(title)})" for title in titles]
    return ["<details>", "<summary><b>On this page</b></summary>", ""] + items + ["", "</details>", ""]


def write_page(out, sheet, colors):
    name = slug(sheet.page.title)
    banner_name = f"images/banner-{name}.svg"
    (out / banner_name).write_text(banner(sheet.title, sheet.page.section, colors), encoding="utf-8")
    nav = [f"[Previous: {sheet.before}]({slug(sheet.before)})" if sheet.before else None, "[Home](Home)",
           f"[Next: {sheet.after}]({slug(sheet.after)})" if sheet.after else None]
    body = [f"![{sheet.title}]({banner_name})", ""] + contents(sheet.lines) + ["\n".join(sheet.lines).strip(), "", "---",
                                                                             "", " | ".join(n for n in nav if n)]
    (out / f"{name}.md").write_text("\n".join(body) + "\n", encoding="utf-8")


def write_images(images, folder):
    for name, data in images.items():
        (folder / name).parent.mkdir(parents=True, exist_ok=True)
        (folder / name).write_bytes(data)


def collect(src, repo, auth, label=PR_LABEL, overlay_prs=True, framedata=None, tags=None, wiki=UPSTREAM):
    """Read the docs of the checkout src, and the pull requests' changes to them, into the pages to write. tags are
    the release tags of the wiki repository wiki, which main() reads, so this reaches the network only for the pull
    requests."""
    report, tag = [], release_tag(src)
    docs = {"README.md": Doc((src / "README.md").read_text(encoding="utf-8"))}
    for path in sorted((src / "docs/guides").glob("*.md")):
        if is_guide(path.relative_to(src).as_posix()):
            docs[path.relative_to(src).as_posix()] = Doc(path.read_text(encoding="utf-8"))
    # A release's pages are its docs as they are: no "Coming soon" from pull requests still open.
    included, pr_images = overlay(docs, auth, report, label) if overlay_prs and not tag else ({}, {})

    readme = docs["README.md"]
    missing = [h for h in README_PART if h not in readme.lines]
    if missing:
        sys.exit(f"README.md has no {' or '.join(missing)} heading; update README_PART in scripts/wiki/build.py")
    start, end = (readme.lines.index(h) for h in README_PART)
    readme.lines, readme.owners = readme.lines[start:end], readme.owners[start:end]
    shown = frozenset().union(*(owner for doc in docs.values() for owner in doc.owners))
    included = {n: pr for n, pr in included.items() if n in shown}

    known = {page.path: page for page in PAGES}
    for path in docs:
        if path not in known:
            known[path] = Page(path, PurePosixPath(path).stem.replace("_", " ").title(), "More guides")
            report.append(f"{path} has no entry in PAGES; it is listed under More guides")
    pages = {path: known[path].title for path in docs}

    images, chain, others = set(), [], []
    for path, doc in docs.items():
        lines = doc.lines[1:] if doc.lines and doc.lines[0].startswith("# ") else doc.lines
        owners = doc.owners[len(doc.owners) - len(lines):]
        chain.append(Sheet(known[path], known[path].title, mark(rewrite(lines, path, pages, images, repo), owners)))

    # Frame data pages are generated elsewhere and come as they are: an index and a page per fighter.
    index = Sheet(FRAME_DATA, FRAME_DATA.title, None)
    if framedata and framedata.is_dir():
        for path in sorted(framedata.glob("*.md")):
            lines = path.read_text(encoding="utf-8").splitlines()
            title, lines = (lines[0][2:], lines[1:]) if lines and lines[0].startswith("# ") \
                else (path.stem.replace("-", " "), lines)
            if path.stem == "Frame-Data":
                index = Sheet(FRAME_DATA, title, lines)
            else:
                # No source path: these pages are not in the repository.
                others.append(Sheet(Page("", path.stem.replace("-", " "), "Frame Data"), title, lines))
    elif framedata:
        report.append(f"no frame data at {framedata}; the wiki is built without it")
    chain.append(index)

    # One reading order: by section, then as PAGES lists them. The sidebar, Home, Previous/Next and the docs
    # site all follow it.
    order, rank = SECTIONS + ["More guides"], {page.path: i for i, page in enumerate(PAGES)}
    chain.sort(key=lambda sheet: (order.index(sheet.page.section), rank.get(sheet.page.path, len(rank))))
    titles = [None] + [sheet.page.title for sheet in chain] + [None]
    chain = [sheet._replace(before=titles[i], after=titles[i + 2]) for i, sheet in enumerate(chain)]
    sections = {}
    for sheet in chain:
        sections.setdefault(sheet.page.section, []).append(sheet.page)
        if not sheet.page.about:
            report.append(f"{sheet.page.path} has no about in PAGES; Home does not say what it covers")

    found = {}
    for image in sorted(images):
        if (src / image).exists():
            found[image.removeprefix("docs/images/")] = (src / image).read_bytes()
        elif image in pr_images and (data := raw(UPSTREAM, pr_images[image], image, auth)):
            found[image.removeprefix("docs/images/")] = data
        else:
            report.append(f"missing image {image}")

    outbound = [("Download", f"https://github.com/{UPSTREAM}/releases"), ("Discord", links()["discord"]),
                ("Report a bug", f"https://github.com/{UPSTREAM}/issues")]
    sha = subprocess.run(["git", "-C", src, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    named = version(src, tag)
    footer = (f"{named}. Generated from the docs at [`{sha[:9]}`](https://github.com/{repo}/tree/{sha}); "
              "edit the docs, not this page.")
    if included:
        # Titles are other people's text: escape what Markdown would read as links or markup.
        titles = {n: re.sub(r"([\\[\]<>*_`])", r"\\\1", pr["title"]) for n, pr in included.items()}
        footer += "\n\nComing soon, from open pull requests:\n\n" + "\n".join(
            f"- [#{n}](https://github.com/{UPSTREAM}/pull/{n}) {titles[n]}" for n in sorted(titles))
    return Built(sha=sha, tag=tag, included=included, report=report, version=named, chain=chain, others=others,
                 sections=sections, images=found, links=outbound, wiki=wiki, tags=tags or {}, footer=footer)


def write_wiki(built, out):
    """Write the wiki's pages into out, and beside it the docs commit and release tag publish.py reads."""
    if out.exists():
        shutil.rmtree(out)
    (out / "images").mkdir(parents=True)
    colors = theme()
    for sheet in built.chain + built.others:
        if sheet.lines is not None:
            write_page(out, sheet, colors)
    write_images(built.images, out / "images")

    (out / "images/banner-Home.svg").write_text(banner("SF4 Ember Netplay", "Player guides", colors), encoding="utf-8")
    text = ["![SF4 Ember Netplay](images/banner-Home.svg)", "", HOME_LEAD, ""]
    table = ("| Page | What it covers |", "| --- | --- |")

    def listing(heading, row, header=()):
        return [line for s, found in built.sections.items() for line in [heading % s, "", *header, *map(row, found), ""]]

    home = listing("## %s", lambda p: f"| [{p.title}]({slug(p.title)}) | {p.about} |", table)
    (out / "Home.md").write_text("\n".join(text + home), encoding="utf-8")
    sidebar_lines = ["**[Home](Home)**", ""] + listing("**%s**", lambda p: f"- [{p.title}]({slug(p.title)})") \
        + ["**Links**", "", *(f"- [{name}]({url})" for name, url in built.links)]
    # The tags are on the wiki publish.py pushes to, which a fork's run is its own.
    if built.tags:
        (out / "Versions.md").write_text(versions(built.wiki, built.tags), encoding="utf-8")
        sidebar_lines.append("- [Versions](Versions)")
    sidebar_lines.append("")
    (out / "_Sidebar.md").write_text("\n".join(sidebar_lines), encoding="utf-8")
    (out / "_Footer.md").write_text(built.footer + "\n", encoding="utf-8")
    # Beside the wiki, not in it: the docs commit publish.py names in its commit message, and the release
    # tag, if the docs are a release's, which publish.py pushes the wiki to instead of its pages.
    out.with_suffix(".sha").write_text(built.sha + "\n" + (built.tag + "\n" if built.tag else ""), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--src", type=Path, default=ROOT, help="the checkout to take the docs from (default: this one)")
    source.add_argument("--nightly-repo", action="store_true", help="take the docs from the Nightly repository")
    parser.add_argument("--out", default=str(ROOT / "build/wiki"))
    parser.add_argument("--framedata", type=Path, help="a folder of generated frame data pages to include")
    parser.add_argument("--wiki", default=UPSTREAM, help="the repository whose wiki lists the release tags, as owner/name")
    prs = parser.add_mutually_exclusive_group()
    prs.add_argument("--no-prs", action="store_true", help="leave out open pull requests")
    prs.add_argument("--all-prs", action="store_true", help=f"include open pull requests without the {PR_LABEL} label")
    args = parser.parse_args()
    src, repo = (nightly_repo(), NIGHTLY_REPO) if args.nightly_repo else (args.src, UPSTREAM)
    built = collect(src, repo, token(), None if args.all_prs else PR_LABEL, not args.no_prs, args.framedata,
                    wiki_tags(args.wiki), args.wiki)
    write_wiki(built, Path(args.out))
    print(f"docs at {built.sha[:9]}, coming soon from "
          f"{', '.join(f'#{n}' for n in sorted(built.included)) or 'no pull requests'}")
    print("\n".join(built.report))


if __name__ == "__main__":
    sys.exit(main())
