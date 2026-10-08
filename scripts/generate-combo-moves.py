#!/usr/bin/env python3
"""Regenerates src/training/ComboMoves.inc, the move names a combo may be typed with.

    python3 scripts/generate-combo-moves.py path/to/USF4FrameData.json

The input is a USF4 move database: {fighter: {"moves": {state: {move: {...}}}}},
each move with moveName, cmnName, numCmd, moveType and optionally
chargeDirection and airmove. Only a move's names and its command are taken.

A name stands for one move or a short sequence ("236P > 236P"). Two moves of a
fighter that share a name keep it for the first; both also get their full,
tagged name ("P Crazy Buffalo", "K Crazy Buffalo"). A command the combo
notation cannot express is left out and listed.
"""
import json
import re
import sys
from pathlib import Path

CODES = {
    "Abel": "JHA", "Adon": "ADN", "Akuma": "GKI", "Balrog": "BSN", "Blanka": "BLK", "C.Viper": "AGL",
    "Cammy": "CMY", "Chun-Li": "CNL", "Cody": "CDY", "Dan": "DAN", "Decapre": "DCP", "Dee Jay": "DJY",
    "Dhalsim": "DSM", "Dudley": "DDL", "E.Honda": "HND", "El Fuerte": "RIC", "Elena": "ELN",
    "Evil Ryu": "RYX", "Fei Long": "FLN", "Gen (Mantis)": "GEN", "Gen (Crane)": "GEN", "Gouken": "GKN",
    "Guile": "GUL", "Guy": "GUY", "Hakan": "HKN", "Hugo": "HUG", "Ibuki": "IBK", "Juri": "JRI",
    "Ken": "KEN", "M.Bison": "VEG", "Makoto": "MKT", "Oni": "GKX", "Poison": "PSN", "Rolento": "RLN",
    "Rose": "ROS", "Rufus": "CHB", "Ryu": "RYU", "Sagat": "SGT", "Sakura": "SKR", "Seth": "BOS",
    "T.Hawk": "HWK", "Vega": "BLR", "Yang": "YAN", "Yun": "YUN", "Zangief": "ZGF",
}
# Shared by every fighter; a fighter's own name for a move wins over these.
UNIVERSAL = [
    ("focus", "MP+MK"), ("focusattack", "MP+MK"), ("redfocus", "LP+MP+MK"), ("redfocusattack", "LP+MP+MK"),
    ("falv1", "MP+MK"), ("falv2", "[MP+MK]"), ("falv3", "[MP+MK]"),
    ("focusattacklv1", "MP+MK"), ("focusattacklv2", "[MP+MK]"), ("focusattacklv3", "[MP+MK]"),
    ("redfalv1", "LP+MP+MK"), ("redfalv2", "[LP+MP+MK]"), ("redfalv3", "[LP+MP+MK]"),
    ("redfocusattacklv1", "LP+MP+MK"), ("redfocusattacklv2", "[LP+MP+MK]"), ("redfocusattacklv3", "[LP+MP+MK]"),
    ("throw", "LP+LK"),
    ("backthrow", "4LP+LK"), ("taunt", "HP+HK"), ("dash", "66"), ("backdash", "44"),
    ("jump", "8"), ("jumpforward", "9"), ("jumpback", "7"),
]
# Community short names, given as another name of a move the database has.
NICKNAMES = [
    ("RYU", "hadouken", "hadoken"), ("KEN", "hadouken", "hadoken"), ("SKR", "hadouken", "hadoken"),
    ("RYX", "hadouken", "hadoken"), ("ZGF", "spd", "spinningpiledriver"), ("ZGF", "greenhand", "banishingflat"),
    ("ZGF", "lariat", "doublelariat"), ("CNL", "sbk", "spinningbirdkick"), ("GUL", "boom", "sonicboom"),
    ("AGL", "seismo", "seismichammer"), ("JHA", "cod", "changeofdirection"), ("FLN", "rekka", "rekkaken"),
    ("BLK", "blankaball", "rollingattack"),
]
BUTTON = "(?:LP|MP|HP|LK|MK|HK)"
BUTTONS = rf"(?:P{{1,3}}|K{{1,3}}|{BUTTON}(?:\+{BUTTON})*)"
MOTION = r"(\[[1-9]\][1-9]{1,9}|360|720|[1-9]{0,10})"
STRENGTH = re.compile(rf"^(?:EX|{BUTTON}|P|K)(?:/{BUTTON})* ")
GENERIC = re.compile(r"^(Close|Far|Crouch|Stand|Jump|Neutral Jump|Diagonal Jump) ")
TYPES = {"special", "super", "ultra", "command-grab", "movement-special", "combo grab", "normal"}
HELD = re.compile(r"\((partial hold|full hold|hold[^)]*|held|charge|store)\)")
RELEASED = {"PP/KK": "PPP", "PP": "PP", "KK": "KK", "P": "P", "K": "K"}


