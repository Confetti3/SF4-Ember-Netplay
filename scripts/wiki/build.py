#!/usr/bin/env python3
"""Build the GitHub wiki from the player docs.

The wiki is generated, never edited by hand. Every page comes from a guide in
this checkout, or with --nightly-repo in the Nightly repository's nightly
branch. Open pull requests on the main repository with the wiki label that
change a guide add the paragraphs they add or change below the current text,
marked "Coming soon" until they merge.

The result goes to build/wiki.
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

ROOT = Path(__file__).resolve().parents[2]
UPSTREAM = "Confetti3/SF4-Ember-Netplay"
NIGHTLY_REPO = "Confetti3/SF4-Ember-Netplay-Nightly"
BRANCH = "nightly"
# Only open pull requests with this label get "Coming soon" notes, so the owner chooses what the wiki shows.
PR_LABEL = "wiki"

SECTIONS = ["Getting started", "Playing online", "Training", "Frame Data", "Tournaments", "Integrations", "Troubleshooting"]
# Source file, wiki page and sidebar section. A guide missing here still gets
# a page, under "More guides", and the build warns about it.
PAGES = [
    ("README.md", "Install", "Getting started"),
    ("docs/guides/SCOPE_AND_LIMITATIONS.md", "Scope and Limitations", "Getting started"),
    ("docs/guides/USER_NETPLAY.md", "Player Guide", "Playing online"),
    ("docs/guides/CUSTOM_ROOMS.md", "Custom Rooms", "Playing online"),
    ("docs/guides/CONTROLLER_MENUS.md", "Controllers and Menus", "Playing online"),
    ("docs/guides/DISCORD.md", "Discord", "Playing online"),
    ("docs/guides/TRAINING_LAB.md", "Training Lab", "Training"),
    ("docs/guides/INTEGRATIONS.md", "Integrations", "Integrations"),
    ("docs/guides/BLUMINT_QUICKSTART.md", "BluMint Quickstart", "Integrations"),
    ("docs/guides/TROUBLESHOOTING.md", "Troubleshooting", "Troubleshooting"),
    ("docs/guides/INPUT_LAG.md", "Input Lag", "Troubleshooting"),
    ("docs/guides/SAVING_LOGS.md", "Saving Logs", "Troubleshooting"),
    ("docs/guides/WINDOWS_DEFENDER.md", "Windows Defender", "Troubleshooting"),
]
# Only the README's install part is for players; the rest is for the repository page.
README_PART = ("## Get started", "## Status and documentation")
IMAGES = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".svg"}
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
            lines = block + [""] if at == 0 else [""] + block
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
    spans, start = [], None
    for i, (line, code) in enumerate(zip(lines + [""], fenced(lines) + [False])):
        if line.strip() or code:
            start = i if start is None else start
        elif start is not None:
            spans.append((start, i))
            start = None
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


def write_page(out, page, title, section, lines, colors):
    banner_name = f"images/banner-{slug(page)}.svg"
    (out / banner_name).write_text(banner(title, section, colors), encoding="utf-8")
    (out / f"{slug(page)}.md").write_text(f"![{title}]({banner_name})\n\n" + "\n".join(lines).strip() + "\n",
                                         encoding="utf-8")


def build(src, repo, out, auth, label=PR_LABEL, overlay_prs=True, framedata=None):
    report = []
    docs = {"README.md": Doc((src / "README.md").read_text(encoding="utf-8"))}
    for path in sorted((src / "docs/guides").glob("*.md")):
        if is_guide(path.relative_to(src).as_posix()):
            docs[path.relative_to(src).as_posix()] = Doc(path.read_text(encoding="utf-8"))
    included, pr_images = overlay(docs, auth, report, label) if overlay_prs else ({}, {})

    readme = docs["README.md"]
    start, end = (readme.lines.index(h) for h in README_PART)
    readme.lines, readme.owners = readme.lines[start:end], readme.owners[start:end]
    shown = frozenset().union(*(owner for doc in docs.values() for owner in doc.owners))
    included = {n: pr for n, pr in included.items() if n in shown}

    known = {path: (page, section) for path, page, section in PAGES}
    for path in docs:
        if path not in known:
            name = PurePosixPath(path).stem.replace("_", " ").title()
            known[path] = (name, "More guides")
            report.append(f"{path} has no entry in PAGES; it is listed under More guides")
    rank = {path: i for i, (path, _, _) in enumerate(PAGES)}
    docs = dict(sorted(docs.items(), key=lambda item: rank.get(item[0], len(rank))))
    pages = {path: known[path][0] for path in docs}

    if out.exists():
        shutil.rmtree(out)
    (out / "images").mkdir(parents=True)
    colors, images, sidebar = theme(), set(), {}
    for path, doc in docs.items():
        page, section = known[path]
        lines = doc.lines[1:] if doc.lines and doc.lines[0].startswith("# ") else doc.lines
        owners = doc.owners[len(doc.owners) - len(lines):]
        write_page(out, page, page, section, mark(rewrite(lines, path, pages, images, repo), owners), colors)
        sidebar.setdefault(section, []).append(page)

    # Frame data pages are generated elsewhere and come as they are: an index and a page per fighter.
    if framedata and framedata.is_dir():
        for path in sorted(framedata.glob("*.md")):
            lines = path.read_text(encoding="utf-8").splitlines()
            title, lines = (lines[0][2:], lines[1:]) if lines and lines[0].startswith("# ") \
                else (path.stem.replace("-", " "), lines)
            write_page(out, path.stem, title, "Frame Data", lines, colors)
            if path.stem == "Frame-Data":
                sidebar["Frame Data"] = ["Frame Data"]
    elif framedata:
        report.append(f"no frame data at {framedata}; the wiki is built without it")

    for image in sorted(images):
        target = out / "images" / image.removeprefix("docs/images/")
        target.parent.mkdir(parents=True, exist_ok=True)
        if (src / image).exists():
            shutil.copyfile(src / image, target)
        elif image in pr_images and (data := raw(UPSTREAM, pr_images[image], image, auth)):
            target.write_bytes(data)
        else:
            report.append(f"missing image {image}")

    order = [s for s in SECTIONS + ["More guides"] if s in sidebar]
    (out / "images/banner-Home.svg").write_text(banner("SF4 Ember Netplay", "Player guides", colors), encoding="utf-8")
    text = ["![SF4 Ember Netplay](images/banner-Home.svg)", "",
            "Guides for playing Ultra Street Fighter IV online with Ember: installing it, playing in rooms, "
            "training, running tournaments and fixing problems.", ""]
    listing = lambda heading: [line for s in order for line in [heading % s, ""]
                               + [f"- [{p}]({slug(p)})" for p in sidebar[s]] + [""]]
    (out / "Home.md").write_text("\n".join(text + listing("## %s")), encoding="utf-8")
    (out / "_Sidebar.md").write_text("\n".join(["**[Home](Home)**", ""] + listing("**%s**")), encoding="utf-8")

    sha = subprocess.run(["git", "-C", src, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    text = f"Generated from the docs at [`{sha[:9]}`](https://github.com/{repo}/tree/{sha}); edit the docs, not this page."
    if included:
        # Titles are other people's text: escape what Markdown would read as links or markup.
        titles = {n: re.sub(r"([\\[\]<>*_`])", r"\\\1", pr["title"]) for n, pr in included.items()}
        text += "\n\nComing soon, from open pull requests:\n\n" + "\n".join(
            f"- [#{n}](https://github.com/{UPSTREAM}/pull/{n}) {titles[n]}" for n in sorted(titles))
    (out / "_Footer.md").write_text(text + "\n", encoding="utf-8")
    # Beside the wiki, not in it: the docs commit publish.py names in its commit message.
    out.with_suffix(".sha").write_text(sha + "\n", encoding="utf-8")
    return sha, included, report


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--src", type=Path, default=ROOT, help="the checkout to take the docs from (default: this one)")
    source.add_argument("--nightly-repo", action="store_true", help="take the docs from the Nightly repository")
    parser.add_argument("--out", default=str(ROOT / "build/wiki"))
    parser.add_argument("--framedata", type=Path, help="a folder of generated frame data pages to include")
    prs = parser.add_mutually_exclusive_group()
    prs.add_argument("--no-prs", action="store_true", help="leave out open pull requests")
    prs.add_argument("--all-prs", action="store_true", help=f"include open pull requests without the {PR_LABEL} label")
    args = parser.parse_args()
    src, repo = (nightly_repo(), NIGHTLY_REPO) if args.nightly_repo else (args.src, UPSTREAM)
    sha, included, report = build(src, repo, Path(args.out), token(), None if args.all_prs else PR_LABEL,
                                  not args.no_prs, args.framedata)
    print(f"docs at {sha[:9]}, coming soon from {', '.join(f'#{n}' for n in sorted(included)) or 'no pull requests'}")
    print("\n".join(report))


if __name__ == "__main__":
    sys.exit(main())
