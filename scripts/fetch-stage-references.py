"""Cache authentic 2014 USFIV screenshots and make uniform 640x360 stage cards.

Requires Pillow. Originals remain in the work directory; crops do not redraw or
remove the game HUD or the source watermark. The manifest pins source hashes.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import urllib.request
from PIL import Image, ImageOps

PAGE = "https://www.supersoluce.com/soluce/ultra-street-fighter-iv/stages"
BASE = "https://www.supersoluce.com/sites/default/files/node/52349/"
# Gallery order, independently matched to the native stage-code table.
CODES = "BLD IND KOR ELV HFP CHN USA LBX AFX RUS BRA AFR VIE MAD BFU JPX SCO EUR JPN LAB RVR VCN CNX JUR DET TRN VNX BRX".split()
# Page labels and file numbering diverge after Cruise Ship Stern. These are the
# actual full-resolution links under each heading, not inferred image numbers.
PHOTO_NUMBERS = list(range(1, 19)) + [20, 22, 23, 24, 25, 26, 27, 28, 50, 29]
IDS = dict(zip("TRN CHN USA RUS BRA AFR VIE EUR RVR VCN SCO JPN LAB IND KOR BLD CNX BRX VNX JPX AFX LBX DET ELV HFP MAD BFU JUR".split(), list(range(22)) + list(range(24, 30))))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, default=Path("assets/selection"))
    parser.add_argument("--work", type=Path, default=Path("build/fighter-selection/stages/originals"))
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    (args.assets / "stages").mkdir(parents=True, exist_ok=True)
    manifest = args.assets / "stage-sources.json"
    old = {item["code"]: item for item in json.loads(manifest.read_text())["images"]} if manifest.exists() else {}

    def fetch(item):
        index, code = item
        filename = f"carte-street-fighter-ultra-{index:03}.jpg"
        url = BASE + filename
        original = args.work / filename
        if not original.exists():
            request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
            original.write_bytes(urllib.request.urlopen(request, timeout=30).read())
        source_hash = hashlib.sha256(original.read_bytes()).hexdigest()
        if code in old and (old[code]["url"] != url or old[code]["source_sha256"] != source_hash):
            raise ValueError(f"Source changed for {code}; review before replacing it")
        relative = f"stages/{code}.jpg"
        target = args.assets / relative
        with Image.open(original) as source:
            size = source.size
            if size != (1280, 720):
                raise ValueError(f"Unexpected source size {size}: {code}")
            ImageOps.fit(source.convert("RGB"), (640, 360), Image.Resampling.LANCZOS).save(target, quality=92)
        return {"id": IDS[code], "code": code, "url": url, "source_page": PAGE,
                "source_size": list(size), "source_sha256": source_hash,
                "path": relative, "size": [640, 360],
                "sha256": hashlib.sha256(target.read_bytes()).hexdigest()}

    with ThreadPoolExecutor(max_workers=4) as pool:
        images = sorted(pool.map(fetch, zip(PHOTO_NUMBERS, CODES)), key=lambda item: item["id"])
    manifest.write_text(json.dumps({"source_date": "2014-06-16", "credit": "SuperSoluce / Pierre Durden; game imagery copyright Capcom",
        "native_stage_table_rva": "0x66b678", "executable_sha256": "5d724595a8ab3c6c6d6f4959187f756f5be35bb497e51e5233c4e73b18b0b9eb",
        "excluded_bonus_ids": [22, 23], "images": images}, indent=2) + "\n", encoding="utf-8")
    print(f"Prepared {len(images)} authentic stage photos at 640x360; originals preserved in {args.work}")

if __name__ == "__main__":
    main()
