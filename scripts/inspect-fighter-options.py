"""Read SFIV roster/localization/resource evidence without modifying the game.

The RVAs come from Dimps.cxx and Dimps__Game__Battle.cxx. Resource suffixes
are reported as evidence, never treated as selectable menu IDs.
"""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path


def localization(path):
    data = path.read_bytes()
    if data[:4] != b"#M4S":
        raise ValueError(f"Not an M4S localization table: {path}")
    count, table = struct.unpack_from("<II", data, 16)
    result = {}
    for i in range(count):
        entry = table + i * 8
        key, value = (entry + n for n in struct.unpack_from("<II", data, entry))
        end = value
        while end + 2 <= len(data) and data[end:end + 2] != b"\0\0":
            end += 2
        if end + 2 > len(data):
            raise ValueError(f"Unterminated M4S string: {path}")
        result[data[key:data.index(b"\0", key)].decode("ascii")] = data[value:end].decode("utf-16le")
    return result


def inspect(game):
    binary = (game / "SSFIV.exe").read_bytes()
    pe = struct.unpack_from("<I", binary, 60)[0]
    section_count = struct.unpack_from("<H", binary, pe + 6)[0]
    optional_size = struct.unpack_from("<H", binary, pe + 20)[0]
    base = struct.unpack_from("<I", binary, pe + 24 + 28)[0]
    sections = [struct.unpack_from("<8sIIII", binary, pe + 24 + optional_size + i * 40)
                for i in range(section_count)]

    def offset(va):
        rva = va - base
        for _, virtual_size, virtual_address, size, raw in sections:
            if virtual_address <= rva < virtual_address + max(size, virtual_size):
                return rva - virtual_address + raw
        raise ValueError(f"Unmapped virtual address {va:x}")

    def strings(rva):
        result = []
        for i in range(44):
            start = offset(struct.unpack_from("<I", binary, offset(base + rva) + 4 * i)[0])
            result.append(binary[start:binary.index(b"\0", start)].decode("ascii"))
        return result

    codes, names = strings(0x66A8A8), strings(0x66A958)
    if codes[0] != "RYU" or codes[-1] != "DCP":
        raise ValueError("Unsupported roster table; do not use these offsets for this executable")
    translations, sources = {}, []
    roots = ["resource", "patch", "dlc", "patch_ae2", "patch_ae2_tu1",
             "patch_ae2_tu1b", "patch_ae2_tu2", "patch_ae2_tu3"]
    for root in roots:
        for path in sorted((game / root).rglob("chara_select*.m4s")):
            if path.parent.name == "ENG":
                translations.update(localization(path))
                sources.append({"path": path.relative_to(game).as_posix(),
                                "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
    resources = {code: {} for code in codes}
    for path in game.rglob("*.col.emb"):
        match = re.fullmatch(r"([A-Z0-9]+)_(\d+)_(\d+)\.col\.emb", path.name)
        if match and match[1] in resources:
            resources[match[1]].setdefault(int(match[2]), set()).add(int(match[3]))
    characters = []
    for i, code in enumerate(codes):
        start = offset(base + 0x539BA8) + i * 7
        flags = list(binary[start:start + 7])
        if any(flag not in (0, 1) for flag in flags) or flags[3] != 0:
            raise ValueError("Unsupported edition table")
        ultras = {key: value for key, value in translations.items() if f"CMD_{code}_" in key}
        if len(ultras) != 2:
            raise ValueError(f"Expected two named Ultras for {code}, found {ultras}")
        characters.append({"id": i, "code": code, "nativeName": names[i],
                           "editions": [edition for edition, flag in zip([13, 1, 2, 0, 4, 14, 16], flags) if flag],
                           "ultras": ultras,
                           "resourceSuffixes": {str(costume): sorted(colors)
                                                for costume, colors in sorted(resources[code].items())}})
    return {"executableSha256": hashlib.sha256(binary).hexdigest(),
            "rosterRva": "0x66a8a8", "editionsRva": "0x539ba8",
            "localizationSources": sources, "characters": characters,
            "selectionLabels": {k: v for k, v in translations.items() if "CMD_" not in k}}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = inspect(args.game)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Recorded {len(result['characters'])} fighters and 88 Ultra names in {args.output}")
