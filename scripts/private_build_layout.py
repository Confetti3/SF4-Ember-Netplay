"""Publish one obvious playable folder and one share ZIP, without copying binaries."""
import hashlib
import json
from pathlib import Path
import shutil
import zipfile

from private_build_retention import archive_prefix, digest, linked, pristine_folder, private_receipt

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


def publish_private_build(dist, incoming, history):
    """Incoming is fully validated before publication; previous ZIP is rollback."""
    dist, incoming, history = map(Path, (dist, incoming, history))
    if linked(dist) or linked(history) or linked(incoming):
        raise ValueError('Linked output directories are not supported')
    build = dist.parent.resolve() / 'build'
    staging = incoming.resolve().parent
    staged = staging.parent == build and staging.name.startswith('private-package-') and not linked(incoming.parent)
    if (staging != dist.resolve() and not staged) or history.resolve() != build / 'private-packages':
        raise ValueError('Unexpected private package paths')
    if private_receipt(incoming) is None:
        raise ValueError('New private package must verify before publication')
    candidate = incoming.with_suffix('')
    if not pristine_folder(candidate, incoming):
        raise ValueError('New playable folder does not match its verified ZIP')
    game, share = dist / 'Game', dist / 'Share with friend.zip'
    checksum = history / 'current.sha256'
    previous, previous_sum = history / 'previous.zip', history / 'previous.zip.sha256'
    play = dist / 'PLAY.cmd'
    for path in (game, share, checksum, previous, previous_sum, play):
        if linked(path):
            raise ValueError(f'Linked output: {path}')
    # Refuse an unknown layout instead of overwriting someone else's files.
    if game.exists() != share.exists():
        raise ValueError('Current Game folder and share ZIP must exist together')
    if share.exists() and private_receipt(share, checksum) is None:
        raise ValueError('Existing share ZIP/checksum must verify before rotation')
    if previous.exists() and private_receipt(previous) is None:
        raise ValueError('Existing rollback ZIP/checksum must verify before rotation')
    if play.exists() and play.read_text() != PLAY_COMMAND:
        raise ValueError('PLAY.cmd has been customized; preserve it before publication')
    history.mkdir(parents=True, exist_ok=True)
    held = history / 'previous-game'
    older, older_sum = history / 'older.zip', history / 'older.sha256'
    if any(path.exists() for path in (held, older, older_sum)):
        raise ValueError('A previous publication needs recovery: temporary backup exists')
    clean, saved = True, None
    if game.exists():
        if any(linked(path) for path in game.rglob('*')):
            raise ValueError('Current playable folder contains a link')
        clean = pristine_folder(game, share)
        with zipfile.ZipFile(share) as package:
            prefix = archive_prefix(package)
            label = json.loads(package.read(prefix + 'build-provenance.json'))['label']
            if not label or Path(label).name != label or '/' in label or '\\' in label or ':' in label:
                raise ValueError('Invalid previous build label')
            saved = history / ('preserved-' + label)
            if not clean and saved.exists():
                raise ValueError(f'Preserved player folder already exists: {saved}')
            packaged = {name[len(prefix):] for name in package.namelist() if name.startswith(prefix)}
        # Added player settings/logs follow the playable folder. Packaged files
        # are always from the new build; modified originals remain in saved.
        if not clean:
            copied_extras = True
            for source in game.rglob('*'):
                relative = source.relative_to(game)
                if source.is_file() and relative.as_posix() not in packaged:
                    target = candidate / relative
                    if not target.exists():
                        target.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(source, target)
                    if not target.is_file() or digest(source) != digest(target):
                        copied_extras = False
                elif source.is_dir():
                    target = candidate / relative
                    if target.exists() and not target.is_dir():
                        copied_extras = False
                    else:
                        target.mkdir(parents=True, exist_ok=True)
            # Ordinary logs/settings do not justify keeping another full game
            # folder after they have been carried forward without changes.
            if copied_extras:
                with zipfile.ZipFile(share) as package:
                    clean = True
                    for entry in package.infolist():
                        if entry.is_dir():
                            continue
                        original = game / entry.filename[len(prefix):]
                        if not original.is_file() or original.stat().st_size != entry.file_size:
                            clean = False
                            break
                        with package.open(entry) as stream:
                            if digest(original) != hashlib.file_digest(stream, 'sha256').hexdigest():
                                clean = False
                                break
    moved = []
    def move(source, target):
        if target.exists():
            raise ValueError(f'Publication would overwrite {target}')
        source.rename(target)
        moved.append((source, target))
    try:
        if game.exists():
            move(game, held)
        move(candidate, game)
        if share.exists():
            if previous.exists():
                move(previous, older)
                move(previous_sum, older_sum)
            move(share, previous)
            move(checksum, previous_sum)
        move(incoming, share)
        move(incoming.with_name(incoming.name + '.sha256'), checksum)
        play.write_text(PLAY_COMMAND, encoding='ascii')
    except (OSError, ValueError):
        # A locked folder or failed move leaves the previous playable build
        # and its archive available instead of half-publishing a new version.
        for source, target in reversed(moved):
            target.rename(source)
        raise
    for obsolete in (older, older_sum):
        if obsolete.exists():
            if obsolete.resolve().parent != history.resolve():
                raise ValueError('Old archive escaped history')
            obsolete.unlink()
    if held.exists():
        if clean:
            # Checked real path within the explicitly designated build folder.
            if held.resolve().parent != history.resolve():
                raise ValueError('Old playable folder escaped history')
            shutil.rmtree(held)
        else:
            held.rename(saved)
    return game, share, saved if not clean else None
