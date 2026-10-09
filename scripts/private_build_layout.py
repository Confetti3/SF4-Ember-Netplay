"""Transactional private publication; user changes are preserved as individual files."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import uuid
import warnings

from private_build_retention import archive_index, assert_plain, file_index

PLAY_COMMAND = '''@echo off
setlocal
if not exist "%~dp0Game\\Launcher.exe" (
    echo The current game folder is missing. Keep PLAY.cmd beside the Game folder.
    pause
    exit /b 1
)
start "" /D "%~dp0Game" "%~dp0Game\\Launcher.exe"
endlocal
'''

PROGRAMS = {".dll", ".exe", ".asi", ".sys", ".ocx", ".cpl", ".drv",
            ".cmd", ".bat", ".ps1", ".vbs", ".js", ".py", ".com", ".scr", ".msi"}


def preflight(folder):
    environment = {key:value for key,value in os.environ.items() if key.upper() != "PSMODULEPATH"}
    subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
                    "-File", str(folder / "preflight.ps1"), "-PackageDir", str(folder)], check=True, env=environment)


def _conflicts(candidate, relative):
    target = candidate / relative
    if target.exists():
        return True
    return any(p.exists() and not p.is_dir() for p in target.parents if p != candidate and candidate in p.parents)


def publish_private_build(dist, incoming, history, *, validate=preflight):
    dist, incoming, history = map(assert_plain, (dist, incoming, history))
    build = dist.parent / "build"
    if incoming.parent.parent != build or not incoming.parent.name.startswith("private-package-") or history != build / "private-packages":
        raise ValueError("Unexpected private package paths")
    receipt, expected = archive_index(incoming)
    candidate = incoming.with_suffix("")
    if file_index(candidate) != expected:
        raise ValueError("New playable folder does not match its verified ZIP")
    game, share, play = dist / "Game", dist / "Share with friend.zip", dist / "PLAY.cmd"
    checksum = history / "current.sha256"
    previous, previous_sum = history / "previous.zip", history / "previous.zip.sha256"
    for path in (game, share, play, checksum, previous, previous_sum):
        assert_plain(path)
    if game.exists() != share.exists() or share.exists() != checksum.exists():
        raise ValueError("Current Game, share ZIP and checksum must exist together")
    if previous.exists() != previous_sum.exists():
        raise ValueError("Rollback archive/checksum pair is incomplete")
    if previous.exists():
        archive_index(previous, previous_sum)
    if play.exists() and play.read_text(encoding="ascii") != PLAY_COMMAND:
        raise ValueError("PLAY.cmd has been customized; preserve it before publication")
    old = archive_index(share, checksum)[1] if share.exists() else {}
    obsolete = {m.replace("\\\\", "/").casefold() for m in re.findall(
        r'^SF4E_PACKAGE_OBSOLETE\("(.*)"\)', (candidate / "PackageInventory.inc").read_text(encoding="utf-8-sig"), re.M)}
    # Scan old Game once. Changed product files and unsafe/conflicting extras
    # are copied individually; unchanged files never cause a full-folder backup.
    current = file_index(game) if game.exists() else {}
    history.mkdir(parents=True, exist_ok=True)
    dist.mkdir(parents=True, exist_ok=True)
    transaction = history / (".publish-" + uuid.uuid4().hex)
    transaction.mkdir()
    preserved = transaction / "preserved"
    saved = history / ("preserved-" + receipt["label"] + "-" + uuid.uuid4().hex[:8])
    moved = []

    def move(source, target):
        if target.exists():
            raise ValueError(f"Publication would overwrite {target}")
        source.rename(target)
        moved.append((source, target))

    try:
        for key, (relative, checksum_value) in current.items():
            source = game / relative
            if key in old and checksum_value == old[key][1]:
                continue
            if key in old or key in obsolete or Path(relative).suffix.lower() in PROGRAMS or relative.lower().startswith("assets/") or _conflicts(candidate, relative):
                target = preserved / relative
            else:
                target = candidate / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        # This must happen AFTER carrying settings/logs forward and before any
        # current output is moved. Share stays the clean, validated candidate ZIP.
        validate(candidate)
        if not play.exists():
            (transaction / "PLAY.cmd").write_text(PLAY_COMMAND, encoding="ascii", newline="\n")
        if game.exists():
            move(game, transaction / "old-game")
        move(candidate, game)
        if share.exists():
            if previous.exists():
                move(previous, transaction / "old-previous.zip")
                move(previous_sum, transaction / "old-previous.sha256")
            move(share, previous)
            move(checksum, previous_sum)
        move(incoming, share)
        move(incoming.with_name(incoming.name + ".sha256"), checksum)
        if preserved.exists():
            move(preserved, saved)
        if not play.exists():
            move(transaction / "PLAY.cmd", play)
    except BaseException:
        rollback_errors = []
        for source, target in reversed(moved):
            try:
                target.rename(source)
            except OSError as error:
                rollback_errors.append(str(error))
        if rollback_errors:
            raise RuntimeError(f"Publication rollback needs recovery at {transaction}: {rollback_errors}")
        shutil.rmtree(transaction)
        raise
    # The last rename is the commit point. Cleanup cannot undo publication or
    # prevent the next run. A unique recovery directory retains any locked data.
    try:
        assert_plain(transaction)
        if transaction.parent != history:
            raise ValueError("Cleanup escaped history")
        shutil.rmtree(transaction)
    except OSError as error:
        warnings.warn(f"Published successfully; cleanup needs attention at {transaction}: {error}")
    if saved.exists():
        print(f"Preserved changed/conflicting files: {saved}")
    return game, share