def key(name):
    return re.sub(r"[^a-z0-9]", "", name.lower())


def first_choice(text):
    """"4 or 6LK" -> "4LK", "623/421PPP" -> "623PPP", "P or K" -> "P"."""
    for separator in (" or ", "/"):
        if separator in text:
            left, right = (part.strip() for part in text.split(separator, 1))
            buttons = re.sub(r"^[1-9]*\s*\+?\s*", "", right.split(separator)[0])
            text = left + buttons if left.isdigit() and not right.isdigit() else left
    return text


def piece(text, charge, generalise):
    """One move of a command, in the notation ParseStep accepts, or None."""
    text = text.strip()
    if text == "FADC":
        return text
    mash = bool(re.search(r"\((?!no mash)[^)]*mash[^)]*\)", text)) or text.lower().startswith("mash ")
    hold, release = bool(HELD.search(text)), "(release)" in text
    stance = "cl." if "(cl)" in text or "+ cl)" in text else "far." if "(far)" in text else ""
    text = re.sub(r"^[Mm]ash ", "", re.sub(r"\s*\([^)]*\)", "", text)).strip()
    released = re.match(r"Hold & Release (PP/KK|PP|KK|P|K)\b", text)
    if released:
        text, release = RELEASED[released.group(1)], True
    text = first_choice(text).replace("d+", "2").replace(",", "").replace("+", "").replace(" ", "")
    text = text.replace("42684268", "720").replace("4268", "360")
    if generalise:
        # A special has one open button: any strength, and EX is a strength the player types.
        text = re.sub(rf"(?<![A-Z])(?:{BUTTON}|PP|KK)$", lambda found: found.group(0)[-1], text)
    text = re.sub(rf"({BUTTON})(?={BUTTON})", r"\1+", text)
    if charge and re.match(r"[1-9]{2}", text):
        text = f"[{text[0]}]{text[1:]}"
    found = re.fullmatch(MOTION + rf"({BUTTONS})?", text)
    if not found or not text:
        return None
    motion, buttons = found.group(1), found.group(2) or ""
    if stance and buttons and motion in ("", "5"):
        motion = stance
    if buttons and hold:
        buttons = f"[{buttons}]"
    if buttons and release:
        buttons = f"]{buttons}["
    return motion + buttons + ("(mash)" if mash and buttons else "")


def notation(move, named, generalise):
    """A move's command as a line of pieces: "236P", "LP > MP > MK", "623P xx FADC"."""
    raw = move.get("numCmd") or ""
    during = re.search(r"\(during ((?:Air )?Scramble)\)", raw)
    repeat = re.search(r"\(?x(\d)", raw)
    raw = re.sub(r"\s+x\d$", "", re.sub(r"\s*\(x\d(~\d)?\)", "", raw))
    raw = re.sub(r"\s*>\s*\(P\)$", "", raw).replace("~", " > ").replace(", ", " > ")
    raw = re.sub(r"(?<!xx) FADC$", " xx FADC", raw)
    if not raw.strip():
        return None
    parts = re.split(r"\s*(>|\bxx\b)\s*", raw)
    texts, joins = parts[0::2], parts[1::2]
    # A button sequence (Raging Demon) is exact buttons, not a special with one open button.
    generalise = generalise and ", " not in (move.get("numCmd") or "")
    pieces = [piece(text, index == 0 and bool(move.get("chargeDirection")), index == 0 and generalise)
              for index, text in enumerate(texts)]
    if not all(pieces):
        return None
    # "(air)" covers the moves from the one it is on; a jump-in that starts with a bare button is all air.
    marked = [index for index, text in enumerate(texts) if "(air)" in text]
    air_from = marked[0] if marked else 0 if move.get("airmove") else None
    if air_from and all(re.fullmatch(rf"[1-9]?{BUTTONS}?", earlier) for earlier in pieces[:air_from]):
        air_from = 0
    if air_from is not None:
        pieces = [("j." + text if index >= air_from and text != "FADC" else text) for index, text in enumerate(pieces)]
    line = pieces[0]
    for join, text in zip(joins, pieces[1:]):
        line += (" xx " if join == "xx" else " > ") + text
    if repeat:
        line = " > ".join([line] * int(repeat.group(1)))
    if during and during.group(1) == "Air Scramble":
        # Decapre's Scramble from a jump; what follows it is in the air too.
        line = "j.[4]6K > " + " > ".join("j." + text for text in line.split(" > "))
    elif during:
        opener = named.get(key(during.group(1)))
        opener = opener and notation(opener, {}, True)
        if not opener:
            return None
        line = opener + " > " + line
    return line


