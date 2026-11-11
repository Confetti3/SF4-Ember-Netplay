"""Ember's level digits: the atlas the game draws fighter levels from.

The art is Ember's own. Each numeral is a few brush strokes written out below as
centre lines, in the manner of Ember's developer mark (assets/input/
mark_developer.png): a chisel-cut start, rough edges and a dry-brush end, then
the Dev mark's flame (a white core, a yellow then orange spread, a red rim and a
shadow). Nothing else goes into it: no font and no outside picture. The strokes
are those of mockups/level-digits/make_font.py, where the look was chosen.

Each digit is rendered at cap heights 20, 40 and 80 pixels (four times
supersampled) as two sprites the same size: `under`, the shadow, rim and
spreads, and `core`, the white stroke. A number draws every under sprite, then
every core sprite, so a slanted digit's glow never covers its neighbour's core
where they tuck. docs/design/FIGHTER_LEVELS.md (The digits atlas) has the rest.

The atlas sits in assets/input beside mark_developer.png, because an updater
from 1.4.1 on accepts a new PNG there by name (scripts/Packaging.ps1).

  python -P scripts/level-digits/make_level_digits.py
      writes assets/input/level_digits.png and src/common/LevelDigits.hxx
  python -P scripts/level-digits/check_level_digits.py
      renders both again and fails when either differs

Pillow only. The build never runs this: both outputs are checked in.
"""
import math
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent.parent
ATLAS = ROOT / "assets" / "input" / "level_digits.png"
HEADER = ROOT / "src" / "common" / "LevelDigits.hxx"

CAP, HALF = 700, 66                       # digit height and half stroke width, in stroke units
SLANT = math.tan(math.radians(17))
SIDE = 18                                 # side bearing
TUCK = 80                                 # slanted digits sit under one another's lean
SIZES = (20, 40, 80)                      # cap heights rendered, in pixels
OVER = 4                                  # supersampling

# Strokes per digit, in a 500 x 700 box, y up, before the slant.
# ("s", points) is a smooth stroke through the points, ("l", points) a straight-edged one.
DIGITS = {
    "0": [("s", [(310, 690), (170, 650), (80, 500), (72, 270), (130, 90), (255, 12)]),
          ("s", [(225, 672), (350, 668), (435, 540), (442, 300), (385, 110), (235, 18)])],
    "1": [("l", [(105, 520), (285, 690)]),
          ("s", [(285, 700), (278, 360), (262, 0)])],
    "2": [("s", [(75, 530), (150, 665), (300, 695), (415, 615), (400, 470), (270, 300), (75, 45)]),
          ("s", [(55, 52), (250, 38), (460, 62)])],
    "3": [("l", [(75, 648), (415, 672)]),
          ("l", [(420, 680), (228, 405)]),
          ("s", [(222, 412), (355, 405), (442, 295), (415, 135), (295, 32), (150, 22), (55, 85)])],
    "4": [("l", [(335, 695), (55, 232)]),
          ("s", [(45, 228), (260, 236), (478, 246)]),
          ("s", [(352, 470), (345, 240), (332, 0)])],
    "5": [("s", [(150, 655), (290, 670), (425, 680)]),
          ("l", [(162, 672), (118, 398)]),
          ("s", [(112, 400), (250, 445), (400, 385), (442, 245), (385, 95), (245, 22), (75, 52)])],
    "6": [("s", [(410, 685), (255, 605), (125, 425), (76, 235), (120, 75), (250, 15)]),
          ("s", [(240, 15), (380, 52), (432, 200), (362, 338), (222, 360), (98, 282)])],
    "7": [("s", [(55, 650), (250, 664), (455, 678)]),
          ("s", [(460, 686), (335, 400), (212, 0)])],
    "8": [("s", [(408, 600), (305, 692), (165, 655), (130, 525), (250, 402), (392, 272), (412, 125),
                 (292, 20), (142, 40), (78, 150)]),
          ("s", [(78, 150), (110, 280), (248, 392), (380, 500), (414, 612)])],
    "9": [("s", [(425, 560), (305, 682), (152, 662), (80, 522), (112, 392), (242, 342), (382, 402),
                 (432, 560)]),
          ("s", [(440, 610), (425, 400), (335, 150), (178, 0)])],
}


def resample(points, smooth, step=10.0):
    """The centre line as points about `step` apart."""
    if smooth and len(points) > 2:
        dense, ext = [], [points[0]] + list(points) + [points[-1]]
        for i in range(1, len(ext) - 2):
            p0, p1, p2, p3 = ext[i - 1], ext[i], ext[i + 1], ext[i + 2]
            for k in range(24):
                t = k / 24.0
                dense.append(tuple(0.5 * ((2 * p1[a]) + (-p0[a] + p2[a]) * t
                                          + (2 * p0[a] - 5 * p1[a] + 4 * p2[a] - p3[a]) * t * t
                                          + (-p0[a] + 3 * p1[a] - 3 * p2[a] + p3[a]) * t ** 3) for a in (0, 1)))
        dense.append(points[-1])
    else:
        dense = list(points)
    out, carry = [dense[0]], 0.0
    for a, b in zip(dense, dense[1:]):
        seg = math.dist(a, b)
        while carry + seg >= step:
            f = (step - carry) / seg
            a = (a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f)
            out.append(a)
            seg, carry = math.dist(a, b), 0.0
        carry += seg
    if math.dist(out[-1], dense[-1]) > step * 0.4:
        out.append(dense[-1])
    return out


