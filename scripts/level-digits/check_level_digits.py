"""Check assets/input/level_digits.png and src/common/LevelDigits.hxx with PIL.

Both are rendered again by make_level_digits.py: the header must be the same
text and the atlas the same pixels (PNG bytes may differ with the encoder).
Each sprite's outer pixel ring is fully transparent, so a sprite drawn smaller
never samples its neighbour; every under sprite has ink, and every core sprite
is ink inside its under sprite's glow.

  python -P scripts/level-digits/check_level_digits.py

Exits 0 when both match, 1 when they do not, and 77 when Pillow is missing (the
test that runs it is then skipped).
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
try:
    import PIL
    from PIL import Image, ImageChops
except ImportError:
    print("Pillow is not installed: level digits not checked")
    sys.exit(77)
import make_level_digits as digits


def ring(alpha, box):
    """The largest alpha on the outer pixel ring of `box` (x, y, w, h)."""
    x, y, w, h = box
    edges = ((x, y, x + w, y + 1), (x, y + h - 1, x + w, y + h), (x, y, x + 1, y + h), (x + w - 1, y, x + w, y + h))
    return max(alpha.crop(edge).getextrema()[1] for edge in edges)


def main():
    atlas, header = digits.render()
    problems = []
    if not digits.HEADER.exists() or digits.HEADER.read_text(encoding="utf-8") != header:
        problems.append(f"{digits.HEADER.relative_to(digits.ROOT)} differs from a new render")
    if not digits.ATLAS.exists():
        problems.append(f"{digits.ATLAS.relative_to(digits.ROOT)} is missing")
    else:
        with Image.open(digits.ATLAS) as opened:
            shipped = opened.convert("RGBA")
        if shipped.size != atlas.size:
            problems.append(f"atlas is {shipped.size}, a new render {atlas.size}")
        elif ImageChops.difference(shipped, atlas).getbbox() is not None:
            problems.append(f"atlas pixels differ from a new render in {ImageChops.difference(shipped, atlas).getbbox()}")
    alpha = atlas.getchannel("A")
    glyphs = digits.build_glyphs()
    y = 0
    for cap in digits.SIZES:
        pad, baseline, height, sizes = digits.size_layout(glyphs, cap)
        x = 0
        for digit, (width, ink, bearing, advance) in zip("0123456789", sizes):
            under, core = (x, y, width, height), (x, y + height, width, height)
            for name, box in (("under", under), ("core", core)):
                if ring(alpha, box):
                    problems.append(f"{cap} px {digit} {name}: the edge of the sprite is not transparent")
            glow = alpha.crop((x, y, x + width, y + height))
            stroke = alpha.crop((x, y + height, x + width, y + 2 * height)).point(lambda v: 255 if v > 127 else 0)
            if glow.getextrema()[1] < 255 or not stroke.getbbox():
                problems.append(f"{cap} px {digit}: a sprite has no solid ink")
                x += width
                continue
            # The core lies inside the glow: nowhere is the stroke opaque where its under sprite is clear.
            if ImageChops.subtract(stroke, glow.point(lambda v: 255 if v else 0)).getbbox():
                problems.append(f"{cap} px {digit}: the core reaches past its glow")
            # The header's measures hold the stroke: all of its ink lies from `pad` in to `ink` past that,
            # give or take the three pixels the Lanczos filter reaches, and inside the pad from top to
            # bottom; the next digit tucks under its lean.
            left, top, right, bottom = alpha.crop((x, y + height, x + width, y + 2 * height)).getbbox()
            if left < pad - 3 or right > pad + ink + 3 or top < pad - 3 or bottom > height - pad + 3:
                problems.append(f"{cap} px {digit}: the stroke spans {left} to {right}, not {pad} to {pad + ink}")
            if not 0 < advance < ink + 2 * bearing:
                problems.append(f"{cap} px {digit}: advance {advance} does not tuck under ink {ink}")
            x += width
        y += 2 * height
    for problem in problems:
        print(problem)
    if problems:
        # A blur or resampling change in another Pillow moves pixels too: say which one rendered.
        print(f"Rendered with Pillow {PIL.__version__}; run make_level_digits.py after a deliberate change.")
        sys.exit(1)
    print(f"Level digits checked: {atlas.width}x{atlas.height}, cap heights {', '.join(map(str, digits.SIZES))}")


if __name__ == "__main__":
    main()
