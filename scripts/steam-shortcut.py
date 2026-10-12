#!/usr/bin/env python3
"""Adds or updates a non-Steam shortcut for Ember with artwork and a Proton
mapping, the way steamtinkerlaunch's addnonsteamgame does, without it.
Steam must be closed. Standard library only, so it runs on a Steam Deck.

  steam-shortcut.py --steam ~/.steam/steam --exe /path/Ember/Launcher.exe \
      [--name Ember] [--launch-options '...'] [--compat proton_experimental] [--art image.png]
"""
import argparse, glob, os, re, shutil, struct, subprocess, sys, zlib

# Binary VDF: 0x00 map, 0x01 string, 0x02 int32, 0x08 end of map.
def read_vdf(data, at=0):
    out = {}
    while data[at] != 0x08:
        kind = data[at]; end = data.index(b"\0", at + 1); key = data[at + 1:end].decode("utf-8"); at = end + 1
        if kind == 0x00: out[key], at = read_vdf(data, at)
        elif kind == 0x01: end = data.index(b"\0", at); out[key] = data[at:end].decode("utf-8", "replace"); at = end + 1
        elif kind == 0x02: out[key] = struct.unpack_from("<i", data, at)[0]; at += 4
        else: raise ValueError("unknown vdf type %d" % kind)
    return out, at + 1

def write_vdf(node):
    out = bytearray()
    for key, value in node.items():
        k = key.encode("utf-8") + b"\0"
        if isinstance(value, dict): out += b"\x00" + k + write_vdf(value)
        elif isinstance(value, int): out += b"\x02" + k + struct.pack("<i", value)
        else: out += b"\x01" + k + value.encode("utf-8") + b"\0"
    return bytes(out) + b"\x08"

def shortcut_appid(exe, name):
    return (zlib.crc32((exe + name).encode("utf-8")) | 0x80000000) & 0xFFFFFFFF

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--steam", required=True); p.add_argument("--exe", required=True)
    p.add_argument("--name", default="Ember"); p.add_argument("--launch-options", default="")
    p.add_argument("--compat", default="proton_experimental"); p.add_argument("--art")
    a = p.parse_args()
    if subprocess.run(["pgrep", "-x", "steam"], capture_output=True).returncode == 0:
        sys.exit("Steam is running; close it first, it overwrites shortcuts.vdf on exit")
    users = [d for d in glob.glob(os.path.join(a.steam, "userdata", "*")) if os.path.basename(d).isdigit() and os.path.basename(d) != "0" and os.path.isdir(os.path.join(d, "config"))]
    if not users: sys.exit("no Steam user under %s/userdata; log in to Steam once" % a.steam)
    exe = '"%s"' % os.path.abspath(a.exe); start = '"%s"' % os.path.dirname(os.path.abspath(a.exe))
    appid = shortcut_appid(exe, a.name)
    for user in users:
        path = os.path.join(user, "config", "shortcuts.vdf")
        root = read_vdf(open(path, "rb").read())[0] if os.path.exists(path) else {"shortcuts": {}}
        entries = root.setdefault("shortcuts", {})
        slot = next((k for k, e in entries.items() if e.get("AppName") == a.name or e.get("appname") == a.name), str(len(entries)))
        entry = entries.get(slot, {})
        entry.update({"appid": struct.unpack("<i", struct.pack("<I", appid))[0], "AppName": a.name, "Exe": exe, "StartDir": start,
            "icon": os.path.abspath(a.art) if a.art else entry.get("icon", ""), "ShortcutPath": "", "LaunchOptions": a.launch_options,
            "IsHidden": 0, "AllowDesktopConfig": 1, "AllowOverlay": 1, "OpenVR": 0, "Devkit": 0, "DevkitGameID": "",
            "DevkitOverrideAppID": 0, "LastPlayTime": entry.get("LastPlayTime", 0), "FlatpakAppID": "", "tags": entry.get("tags", {})})
        entries[slot] = entry
        if os.path.exists(path): shutil.copy(path, path + ".bak")
        open(path, "wb").write(write_vdf(root))
        if a.art:
            grid = os.path.join(user, "config", "grid"); os.makedirs(grid, exist_ok=True)
            ext = os.path.splitext(a.art)[1]
            for suffix in ("", "p", "_hero"): shutil.copy(a.art, os.path.join(grid, "%d%s%s" % (appid, suffix, ext)))
        print("shortcut %s (appid %d) written for user %s" % (a.name, appid, os.path.basename(user)))
    if a.compat:
        config = os.path.join(a.steam, "config", "config.vdf"); text = open(config, encoding="utf-8").read()
        block = '\t\t\t\t\t"%d"\n\t\t\t\t\t{\n\t\t\t\t\t\t"name"\t\t"%s"\n\t\t\t\t\t\t"config"\t\t""\n\t\t\t\t\t\t"priority"\t\t"250"\n\t\t\t\t\t}\n' % (appid, a.compat)
        text = re.sub(r'\n[ \t]*"%d"\s*\{[^}]*\}' % appid, "", text)  # Replace an earlier mapping.
        m = re.search(r'"CompatToolMapping"\s*\{[ \t]*\n', text)
        if not m:
            m = re.search(r'"Steam"\s*\{[ \t]*\n', text)
            if not m: sys.exit("config.vdf has no Steam section; set Proton in Properties > Compatibility")
            block = '\t\t\t\t"CompatToolMapping"\n\t\t\t\t{\n' + block + '\t\t\t\t}\n'
        shutil.copy(config, config + ".bak")
        open(config, "w", encoding="utf-8").write(text[:m.end()] + block + text[m.end():])
        print("compatibility tool %s set" % a.compat)

if __name__ == "__main__":
    main()
