"""Builds site/assets/faces.webp, the rooms page's fighter faces.

One row of 64-pixel squares in native fighter ID order (src/common/FighterMetadata.inc),
each cut around the head (HEADS) of the fighter's first packaged outfit cutout
(assets/selection/<CODE>/costume-*/color-0-cutout.png). The page shows face N
at x = -N * size. Run from the repository root:

    python server/ember-short/deploy/make-faces.py

Needs Pillow with WebP support.
"""
import glob
import os
import sys

from PIL import Image

SIDE = 64
# Each fighter's head in its cutout's painted area: centre x as a fraction of
# the width, centre y as a fraction of the height, and the square's side as a
# fraction of the height. Read off the cutouts by eye.
HEADS = {
    "RYU": (0.55, 0.085, 0.17), "KEN": (0.53, 0.085, 0.17), "CNL": (0.30, 0.075, 0.15),
    "HND": (0.68, 0.095, 0.15), "BLK": (0.60, 0.130, 0.20), "ZGF": (0.55, 0.130, 0.18),
    "GUL": (0.63, 0.110, 0.15), "DSM": (0.47, 0.080, 0.15), "BSN": (0.60, 0.130, 0.17),
    "BLR": (0.45, 0.110, 0.16), "SGT": (0.55, 0.110, 0.16), "VEG": (0.45, 0.100, 0.17),
    "AGL": (0.50, 0.090, 0.15), "CHB": (0.47, 0.100, 0.17), "RIC": (0.66, 0.150, 0.18),
    "JHA": (0.55, 0.100, 0.15), "BOS": (0.50, 0.075, 0.15), "GKI": (0.55, 0.110, 0.17),
    "GKN": (0.52, 0.070, 0.18), "HWK": (0.50, 0.065, 0.15), "CMY": (0.47, 0.080, 0.15),
    "FLN": (0.50, 0.080, 0.15), "DJY": (0.45, 0.100, 0.16), "SKR": (0.40, 0.090, 0.15),
    "ROS": (0.48, 0.100, 0.15), "GEN": (0.37, 0.100, 0.16), "DAN": (0.47, 0.090, 0.16),
    "GUY": (0.55, 0.090, 0.15), "CDY": (0.45, 0.100, 0.16), "IBK": (0.50, 0.130, 0.15),
    "MKT": (0.50, 0.065, 0.15), "DDL": (0.65, 0.070, 0.16), "ADN": (0.30, 0.120, 0.17),
    "HKN": (0.62, 0.085, 0.13), "JRI": (0.35, 0.100, 0.15), "YUN": (0.30, 0.080, 0.15),
    "YAN": (0.40, 0.065, 0.15), "RYX": (0.48, 0.100, 0.17), "GKX": (0.45, 0.100, 0.20),
    "RLN": (0.55, 0.110, 0.16), "ELN": (0.78, 0.100, 0.20), "PSN": (0.78, 0.055, 0.11),
    "HUG": (0.45, 0.080, 0.15), "DCP": (0.50, 0.090, 0.15),
}


def codes(root):
    path = os.path.join(root, "src", "common", "FighterMetadata.inc")
    with open(path, encoding="utf-8") as metadata:
        return [line.split('"')[1] for line in metadata if line.strip().startswith('{"')]


def face(root, code):
    files = sorted(glob.glob(os.path.join(root, "assets", "selection", code, "costume-*", "color-0-cutout.png")))
    if not files or code not in HEADS:
        raise SystemExit(f"no outfit cutout or head for {code}")
    image = Image.open(files[0]).convert("RGBA")
    image = image.crop(image.getchannel("A").getbbox())
    width, height = image.size
    x, y, side = HEADS[code]
    side = int(side * height)
    left, top = int(x * width) - side // 2, int(y * height) - side // 2
    # A square past the cutout's edge stays transparent there.
    square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    square.alpha_composite(image.crop((max(0, left), max(0, top), left + side, top + side)), (max(0, -left), max(0, -top)))
    return square.resize((SIDE, SIDE), Image.LANCZOS)


def main():
    root = os.getcwd()
    names = codes(root)
    sheet = Image.new("RGBA", (SIDE * len(names), SIDE), (0, 0, 0, 0))
    for index, code in enumerate(names):
        sheet.alpha_composite(face(root, code), (index * SIDE, 0))
    out = os.path.join(root, "server", "ember-short", "deploy", "site", "assets", "faces.webp")
    sheet.save(out, "WEBP", quality=82, method=6)
    print(f"{out}: {len(names)} faces, {os.path.getsize(out)} bytes")
    if len(sys.argv) > 1:
        preview = Image.new("RGBA", (SIDE * 11, SIDE * 4), (88, 80, 72, 255))
        for index in range(len(names)):
            tile = sheet.crop((index * SIDE, 0, (index + 1) * SIDE, SIDE))
            preview.alpha_composite(tile, ((index % 11) * SIDE, (index // 11) * SIDE))
        preview.resize((SIDE * 11 * 2, SIDE * 4 * 2), Image.NEAREST).save(sys.argv[1])


if __name__ == "__main__":
    main()
