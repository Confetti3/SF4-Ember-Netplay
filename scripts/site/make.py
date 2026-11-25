#!/usr/bin/env python3
"""Build embernetplay.link's pages.

  python3 scripts/site/make.py --frame-data FOLDER [--out build]
  python3 scripts/site/make.py --wiki-frame-data            # takes the frame data pages from the published wiki

Writes two folders under --out, each as its setup script installs it:

  site/          the main site: the home, rooms, rankings and link pages (server/ember-short/deploy/site),
                 the guides (README.md and docs/guides) and the frame data pages. It is the site/ folder
                 server/ember-short/deploy/setup.sh expects, and MANIFEST lists every file in it.
  archive-site/  the replay archive's page (server/ember/archive-site/site), which
                 server/ember/archive-site/deploy/setup.sh expects as its site/ folder.

Every page is the one layout (server/ember-short/deploy/site/layout.html) around its own content.
Needs markdown-it-py and Pillow. test_site.py checks the result; preview.py serves it.
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image

import frames
import guides
import shell
from shell import ARCHIVE, MAIN, ROOT, SOURCE

ARCHIVE_SOURCE = ROOT / "server/ember/archive-site/site"
WIKI = "https://github.com/Confetti3/SF4-Ember-Netplay.wiki.git"
# Screenshots the home page shows, by the name it serves them under (assets/shots/<name>.webp).
SHOTS = {
    "match": "docs/images/guides/user_netplay/match-hud.webp",
    "room": "docs/images/ember-room.png",
    "king": "docs/images/ember-king-of-the-hill.png",
    "training": "docs/images/guides/training_lab/meter.webp",
    "replays": "docs/images/guides/user_netplay/replays.webp",
}
SHOT_WIDTH = 960
# What a script or stylesheet of the archive page calls a file the build must not rename: the archive's
# setup.sh finds these names to stamp each with its file's hash.
ARCHIVE_STAMPED = {"index.html": ['href="assets/archive.css"', 'src="assets/archive.js"', "<title>Ember replay archive</title>"],
                   "assets/archive.css": ["url(faces.webp)", "url(flags.png)"],
                   "assets/archive.js": ["'assets/flags.json'"]}


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def save_webp(source, target, width=None):
    """`source` as a WebP at `target`, no wider than `width`."""
    target.parent.mkdir(parents=True, exist_ok=True)
    with Image.open(source) as image:
        image = image.convert("RGBA" if image.mode in ("RGBA", "LA", "P") else "RGB")
        if width and image.width > width:
            image = image.resize((width, round(image.height * width / image.width)), Image.Resampling.LANCZOS)
        image.save(target, quality=84, method=6)


def copy_media(source, target):
    if target.suffix != source.suffix:
        save_webp(source, target)
    else:
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)


def wiki_frame_data(out):
    """The published wiki's pages, which hold the frame data: a shallow clone, refreshed when it is there."""
    checkout = out / "wiki-published"
    if checkout.exists():
        subprocess.run(["git", "-C", checkout, "pull", "-q", "--ff-only"], check=True)
    else:
        subprocess.run(["git", "clone", "-q", "--depth", "1", WIKI, checkout], check=True)
    return checkout


def build_site(src, out, frame_folder, host):
    """The main site into `out`, as `host` links it."""
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(SOURCE / "assets", out / "assets")
    shutil.copyfile(SOURCE / "favicon.ico", out / "favicon.ico")
    for name, source in SHOTS.items():
        save_webp(src / source, out / f"assets/shots/{name}.webp", SHOT_WIDTH)
    write(out / "assets/faces.css", frames.faces_css(len(frames.roster())))

    frame_site = frames.Site(frame_folder)
    guide_site = guides.Site(src, frame_data=True)
    layout = (SOURCE / "layout.html").read_text(encoding="utf-8")
    pages = [shell.source_page(path, host, roster=frame_site.roster()) for path in sorted((SOURCE / "pages").glob("*.html"))]
    for page in pages + guide_site.pages + frame_site.pages:
        write(out / page.file, shell.render(page, host, layout))
    for served, source in guide_site.media.items():
        copy_media(source, out / "docs/files" / served)
    write(out / "docs/search.json", guide_site.search_json())

    files = sorted(path.relative_to(out).as_posix() for path in out.rglob("*") if path.is_file())
    write(out / "MANIFEST", "\n".join(files) + "\n")
    return files


def build_archive(out, host):
    """The replay archive's page into `out`: index.html and the one stylesheet and script its host serves."""
    if out.exists():
        shutil.rmtree(out)
    layout = (SOURCE / "layout.html").read_text(encoding="utf-8")
    page = shell.source_page(ARCHIVE_SOURCE / "page.html", host)
    write(out / page.file, shell.render(page, host, layout))
    # Its host serves only what its own setup installs, so the sheets the page needs go out as one:
    # the site's, the faces' places and the page's own.
    sheets = [(SOURCE / "assets/site.css").read_text(encoding="utf-8"), frames.faces_css(len(frames.roster())),
              (ARCHIVE_SOURCE / "assets/archive.css").read_text(encoding="utf-8")]
    write(out / "assets/archive.css", "\n".join(sheets))
    shutil.copyfile(ARCHIVE_SOURCE / "assets/archive.js", out / "assets/archive.js")
    for file, names in ARCHIVE_STAMPED.items():
        text = (out / file).read_text(encoding="utf-8")
        missing = [name for name in names if name not in text]
        if missing:
            raise ValueError(f"archive-site/{file} lacks {missing}, which server/ember/archive-site/deploy/setup.sh looks for")


def build(src, out, frame_folder, main=MAIN, archive=ARCHIVE):
    files = build_site(src, out / "site", frame_folder, main)
    build_archive(out / "archive-site", archive)
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--src", type=Path, default=ROOT, help="the checkout to take the guides from (default: this one)")
    parser.add_argument("--out", type=Path, default=ROOT / "build", help="where site/ and archive-site/ are written")
    data = parser.add_mutually_exclusive_group(required=True)
    data.add_argument("--frame-data", type=Path, help="a folder with the frame data pages (Frame-Data.md and one a fighter)")
    data.add_argument("--wiki-frame-data", action="store_true", help="take the frame data pages from the published wiki")
    args = parser.parse_args()
    folder = wiki_frame_data(args.out) if args.wiki_frame_data else args.frame_data
    files = build(args.src, args.out, folder)
    print(f"{len(files)} files in {args.out / 'site'}; the archive page in {args.out / 'archive-site'}")


if __name__ == "__main__":
    sys.exit(main())
