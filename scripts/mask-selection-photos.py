"""Create uniform, non-destructive costume cutouts from the source manifest.

Uses local ONNX segmentation. The original photograph supplies every RGB pixel;
the model supplies only an alpha mask. Sources are never overwritten.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import time

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

CANVAS = (512, 768)
MARGIN = 32
PIPELINE = "costume-cutout-v1"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("assets", type=Path)
    parser.add_argument("--work", type=Path, default=Path("build/fighter-selection/masking"))
    parser.add_argument("--model", default="birefnet-general-lite")
    parser.add_argument("--fighters", default="", help="Comma-separated native codes; omit for all")
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--source-manifest", default="sources.json")
    parser.add_argument("--provider", default="CPUExecutionProvider",
                        choices=["CPUExecutionProvider", "DmlExecutionProvider", "CUDAExecutionProvider"])
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("REMBG_HOME", str((args.work / "models").resolve()))
    os.environ.setdefault("OMP_NUM_THREADS", "4")
    from rembg import new_session, remove

    records = json.loads((args.assets / args.source_manifest).read_text(encoding="utf-8"))["images"]
    if args.fighters:
        fighters = set(args.fighters.upper().split(","))
        records = [record for record in records if record["fighter"] in fighters]
    if args.limit:
        records = records[:args.limit]
    report_path = args.assets / "cutouts.json"
    overrides_path = Path(__file__).with_name("selection-mask-overrides.json")
    overrides = json.loads(overrides_path.read_text(encoding="utf-8"))["images"] if overrides_path.exists() else {}
    report = json.loads(report_path.read_text(encoding="utf-8")) if report_path.exists() else {"images": []}
    prior = {record["source_file"]: record for record in report["images"]}
    session = None
    start = time.monotonic()
    for index, record in enumerate(records, 1):
        relative = Path(record["file"])
        source_path = args.assets / relative
        source_hash = hashlib.sha256(source_path.read_bytes()).hexdigest()
        if source_hash != record["sha256"]:
            raise RuntimeError(f"Source hash changed: {source_path}")
        target_relative = relative.with_name(relative.stem + "-cutout.png")
        target = args.assets / target_relative
        cached = prior.get(record["file"], {})
        override = overrides.get(record["file"], {})
        override_hash = hashlib.sha256(json.dumps(override, sort_keys=True).encode()).hexdigest() if override else ""
        if override and override["source_sha256"] != source_hash:
            raise RuntimeError(f"Manual mask no longer matches source: {source_path}")
        if (cached.get("pipeline") == PIPELINE and cached.get("model") == args.model and
                cached.get("override_sha256", "") == override_hash and
                cached.get("source_sha256") == source_hash and target.exists() and
                hashlib.sha256(target.read_bytes()).hexdigest() == cached.get("sha256")):
            continue
        if session is None:
            print(f"Loading {args.model} for local masking...", flush=True)
            import onnxruntime as ort
            if args.provider == "CUDAExecutionProvider":
                ort.preload_dlls()
            options = ort.SessionOptions()
            if args.provider == "DmlExecutionProvider":
                options.enable_mem_pattern = False
                options.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
            providers = [args.provider]
            if args.provider == "CUDAExecutionProvider":
                # Keep cuDNN scratch space bounded on desktop display GPUs.
                providers = [(args.provider, {"cudnn_conv_use_max_workspace": "0",
                                              "arena_extend_strategy": "kSameAsRequested"})]
            session = new_session(args.model, providers=providers, sess_opts=options)
            if args.provider not in session.inner_session.get_providers():
                raise RuntimeError(f"Requested mask provider is unavailable: {args.provider}")
        photo = Image.open(source_path).convert("RGB")
        mask_path = args.work / "masks" / args.model / relative.with_name(relative.stem + "-" + source_hash[:12] + ".png")
        mask_path.parent.mkdir(parents=True, exist_ok=True)
        if not mask_path.exists():
            mask = remove(photo, session=session, only_mask=True).convert("L")
            mask.save(mask_path)
        else:
            mask = Image.open(mask_path).convert("L")
        alpha = np.asarray(mask).copy()
        # Background crowds and distant scenery can form separate components.
        # Keep the main fighter and its soft boundary, without filling real holes.
        labels, count = ndimage.label(alpha >= 24)
        if not count:
            raise RuntimeError(f"Empty foreground mask: {source_path}")
        areas = np.bincount(labels.ravel()); areas[0] = 0
        foreground = labels == int(areas.argmax())
        support = ndimage.binary_dilation(foreground, iterations=2)
        alpha[~support] = 0
        alpha[alpha < 3] = 0
        # Visually traced corrections preserve small costume accessories that
        # automatic segmentation can omit. They affect alpha only, never RGB.
        if override:
            correction = Image.new("L", (photo.width * 4, photo.height * 4), 0)
            draw = ImageDraw.Draw(correction)
            for polygon in override.get("include_polygons", []):
                draw.polygon([(x * 4, y * 4) for x, y in polygon], fill=255)
            correction = correction.resize(photo.size, Image.Resampling.LANCZOS)
            alpha = np.maximum(alpha, np.asarray(correction))
            exclusion = Image.new("L", (photo.width * 4, photo.height * 4), 0)
            draw = ImageDraw.Draw(exclusion)
            for polygon in override.get("exclude_polygons", []):
                draw.polygon([(x * 4, y * 4) for x, y in polygon], fill=255)
            exclusion = exclusion.resize(photo.size, Image.Resampling.LANCZOS)
            alpha = (alpha.astype(np.uint16) * (255 - np.asarray(exclusion).astype(np.uint16)) // 255).astype(np.uint8)
        mask = Image.fromarray(alpha)
        bounds = Image.fromarray((alpha >= 8).astype(np.uint8) * 255).getbbox()
        if bounds is None:
            raise RuntimeError(f"Empty cutout: {source_path}")
        photo = photo.convert("RGBA"); photo.putalpha(mask)
        cutout = photo.crop(bounds)
        factor = min((CANVAS[0] - 2 * MARGIN) / cutout.width, (CANVAS[1] - 2 * MARGIN) / cutout.height)
        size = (max(1, round(cutout.width * factor)), max(1, round(cutout.height * factor)))
        cutout = cutout.resize(size, Image.Resampling.LANCZOS)
        canvas = Image.new("RGBA", CANVAS, (0, 0, 0, 0))
        offset = ((CANVAS[0] - size[0]) // 2, CANVAS[1] - MARGIN - size[1])
        canvas.alpha_composite(cutout, offset)
        target.parent.mkdir(parents=True, exist_ok=True)
        canvas.save(target, optimize=True)
        prior[record["file"]] = {
            "fighter": record["fighter"], "costume": record["costume"], "color": record["color"],
            "source_file": record["file"], "source_sha256": source_hash,
            "file": target_relative.as_posix(), "sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
            "pipeline": PIPELINE, "model": args.model, "canvas": list(CANVAS), "margin": MARGIN,
            "provider": args.provider,
            "override_sha256": override_hash, "correction_note": override.get("reason", ""),
            "source_bounds": list(bounds), "scaled_size": list(size), "offset": list(offset),
            "mask_file": str(mask_path), "visual_review": "pending",
        }
        report = {"schema": 1, "updated_utc": datetime.now(timezone.utc).isoformat(),
                  "images": sorted(prior.values(), key=lambda r: (r["fighter"], r["costume"], r["color"]))}
        temporary = report_path.with_suffix(".json.tmp")
        temporary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        temporary.replace(report_path)
        print(f"{index}/{len(records)} {target_relative.as_posix()} ({time.monotonic() - start:.1f}s)", flush=True)
    print(f"Finished: {len(records)} requested photos; {len(prior)} cached cutouts.", flush=True)


if __name__ == "__main__":
    main()
