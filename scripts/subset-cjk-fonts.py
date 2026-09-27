"""Write the embedded CJK subset fonts from the catalogs.

Each Japanese, Korean and Simplified Chinese catalog is drawn by a subset of
Noto Sans CJK (SIL OFL 1.1) holding every character that catalog uses, plus
every language's native name so the language picker can always draw them.
Run it after changing a CJK catalog; the Localization test fails until the
fonts cover the text.

Needs fontTools and the Noto Sans CJK Regular OTFs from the notofonts/noto-cjk
tag Sans2.004 (Sans/OTF/{Japanese,Korean,SimplifiedChinese}).
"""
import argparse
import hashlib
import json
from pathlib import Path

from fontTools import subset, version as fonttools_version
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parent.parent
# Punctuation and full-width forms a translation may add without the catalog
# having used them yet, so a small wording change rarely needs a new subset.
EXTRA = [chr(c) for c in range(0x3000, 0x3020)] + [chr(c) for c in range(0xFF01, 0xFF5F)] + ["・", "ー"]


def msgstrs(path):
    """Every msgstr body in a catalog, with PO escapes decoded."""
    text, out, current = path.read_text(encoding="utf-8"), [], None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("msgstr "):
            current = [line[7:]]
            out.append(current)
        elif line.startswith('"') and current is not None:
            current.append(line)
        else:
            current = None
    decode = lambda s: s[1:-1].replace('\\"', '"').replace("\\n", "\n").replace("\\\\", "\\")
    return ["".join(decode(part) for part in parts) for parts in out]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, default=ROOT.parent / "font-sources" / "noto-cjk-Sans2.004",
                        help="folder holding the Noto Sans CJK Regular OTFs")
    args = parser.parse_args()
    # locales/locales.json declares the languages, their scripts and each
    # script's font; the build reads the same file.
    registry = json.loads((ROOT / "locales" / "locales.json").read_text(encoding="utf-8"))
    manifest = {"source": "notofonts/noto-cjk Sans2.004 (SIL OFL 1.1)", "fontTools": fonttools_version, "fonts": {}}
    for script in (entry for entry in registry["scripts"] if entry["font"]):
        output_name = script["font"]
        text = set(EXTRA)
        for locale in registry["locales"]:
            if locale["script"] == script["id"]:
                text |= set(locale["name"])
                for value in msgstrs(ROOT / "locales" / f"{locale['tag']}.po"):
                    text |= set(value)
        source = args.source / script["source"]
        font = TTFont(source, recalcTimestamp=False)
        cmap = font.getBestCmap()
        wanted = sorted(ord(c) for c in text if ord(c) >= 0x20 and ord(c) in cmap)
        options = subset.Options()
        # ImGui draws from cmap, hmtx and outlines only; layout tables and
        # hinting are dead weight in an embedded font.
        options.layout_features = []
        options.drop_tables += ["GSUB", "GPOS", "GDEF", "BASE", "VORG", "vhea", "vmtx", "DSIG"]
        options.hinting = False
        options.desubroutinize = True
        options.name_IDs = ["*"]
        options.name_languages = ["*"]
        options.recalc_timestamp = False
        subsetter = subset.Subsetter(options)
        subsetter.populate(unicodes=wanted)
        subsetter.subset(font)
        output = ROOT / "src" / "ui" / "fonts" / output_name
        font.save(output)
        data = output.read_bytes()
        manifest["fonts"][output_name] = {
            "sourceSha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "sha256": hashlib.sha256(data).hexdigest(),
            "codepoints": len(wanted),
            "bytes": len(data),
        }
        print(f"{output_name}: {len(wanted)} codepoints, {len(data)} bytes")
    (ROOT / "src" / "ui" / "fonts" / "NotoSansCJK-Subset.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
