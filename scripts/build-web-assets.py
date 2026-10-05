"""Regenerate the embernetplay.link page assets from Ember's own brand files.

Requires Pillow, fontTools and brotli. Writes into
rust/ember-short/deploy/site, which setup.sh installs on the server:

  favicon.ico                         16, 24, 32 and 48 px from src/ui/ember.ico
  assets/ember-emblem.png             the largest ico image, scaled for the header
  assets/ember-background.webp, .jpg  assets/brand/ember-menu-background.png, compressed
  assets/fonts/inter-latin-*.woff2    Inter Regular and SemiBold, Latin subset
  assets/fonts/OFL.txt                Inter's license, copied unchanged
"""
import shutil
from pathlib import Path

from fontTools import subset
from PIL import Image

repo = Path(__file__).resolve().parent.parent
site = repo / "rust/ember-short/deploy/site"
assets = site / "assets"
fonts = assets / "fonts"

# Google Fonts' "latin" range; the @font-face rules in site.css name the same one.
LATIN = ("U+0000-00FF,U+0131,U+0152-0153,U+02BB-02BC,U+02C6,U+02DA,U+02DC,U+0304,U+0308,U+0329,"
         "U+2000-206F,U+20AC,U+2122,U+2191,U+2193,U+2212,U+2215,U+FEFF,U+FFFD")
FEATURES = "kern,liga,calt,ccmp,locl,mark,mkmk,tnum,zero,case"
BACKGROUND_WIDTH = 1600
EMBLEM_SIZE = 160


def emblem() -> None:
    with Image.open(repo / "src/ui/ember.ico") as ico:
        largest = max(ico.info["sizes"])
        ico.size = largest
        image = ico.convert("RGBA")
    # A 256-colour palette with alpha looks the same here at a quarter of the bytes.
    header = image.resize((EMBLEM_SIZE, EMBLEM_SIZE), Image.Resampling.LANCZOS)
    header.quantize(colors=256, method=Image.Quantize.FASTOCTREE).save(
        assets / "ember-emblem.png", optimize=True)
    image.save(site / "favicon.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48)])


def background() -> None:
    with Image.open(repo / "assets/brand/ember-menu-background.png") as source:
        image = source.convert("RGB")
    height = round(image.height * BACKGROUND_WIDTH / image.width)
    image = image.resize((BACKGROUND_WIDTH, height), Image.Resampling.LANCZOS)
    image.save(assets / "ember-background.webp", quality=72, method=6)
    image.save(assets / "ember-background.jpg", quality=74, optimize=True, progressive=True)


def font(name: str, weight: int) -> None:
    subset.main([
        str(repo / "src/ui/fonts" / name),
        f"--unicodes={LATIN}",
        f"--layout-features={FEATURES}",
        "--flavor=woff2",
        "--no-hinting",
        "--desubroutinize",
        f"--output-file={fonts / f'inter-latin-{weight}.woff2'}",
    ])


def main() -> None:
    fonts.mkdir(parents=True, exist_ok=True)
    emblem()
    background()
    font("Inter-Regular.ttf", 400)
    font("Inter-SemiBold.ttf", 600)
    shutil.copyfile(repo / "src/ui/fonts/OFL.txt", fonts / "OFL.txt")
    for path in sorted([site / "favicon.ico", *assets.glob("ember-*"), *fonts.iterdir()]):
        print(f"{path.stat().st_size:>8}  {path.relative_to(site)}")


if __name__ == "__main__":
    main()
