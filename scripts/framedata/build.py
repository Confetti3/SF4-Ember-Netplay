#!/usr/bin/env python3
"""Builds assets/framedata/usf4.json, the table the phone remote's Frames tab reads.

  python3 scripts/framedata/build.py SIM_DIR [--out assets/framedata/usf4.json] [--overrides assets/framedata/overrides.json]

SIM_DIR holds Ember's per-fighter frame data (<CODE>.json, one per fighter) as the simulator writes it
(docs/development/FRAME_DATA.md says where it comes from). Overrides are applied last, so a hand correction
survives the next rebuild. Run scripts/framedata/validate.py on the result.
"""
import glob, json, os, re, sys

COLS = ["section", "move", "input", "strength", "startup", "active", "recovery", "total", "block_adv", "hit_adv",
        "hitstun", "blockstun", "inv", "extra", "measured", "script"]
STRENGTH = {"LP": "LP", "MP": "MP", "HP": "HP", "LK": "LK", "MK": "MK", "HK": "HK", "PP": "EX", "KK": "EX"}
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def text(v):
    return "-" if v is None else str(v)


def advantage(v, knockdown=False):
    if knockdown:
        return "KD"
    return "-" if v is None else (f"+{v}" if isinstance(v, int) and v > 0 else str(v))


def strength(key):
    return STRENGTH.get(re.split(r"[+.]", key)[-1], "")


def row(m):
    status, seen = m.get("status", {}), m.get("calibrated", {})
    measured = ",".join(k for k in seen if status.get(k) != "sim_differs_calibration")  # the game showed the same number
    notes = list(m.get("notes", [])) + [f"game showed {k} {v}" for k, v in seen.items() if status.get(k) == "sim_differs_calibration"]
    return [m["section"], m["name"], m["input"], strength(m["input"]), text(m["startup"]), text(m["active"]), text(m["recovery"]),
            text(m["total"]), advantage(m["on_block"]), advantage(m["on_hit"], m.get("knockdown")), text(m["hitstun"]), text(m["blockstun"]),
            text(m["invuln"]), "; ".join(notes), measured, str(m["script"])]


def main(argv):
    if not argv or argv[0].startswith("-"):
        sys.exit(__doc__)
    sim = argv[0]
    arg = lambda name, default: argv[argv.index(name) + 1] if name in argv else default
    out = arg("--out", os.path.join(ROOT, "assets", "framedata", "usf4.json"))
    overrides_path = arg("--overrides", os.path.join(ROOT, "assets", "framedata", "overrides.json"))
    overrides = json.load(open(overrides_path, encoding="utf-8")) if os.path.exists(overrides_path) else {}
    fighters, made, sims = {}, set(), set()
    for path in sorted(glob.glob(os.path.join(sim, "*.json"))):
        data = json.load(open(path, encoding="utf-8"))
        if not isinstance(data, dict) or "moves" not in data:
            continue  # the all-fighters summary
        fighters[data["fighter"]] = [row(m) for m in data["moves"]]
        made.add(data.get("generated")); sims.add(data.get("sim"))
    for name, edits in overrides.items():
        if name.startswith("_"):
            continue
        for edit in edits:
            rows = [r for r in fighters.get(name, []) if r[2] == edit["input"] and r[-1] == str(edit.get("script", r[-1]))]
            if len(rows) != 1:
                sys.exit(f"override for {name} {edit['input']} (script {edit.get('script', 'any')}): {len(rows)} moves match, need one")
            for col, value in edit["set"].items():
                rows[0][COLS.index(col)] = str(value)
    table = {"cols": COLS, "meta": {"generated": sorted(d for d in made if d), "sim": sorted(s for s in sims if s),
             "moves": sum(len(v) for v in fighters.values())}, "fighters": dict(sorted(fighters.items()))}
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(table, f, ensure_ascii=False, separators=(",", ":"))
        f.write("\n")
    print(f"{len(fighters)} fighters, {table['meta']['moves']} moves -> {out}")


if __name__ == "__main__":
    main(sys.argv[1:])
