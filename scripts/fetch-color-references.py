"""Split the pinned 2010 EventHubs sheets into authentic numbered color photos.

Crop rectangles are source-hash-bound. PNG crops preserve the decoded RGB pixels;
the existing default-palette gallery photographs are never overwritten.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import time
from urllib.request import Request, urlopen

from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, default=Path("assets/selection"))
    parser.add_argument("--work", type=Path, default=Path("build/fighter-selection/reference/colors"))
    args = parser.parse_args()
    sources = json.loads(Path(__file__).with_name("selection-color-sheets.json").read_text(encoding="utf-8"))
    existing = json.loads((args.assets / "sources.json").read_text(encoding="utf-8"))["images"]
    occupied = {(r["fighter"], r["costume"], r["color"]) for r in existing}
    args.work.mkdir(parents=True, exist_ok=True)
    records = []
    for sheet in sources["sheets"]:
        widths = [box[2] - box[0] for box in sheet["boxes"]]
        if max(widths) - min(widths) > 2:
            raise RuntimeError(f"Inconsistent photo column widths: {sheet['file']}")
        path = args.work / sheet["file"]
        if not path.exists():
            with urlopen(Request(sheet["url"], headers={"User-Agent": "Mozilla/5.0"}), timeout=30) as response:
                data = response.read(4 * 1024 * 1024 + 1)
            if len(data) > 4 * 1024 * 1024:
                raise RuntimeError(f"Oversize source: {sheet['url']}")
            if hashlib.sha256(data).hexdigest() != sheet["sha256"]:
                raise RuntimeError(f"Source changed: {sheet['url']}")
            path.write_bytes(data)
            time.sleep(2)
        if hashlib.sha256(path.read_bytes()).hexdigest() != sheet["sha256"]:
            raise RuntimeError(f"Source changed: {path}")
        photo = Image.open(path).convert("RGB")
        if list(photo.size) != sheet["size"]:
            raise RuntimeError(f"Wrong sheet size: {path}")
        for color, box in enumerate(sheet["boxes"]):
            key = (sheet["fighter"], sheet["costume"], color)
            if key in occupied:
                continue
            left, top, right, bottom = box
            if not (0 <= left < right <= photo.width and 0 <= top < bottom <= photo.height):
                raise RuntimeError(f"Invalid crop: {path}: {box}")
            relative = f"{key[0]}/costume-{key[1]}/color-{color}.png"
            target = args.assets / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            crop = photo.crop(box)
            crop.save(target, optimize=True)
            records.append(dict(fighter=key[0], costume=key[1], color=color,
                                file=relative, source=sheet["url"], source_page=sources["index_page"],
                                credit=sources["credit"], source_sha256=sheet["sha256"],
                                source_size=sheet["size"], crop_bounds=box,
                                sha256=hashlib.sha256(target.read_bytes()).hexdigest()))
            occupied.add(key)
    manifest = dict(schema=1, retrieved_utc=datetime.now(timezone.utc).isoformat(),
                    coverage="Numbered colors from 69 SFIV/SSFIV costume sheets (2010)", images=records)
    (args.assets / "color-sources.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Imported {len(records)} individual color photos; original gallery sources preserved.")


if __name__ == "__main__":
    main()
