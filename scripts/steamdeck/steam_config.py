"""Adds the SF4 Ember Netplay tile to Steam and points USF4's launch option at steam-launch.sh.

    python3 steam_config.py <Steam folder> <Ember folder> <artwork folder>

Steam must be closed: it rewrites both files from memory when it exits.
"""
import glob
import os
import re
import shutil
import sys
import zlib

NAME = "SF4 Ember Netplay"


def read(b, i=0):
    """Steam's binary VDF: 0 opens a map, 1 a string, 2 an int32, 8 ends a map."""
    d = {}
    while (t := b[i]) != 8:
        j = b.index(0, i + 1)
        k, i = b[i + 1:j].decode(errors="surrogateescape"), j + 1
        if t == 0:
            d[k], i = read(b, i)
        elif t == 1:
            j = b.index(0, i)
            d[k], i = b[i:j].decode(errors="surrogateescape"), j + 1
        elif t == 2:
            d[k], i = int.from_bytes(b[i:i + 4], "little", signed=True), i + 4
        else:
            raise ValueError(f"unknown VDF type {t}")
    return d, i + 1


def write(d):
    out = b""
    for k, v in d.items():
        k = k.encode(errors="surrogateescape") + b"\0"
        if isinstance(v, dict):
            out += b"\0" + k + write(v)
        elif isinstance(v, int):
            out += b"\2" + k + v.to_bytes(4, "little", signed=True)
        else:
            out += b"\1" + k + v.encode(errors="surrogateescape") + b"\0"
    return out + b"\x08"


def launch_option(old, wrapper):
    """The player's own option with the wrapper put in front of the game command."""
    if wrapper in old:
        return old
    if "%command%" in old:
        return old.replace("%command%", f"{wrapper} %command%", 1)
    return f"{wrapper} %command% {old}".strip()


def set_launch_option(text, wrapper):
    """USF4's launch option in the text of localconfig.vdf, or None without an apps list."""
    apps = re.search(r'\n(\t+)"apps"\n\t+\{', text)
    if not apps:
        return None
    ind = apps.group(1) + "\t"
    escape = lambda s: s.replace("\\", "\\\\").replace('"', '\\"')
    unescape = lambda s: re.sub(r"\\(.)", r"\1", s)
    game = re.compile(r'\n' + ind + r'"45760"\n' + ind + r'\{').search(text, apps.end())
    if not game:
        line = f'{ind}\t"LaunchOptions"\t\t"{escape(launch_option("", wrapper))}"'
        return text[:apps.end()] + f'\n{ind}"45760"\n{ind}{{\n{line}\n{ind}}}' + text[apps.end():]
    end = text.index("\n" + ind + "}", game.end())
    body = text[game.end():end]
    old = re.search(r'\n\t+"LaunchOptions"\t\t"((?:[^"\\\n]|\\.)*)"', body)
    line = f'{ind}\t"LaunchOptions"\t\t"{escape(launch_option(unescape(old.group(1)) if old else "", wrapper))}"'
    body = body[:old.start()] + "\n" + line + body[old.end():] if old else "\n" + line + body
    return text[:game.end()] + body + text[end:]


def add_shortcut(shortcuts, dest, grid):
    """The tile as a shortcuts.vdf entry, replacing an earlier one; returns its app ID."""
    exe = f'"{dest}/ember-play.sh"'
    appid = zlib.crc32((exe + NAME).encode()) | 0x80000000  # how Steam numbers a shortcut
    kept = [s for s in shortcuts.values() if NAME not in (s.get("appname"), s.get("AppName"))]
    kept.append({
        "appid": appid - 2**32, "appname": NAME, "Exe": exe, "StartDir": f'"{dest}/"',
        "icon": os.path.join(grid, f"{appid}_icon.png"), "ShortcutPath": "", "LaunchOptions": "",
        "IsHidden": 0, "AllowDesktopConfig": 1, "AllowOverlay": 1, "OpenVR": 0, "Devkit": 0,
        "DevkitGameID": "", "DevkitOverrideAppID": 0, "LastPlayTime": 0, "FlatpakAppID": "", "tags": {},
    })
    return {str(i): s for i, s in enumerate(kept)}, appid


def main(steam, dest, art):
    done = 0
    for config in glob.glob(os.path.join(steam, "userdata/*/config")):
        f = os.path.join(config, "localconfig.vdf")
        text = open(f, encoding="utf-8").read() if os.path.exists(f) else ""
        text = set_launch_option(text, f"{dest}/steam-launch.sh")
        if text is None:
            print("No apps list in", f)
            continue
        open(f, "w", encoding="utf-8").write(text)
        grid = os.path.join(config, "grid")
        os.makedirs(grid, exist_ok=True)
        f = os.path.join(config, "shortcuts.vdf")
        shortcuts = read(open(f, "rb").read())[0]["shortcuts"] if os.path.exists(f) else {}
        shortcuts, appid = add_shortcut(shortcuts, dest, grid)
        open(f, "wb").write(write({"shortcuts": shortcuts}))
        for src, suffix in (("cover", "p"), ("wide", ""), ("hero", "_hero"), ("logo", "_logo"), ("icon", "_icon")):
            if os.path.exists(os.path.join(art, src + ".png")):
                shutil.copyfile(os.path.join(art, src + ".png"), os.path.join(grid, f"{appid}{suffix}.png"))
        done += 1
    if not done:
        sys.exit("Could not add Ember to Steam: no Steam account found. Sign in to Steam once, then run this again.")


if __name__ == "__main__":
    main(*sys.argv[1:])
