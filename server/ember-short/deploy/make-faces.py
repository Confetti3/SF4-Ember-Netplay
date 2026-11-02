"""Builds site/assets/faces.webp, the rooms page's fighter faces.

One row of 96-pixel squares in native fighter ID order (src/common/FighterMetadata.inc),
each cut around the head (HEADS) of the fighter's character select portrait in the
game's files (ui/chara_select/chara/sel_<CODE>.tex.emz, read the way
src/ui/SelectionArt.cxx reads it). The page shows them at 32 pixels, so the
squares stay sharp on high-density screens. Face N is at x = -N * size. Run from
the repository root with the game's folder:

    python server/ember-short/deploy/make-faces.py "<...>/Super Street Fighter IV - Arcade Edition" [preview.png]

Needs Pillow with WebP and DDS support.
"""
import io
import os
import struct
import sys
import zlib

from PIL import Image

SIDE = 96
# The game's portrait folders, newest first, as SelectionArt.cxx searches them.
ROOTS = ["patch_ae2_tu3", "patch_ae2_tu2", "patch_ae2", "dlc/04_ae2", "dlc/03_character_free", "resource"]
# Each fighter's head in the 1024-pixel portrait: centre x and centre y, and the
# square's side, as fractions of the portrait. The square holds the whole head,
# hair to chin, with a little room around it, the face in the middle.
HEADS = {
    "RYU": (0.525, 0.270, 0.300), "KEN": (0.540, 0.260, 0.300), "CNL": (0.500, 0.230, 0.275),
    "HND": (0.575, 0.270, 0.300), "BLK": (0.500, 0.245, 0.325), "ZGF": (0.615, 0.280, 0.300),
    "GUL": (0.460, 0.280, 0.300), "DSM": (0.550, 0.330, 0.300), "BSN": (0.560, 0.290, 0.300),
    "BLR": (0.545, 0.305, 0.300), "SGT": (0.550, 0.355, 0.300), "VEG": (0.460, 0.270, 0.300),
    "AGL": (0.475, 0.275, 0.275), "CHB": (0.460, 0.320, 0.300), "RIC": (0.425, 0.280, 0.300),
    "JHA": (0.460, 0.280, 0.275), "BOS": (0.525, 0.255, 0.275), "GKI": (0.440, 0.270, 0.300),
    "GKN": (0.440, 0.280, 0.275), "HWK": (0.510, 0.265, 0.300), "CMY": (0.540, 0.280, 0.275),
    "FLN": (0.450, 0.255, 0.275), "DJY": (0.410, 0.260, 0.300), "SKR": (0.525, 0.305, 0.275),
    "ROS": (0.510, 0.245, 0.275), "GEN": (0.410, 0.280, 0.300), "DAN": (0.450, 0.270, 0.300),
    "GUY": (0.450, 0.305, 0.275), "CDY": (0.505, 0.265, 0.275), "IBK": (0.450, 0.270, 0.275),
    "MKT": (0.460, 0.305, 0.275), "DDL": (0.460, 0.280, 0.300), "ADN": (0.550, 0.340, 0.300),
    "HKN": (0.490, 0.255, 0.325), "JRI": (0.500, 0.290, 0.275), "YUN": (0.500, 0.310, 0.300),
    "YAN": (0.450, 0.290, 0.300), "RYX": (0.450, 0.240, 0.300), "GKX": (0.460, 0.205, 0.325),
    "RLN": (0.460, 0.225, 0.300), "ELN": (0.450, 0.270, 0.300), "PSN": (0.530, 0.320, 0.275),
    "HUG": (0.460, 0.285, 0.300), "DCP": (0.445, 0.250, 0.275),
}


def codes(root):
    path = os.path.join(root, "src", "common", "FighterMetadata.inc")
    with open(path, encoding="utf-8") as metadata:
        return [line.split('"')[1] for line in metadata if line.strip().startswith('{"')]


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def portrait(game, code):
    """The first entry of the #EMB archive inside the #EMZ file: a DDS image."""
    for folder in ROOTS:
        path = os.path.join(game, folder, "ui", "chara_select", "chara", f"sel_{code}.tex.emz")
        if not os.path.exists(path):
            continue
        data = open(path, "rb").read()
        if data[:4] != b"#EMZ":
            raise SystemExit(f"{path}: not an EMZ file")
        size, start = u32(data, 8), u32(data, 12)
        data = zlib.decompress(data[start:], -zlib.MAX_WBITS)
        if len(data) != size or data[:4] != b"#EMB":
            raise SystemExit(f"{path}: not an EMB archive")
        table = u32(data, 24)
        image, length = table + u32(data, table), u32(data, table + 4)
        if data[image:image + 4] != b"DDS ":
            raise SystemExit(f"{path}: no DDS image")
        return Image.open(io.BytesIO(data[image:image + length])).convert("RGBA")
    raise SystemExit(f"no portrait for {code} under {game}")


def face(game, code):
    image = portrait(game, code)
    width, height = image.size
    x, y, side = HEADS[code]
    side = round(side * width)
    left, top = round(x * width) - side // 2, round(y * height) - side // 2
    # A square past the portrait's edge stays transparent there.
    square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    square.alpha_composite(image.crop((max(0, left), max(0, top), left + side, top + side)), (max(0, -left), max(0, -top)))
    return square.resize((SIDE, SIDE), Image.LANCZOS)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    root, game = os.getcwd(), sys.argv[1]
    names = codes(root)
    if sorted(names) != sorted(HEADS):
        raise SystemExit("HEADS and FighterMetadata.inc name different fighters")
    sheet = Image.new("RGBA", (SIDE * len(names), SIDE), (0, 0, 0, 0))
    for index, code in enumerate(names):
        sheet.alpha_composite(face(game, code), (index * SIDE, 0))
    out = os.path.join(root, "server", "ember-short", "deploy", "site", "assets", "faces.webp")
    sheet.save(out, "WEBP", quality=86, method=6)
    print(f"{out}: {len(names)} faces, {os.path.getsize(out)} bytes")
    if len(sys.argv) > 2:
        preview = Image.new("RGBA", (SIDE * 11, SIDE * 4), (88, 80, 72, 255))
        for index in range(len(names)):
            tile = sheet.crop((index * SIDE, 0, (index + 1) * SIDE, SIDE))
            preview.alpha_composite(tile, ((index % 11) * SIDE, (index // 11) * SIDE))
        preview.save(sys.argv[2])


if __name__ == "__main__":
    main()
