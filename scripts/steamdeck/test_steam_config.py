"""Checks steam_config.py on small localconfig.vdf and shortcuts.vdf fixtures."""
import os
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from steam_config import NAME, launch_option, main, read, set_launch_option, write

W = "/home/deck/.local/share/Steam/steamapps/compatdata/45760/pfx/drive_c/Ember/steam-launch.sh"
APPS = '"UserLocalConfigStore"\n{\n\t"Software"\n\t{\n\t\t"Valve"\n\t\t{\n\t\t\t"Steam"\n\t\t\t{\n\t\t\t\t"apps"\n\t\t\t\t{\n'
TAIL = "\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n"
GAME = lambda body: APPS + '\t\t\t\t\t"45760"\n\t\t\t\t\t{\n' + body + "\t\t\t\t\t}\n" + TAIL

# The player's own option is kept around the quoted wrapper, an earlier wrapper
# from any folder is replaced, and a rerun changes nothing.
Q = f'"{W}" %command%'
assert launch_option("", W) == Q
assert launch_option("PROTON_LOG=1 %command% -x", W) == f"PROTON_LOG=1 {Q} -x"
assert launch_option("-nosound", W) == f"{Q} -nosound"
assert launch_option(f"PROTON_LOG=1 {Q}", W) == f"PROTON_LOG=1 {Q}"
assert launch_option("/home/deck/Ember/steam-launch.sh %command%", W) == Q
assert launch_option('"/run/media/deck/SD card/Ember/steam-launch.sh" %command% -x', W) == f"{Q} -x"

# Played before: the option goes into the game's block once, even on a rerun.
once = set_launch_option(GAME('\t\t\t\t\t\t"LastPlayed"\t\t"1"\n'), W)
assert set_launch_option(once, W) == once and once.count("LaunchOptions") == 1
assert f'\t\t\t\t\t\t"LaunchOptions"\t\t"\\"{W}\\" %command%"' in once

# An older option with an escaped quote is wrapped, not lost.
old = set_launch_option(GAME('\t\t\t\t\t\t"LaunchOptions"\t\t"A=\\"x\\" %command%"\n'), W)
assert f'"LaunchOptions"\t\t"A=\\"x\\" \\"{W}\\" %command%"' in old and old.count("LaunchOptions") == 1

# Never played: a block for the game is added, and the braces still balance.
new = set_launch_option(APPS + '\t\t\t\t\t"7"\n\t\t\t\t\t{\n\t\t\t\t\t}\n' + TAIL, W)
assert '"45760"' in new and new.count("{") == new.count("}")
assert set_launch_option('"UserLocalConfigStore"\n{\n}\n', W) is None

# The binary format reads back byte for byte.
other = {"appid": -2147483647, "appname": "Other", "Exe": '"/x"', "tags": {}}
blob = write({"shortcuts": {"0": other}})
assert read(blob)[0] == {"shortcuts": {"0": other}} and write(read(blob)[0]) == blob

with tempfile.TemporaryDirectory() as tmp:
    config = os.path.join(tmp, "userdata/1/config")
    os.makedirs(config)
    os.makedirs(os.path.join(tmp, "art"))
    open(os.path.join(tmp, "art/cover.png"), "w").write("png")
    open(os.path.join(config, "localconfig.vdf"), "w").write(GAME(""))
    open(os.path.join(config, "shortcuts.vdf"), "wb").write(blob)
    # The tile: one Ember shortcut after a rerun, the other shortcut kept, the cover in the grid.
    main(tmp, "/home/deck/Ember", os.path.join(tmp, "art"))
    main(tmp, "/home/deck/Ember", os.path.join(tmp, "art"))
    # Moving Ember to another folder replaces the tile and its artwork.
    main(tmp, "/home/deck/Ember2", os.path.join(tmp, "art"))
    covers = [f for f in os.listdir(os.path.join(config, "grid")) if f.endswith("p.png")]
    assert len(covers) == 1, covers
    shortcuts = read(open(os.path.join(config, "shortcuts.vdf"), "rb").read())[0]["shortcuts"]
    assert sorted(s["appname"] for s in shortcuts.values()) == ["Other", NAME]
    assert any(f.endswith("p.png") for f in os.listdir(os.path.join(config, "grid")))
    # No account at all fails instead of reporting success.
    os.makedirs(os.path.join(tmp, "none/userdata"))
    try:
        main(os.path.join(tmp, "none"), "/home/deck/Ember", os.path.join(tmp, "art"))
        raise AssertionError("no account must fail")
    except SystemExit as e:
        assert e.code
print("steam_config: ok")
