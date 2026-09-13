"""Validate source preservation and framing; render contact sheets for review."""
import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image, ImageDraw


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("assets", type=Path)
    parser.add_argument("--output", type=Path, default=Path("build/fighter-selection/masking/review"))
    parser.add_argument("--complete", action="store_true", help="Require a cutout for every source")
    parser.add_argument("--source-manifest", action="append", help="Review only the named manifest(s)")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    manifests = args.source_manifest or [name for name in ("sources.json", "color-sources.json") if (args.assets / name).exists()]
    sources = [row for name in manifests for row in json.loads((args.assets / name).read_text(encoding="utf-8"))["images"]]
    records = json.loads((args.assets / "cutouts.json").read_text(encoding="utf-8"))["images"]
    by_source = {r["source_file"]: r for r in records}
    records = [by_source[source["file"]] for source in sources if source["file"] in by_source]
    errors = []
    for source in sources:
        path = args.assets / source["file"]
        if hashlib.sha256(path.read_bytes()).hexdigest() != source["sha256"]:
            errors.append(f"Source changed: {path}")
        record = by_source.get(source["file"])
        if not record:
            if args.complete:
                errors.append(f"Missing cutout: {path}")
            continue
        target = args.assets / record["file"]
        if record["source_sha256"] != source["sha256"]:
            errors.append(f"Cutout uses an outdated source: {target}")
        if hashlib.sha256(target.read_bytes()).hexdigest() != record["sha256"]:
            errors.append(f"Cutout hash changed: {target}")
        with Image.open(target) as image:
            if image.mode != "RGBA" or image.size != (512, 768):
                errors.append(f"Wrong format or size: {target}")
                continue
            alpha = image.getchannel("A")
            box = alpha.getbbox()
            if not box or alpha.getextrema() != (0, 255):
                errors.append(f"Missing transparent/opaque foreground: {target}")
            elif box[0] < 30 or box[1] < 30 or box[2] > 482 or box[3] > 738:
                errors.append(f"Foreground outside uniform margins: {target}: {box}")
            if record["offset"][1] + record["scaled_size"][1] != 736:
                errors.append(f"Incorrect bottom alignment: {target}")
    # Stable alphabetical sheets, five costumes per row; names stay readable.
    for page_start in range(0, len(records), 30):
        page = records[page_start:page_start + 30]
        sheet = Image.new("RGB", (1200, 6 * 338), (25, 29, 40))
        draw = ImageDraw.Draw(sheet)
        for index, record in enumerate(page):
            x, y = (index % 5) * 240, (index // 5) * 338
            for gy in range(y + 24, y + 330, 18):
                for gx in range(x + 10, x + 230, 18):
                    if ((gx - x - 10) // 18 + (gy - y - 24) // 18) % 2 == 0:
                        draw.rectangle((gx, gy, min(gx + 17, x + 229), min(gy + 17, y + 329)), fill=(38, 43, 55))
            image = Image.open(args.assets / record["file"])
            image.thumbnail((224, 304), Image.Resampling.LANCZOS)
            sheet.paste(image, (x + (240 - image.width) // 2, y + 26), image)
            draw.text((x + 12, y + 7), f'{record["fighter"]} / Outfit {record["costume"] + 1:02d} / Color {record["color"] + 1:02d}', fill="white")
        sheet.save(args.output / f"cutouts-{page_start // 30 + 1:02d}.png")
    result = {"sources": len(sources), "cutouts": len(records), "complete": len(records) == len(sources),
              "errors": errors, "visual_review": "Contact sheets require human visual inspection."}
    (args.output / "validation.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    if errors:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