def stroke_outline(points, smooth, rng):
    """One brush stroke as a slanted polygon: chisel start, rough edges, dry-brush end."""
    line = resample(points, smooth)
    n = len(line)
    tangents = []
    for i in range(n):
        a, b = line[max(0, i - 1)], line[min(n - 1, i + 1)]
        d = math.dist(a, b) or 1.0
        tangents.append(((b[0] - a[0]) / d, (b[1] - a[1]) / d))
    phase = [rng.uniform(0, 6.28) for _ in range(4)]

    def width(i, side):
        t = i / max(1, n - 1)
        pressure = 0.9 + 0.16 * math.sin(math.pi * min(1.0, t * 1.6)) - 0.2 * t ** 2.2
        wobble = 3.4 * math.sin(i * 0.55 + phase[side]) + 2.2 * math.sin(i * 1.35 + phase[side + 2])
        return HALF * pressure + wobble

    left, right = [], []
    for i, (p, t) in enumerate(zip(line, tangents)):
        nx, ny = -t[1], t[0]
        wl, wr = width(i, 0), width(i, 1)
        left.append((p[0] + nx * wl, p[1] + ny * wl))
        right.append((p[0] - nx * wr, p[1] - ny * wr))

    # Start: a slanted cut, as a flat brush set down.
    p, t = line[0], tangents[0]
    nx, ny = -t[1], t[0]
    w = HALF * 0.9
    start = [(p[0] - nx * w * 0.9 - t[0] * w * 0.05, p[1] - ny * w * 0.9 - t[1] * w * 0.05),
             (p[0] - nx * w * 0.2 - t[0] * w * 0.30, p[1] - ny * w * 0.2 - t[1] * w * 0.30),
             (p[0] + nx * w * 0.5 - t[0] * w * 0.42, p[1] + ny * w * 0.5 - t[1] * w * 0.42),
             (p[0] + nx * w * 0.95 - t[0] * w * 0.22, p[1] + ny * w * 0.95 - t[1] * w * 0.22)]

    # End: the bristles leave the paper one clump after another.
    p, t = line[-1], tangents[-1]
    nx, ny = -t[1], t[0]
    w = width(n - 1, 0)
    clumps = 4
    lengths = [rng.uniform(0.3, 1.25) * HALF for _ in range(clumps)]
    lengths[rng.randrange(clumps)] *= 1.7
    end = []
    for j in range(clumps):
        across_tip = w * (1 - (2 * j + 1) / clumps)
        across_gap = w * (1 - (2 * j + 2) / clumps)
        end.append((p[0] + nx * across_tip + t[0] * lengths[j], p[1] + ny * across_tip + t[1] * lengths[j]))
        if j < clumps - 1:
            back = rng.uniform(0.0, 0.25) * HALF
            end.append((p[0] + nx * across_gap - t[0] * back, p[1] + ny * across_gap - t[1] * back))

    polygon = start + left + end + right[::-1]
    return [(x + y * SLANT, y) for x, y in polygon]


def build_glyphs():
    """digit -> (polygons with the ink's left edge at x 0, the ink's width), in stroke units."""
    glyphs = {}
    for digit, strokes in DIGITS.items():
        rng = random.Random(ord(digit) * 7919)
        polys = [stroke_outline(points, kind == "s", rng) for kind, points in strokes]
        xmin = min(x for poly in polys for x, _ in poly)
        xmax = max(x for poly in polys for x, _ in poly)
        glyphs[digit] = ([[(x - xmin, y) for x, y in poly] for poly in polys], xmax - xmin)
    return glyphs


def shifted(mask, dx, dy):
    """`mask` moved by whole pixels, what leaves the cell dropped."""
    out = Image.new("L", mask.size, 0)
    out.paste(mask, (dx, dy))
    return out


def flame_layers(mask, cap):
    """The Dev mark's colours on a mask, as the two sprites: under and core."""
    k = cap / 60.0

    def spread(radius, soften):
        grown = mask.filter(ImageFilter.GaussianBlur(max(0.3, radius * k)))
        grown = grown.point(lambda v: min(255, int(v * 3.2)))
        return grown.filter(ImageFilter.GaussianBlur(max(0.2, soften * k)))

    under = Image.new("RGBA", mask.size, (0, 0, 0, 0))
    shadow = shifted(spread(1.6, 1.2), int(2.5 * k) or 1, int(3 * k) or 1)
    for color, layer in (((0, 0, 0), shadow.point(lambda v: int(v * 0.75))),
                         ((190, 0, 0), spread(4.6, 2.4).point(lambda v: int(v * 0.6))),
                         ((236, 18, 0), spread(2.9, 0.35)),
                         ((255, 104, 8), spread(1.15, 0.35)),
                         ((255, 226, 92), spread(0.6, 0.25))):
        under = Image.alpha_composite(under, Image.merge("RGBA", [Image.new("L", mask.size, c) for c in color] + [layer]))
    core = Image.merge("RGBA", [Image.new("L", mask.size, c) for c in (255, 253, 240)] + [mask])
    return under, core


