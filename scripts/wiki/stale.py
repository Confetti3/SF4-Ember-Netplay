#!/usr/bin/env python3
"""Check that the pictures in the docs still show the current interface.

docs/images/manifest.tsv says how each picture is made, and docs/images/captured.txt
names the commit scripts/capture-readme.ps1 last captured them from. The pictures
are stale when what they show changed after that commit: an error, so the wiki is
not published with them. The check reads the git history, so the checkout needs it.
"""
import csv
import functools
import subprocess
import sys
from pathlib import Path

import build

MANIFEST = Path("docs/images/manifest.tsv")
CAPTURED = Path("docs/images/captured.txt")
# What each kind of capture shows, so a change there makes the picture stale. The
# pictures taken by hand (game, deck, still) are not checked until their capture is scripted.
WATCHED = {
    "render": ["src/ui", "src/tests/ui_render_*", "locales/en.po", "assets"],
    "game": [],
    "deck": [],
    "still": [],
}


def read(src):
    with (src / MANIFEST).open(encoding="utf-8", newline="") as file:
        return list(csv.DictReader(file, delimiter="\t"))


def stale(rows, changed):
    """What changed after the capture, per kind of picture in the manifest; changed(path) is the commits since."""
    kinds = sorted({row["source"].split()[0] for row in rows})
    return [f"{kind} pictures: {path} changed after their capture, last in {commits[0]}"
            for kind in kinds for path in WATCHED[kind] if (commits := changed(path))]


def main():
    if not (build.ROOT / CAPTURED).is_file():
        sys.exit(f"no {CAPTURED}: the pictures were never captured by scripts/capture-readme.ps1")
    since = (build.ROOT / CAPTURED).read_text(encoding="utf-8").strip()
    if subprocess.run(["git", "-C", build.ROOT, "cat-file", "-e", f"{since}^{{commit}}"], capture_output=True).returncode:
        sys.exit(f"{CAPTURED} names {since[:9]}, which this checkout does not have; fetch the whole history")

    @functools.cache
    def changed(path):
        return subprocess.run(["git", "-C", build.ROOT, "log", "--format=%h", f"{since}..HEAD", "--", path],
                              capture_output=True, text=True, check=True).stdout.split()

    errors = stale(read(build.ROOT), changed)
    for line in errors:
        print("error:", line)
    print(f"pictures captured at {since[:9]}, {len(errors)} reasons they are stale")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
