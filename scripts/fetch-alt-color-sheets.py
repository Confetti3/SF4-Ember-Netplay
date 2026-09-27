"""Crop every numbered palette of the alternate costumes from the wiki's color sheets.

Street Fighter Wiki's "Alternate Costumes/Street Fighter IV series" page shows
each alternate costume as one sheet of in-game screenshots: two rows holding
every palette in the game's order. This script downloads the original sheets
(the CDN serves a lossy WebP unless the original is asked for), verifies each
against the SHA-1 the wiki reports, cuts the grid into one photograph per
palette and records every crop in `assets/selection/alt-color-sources.json`.

The crops are masking inputs, kept under build/, not package assets: the
package ships only the cutouts that `mask-selection-photos.py` makes from
them. Colors that already have a photograph keep it. Needs Pillow.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import time
import urllib.parse
import urllib.request

from PIL import Image

API = "https://streetfighter.fandom.com/api.php"
PAGE = "Alternate Costumes/Street Fighter IV series"
PAGE_URL = "https://streetfighter.fandom.com/wiki/Alternate_Costumes/Street_Fighter_IV_series"
CREDIT = "Street Fighter Wiki (Fandom) contributors, Alternate Costumes/Street Fighter IV series; game imagery copyright Capcom"
HEADERS = {"User-Agent": "Mozilla/5.0 (Ember selection references)"}
# Wiki headings use English names; native codes follow the Japanese boss names.
CODES = {
    "Abel": "JHA", "Adon": "ADN", "Akuma": "GKI", "Balrog": "BSN", "Blanka": "BLK", "C. Viper": "AGL",
    "Cammy": "CMY", "Chun-Li": "CNL", "Cody": "CDY", "Dan": "DAN", "Decapre": "DCP", "Dee Jay": "DJY",
    "Dhalsim": "DSM", "Dudley": "DDL", "E. Honda": "HND", "El Fuerte": "RIC", "Elena": "ELN",
    "Evil Ryu": "RYX", "Fei Long": "FLN", "Gen": "GEN", "Gouken": "GKN", "Guile": "GUL", "Guy": "GUY",
    "Hakan": "HKN", "Hugo": "HUG", "Ibuki": "IBK", "Juri": "JRI", "Ken Masters": "KEN", "M. Bison": "VEG",
    "Makoto": "MKT", "Oni": "GKX", "Poison": "PSN", "Rolento": "RLN", "Rose": "ROS", "Rufus": "CHB",
    "Ryu": "RYU", "Sagat": "SGT", "Sakura": "SKR", "Seth": "BOS", "T. Hawk": "HWK", "Vega": "BLR",
    "Yang": "YAN", "Yun": "YUN", "Zangief": "ZGF",
}
# Sheet position (row-major, two rows) to native zero-based color. Each sheet
# opens with the two stylized shader palettes (colors 11 and 12, native 10 and
# 11: rim-lit, then cross-hatched), then the numbered palettes in order. That
# was matched against the EventHubs photographs of Ken's and Ryu's Alternate 2.
# Earlier costumes have 12 palettes, the Vacation, Wild and Horror packs 22.
ORDER = {12: [10, 11] + list(range(10)), 22: [10, 11] + list(range(10)) + list(range(12, 22))}
# The CDN strips metadata from this JPEG, so its bytes cannot match the wiki's
# SHA-1; the served file is pinned instead (same 4928x1328 image).
SERVED_SHA1 = {"HugoAlt4.jpg": "d33064d17a1f8b2c6569b133c74c9f718d36ebaf"}


def api(**params):
    query = urllib.parse.urlencode({"format": "json", **params})
    request = urllib.request.Request(f"{API}?{query}", headers=HEADERS)
    return json.load(urllib.request.urlopen(request, timeout=30))


def sheets():
    """(character, costume, file) for every gallery entry on the page.

    The costume is the entry's place in its gallery, which lists Alternate 1
    onward; captions carry typos (BlankaAlt6 is captioned "Alternate 5")."""
    text = api(action="parse", page=PAGE, prop="wikitext")["parse"]["wikitext"]["*"]
    result = []
    for section in re.finditer(r"=== \[\[([^\]|]+)(?:\|[^\]]*)?\]\] ===(.*?)(?==== |\Z)", text, re.S):
        files = re.findall(r"^\s*(?:File:)?([^|\n<]+\.(?:png|jpg))\|\s*Alternate \d", section.group(2), re.M)
        result.extend((section.group(1), costume, name.strip()) for costume, name in enumerate(files, 1))
    return result


def imageinfo(files):
    info = {}
    for start in range(0, len(files), 50):
        titles = "|".join("File:" + name for name in files[start:start + 50])
        for page in api(action="query", titles=titles, prop="imageinfo", iiprop="url|size|sha1")["query"]["pages"].values():
            info[page["title"].removeprefix("File:").replace(" ", "_")] = page["imageinfo"][0]
    return info


def download(url, sha1, target):
    sha1 = SERVED_SHA1.get(target.name, sha1)
    if target.exists() and hashlib.sha1(target.read_bytes()).hexdigest() == sha1:
        return
    request = urllib.request.Request(url + "&format=original", headers={**HEADERS, "Accept": "image/png,image/jpeg"})
    for attempt in range(3):
        try:
            data = urllib.request.urlopen(request, timeout=60).read()
            break
        except OSError:
            if attempt == 2:
                raise
            time.sleep(2 * (attempt + 1))
    if hashlib.sha1(data).hexdigest() != sha1:
        raise RuntimeError(f"Sheet differs from the wiki's SHA-1: {url}")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)


def grid(width, height, colors):
    """Boxes of an even two-row grid, row-major."""
    columns = colors // 2
    return [(round(c * width / columns), round(r * height / 2), round((c + 1) * width / columns), round((r + 1) * height / 2))
            for r in range(2) for c in range(columns)]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--assets", type=Path, default=Path("assets/selection"))
    parser.add_argument("--sheets", type=Path, default=Path("../costume-sources/fandom-usf4-alts"),
                        help="download cache for the original sheets")
    parser.add_argument("--crops", type=Path, default=Path("build/fighter-selection/alt-crops"))
    args = parser.parse_args()
    entries = sheets()
    unknown = sorted({character for character, _, _ in entries} - CODES.keys())
    if unknown:
        raise RuntimeError(f"Unmapped wiki characters: {unknown}")
    info = imageinfo([name.replace(" ", "_") for _, _, name in entries])
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(lambda e: download(info[e[2].replace(" ", "_")]["url"], info[e[2].replace(" ", "_")]["sha1"],
                                         args.sheets / e[2]), entries))
    alternates = {}
    for character, costume, _ in entries:
        alternates[character] = max(alternates.get(character, 0), costume)
    records, kept = [], 0
    for character, costume, name in entries:
        code = CODES[character]
        # Six alternates means four earlier costumes before the three packs.
        colors = 22 if costume >= alternates[character] - 2 else 12
        source = args.sheets / name
        sheet = Image.open(source).convert("RGB")
        meta = info[name.replace(" ", "_")]
        for position, box in enumerate(grid(sheet.width, sheet.height, colors)):
            color = ORDER[colors][position]
            relative = f"{code}/costume-{costume}/color-{color}.png"
            existing = [args.assets / code / f"costume-{costume}" / f"color-{color}{ext}" for ext in (".png", ".jpg")]
            if any(path.exists() for path in existing):
                kept += 1
                continue
            target = args.crops / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            sheet.crop(box).save(target, optimize=True)
            records.append({
                "fighter": code, "costume": costume, "color": color, "file": relative,
                "sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
                "source": meta["url"] + "&format=original", "source_page": PAGE_URL, "credit": CREDIT,
                "source_sha1": SERVED_SHA1.get(name, meta["sha1"]), "source_size": [sheet.width, sheet.height],
                "crop_bounds": list(box), "sheet_position": position,
            })
    manifest = {"schema": 1, "retrieved_utc": datetime.now(timezone.utc).isoformat(),
                "coverage": f"{len(records)} alternate-costume palettes cropped from {len(entries)} wiki sheets; "
                            f"{kept} palettes already photographed were kept",
                "images": records}
    (args.assets / "alt-color-sources.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Cropped {len(records)} palettes from {len(entries)} sheets into {args.crops}; kept {kept} existing photographs")


if __name__ == "__main__":
    main()