def size_layout(glyphs, cap):
    """One size's measures in its pixels: the pad, the baseline in a cell, the cell height, and per digit
    (cell width, ink width, bearing, advance)."""
    scale = cap / CAP
    pad = int(cap * 0.3) + 4
    top = max(y for polys, _ in glyphs.values() for poly in polys for _, y in poly)
    bottom = min(y for polys, _ in glyphs.values() for poly in polys for _, y in poly)
    baseline = pad + math.ceil(top * scale)
    height = baseline + math.ceil(-bottom * scale) + pad
    digits = []
    for digit in "0123456789":
        ink = glyphs[digit][1] * scale
        digits.append((math.ceil(ink) + 2 * pad, round(ink), round(SIDE * scale),
                       round((glyphs[digit][1] + 2 * SIDE - TUCK) * scale)))
    return pad, baseline, height, digits


def render_digit(polys, cap, pad, baseline, width, height):
    """A digit's ink as a mask `width` x `height`, its left edge `pad` in and y 0 on `baseline`."""
    scale = cap / CAP * OVER
    big = Image.new("L", (width * OVER, height * OVER), 0)
    draw = ImageDraw.Draw(big)
    for poly in polys:
        draw.polygon([(pad * OVER + x * scale, baseline * OVER - y * scale) for x, y in poly], fill=255)
    return big.resize((width, height), Image.LANCZOS)


def render():
    """The atlas (RGBA) and the header's text. Rows from the top: each size's under sprites, then its
    core sprites, sizes from the smallest; digits 0 to 9 left to right."""
    glyphs = build_glyphs()
    sizes, rows = [], []
    for cap in SIZES:
        pad, baseline, height, digits = size_layout(glyphs, cap)
        unders, cores = [], []
        for digit, (width, *_rest) in zip("0123456789", digits):
            under, core = flame_layers(render_digit(glyphs[digit][0], cap, pad, baseline, width, height), cap)
            unders.append(under)
            cores.append(core)
        sizes.append((cap, pad, baseline, height, digits))
        rows += [unders, cores]
    atlas = Image.new("RGBA", (max(sum(s.width for s in row) for row in rows), sum(row[0].height for row in rows)), (0, 0, 0, 0))
    y, entries = 0, []
    for (cap, pad, baseline, height, digits), unders, cores in zip(sizes, rows[0::2], rows[1::2]):
        x, cells = 0, []
        for (width, ink, bearing, advance), under, core in zip(digits, unders, cores):
            atlas.paste(under, (x, y))
            atlas.paste(core, (x, y + height))
            cells.append(f"        {{{{{x}, {y}, {width}, {height}}}, {{{x}, {y + height}, {width}, {height}}}, "
                         f"{bearing}, {ink}, {advance}}},")
            x += width
        entries.append(f"    {{{cap}, {pad}, {baseline}, {{\n" + "\n".join(cells) + "\n    }},")
        y += 2 * height
    header = HEADER_TEMPLATE.format(width=atlas.width, height=atlas.height, count=len(SIZES), sizes="\n".join(entries))
    return atlas, header


HEADER_TEMPLATE = """#pragma once
// Ember's level digits in assets/input/level_digits.png (FIGHTER_LEVELS.md, The
// digits atlas). Written with the atlas by
// scripts/level-digits/make_level_digits.py; check_level_digits.py fails when
// either differs from a new render. Do not edit.
namespace sf4e {{ namespace level_digits {{
// A sprite's place in the atlas, in its pixels.
struct Cell {{ int x, y, w, h; }};
// A digit at one size: its two sprites, `under` (the shadow, red rim, orange
// and yellow spreads) and `core` (the white stroke), the same size and drawn at
// the same place; then, in that size's pixels, the ink's left bearing, the
// ink's width (its lean included) and the advance. A sprite's ink starts `pad`
// in from its left edge.
struct Digit {{ Cell under, core; int bearing, ink, advance; }};
// One size: its cap height, the glow pad round the ink, the baseline's row in
// every sprite of the size, and the digits 0 to 9.
struct Size {{ int cap, pad, baseline; Digit digits[10]; }};
constexpr int AtlasWidth = {width}, AtlasHeight = {height};
// From the smallest cap height.
constexpr int SizeCount = {count};
constexpr Size Sizes[SizeCount] = {{
{sizes}
}};
}} }}
"""


def main():
    atlas, header = render()
    atlas.save(ATLAS, optimize=True)
    HEADER.write_text(header, encoding="utf-8", newline="\n")
    print(f"Level digits: {atlas.width}x{atlas.height} -> {ATLAS.relative_to(ROOT)}, {HEADER.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
