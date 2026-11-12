# The phone remote's page (src/training/RemotePage.hxx): every element its script looks up by id exists, and the
# Frames tab reads only columns that usf4.json has. With --syntax instead of the table: the script parses (needs node).
import json, os, re, shutil, subprocess, sys, tempfile
sys.dont_write_bytecode = True

page = sys.argv[1]
chunks = re.findall(r'R"HTML\(([\s\S]*?)\)HTML"', open(page, encoding="utf-8").read())
html = "".join(chunks)
scripts = re.findall(r"<script>([\s\S]*?)</script>", html)
assert len(scripts) == 1, f"{len(scripts)} scripts"
script = scripts[0]

if sys.argv[2] == "--syntax":
    node = shutil.which("node")
    if not node:
        sys.exit(77)
    # A syntax error in the script is a blank page on the phone.
    path = os.path.join(tempfile.mkdtemp(), "page.js")
    open(path, "w", encoding="utf-8").write(script)
    result = subprocess.run([node, "--check", path], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    sys.exit(0)

ids = set(re.findall(r'\bid="([\w-]+)"', html)) | set(re.findall(r'\.id="([\w-]+)"', script))
missing = set(re.findall(r'\$\("([\w-]+)"\)', script)) - ids
assert not missing, f"the script looks up ids the page does not have: {sorted(missing)}"
cols = set(json.load(open(sys.argv[2], encoding="utf-8"))["cols"])
shown = set()
for name in ("FC", "FX"):
    m = re.search(rf"const {name}=\[([^\]]*)\]", script)
    assert m, f"no {name} list"
    shown |= set(re.findall(r'"(\w+)"', m.group(1)))
paint = re.search(r"function paintFrames\(\)\{([\s\S]*?)\nfunction ", script)
assert paint, "no paintFrames"
shown |= set(re.findall(r"\br\.(\w+)", paint.group(1)))
assert shown <= cols, f"the Frames tab reads columns usf4.json lacks: {sorted(shown - cols)}"
