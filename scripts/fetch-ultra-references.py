"""Import move-specific period screenshots from the pinned Ultra source index.

Requires Pillow and FFmpeg for pinned video frames. Original downloads are
retained; cards are 256x144 PNGs.
Downloads are sequential and stop on rate limits instead of hammering the site.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
import urllib.request
from PIL import Image, ImageOps


def download(url, target, limit):
    if target.exists():
        return
    request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(request, timeout=25) as response:
        data = response.read(limit + 1)
    if len(data) > limit:
        raise ValueError(f"Oversized Ultra source: {url}")
    target.write_bytes(data)
    time.sleep(2)


def prepare_original(row, work):
    original = work / f"{row['code']}-ultra-{row['ultra']}.source"
    if "frame_seconds" in row:
        # Share a retained clip when several moves come from one period trailer.
        video = work / (hashlib.sha256(row["url"].encode()).hexdigest()[:16] + ".mp4")
        download(row["url"], video, 64 * 1024 * 1024)
        if hashlib.sha256(video.read_bytes()).hexdigest() != row["video_sha256"]:
            raise ValueError(f"Video changed; review before replacing {row['path']}")
        if not original.exists():
            subprocess.run([
                "ffmpeg", "-hide_banner", "-loglevel", "error", "-ss", str(row["frame_seconds"]),
                "-i", str(video), "-frames:v", "1", "-f", "image2", "-c:v", "png", str(original),
            ], check=True)
    else:
        download(row["url"], original, 4 * 1024 * 1024)
    return original


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, default=Path("assets/selection"))
    parser.add_argument("--work", type=Path, default=Path("build/fighter-selection/reference/ultras/originals"))
    args = parser.parse_args()
    index = json.loads(Path(__file__).with_name("selection-ultra-sources.json").read_text(encoding="utf-8"))
    manifest = args.assets / "ultra-sources.json"
    previous = {row["path"]: row for row in json.loads(manifest.read_text())["images"]} if manifest.exists() else {}
    args.work.mkdir(parents=True, exist_ok=True)
    imported = []
    for row in index["images"]:
        original = prepare_original(row, args.work)
        source_hash = hashlib.sha256(original.read_bytes()).hexdigest()
        old = previous.get(row["path"])
        if old and (old["url"] != row["url"] or old["source_sha256"] != source_hash):
            raise ValueError(f"Source changed; review before replacing {row['path']}")
        target = args.assets / row["path"]
        target.parent.mkdir(parents=True, exist_ok=True)
        with Image.open(original) as source:
            source_size = list(source.size)
            if source.width < 200 or source.height < 100 or source.width > 4096 or source.height > 4096:
                raise ValueError(f"Unexpected Ultra photo dimensions: {row['path']} {source.size}")
            ImageOps.fit(source.convert("RGB"), (256, 144), Image.Resampling.LANCZOS).save(target)
        imported.append(dict(row, source_sha256=source_hash, source_size=source_size, size=[256, 144],
                             sha256=hashlib.sha256(target.read_bytes()).hexdigest()))
        print(f"{row['code']} Ultra {row['ultra'] + 1}: {row['move']}", flush=True)
    manifest.write_text(json.dumps(dict(credit=index["credit"], images=imported), indent=2) + "\n", encoding="utf-8")
    print(f"Imported {len(imported)} Ultra photos; originals preserved in {args.work}")


if __name__ == "__main__":
    main()
