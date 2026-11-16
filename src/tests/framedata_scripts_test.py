# scripts/framedata: build.py turns the simulator's per-fighter files into the table (cells as text, "game showed" notes,
# measured set, KD, signed advantage, overrides that must match one move) and validate.py refuses a broken table.
import contextlib, importlib.util, io, json, os, sys, tempfile
sys.dont_write_bytecode = True

def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(sys.argv[1], name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

build, validate = load("build"), load("validate")

def move(name, inp, **kw):
    m = {"section": "Normals", "name": name, "input": inp, "startup": 4, "active": 2, "recovery": 8, "total": 14, "on_block": 1,
         "on_hit": 5, "hitstun": 12, "blockstun": 9, "invuln": None, "script": 100, "calibrated": {}, "status": {}, "notes": []}
    m.update(kw)
    return m

root = tempfile.mkdtemp()
sim = os.path.join(root, "sim")
os.mkdir(sim)
json.dump({"fighter": "Ryu", "generated": "g", "sim": "s", "moves": [
    move("Standing LP", "5.LP", calibrated={"startup": 4, "on_hit": 6}, status={"startup": "match", "on_hit": "sim_differs_calibration"}),
    move("Sweep", "2.HK", on_hit=0, knockdown=True, on_block=-3)]}, open(os.path.join(sim, "RYU.json"), "w"))
json.dump({"all": "summary"}, open(os.path.join(sim, "ALL.json"), "w"))  # no "moves": skipped
out = os.path.join(root, "t.json")
overrides = os.path.join(root, "o.json")

def run():
    with contextlib.redirect_stdout(io.StringIO()):
        build.main([sim, "--out", out, "--overrides", overrides])
    return json.load(open(out))

def ryu(table):
    return {row[2]: dict(zip(build.COLS, row)) for row in table["fighters"]["Ryu"]}

def refused(change):
    json.dump(change, open(overrides, "w"))
    try:
        run()
    except SystemExit as e:
        return bool(e.code)
    return False

good = run()
assert good["meta"]["moves"] == 2 and list(good["fighters"]) == ["Ryu"]
lp, sweep = ryu(good)["5.LP"], ryu(good)["2.HK"]
assert lp["strength"] == "LP" and lp["block_adv"] == "+1" and sweep["block_adv"] == "-3" and sweep["hit_adv"] == "KD"
assert lp["inv"] == "-" and lp["measured"] == "startup" and lp["extra"] == "game showed on_hit 6", lp
assert validate.check(out) == []

json.dump({"Ryu": [{"input": "5.LP", "set": {"startup": 5}}]}, open(overrides, "w"))
assert ryu(run())["5.LP"]["startup"] == "5"
assert refused({"Ryu": [{"input": "5.HP", "set": {"startup": 5}}]}), "an override that matches no move is refused"
assert refused({"Ken": [{"input": "5.LP", "set": {"startup": 5}}]}), "an override for a fighter not in the table is refused"

def broken(edit):
    table = json.loads(json.dumps(good))
    edit(table["fighters"]["Ryu"])
    json.dump(table, open(out, "w"))
    return validate.check(out)

def short_total(rows):
    rows[0][build.COLS.index("total")] = "2"

def twice(rows):
    rows.append(list(rows[0]))

def odd_section(rows):
    rows[0][0] = "Moves"

def worded_startup(rows):
    rows[0][build.COLS.index("startup")] = "abc"

def short_row(rows):
    rows[0].pop()

assert broken(short_total), "total shorter than startup"
assert broken(twice), "listed twice"
assert broken(odd_section), "unknown section"
assert broken(worded_startup), "startup not a count"
assert broken(short_row), "short row"
