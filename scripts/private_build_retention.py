"""Keep the current private package and one verified rollback archive."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import zipfile
from datetime import datetime


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def linked(path):
    return path.is_symlink() or getattr(path, 'is_junction', lambda: False)()


def archive_prefix(package):
    receipts = [name for name in package.namelist() if name.count('/') == 1 and name.endswith('/build-provenance.json')]
    if len(receipts) != 1:
        raise ValueError('Expected one package root')
    return receipts[0].split('/')[0] + '/'


def private_receipt(archive, checksum=None):
    checksum = checksum or archive.with_name(archive.name + '.sha256')
    if linked(archive) or linked(checksum):
        return None
    try:
        if checksum.read_text().split()[0].lower() != digest(archive):
            return None
        with zipfile.ZipFile(archive) as package:
            prefix = archive_prefix(package)
            if prefix + 'PRIVATE_TEST_BUILD.txt' not in package.namelist():
                return None
            receipt = json.loads(package.read(prefix + 'build-provenance.json'))
        if receipt.get('kind') != 'private-test-build' or prefix != 'sf4-ember-netplay-' + receipt['label'] + '/':
            return None
        created = datetime.fromisoformat(receipt['createdUtc'].replace('Z', '+00:00'))
        if created.tzinfo is None:
            return None
        return created
    except (OSError, ValueError, TypeError, AttributeError, KeyError, IndexError, zipfile.BadZipFile):
        return None


def pristine_folder(folder, archive):
    """Used folders with added/changed settings or logs must never be removed."""
    if linked(folder) or not folder.is_dir():
        return False
    try:
        children = list(folder.rglob('*'))
        if any(linked(path) for path in children):
            return False
        with zipfile.ZipFile(archive) as package:
            prefix = archive_prefix(package)
            expected = {}
            for entry in package.infolist():
                if entry.is_dir():
                    continue
                if not entry.filename.startswith(prefix):
                    return False
                name = entry.filename[len(prefix):]
                if not name or '\\' in name or ':' in name or any(part in ('', '.', '..') for part in name.split('/')):
                    return False
                if name in expected:
                    return False
                expected[name] = entry
            actual = {p.relative_to(folder).as_posix(): p for p in children if p.is_file()}
            if actual.keys() != expected.keys():
                return False
            expected_dirs = {parent for name in expected for parent in Path(name).parents if parent != Path('.')}
            if {p.relative_to(folder) for p in children if p.is_dir()} != expected_dirs:
                return False
            for name, path in actual.items():
                if path.stat().st_size != expected[name].file_size:
                    return False
                with package.open(expected[name]) as stream:
                    if digest(path) != hashlib.file_digest(stream, 'sha256').hexdigest():
                        return False
        return True
    except (OSError, ValueError, zipfile.BadZipFile):
        return False


def prune_private_builds(dist, current):
    # Resolve and constrain every deletion to a direct child of this output
    # directory. Unknown, public, corrupt, linked and modified output is kept.
    if linked(dist) or linked(current) or current.resolve().parent != dist.resolve():
        raise ValueError('Retention requires a current ZIP directly inside dist')
    if private_receipt(current) is None:
        raise ValueError('Current private ZIP/checksum must verify before cleanup')
    candidates = []
    for archive in dist.glob('sf4-ember-netplay-*.zip'):
        if not re.fullmatch(r'sf4-ember-netplay-[a-zA-Z0-9._-]+\.zip', archive.name):
            continue
        created = private_receipt(archive)
        if created is not None:
            candidates.append((created, archive))
    others = sorted((item for item in candidates if item[1] != current), reverse=True)
    keep = {current, *(item[1] for item in others[:1])}
    removed, preserved = [], []
    for _, archive in others:
        folder = archive.with_suffix('')
        if folder.exists():
            if folder.resolve().parent != dist.resolve() or not pristine_folder(folder, archive):
                preserved.append(folder.name)
                continue
            shutil.rmtree(folder)
            removed.append(folder.name)
        if archive not in keep:
            # These paths have already been verified as direct, unlinked children.
            if archive.resolve().parent != dist.resolve():
                raise ValueError('Archive escaped dist')
            archive.unlink()
            archive.with_name(archive.name + '.sha256').unlink()
            removed.append(archive.name)
    return removed, preserved
