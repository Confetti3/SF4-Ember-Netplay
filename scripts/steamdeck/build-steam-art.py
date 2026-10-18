"""Regenerate the Steam library artwork in assets/steamdeck from the brand art (requires Pillow).

    uv run --with pillow python scripts/steamdeck/build-steam-art.py

The Steam Deck installer copies these into Steam's grid folder for the
SF4 Ember Netplay shortcut. Sizes are the ones Steam asks for.
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

root = Path(__file__).resolve().parents[2]
out = root / "assets/steamdeck"
out.mkdir(exist_ok=True)
background = Image.open(root / "assets/brand/ember-menu-background.png").convert("RGBA")
mark = Image.open(root / "src/ui/ember.png").convert("RGBA")
font = str(root / "src/ui/fonts/Inter-SemiBold.ttf")
cream, ember = (242, 234, 222), (255, 135, 52)


def cover(w, h):
    """The background scaled to fill w x h, keeping the mountains on the right in view."""
    scale = max(w / background.width, h / background.height)
    image = background.resize((round(background.width * scale), round(background.height * scale)), Image.Resampling.LANCZOS)
    left = image.width - w
    top = (image.height - h) // 2
    return image.crop((left, top, left + w, top + h))


def logo(w):
    """The mark beside "EMBER" and "SF4 NETPLAY", on transparency, w pixels wide."""
    h = w * 3 // 8
    image = Image.new("RGBA", (w, h))
    image.alpha_composite(mark.resize((h, h), Image.Resampling.LANCZOS))
    draw = ImageDraw.Draw(image)
    draw.text((h * 1.05, h * 0.18), "EMBER", font=ImageFont.truetype(font, h * 40 // 100), fill=cream)
    draw.text((h * 1.08, h * 0.64), "SF4 NETPLAY", font=ImageFont.truetype(font, h * 15 // 100), fill=ember)
    return image


capsule = cover(600, 900)
capsule.alpha_composite(logo(520), (40, 80))
capsule.convert("RGB").save(out / "cover.png")
wide = cover(920, 430)
wide.alpha_composite(logo(520), (40, 120))
wide.convert("RGB").save(out / "wide.png")
cover(1920, 620).convert("RGB").save(out / "hero.png")
logo(1280).save(out / "logo.png")
mark.resize((256, 256), Image.Resampling.LANCZOS).save(out / "icon.png")
