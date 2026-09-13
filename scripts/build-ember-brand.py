"""Regenerate icon/atlas bytes from the approved PNG (requires Pillow).

Only size and format change; the original artwork is kept intact. Normal product
builds consume these checked-in assets and do not require Python or Pillow.
"""
from pathlib import Path
from PIL import Image

root = Path(__file__).resolve().parent.parent / "src/ui"
with Image.open(root / "ember.png") as source:
    image = source.convert("RGBA")
    image.save(root / "ember.ico", sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
    atlas = image.resize((128, 128), Image.Resampling.LANCZOS)
    (root / "ember.rgba").write_bytes(atlas.tobytes())
