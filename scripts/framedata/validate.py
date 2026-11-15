#!/usr/bin/env python3
"""Checks assets/framedata/usf4.json (or the file given): the shape the phone remote reads, and numbers that cannot be right.

  python3 scripts/framedata/validate.py [FILE]        exit 1 and a list of problems, or 0

Cheap on purpose: it finds a broken edit, not a wrong frame. A wrong frame is settled in the game (docs/development/FRAME_DATA.md).
"""
import json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
COLS = ["section", "move", "input", "strength", "startup", "active", "recovery", "total", "block_adv", "hit_adv",
        "hitstun", "blockstun", "inv", "extra", "measured", "script"]
SECTIONS = {"Normals", "Command normals", "Throws", "Specials", "Super", "Ultra 1", "Ultra 2", "Other"}
STRENGTHS = {"", "LP", "MP", "HP", "LK", "MK", "HK", "EX"}
COUNT = re.compile(r"-|\d+")                     # a frame count, or none
STARTUP = re.compile(r"-|\d+(\+\d+)?")           # supers: flash+rest
ACTIVE = re.compile(r"-|\d+(\(\d+\)\d+)*")       # 3, or 2(6)2 for hits with gaps
ADVANTAGE = re.compile(r"-|KD|[+-]?\d+")
MEASURED = {"startup", "startup_meter", "active", "recovery", "total", "on_hit", "on_hit_crouch", "on_block", "hitstun", "blockstun"}


def check(path):
    problems = []
    def bad(where, what):
        problems.append(f"{where}: {what}")
    data = json.load(open(path, encoding="utf-8"))
    if data.get("cols") != COLS:
        bad("cols", f"expected {COLS}")
    fighters = data.get("fighters")
    if not isinstance(fighters, dict) or not fighters:
        bad("fighters", "missing")
        return problems
    for name, rows in fighters.items():
        seen = set()
        if not rows:
            bad(name, "no moves")
        for r in rows:
            where = f"{name} {r[1] if len(r) > 1 else '?'}"
            if len(r) != len(COLS):
                bad(where, f"{len(r)} cells, expected {len(COLS)}"); continue
            c = dict(zip(COLS, r))
            if not all(isinstance(v, str) for v in r):
                bad(where, "a cell is not text")
            if c["section"] not in SECTIONS: bad(where, f"section {c['section']!r}")
            if c["strength"] not in STRENGTHS: bad(where, f"strength {c['strength']!r}")
            for col, rule in (("startup", STARTUP), ("active", ACTIVE), ("recovery", COUNT), ("total", COUNT), ("hitstun", COUNT),
                              ("blockstun", COUNT), ("block_adv", ADVANTAGE), ("hit_adv", ADVANTAGE)):
                if not rule.fullmatch(c[col]): bad(where, f"{col} {c[col]!r}")
            if c["startup"].isdigit() and c["total"].isdigit() and int(c["total"]) < int(c["startup"]):
                bad(where, f"total {c['total']} is shorter than startup {c['startup']}")
            if not c["script"].isdigit(): bad(where, f"script {c['script']!r}")
            if c["measured"] and not set(c["measured"].split(",")) <= MEASURED: bad(where, f"measured {c['measured']!r}")
            key = (c["section"], c["input"], c["move"], c["script"])
            if key in seen: bad(where, "listed twice")
            seen.add(key)
    return problems


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "assets", "framedata", "usf4.json")
    found = check(path)
    for p in found[:50]:
        print(p)
    print(f"{len(found)} problems" if found else "ok")
    sys.exit(1 if found else 0)
