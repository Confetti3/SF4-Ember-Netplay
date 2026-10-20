# Splits docs/guides/ACCESSIBILITY.md into the text read aloud for each section that links a recording
# ("[Listen to this section](audio/accessibility/<name>.ogg)"). Prints one JSON line per section:
# {"name": ..., "text": ...}. Markdown is turned into plain sentences; links and the listen lines are dropped.
import json, re, sys
text = open(sys.argv[1], encoding="utf-8").read()
for block in re.split(r"\n(?=## )", text):
    found = re.search(r"\[Listen to this section\]\(audio/accessibility/([\w-]+)\.ogg\)", block)
    if not found:
        continue
    lines = []
    for line in block.splitlines():
        line = line.strip()
        if not line or line.startswith("[Listen"):
            continue
        line = re.sub(r"^#+\s*", "", line)
        line = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", line).replace("`", "")
        if line.startswith("- "):
            line = line[2:]
        lines.append(line)
    # A bullet or heading is its own sentence; wrapped lines join with a space.
    sentences, current = [], ""
    for line in lines:
        current = (current + " " + line).strip()
        if current.endswith((".", ":", "?", "!")) or line == lines[0]:
            sentences.append(current if current.endswith((".", ":", "?", "!")) else current + ".")
            current = ""
    if current:
        sentences.append(current + ".")
    print(json.dumps({"name": found.group(1), "text": " ".join(sentences)}))
