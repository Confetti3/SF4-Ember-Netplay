#!/usr/bin/env python3
"""Windows import libraries are found case-insensitively; xwin's splat has them in
lower case only. For every word in the source that names one of them in another
case (Bcrypt, AdvApi32, ...), add a symlink. Usage: case-links.py <source> <splat>"""
import pathlib
import re
import sys

src, splat = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
words = set()
word = re.compile(rb"[A-Za-z][A-Za-z0-9_]{2,}")
for path in src.rglob("*"):
    if path.suffix in (".cmake", ".txt", ".cxx", ".cpp", ".hxx", ".h", ".rs", ".toml") and path.is_file():
        words.update(w.decode() for w in word.findall(path.read_bytes()))
made = 0
for libdir in list(splat.glob("sdk/lib/*/*")) + list(splat.glob("crt/lib/*")):
    present = {p.name for p in libdir.iterdir()}
    for w in words:
        lower = w.lower() + ".lib"
        variant = w + ".lib"
        if lower in present and variant not in present and variant != lower:
            (libdir / variant).symlink_to(lower)
            made += 1
print(f"case links made: {made}")
