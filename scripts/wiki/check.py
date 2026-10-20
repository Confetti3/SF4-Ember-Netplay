#!/usr/bin/env python3
"""Check a built wiki: every page link and image must exist.

Fails on a dead page link or a missing image. Warns about a link to a heading
that is not on its page, and about a section of numbered steps with no
screenshot, since every step should show what the player sees.
"""
import re
import sys
from pathlib import Path

from build import LINK, fenced


def anchor(heading):
    """GitHub's id for a heading, near enough to catch a renamed one."""
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", heading).strip().lower()
    return re.sub(r"[^\w\- ]", "", text).replace(" ", "-")


def headings(text):
    return {anchor(h) for h in re.findall(r"^(?:> )*#{1,6} (.+)$", text, re.M)}


def check(out):
    errors, warnings = [], []
    texts = {path.stem: path.read_text(encoding="utf-8") for path in out.glob("*.md")}
    for page, text in texts.items():
        lines = text.splitlines()
        body = "\n".join(line for line, code in zip(lines, fenced(lines)) if not code)
        for _, link in LINK.findall(body):
            if re.match(r"[a-z][a-z0-9+.-]*:", link):
                continue
            target, _, heading = link.partition("#")
            if target.startswith("images/"):
                if not (out / target).is_file():
                    errors.append(f"{page}: missing image {target}")
            elif target and target not in texts:
                errors.append(f"{page}: link to missing page {target}")
            elif heading and heading not in headings(texts[target or page]):
                warnings.append(f"{page}: no heading #{heading} on {target or page}")
        if page.startswith("_"):
            continue
        steps = re.sub(r"!\[[^\]]*\]\(images/banner-[^)]*\)", "", body)
        for section in re.split(r"^(?:> )*#{1,6} ", steps, flags=re.M):
            title, _, section = section.partition("\n")
            if re.search(r"^(?:> )*\s*\d+\. ", section, re.M) and "![" not in section and "<img" not in section:
                warnings.append(f"{page}: numbered steps without a screenshot under \"{title or 'the top of the page'}\"")
    return errors, warnings


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parents[2] / "build/wiki")
    errors, warnings = check(out)
    for line in warnings:
        print("warning:", line)
    for line in errors:
        print("error:", line)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
