#!/usr/bin/env python3
"""Publish a built wiki: replace the wiki repository's pages with build/wiki and push.

Commits only when something changed. --dry-run clones, copies and shows the
difference without committing. The token comes from GH_TOKEN, GITHUB_TOKEN or
`gh auth token`, as for the build.
"""
import argparse
import base64
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import build

BRANCH = "master"


def lost(old, new):
    """The pages and heading links an old wiki had that a new one lacks, since players have linked to them."""
    pages = sorted(set(old) - set(new))
    anchors = sorted(f"{page}#{name}" for page in set(old) & set(new)
                     for name in build.headings(old[page]) - build.headings(new[page]))
    return pages, anchors


def side(out):
    """The docs commit and the release tag (None for nightly docs) that build.py wrote beside the wiki in out."""
    words = out.with_suffix(".sha").read_text(encoding="utf-8").split()
    return words[0][:9], words[1] if len(words) > 1 else None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=build.ROOT / "build/wiki", help="the built wiki")
    parser.add_argument("--repo", required=True, help="the repository whose wiki to publish, as owner/name")
    parser.add_argument("--allow-removal", action="store_true", help="publish although pages that were linked to are gone")
    parser.add_argument("--dry-run", action="store_true", help="show what would change, do not commit or push")
    args = parser.parse_args()
    if not (args.out / "Home.md").is_file():
        sys.exit(f"no built wiki in {args.out}; run build.py first")
    auth = build.token()
    if not auth:
        sys.exit("no GitHub token: set GH_TOKEN or sign in with gh")
    # The docs commit, and the release tag build.py found at it: a release goes to a tag of the wiki and leaves
    # the pages alone, so it may lack what nightly has since gained.
    sha, tag = side(args.out)
    # A release's wiki is written once; a run made again for it finds its tag there and stops.
    if tag and f"refs/tags/{tag}" in build.wiki_tags(args.repo):
        print(f"the wiki already has {tag}")
        return

    with tempfile.TemporaryDirectory() as folder:
        wiki = Path(folder)
        # The token goes in a header, as actions/checkout does, and in the environment, so it is in no URL, no
        # saved config and no command line that a failing git command prints.
        basic = base64.b64encode(f"x-access-token:{auth}".encode()).decode()
        env = {**os.environ, "GIT_TERMINAL_PROMPT": "0", "GIT_CONFIG_COUNT": "1",
               "GIT_CONFIG_KEY_0": "http.https://github.com/.extraheader",
               "GIT_CONFIG_VALUE_0": f"AUTHORIZATION: basic {basic}"}
        git = lambda *command: subprocess.run(["git", "-C", wiki, *command], check=True, env=env)
        clone = subprocess.run(["git", "clone", "-q", "--depth", "1", "-b", BRANCH,
                                f"https://github.com/{args.repo}.wiki.git", wiki], capture_output=True, text=True, env=env)
        if clone.returncode:
            # A repository whose wiki was never created has no page a pull request could drop.
            if args.dry_run and "Repository not found" in clone.stderr:
                print(f"{args.repo} has no wiki yet; no published page can be lost")
                return
            sys.exit(f"could not clone the wiki of {args.repo}: {clone.stderr.strip()}")
        # Frame data is generated on a lab PC, so a build without it (in CI) keeps the published pages.
        keep = set()
        if not (args.out / "Frame-Data.md").exists():
            keep = {*wiki.glob("Frame-Data*.md"), *wiki.glob("images/banner-Frame-Data*.svg")}
        read = lambda: {path.stem: path.read_text(encoding="utf-8") for path in wiki.glob("[!_]*.md")}
        before = read()
        for item in wiki.rglob("*"):
            if item.is_file() and ".git" not in item.relative_to(wiki).parts and item not in keep:
                item.unlink()
        shutil.copytree(args.out, wiki, dirs_exist_ok=True)
        pages, anchors = lost(before, read())
        for line in anchors:
            print(f"warning: the link {line} no longer has a heading")
        if pages and not (args.allow_removal or tag):
            sys.exit("these pages would be removed, which breaks links to them: " + ", ".join(pages)
                     + "; keep them (a short page pointing to the new one) or pass --allow-removal")
        git("add", "-A")
        if subprocess.run(["git", "-C", wiki, "diff", "--cached", "--quiet"]).returncode == 0:
            print("the wiki is already up to date")
            return
        git("diff", "--cached", "--stat")
        if args.dry_run:
            return
        git("commit", "-q", "-m", f"{'Release ' + tag if tag else 'Update'} from the docs at {sha}")
        # A release is a tag only: the pages stay nightly's, and the next build lists the tag on Versions.
        git("push", "-q", "origin", f"HEAD:refs/tags/{tag}" if tag else f"HEAD:{BRANCH}")
        print("published")


if __name__ == "__main__":
    main()
