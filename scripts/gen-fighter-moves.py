#!/usr/bin/env python3
"""The fighters' own moves for the dummy reply's pick list (src/training/FighterMoves.inc):
scripts/fixtures/usf4-command-moves.tsv (every move the game's .bcm lets the fighter start from neutral, keyed
in our notation, named by the wiki where the lab's join found the move; lab framedata/usf4-pick-moves.py) with the fighter codes of src/common/FighterMetadata.inc.
Moves every fighter has (plain normals, dashes, throws, jumps) are UniversalMoves in FighterMoves.hxx.
  gen-fighter-moves.py scripts/fixtures/usf4-command-moves.tsv src/common/FighterMetadata.inc src/training/FighterMoves.inc"""
import csv, re, sys
tsv, meta, out = sys.argv[1:4]
codes = {}
for line in open(meta, encoding='utf-8'):
    m = re.match(r'\s*\{"([A-Z0-9]{3})", "([^"]+)"', line)
    if m: codes[m.group(2)] = m.group(1)
universal = re.compile(r'^(cl\.|far\.|j\.|[25789])?[LMH][PK]$|^[456]?LP\+LK$|^(44|66)$')  # a command normal (6MP) is the fighter's
rows = []
for r in csv.DictReader(open(tsv, encoding='utf-8'), delimiter='\t'):
    code, key = codes.get(r['fighter']), r['game_key'].strip()
    name, strength = r['move'].strip(), r['strength'].strip()
    # A follow-up or a later part is not a reply; one row per input, under its first name.
    if not code or not key or key.count('+') > 1 or universal.match(key) or 'Followup' in name or 'Part ' in name or any(c == code and k == key for c, _, k in rows): continue
    if strength and strength not in name and strength != 'EX' and not name.startswith('EX '): name += ' ' + strength
    rows.append((code, name, key))
# An EX move the game gives each button pair its own version of (Juri's Fuhajin, Decapre's Scramble) is named by its pair; a pair
# alone (Guy's Senpukyaku, one move for any two kicks) is the plain two-button EX.
PAIR = re.compile(r'([LMH])([PK])\+[LMH]\2')
for i, (code, name, key) in enumerate(rows):
    pair = PAIR.search(key)
    if not pair: continue
    siblings = [k for c, _, k in rows if c == code and k != key and PAIR.sub(r'\2\2', k) == PAIR.sub(r'\2\2', key)]
    if siblings:
        if pair[0] not in name: name += ' ' + pair[0]
    else: key = PAIR.sub(pair[2] * 2, key)
    rows[i] = (code, name, key)
# The game's own inputs (.bcm, as the atlas writes them) where the join finds none or a wrong one: a row with that key
# is renamed, else added. Teleports: one wiki row, four inputs. Release moves: the dummy holds the button through the
# stun and lets go on its first free frame; the EX Q-bomb's 210 frames of charge outlast a knockdown, so it holds first.
TELEPORT = [(d, m, b) for d, m in (('Forward', '623'), ('Back', '421')) for b in ('PPP', 'KKK')]
HAND = [(code, f'{move} {d} {b}', air + m + b) for code, move, air in (('GKI', 'Ashura Senku', ''), ('RYX', 'Ashura Senku', ''),
        ('DSM', 'Yoga Teleport', ''), ('DSM', 'Air Yoga Teleport', 'j.'), ('BOS', 'Yoga Teleport', ''), ('VEG', 'Bison Warp', ''))
        for d, m, b in TELEPORT] + [
    ('BLK', 'Shout of Earth (Anti-Air)', '[1]318PPP'), ('BLK', 'Shout of Earth (Anti-Ground)', '[1]318KKK'),
    ('AGL', 'Burning Dance', 'j.214214KKK'), ('VEG', 'Psycho Punisher', '[4]646PPP'), ('VEG', 'Devil Reverse', '[2]8LP'),
    ('VEG', 'EX Devil Reverse', '[2]8PP'), ('GUY', 'EX Bushin Izuna Otoshi', '236PP'), ('GKX', 'Anti-Air Meido Gohado', '236236KKK'),
    ('RLN', 'Patriot Sweeper', '236236PPP'), ('RLN', 'Back Roll LP', '214LP'), ('RLN', 'Back Roll MP', '214MP'),
    ('RLN', 'Back Roll HP', '214HP'), ('RLN', 'EX Back Roll', '214PP'), ('HWK', 'EX Condor Dive', 'j.KKK'),
    ('JRI', 'Fuhajin Store LK', '236LK'), ('JRI', 'Fuhajin Store MK', '236MK'), ('JRI', 'Fuhajin Store HK', '236HK'),
    ('JRI', 'Fuhajin Release LK', ']LK['), ('JRI', 'Fuhajin Release MK', ']MK['), ('JRI', 'Fuhajin Release HK', ']HK['),
    ('CDY', 'Zonk Knuckle LP', ']LP['), ('CDY', 'Zonk Knuckle MP', ']MP['), ('CDY', 'Zonk Knuckle HP', ']HP['), ('CDY', 'EX Zonk Knuckle', ']PP['),
    ('RIC', 'Quesadilla Bomb LK', ']LK['), ('RIC', 'Quesadilla Bomb MK', ']MK['), ('RIC', 'Quesadilla Bomb HK', ']HK['),
    ('RIC', 'EX Quesadilla Bomb', '[KK]210f > ]KK['), ('BSN', 'Turn Punch PPP', ']PPP['), ('BSN', 'Turn Punch KKK', ']KKK[')]
for code, name, key in HAND:
    at = next((i for i, (c, _, k) in enumerate(rows) if c == code and k == key), None)
    if at is None: rows.insert(max((i + 1 for i, (c, _, _) in enumerate(rows) if c == code), default=len(rows)), (code, name, key))
    else: rows[at] = (code, name, key)
# A reply from wake-up as the game takes it (tried in game): a height-gated air move pressed on a frame of the jump
# past its .bcm height (Tenmakujinkyaku, Body Press), a pickup a dash on to the item. Teiga is timed from its jump,
# not from a frame: its style key is a style change or a normal, of different lengths.
REPLY = {('BLR', '2LP+HP'): '66 > 2LP+HP', ('BLR', '2LP+MP'): '66 > 2LP+MP', ('BLR', '2MP+HP'): '66 > 2MP+HP', ('CDY', '2PPP'): '66 > 2PPP',
         ('RYX', 'fj.2MK'): '9 > 2MK#22', ('HUG', 'j.2HP'): '8 > 2HP#26', ('GEN', '5KKK > j.236236KKK'): '5KKK > 8 > ~ 236236KKK@+8'}
rows = [(c, n, REPLY.get((c, k), k)) for c, n, k in rows]
with open(out, 'w', encoding='utf-8', newline='\n') as f:
    f.write("// Generated by scripts/gen-fighter-moves.py from scripts/fixtures/usf4-command-moves.tsv: {code, name, moves}.\n")
    for code, name, key in rows: f.write('    {"%s", "%s", "%s"},\n' % (code, name.replace('"', '\\"'), key))
print(len(rows), 'rows,', len({c for c, _, _ in rows}), 'fighters')
