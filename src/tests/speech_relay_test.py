# The speech relay's voice packs (src/platform/speech_relay.py): which state a language's pack is in,
# replacing an older version whole and only once the new one is unpacked, refusing a pack without a
# manifest or with names that leave its folder, removing it so it stays removed until asked for,
# a download checked against its SHA-256, and the status file's lines.
import hashlib, importlib.util, json, os, pathlib, sys, tempfile, zipfile
sys.dont_write_bytecode = True

spec = importlib.util.spec_from_file_location("relay", sys.argv[1])
relay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(relay)
root = tempfile.mkdtemp()
relay.PACKS_DIR = os.path.join(root, "packs")
folder = os.path.join(relay.PACKS_DIR, "en")

def make_pack(version, clips, extra=None, manifest=True):
    path = os.path.join(root, "pack-%d.zip" % version)
    with zipfile.ZipFile(path, "w") as z:
        if manifest:
            z.writestr("manifest.json", json.dumps({"format": 1, "locale": "en", "version": version}))
        for name in clips:
            z.writestr(name + ".opus", b"opus")
        for name, data in (extra or {}).items():
            z.writestr(name, data)
    return path

relay.PACKS.clear()
assert relay.pack_state("en") == "none"
relay.PACKS["en"] = (1, "unused", 100, "unused")
assert relay.pack_state("en") == "missing"
relay.install_pack(make_pack(1, ["a", "b"]), folder)
assert relay.pack_state("en") == "current" and relay.pack_version(folder) == 1
# A newer listing makes the installed pack outdated; installing it replaces the old one whole.
relay.PACKS["en"] = (2, "unused", 100, "unused")
assert relay.pack_state("en") == "outdated"
relay.install_pack(make_pack(2, ["c"], {"../escape.opus": b"x", "sub/clip.opus": b"x"}), folder)
assert relay.pack_state("en") == "current" and sorted(os.listdir(folder)) == ["c.opus", "manifest.json"]
assert not os.path.exists(folder + ".old") and not os.path.exists(folder + ".part")
assert not os.path.exists(os.path.join(root, "escape.opus"))
# A pack without a manifest is refused, and the installed one stays.
try:
    relay.install_pack(make_pack(3, ["d"], manifest=False), folder)
    raise AssertionError("a pack without a manifest was installed")
except ValueError:
    pass
assert relay.pack_version(folder) == 2 and os.path.exists(os.path.join(folder, "c.opus"))
# Removed stays removed (no download) until the player asks for it again.
relay.remove_pack("en")
assert relay.pack_state("en") == "removed" and not os.path.exists(folder)
assert relay.pack("en") is None and "en" not in relay.downloading
relay.install_pack(make_pack(2, ["c"]), folder)
assert relay.pack_state("en") == "current"
# A download is checked against its SHA-256 and leaves nothing when it does not match.
source = make_pack(4, ["e"])
digest = hashlib.sha256(open(source, "rb").read()).hexdigest()
target, seen = os.path.join(root, "got.zip"), []
relay.fetch(pathlib.Path(source).as_uri(), target, 0, digest, seen.append)
assert open(target, "rb").read() == open(source, "rb").read() and sum(seen) == os.path.getsize(source)
try:
    relay.fetch(pathlib.Path(source).as_uri(), os.path.join(root, "bad.zip"), 0, "0" * 64, lambda n: None)
    raise AssertionError("a download with the wrong SHA-256 was kept")
except ValueError:
    pass
assert not os.path.exists(os.path.join(root, "bad.zip")) and not os.path.exists(os.path.join(root, "bad.zip.part"))
assert relay.status_text("downloading 42", "en") == "downloading 42\npack current 100\n"
assert relay.status_text("system", "ja") == "system\npack none 0\n"
print("speech relay pack checks passed")