def same_move(a, b):
    return a.replace("[", "").replace("]", "") == b.replace("[", "").replace("]", "")


def main():
    source = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    rows, origin, skipped, unresolved = {}, {}, [], []
    for fighter, data in source.items():
        code = CODES[fighter]
        stance = key(fighter[3:]) if code == "GEN" else ""
        every = [move for moves in data["moves"].values() for move in moves.values()]
        named = {key(move.get("moveName") or ""): move for move in every}
        for move in every:
            name = move.get("moveName") or ""
            special = move.get("moveType") != "normal"
            if move.get("moveType") not in TYPES or GENERIC.match(name):
                continue
            command, exact = notation(move, named, special), notation(move, named, False)
            if not command:
                skipped.append(f"{code} {name}: {move.get('numCmd')}")
                continue
            # The full name keeps its tags: strength, "(3K)", and Gen's stance.
            full = key(name) + stance
            for label in (name, move.get("cmnName") or ""):
                label = re.sub(r"\s*\([^)]*\)", "", label)
                if special:
                    label = STRENGTH.sub("", label)
                short = key(label)
                if not short or short.isdigit():
                    continue
                known = rows.setdefault((code, short), command)
                origin.setdefault((code, short), (full, exact, command))
                if same_move(known, command):
                    # The same move listed with and without its charge: the charged one is right.
                    if "[" in command and "[" not in known and command.replace("[", "").replace("]", "") == known:
                        rows[(code, short)] = command
                    continue
                for tagged, tagged_command, general in (origin[(code, short)], (full, exact, command)):
                    # The move that holds the short name needs no tag when its full name is that name.
                    if tagged == short and same_move(rows[(code, short)], general):
                        continue
                    if tagged == short or not same_move(rows.setdefault((code, tagged), tagged_command), tagged_command):
                        unresolved.append(f"{code} {short}: {tagged} is {rows.get((code, tagged))}, not {tagged_command}")
    for code, nickname, name in NICKNAMES:
        rows.setdefault((code, nickname), rows[(code, name)])
    lines = [
        "// Generated by scripts/generate-combo-moves.py; do not edit by hand.",
        "// {fighter code, name, notation}. Fighter \"\" is everyone. Names are lower",
        "// case letters and digits only (the lookup drops everything else). A name",
        "// may stand for several moves (\"236P > 236P\"). A first move that ends in",
        "// one P or K takes a strength: \"HP Hadoken\" -> 236HP, \"EX Hadoken\" -> 236PP.",
    ]
    lines += [f'    {{"", "{name}", "{command}"}},' for name, command in UNIVERSAL]
    lines += [f'    {{"{code}", "{name}", "{command}"}},' for (code, name), command in sorted(rows.items())]
    target = Path(__file__).resolve().parent.parent / "src" / "training" / "ComboMoves.inc"
    target.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(rows)} names for {len({code for code, _ in rows})} fighters")
    for title, items in (("not expressible", skipped), ("name clashes left", sorted(set(unresolved)))):
        print(f"{len(items)} {title}")
        for item in items:
            print("  " + item)


if __name__ == "__main__":
    main()
