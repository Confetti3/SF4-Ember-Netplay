"""Check assets/flags against scripts/countries.json with PIL.

The atlas has exactly the size the layout gives, every country's cell is an
opaque flag that is not one flat colour, and the padding around each cell and
the cells after the last country are fully transparent. flags.json names the
same codes at the same indices as the manifest, with the same layout. The
embernetplay.link rankings page's copies (server/ember-short/deploy/site/assets
flags.png, flags.json and flags-LICENSE.txt) are the same files.
Run it with python -I after scripts/build-flag-atlas.mjs.
"""
import json
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
# Flags that are not rectangles: their cell is transparent around the flag.
SHAPED = {"NP"}


def main():
    manifest = json.loads((ROOT / "scripts" / "countries.json").read_text(encoding="utf-8"))
    index = json.loads((ROOT / "assets" / "flags" / "flags.json").read_text(encoding="utf-8"))
    layout = manifest["atlas"]
    cw, ch, pad, columns = layout["cellWidth"], layout["cellHeight"], layout["padding"], layout["columns"]
    codes = [country["code"] for country in manifest["countries"]]
    sw, sh = cw + 2 * pad, ch + 2 * pad
    rows = -(-len(codes) // columns)
    problems = []
    if index["countries"] != {code: i for i, code in enumerate(codes)}:
        problems.append("flags.json does not index the manifest's codes in order")
    for key, value in (("cellWidth", cw), ("cellHeight", ch), ("padding", pad), ("columns", columns),
                       ("width", columns * sw), ("height", rows * sh)):
        if index.get(key) != value:
            problems.append(f"flags.json {key} is {index.get(key)}, not {value}")
    if not (ROOT / "assets" / "flags" / "LICENSE").read_text(encoding="utf-8").startswith("The MIT License"):
        problems.append("assets/flags/LICENSE is not the MIT licence")
    site = ROOT / "server" / "ember-short" / "deploy" / "site" / "assets"
    for name, copy in (("flags.png", "flags.png"), ("flags.json", "flags.json"), ("LICENSE", "flags-LICENSE.txt")):
        original, copied = ROOT / "assets" / "flags" / name, site / copy
        same = (original.read_bytes() == copied.read_bytes() if name.endswith(".png")
                else original.read_text(encoding="utf-8").splitlines() == copied.read_text(encoding="utf-8").splitlines())
        if not same:
            problems.append(f"the rankings page's assets/{copy} is not a copy of assets/flags/{name}")
    with Image.open(ROOT / "assets" / "flags" / "flags.png") as opened:
        atlas = opened.convert("RGBA") if opened.mode != "RGBA" else opened.copy()
    if atlas.size != (columns * sw, rows * sh):
        problems.append(f"atlas is {atlas.size}, not {(columns * sw, rows * sh)}")
    else:
        alpha = atlas.getchannel("A")
        for slot in range(columns * rows):
            x, y = slot % columns * sw, slot // columns * sh
            cell = atlas.crop((x + pad, y + pad, x + pad + cw, y + pad + ch))
            cell_alpha = alpha.crop((x + pad, y + pad, x + pad + cw, y + pad + ch))
            # The padding: the slot with the cell cleared.
            ring = alpha.crop((x, y, x + sw, y + sh)).copy()
            ring.paste(0, (pad, pad, pad + cw, pad + ch))
            if ring.getextrema()[1] != 0:
                problems.append(f"slot {slot}: padding is not transparent")
            if slot >= len(codes):
                if cell_alpha.getextrema()[1] != 0:
                    problems.append(f"slot {slot}: unused cell is not transparent")
                continue
            if codes[slot] in SHAPED:
                if cell_alpha.getextrema()[1] < 255:
                    problems.append(f"{codes[slot]}: shaped flag has no opaque pixel")
            elif cell_alpha.getextrema()[0] < 255:
                problems.append(f"{codes[slot]}: flag is not opaque")
            colours = cell.convert("RGB").getcolors(cw * ch)
            if colours is None or len(colours) < 2:
                problems.append(f"{codes[slot]}: flag is one flat colour")
    for problem in problems:
        print(problem)
    if problems:
        sys.exit(1)
    print(f"Flag atlas checked: {len(codes)} flags in {atlas.size[0]}x{atlas.size[1]}")


if __name__ == "__main__":
    main()
