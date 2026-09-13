"""Fetch the 2015 costume gallery with an explicit native-option mapping.

The gallery shows each outfit in its original palette. The separately pinned
Capcom Horror gallery supplies the final pack. Other numbered palettes are absent.
Files are cached locally; each source URL and SHA-256 is recorded for review.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import urllib.request
import urllib.error

SOURCE = "https://www.sfgalleries.net/art/sf4/usf4/alt/"
# Gallery filenames use Japanese boss names. Map them to native codes, not
# their different English display names: mbison=Boxer, balrog=Claw, vega=Dictator.
SLUGS = {
    "RYU": "ryu", "KEN": "ken", "CNL": "chun-li", "HND": "ehonda",
    "BLK": "blanka", "ZGF": "zangief", "GUL": "guile", "DSM": "dhalsim",
    "BSN": "mbison", "BLR": "balrog", "SGT": "sagat", "VEG": "vega",
    "AGL": "cviper", "CHB": "rufus", "RIC": "elfuerte", "JHA": "abel",
    "BOS": "seth", "GKI": "gouki", "GKN": "gouken", "HWK": "thawk",
    "CMY": "cammy", "FLN": "feilong", "DJY": "deejay", "SKR": "sakura",
    "ROS": "rose", "GEN": "gen", "DAN": "dan", "GUY": "guy", "CDY": "cody",
    "IBK": "ibuki", "MKT": "makoto", "DDL": "dudley", "ADN": "adon",
    "HKN": "hakan", "JRI": "juri", "YUN": "yun", "YAN": "yang",
    "RYX": "evilryu", "GKX": "oni", "RLN": "rolento", "ELN": "elena",
    "PSN": "poison", "HUG": "hugo", "DCP": "decapre",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    manifest_path = args.output / "sources.json"
    previous = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.exists() else {"images": []}
    previous_by_file = {record["file"]: record for record in previous["images"]}
    html = urllib.request.urlopen(SOURCE, timeout=20).read().decode()
    urls = sorted(set(re.findall(r'href="(usf4-alt-[a-z-]+-[1-5]\.jpg)"', html)))
    if len(urls) != 201:
        raise RuntimeError(f"Gallery changed: expected 201 references, found {len(urls)}")
    code_by_slug = {slug: code for code, slug in SLUGS.items()}
    records = []
    for filename in urls:
        slug, costume = re.fullmatch(r"usf4-alt-(.*)-(\d).jpg", filename).groups()
        code = code_by_slug[slug]
        records.append({"fighter": code, "costume": int(costume), "color": 0,
                        "file": f"{code}/costume-{costume}/color-0.jpg", "source": SOURCE + filename,
                        "source_page": SOURCE, "credit": "Street Fighter Galleries / slateman; game artwork Capcom"})
    records.extend(json.loads(Path(__file__).with_name("selection-horror-sources.json").read_text(encoding="utf-8"))["images"])
    for record in records:
        prior = previous_by_file.get(record["file"], {})
        expected_source = record["source"]
        if prior.get("source") not in (None, expected_source, expected_source.replace(".jpg", "_thumb.jpg")):
            raise RuntimeError(f"Cached source mapping differs; migrate the reviewed assets first: {record['file']}")
        if "sha256" in prior:
            for field in ("source", "resolution_note", "sha256"):
                if field in prior:
                    record[field] = prior[field]

    def fetch(record):
        target = args.output / record["file"]
        if not target.exists():
            try:
                data = urllib.request.urlopen(record["source"], timeout=20).read()
            except urllib.error.HTTPError as error:
                thumbnail = record["source"].replace(".jpg", "_thumb.jpg")
                if error.code != 404 or thumbnail.removeprefix(SOURCE) not in html:
                    record["error"] = str(error)
                    return record
                try:
                    data = urllib.request.urlopen(thumbnail, timeout=20).read()
                    record["source"] = thumbnail
                    record["resolution_note"] = "Gallery thumbnail; full-size source returned 404"
                except (urllib.error.URLError, TimeoutError) as fallback_error:
                    record["error"] = str(fallback_error)
                    return record
            except (urllib.error.URLError, TimeoutError) as error:
                record["error"] = str(error)
                print(f"Unavailable: {record['source']}: {error}", flush=True)
                return record
            if not (data.startswith(b"\xff\xd8\xff") or data.startswith(b"\x89PNG\r\n\x1a\n")) or len(data) > 4 * 1024 * 1024:
                raise RuntimeError(f"Unexpected image content: {record['source']}")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        digest = hashlib.sha256(target.read_bytes()).hexdigest()
        if "sha256" in record and record["sha256"] != digest:
            raise RuntimeError(f"Reference bytes changed; review before updating source hash: {target}")
        record["sha256"] = digest
        return record

    with ThreadPoolExecutor(max_workers=4) as pool:
        records = list(pool.map(fetch, records))
    manifest = {"schema": 1, "retrieved_utc": datetime.now(timezone.utc).isoformat(),
                "coverage": "245 alternate-outfit references including Horror, default palette only",
                "images": records}
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "sources.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    count = sum("sha256" in record for record in records)
    print(f"Cached {count}/{len(records)} costume references and their source hashes in {args.output.resolve()}")


if __name__ == "__main__":
    main()
