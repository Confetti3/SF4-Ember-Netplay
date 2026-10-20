#!/usr/bin/env python3
"""Publish a built wiki: replace the wiki repository's pages with build/wiki and push.

Commits only when something changed. --dry-run clones, copies and shows the
difference without committing. The token comes from GH_TOKEN, GITHUB_TOKEN or
`gh auth token`, as for the build.
"""
import argparse
import base64
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import build

BRANCH = "master"


def carry(old, new, prefix):
    """Copy the Frame Data section of an old sidebar or home page into the new one, after Training."""
    split = lambda text: re.split(rf"(?m)^(?={re.escape(prefix)})", text)
    frame, parts = [p for p in split(old) if p.startswith(prefix + "Frame Data")], split(new)
    at = next((i + 1 for i, p in enumerate(parts) if p.startswith(prefix + "Training")), len(parts))
    return "".join(parts[:at] + frame + parts[at:])


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=build.ROOT / "build/wiki", help="the built wiki")
    parser.add_argument("--repo", required=True, help="the repository whose wiki to publish, as owner/name")
    parser.add_argument("--dry-run", action="store_true", help="show what would change, do not commit or push")
    args = parser.parse_args()
    if not (args.out / "Home.md").is_file():
        sys.exit(f"no built wiki in {args.out}; run build.py first")
    auth = build.token()
    if not auth:
        sys.exit("no GitHub token: set GH_TOKEN or sign in with gh")
    sha = args.out.with_suffix(".sha").read_text(encoding="utf-8").strip()[:9]

    with tempfile.TemporaryDirectory() as folder:
        wiki = Path(folder)
        # The token goes in a header, as actions/checkout does, so it is never saved or printed with the URL.
        basic = base64.b64encode(f"x-access-token:{auth}".encode()).decode()
        header = ["-c", f"http.https://github.com/.extraheader=AUTHORIZATION: basic {basic}"]
        git = lambda *command: subprocess.run(["git", *header, "-C", wiki, *command], check=True)
        subprocess.run(["git", *header, "clone", "-q", "--depth", "1", "-b", BRANCH,
                        f"https://github.com/{args.repo}.wiki.git", wiki], check=True)
        # Frame data is generated on a lab PC, so a build without it (in CI) keeps the published pages.
        keep = set()
        if not (args.out / "Frame-Data.md").exists():
            keep = {*wiki.glob("Frame-Data*.md"), *wiki.glob("images/banner-Frame-Data*.svg")}
        old = {name: (wiki / name).read_text(encoding="utf-8") for name in ("_Sidebar.md", "Home.md")
               if keep and (wiki / name).exists()}
        for item in wiki.rglob("*"):
            if item.is_file() and ".git" not in item.relative_to(wiki).parts and item not in keep:
                item.unlink()
        shutil.copytree(args.out, wiki, dirs_exist_ok=True)
        for name, prefix in (("_Sidebar.md", "**"), ("Home.md", "## ")):
            if name in old:
                (wiki / name).write_text(carry(old[name], (wiki / name).read_text(encoding="utf-8"), prefix),
                                         encoding="utf-8")
        git("add", "-A")
        if subprocess.run(["git", "-C", wiki, "diff", "--cached", "--quiet"]).returncode == 0:
            print("the wiki is already up to date")
            return
        git("diff", "--cached", "--stat")
        if args.dry_run:
            return
        git("commit", "-q", "-m", f"Update from the docs at {sha}")
        git("push", "-q", "origin", f"HEAD:{BRANCH}")
        print("published")


if __name__ == "__main__":
    main()
