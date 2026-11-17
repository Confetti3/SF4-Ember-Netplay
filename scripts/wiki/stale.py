#!/usr/bin/env python3
"""Say which pictures in the docs are older than the interface they show.

docs/images/manifest.tsv says how each picture is made. A picture is stale when
a path its kind of capture shows (WATCHED) changed in a commit after the commit
it was captured from. capture-readme.ps1 captures every picture it can and
prints a `Pictures-from: <sha>` trailer for the commit that records the run, so
the newest commit with that trailer names the commit all of them show, those
whose bytes did not change too, and a rebase or cherry-pick keeps it. A picture
changed after that commit dates from its own last commit. A trailer naming a
commit this checkout lacks is reported as an unknown capture source, never as
fresh. The check needs the history, not the file contents.

It reports and never blocks: the lines go to the output (the workflow adds them
to the run's summary) and it exits 0, since an older picture beats no wiki. A
picture with no manifest row, or a row whose picture is gone, is a warning too.
Only a manifest it cannot read fails it. --due exits 1 when a picture is stale,
for the job that captures them again.
"""
import argparse
import csv
import functools
import subprocess
import sys
from pathlib import Path

import build

ROOT = build.ROOT
MANIFEST = Path("docs/images/manifest.tsv")
TRAILER = "Pictures-from"
# What each kind of capture shows, so a change there makes its pictures stale. The
# pictures taken by hand (game, deck, still) are not checked until their capture is scripted.
WATCHED = {
    "render": ("src/ui", "src/tests/ui_render_*", "locales/en.po", "assets"),
    "game": (),
    "deck": (),
    "still": (),
}


def read(src):
    """The manifest's rows; exits on a row without a picture or with a kind of capture WATCHED lacks."""
    with (src / MANIFEST).open(encoding="utf-8", newline="") as file:
        rows = list(csv.DictReader(file, delimiter="\t"))
    bad = [row for row in rows if not row.get("picture") or (row.get("source") or "?").split()[0] not in WATCHED]
    if bad:
        sys.exit(f"{MANIFEST}: rows without a picture or with an unknown source: {bad}")
    return rows


def unlisted(rows, present):
    """The pictures under docs/images without a manifest row, and the rows whose picture is gone."""
    listed = {row["picture"] for row in rows}
    return ([f"{picture} has no row in {MANIFEST}" for picture in sorted(present - listed)]
            + [f"{MANIFEST} lists {picture}, which is gone" for picture in sorted(listed - present)])


def stale(rows, root):
    """The stale pictures of the manifest rows, from the history of the checkout root."""
    found = []
    for row in rows:
        paths = WATCHED[row["source"].split()[0]]
        if not paths:
            continue
        commit, known = source(root, f"docs/images/{row['picture']}")
        if not known:
            found.append(f"{row['picture']} ({row['source']}): capture source unknown, its {TRAILER} commit is not here")
        elif commits := newer(root, commit, paths):
            found.append(f"{row['picture']} ({row['source']}): {', '.join(paths)} changed after it, "
                         f"{len(commits)} commits, last {commits[0]}")
    return found


def git(root, *arguments):
    return subprocess.run(["git", "-C", root, *arguments], capture_output=True, text=True, check=True).stdout


def holds(root, *arguments):
    return subprocess.run(["git", "-C", root, *arguments], capture_output=True).returncode == 0


@functools.cache
def capture(root):
    """The newest commit that records a capture, and the commit its Pictures-from trailer names; Nones for none."""
    log = git(root, "log", f"--grep=^{TRAILER}:", f"--format=%h %(trailers:key={TRAILER},valueonly,separator= )")
    for line in log.splitlines():
        commit, *named = line.split()
        if named:
            return commit, named[0]
    return None, None


@functools.cache
def source(root, path):
    """The commit a picture shows, and whether the checkout has it: the last capture's, unless the picture
    changed after it. (None, True) for a picture never committed."""
    commit = git(root, "log", "-1", "--format=%h", "--", path).strip() or None
    recorded, named = capture(root)
    if not commit or not recorded or not holds(root, "merge-base", "--is-ancestor", commit, recorded):
        return commit, True
    return named, holds(root, "cat-file", "-e", f"{named}^{{commit}}")


@functools.cache
def newer(root, commit, paths):
    """The commits after `commit` that change `paths`; all of them for a file never committed."""
    return git(root, "log", "--format=%h", f"{commit}..HEAD" if commit else "HEAD", "--", *paths).split()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--due", action="store_true", help="exit 1 when a picture is stale, for the capture job")
    args = parser.parse_args()
    rows, images = read(ROOT), ROOT / "docs/images"
    present = {p.relative_to(images).as_posix() for p in images.rglob("*") if p.suffix.lower() in build.IMAGES}
    found = stale(rows, ROOT)
    for line in unlisted(rows, present):
        print("warning:", line)
    for line in found:
        print("warning: stale picture", line)
    print(f"{len(found)} pictures older than what they show")
    return 1 if args.due and found else 0


if __name__ == "__main__":
    sys.exit(main())
