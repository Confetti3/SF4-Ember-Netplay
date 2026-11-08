#!/usr/bin/env python3
"""Check that the pictures and guides still describe the current interface.

docs/images/manifest.tsv says how each picture is made, and docs/images/captured.txt
names the commit scripts/capture-readme.ps1 last captured them from. The pictures
are stale when what they show changed after that commit: an error, so the wiki is
not published with them. GUIDES names the source each guide describes; a guide
last touched before a change there is stale too, a warning until the guides have
caught up with their sources. A guide that was read and found right is touched to
say so. The check reads the git history, so the checkout needs it.
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
    "uilab": ["src/ui", "locales/en.po", "assets"],
    "game": [],
    "deck": [],
    "still": [],
}
# The source behind each guide: the files whose change calls for reading the guide again.
GUIDES = {
    "README.md": ["src/launcher", "scripts/installer"],
    "docs/guides/STEAM_DECK.md": ["scripts/steamdeck"],
    "docs/guides/LINUX.md": ["scripts/wine-dxvk"],
    "docs/guides/USER_NETPLAY.md": ["src/ui/ApplicationShell.cxx", "src/ui/RoomPanel.cxx", "src/ui/ReplaysPanel.cxx",
                                    "src/ui/UpdatePrompt.cxx", "src/launcher/update"],
    "docs/guides/CUSTOM_ROOMS.md": ["src/ui/RoomPanelTables.cxx", "src/ui/RoomActions.cxx", "src/ui/RoomControls.cxx",
                                    "src/ui/PublicRoomsPanel.cxx"],
    "docs/guides/CONTROLLER_MENUS.md": ["src/ui/ControllersScreen.cxx", "src/ui/ButtonLayoutScreen.cxx",
                                        "src/ui/PadSetupScreen.cxx", "src/ui/ControllerNavigation.cxx"],
    "docs/guides/V2_MENUS.md": ["src/ui/V2.cxx", "src/ui/ApplicationShell__Settings.cxx", "src/ui/ApplicationShell__Screens.cxx"],
    "docs/guides/ACCESSIBILITY.md": ["src/ui/ApplicationShell__Accessibility.cxx"],
    "docs/guides/GAME_THEMES.md": ["src/sf4e/sf4e__GameThemes.cxx"],
    "docs/guides/DISCORD.md": ["src/discord"],
    "docs/guides/TRAINING_LAB.md": ["src/training", "src/ui/TrainingPanel.cxx", "src/ui/TrainingMeter.cxx"],
    "docs/guides/TOURNAMENT_MODE.md": ["src/ui/IdentityPanel.cxx", "src/ui/IdentityMatches.cxx"],
    "docs/guides/INTEGRATIONS.md": ["server"],
    "docs/guides/BLUMINT_QUICKSTART.md": ["server"],
    "docs/guides/TROUBLESHOOTING.md": ["src/launcher/launcher.cxx"],
    "docs/guides/INPUT_LAG.md": ["src/sf4e/sf4e__DirectInputFilter.cxx"],
    "docs/guides/SAVING_LOGS.md": ["src/ui/ProblemReportPanel.cxx"],
    "docs/guides/WINDOWS_DEFENDER.md": ["scripts/prepare-defender-submission.ps1"],
}


def read(src):
    with (src / MANIFEST).open(encoding="utf-8", newline="") as file:
        return list(csv.DictReader(file, delimiter="\t"))


def stale(rows, changed):
    """What changed after the capture, per kind of picture in the manifest; changed(path) is the commits since."""
    kinds = sorted({row["source"].split()[0] for row in rows})
    return [f"{kind} pictures: {path} changed after their capture, last in {commits[0]}"
            for kind in kinds for path in WATCHED[kind] if (commits := changed(path))]


def guides(last, changed):
    """The guides whose source changed since they were last touched; last(path) is a file's last commit."""
    return [f"{guide}: {path} changed after it, last in {commits[0]}" for guide, paths in GUIDES.items()
            for path in paths if (commits := changed(last(guide), path))]


def main():
    if not (build.ROOT / CAPTURED).is_file():
        sys.exit(f"no {CAPTURED}: the pictures were never captured by scripts/capture-readme.ps1")
    since = (build.ROOT / CAPTURED).read_text(encoding="utf-8").strip()
    if subprocess.run(["git", "-C", build.ROOT, "cat-file", "-e", f"{since}^{{commit}}"], capture_output=True).returncode:
        sys.exit(f"{CAPTURED} names {since[:9]}, which this checkout does not have; fetch the whole history")

    def log(*arguments):
        return subprocess.run(["git", "-C", build.ROOT, "log", "--format=%h", *arguments],
                              capture_output=True, text=True, check=True).stdout.split()

    changed = functools.cache(lambda since, path: log(f"{since}..HEAD", "--", path))

    @functools.cache
    def last(path):
        commits = log("-1", "--", path)
        if not commits:
            sys.exit(f"{path} is not committed")
        return commits[0]

    errors = stale(read(build.ROOT), functools.partial(changed, since))
    warnings = guides(last, changed)
    for line in errors:
        print("error:", line)
    for line in warnings:
        print("warning:", line, "(read the guide against it; the warning ends with the guide's next edit)")
    print(f"pictures captured at {since[:9]}, {len(errors)} reasons they are stale, "
          f"{len(warnings)} reasons to read a guide again")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
