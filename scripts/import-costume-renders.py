"""Make costume cutouts from renders of the game's own models and palettes.

The renders are front views of each fighter in its stance, drawn from an
installed game's files with transparency: `<CODE>/<set>_<colour>.webp`, with
set and colour counted from 1 as in the game's `<CODE>_<set>_<colour>.col.emb`
files (colours 11 and 12 use colour 1's with the game's own shaders). Each one
replaces the cutout of the same costume and colour, framed like the photograph
cutouts, and its record in `cutouts.json`.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re

from PIL import Image

PIPELINE = "costume-render-v1"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renders", type=Path, help="the folder of renders, one subfolder per fighter")
    parser.add_argument("--assets", type=Path, default=Path("assets/selection"))
    parser.add_argument("--canvas", type=lambda text: tuple(int(p) for p in text.lower().split("x")), default=(256, 384))
    parser.add_argument("--margin", type=int, default=16)
    parser.add_argument("--colors", type=int, default=256)
    args = parser.parse_args()
    manifest_path = args.assets / "cutouts.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    records = {(r["fighter"], r["costume"], r["color"]): r for r in manifest["images"]}
    width, height = args.canvas
    for render in sorted(args.renders.glob("*/*.webp")):
        match = re.fullmatch(r"(\d+)_(\d+)", render.stem)
        fighter, costume, color = render.parent.name, int(match[1]) - 1, int(match[2]) - 1
        image = Image.open(render).convert("RGBA")
        bounds = image.getchannel("A").point(lambda a: 255 if a >= 8 else 0).getbbox()
        if bounds is None:
            raise RuntimeError(f"Empty render: {render}")
        image = image.crop(bounds)
        factor = min((width - 2 * args.margin) / image.width, (height - 2 * args.margin) / image.height)
        size = (max(1, round(image.width * factor)), max(1, round(image.height * factor)))
        image = image.resize(size, Image.Resampling.LANCZOS)
        canvas = Image.new("RGBA", args.canvas, (0, 0, 0, 0))
        offset = ((width - size[0]) // 2, height - args.margin - size[1])
        canvas.alpha_composite(image, offset)
        relative = Path(fighter, f"costume-{costume}", f"color-{color}-cutout.png")
        target = args.assets / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        # A palette PNG with per-entry alpha, which WIC decodes like RGBA.
        canvas.quantize(colors=args.colors, method=Image.Quantize.FASTOCTREE,
                        dither=Image.Dither.FLOYDSTEINBERG).save(target, optimize=True)
        records[(fighter, costume, color)] = {
            "fighter": fighter, "costume": costume, "color": color,
            "source_file": f"render {fighter}/{render.name}",
            "source_sha256": hashlib.sha256(render.read_bytes()).hexdigest(),
            "file": relative.as_posix(), "sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
            "pipeline": PIPELINE, "canvas": list(args.canvas), "margin": args.margin,
            "source_bounds": list(bounds), "scaled_size": list(size), "offset": list(offset),
            "colors": args.colors,
        }
    manifest["updated_utc"] = datetime.now(timezone.utc).isoformat()
    manifest["images"] = [records[key] for key in sorted(records)]
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"{sum(r['pipeline'] == PIPELINE for r in manifest['images'])} render cutouts, {len(records)} in all")


if __name__ == "__main__":
    main()
