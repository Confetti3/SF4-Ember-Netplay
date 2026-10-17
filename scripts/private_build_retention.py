"""Verified archive indexing and reparse-safe paths for private publication."""
import hashlib
import json
from pathlib import Path
import re
import stat
import sys
import zipfile
from datetime import datetime

if sys.version_info < (3, 12):
    raise RuntimeError("Private packaging requires Python 3.12 or newer")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def assert_plain(path):
    """Check ancestors too: resolve() alone would conceal a junction."""
    path = Path(path).absolute()
    for item in (path, *path.parents):
        try:
            info = item.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise ValueError(f"Linked/reparse path is not supported: {item}")
    return path


def file_index(folder):
    folder = assert_plain(folder)
    if not folder.is_dir():
        raise ValueError(f"Missing package folder: {folder}")
    result = {}
    # Path.walk does not follow directory symlinks; reject each entry before use.
    for directory, dirs, files in folder.walk(follow_symlinks=False):
        for name in dirs + files:
            path = assert_plain(directory / name)
            if path.is_file():
                relative = path.relative_to(folder).as_posix()
                if relative.casefold() in result:
                    raise ValueError("Case-colliding package paths")
                result[relative.casefold()] = (relative, digest(path))
    return result


def archive_index(archive, checksum=None):
    """Validate one ZIP, then hash each member once into a case-insensitive index."""
    archive = assert_plain(archive)
    checksum = assert_plain(checksum or archive.with_name(archive.name + ".sha256"))
    if checksum.read_text(encoding="utf-8-sig").split()[0].lower() != digest(archive):
        raise ValueError("Archive checksum mismatch")
    with zipfile.ZipFile(archive) as package:
        receipts = [n for n in package.namelist() if n.count("/") == 1 and n.endswith("/build-provenance.json")]
        if len(receipts) != 1:
            raise ValueError("Expected one package root")
        prefix = receipts[0].split("/")[0] + "/"
        receipt = json.loads(package.read(receipts[0]).decode("utf-8-sig"))
        label = receipt.get("label", "")
        if not re.fullmatch(r"[a-zA-Z0-9._-]+", label) or prefix != "sf4-ember-netplay-" + label + "/":
            raise ValueError("Invalid private package label")
        if receipt.get("kind") != "private-test-build" or prefix + "PRIVATE_TEST_BUILD.txt" not in package.namelist():
            raise ValueError("Not a private package")
        if datetime.fromisoformat(receipt["createdUtc"].replace("Z", "+00:00")).tzinfo is None:
            raise ValueError("Receipt timestamp requires a timezone")
        index = {}
        for entry in package.infolist():
            if entry.is_dir():
                continue
            name = entry.filename.removeprefix(prefix)
            if not entry.filename.startswith(prefix) or not name or "\\" in name or ":" in name or any(p in ("", ".", "..") for p in name.split("/")):
                raise ValueError("Unsafe archive member")
            if stat.S_ISLNK(entry.external_attr >> 16) or name.casefold() in index:
                raise ValueError("Linked or duplicate archive member")
            with package.open(entry) as stream:
                index[name.casefold()] = (name, hashlib.file_digest(stream, "sha256").hexdigest())
    return receipt, index
