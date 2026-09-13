"""Report exact photo coverage without treating a default photo as another color.

Run from the repository root. Native portrait fallback is reported separately;
it is only available when the installed game can supply the corresponding art.
"""
import argparse
import json
import re
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, default=Path("assets/selection"))
    parser.add_argument("--output", type=Path,
                        default=Path("build/fighter-selection/coverage-audit.json"))
    args = parser.parse_args()
    metadata = Path("src/common/FighterMetadata.inc").read_text(encoding="utf-8")
    fighters = re.findall(r'\{"([A-Z0-9]+)", "([^"]+)".*?, (\d+)u\}', metadata)
    if len(fighters) != 44:
        raise ValueError("The audit must cover the complete native roster")
    rows = []
    for code, name, editions in fighters:
        # Matches FighterCatalog's original-roster and released-pack rules.
        base = 4 if int(editions) & (1 << 13) else 3
        for costume in range(base + 3):
            count = 12 if costume < base else 22
            photos, missing = [], []
            for color in range(count):
                stem = args.assets / code / f"costume-{costume}" / f"color-{color}"
                exists = any(Path(str(stem) + suffix).is_file()
                             for suffix in ("-cutout.png", ".png", ".jpg"))
                (photos if exists else missing).append(color + 1)
            rows.append(dict(fighter=code, name=name, costume=costume,
                             count=count, photos=photos, missing=missing))
    photographed = sum(len(row["photos"]) for row in rows)
    total = sum(row["count"] for row in rows)
    defaults = sum(row["costume"] == 0 and 1 in row["missing"] for row in rows)
    report = dict(fighters=len(fighters), costumes=len(rows),
                  valid_color_options=total, photographed_options=photographed,
                  native_portrait_defaults=defaults,
                  options_without_matching_preview=total - photographed - defaults,
                  rows=rows)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items() if key != "rows"}))


if __name__ == "__main__":
    main()
