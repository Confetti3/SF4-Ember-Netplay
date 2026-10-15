"""Builds site/assets/og.jpg, the 1200x630 picture Discord and other sites show
for an embernetplay.link link: the menu artwork under the site's dark tint,
the emblem, EMBER and one line about Ember, in the site's Inter. Run from the
repository root:

    python server/ember-short/deploy/make-og.py

Needs Pillow with FreeType (which reads the WOFF2 fonts).
"""
import os

from PIL import Image, ImageDraw, ImageFont

WIDTH, HEIGHT = 1200, 630
INK = (20, 19, 18)
IVORY = (243, 235, 221)
MUTED = (181, 169, 155)
EMBER = (255, 135, 56)


def main():
    site = os.path.join(os.getcwd(), "server", "ember-short", "deploy", "site")
    asset = lambda *parts: os.path.join(site, "assets", *parts)
    art = Image.open(asset("ember-background.jpg")).convert("RGB")
    scale = max(WIDTH / art.width, HEIGHT / art.height)
    art = art.resize((round(art.width * scale), round(art.height * scale)), Image.LANCZOS)
    left = (art.width - WIDTH) * 7 // 10
    top = (art.height - HEIGHT) // 2
    canvas = art.crop((left, top, left + WIDTH, top + HEIGHT)).convert("RGBA")

    # The site's tint: dark on the left where the text sits, lighter on the right.
    tint = Image.new("RGBA", (WIDTH, HEIGHT))
    pixels = tint.load()
    for x in range(WIDTH):
        alpha = round(255 * (0.95 - 0.5 * max(0.0, (x / WIDTH - 0.35) / 0.65)))
        for y in range(HEIGHT):
            pixels[x, y] = INK + (alpha,)
    canvas.alpha_composite(tint)

    emblem = Image.open(asset("ember-emblem.png")).convert("RGBA").resize((150, 150), Image.LANCZOS)
    canvas.alpha_composite(emblem, (80, 150))
    draw = ImageDraw.Draw(canvas)
    heavy = ImageFont.truetype(asset("fonts", "inter-latin-600.woff2"), 112)
    small = ImageFont.truetype(asset("fonts", "inter-latin-600.woff2"), 28)
    body = ImageFont.truetype(asset("fonts", "inter-latin-400.woff2"), 40)
    draw.text((254, 150), "EMBER", font=heavy, fill=IVORY)
    draw.text((260, 284), "S F 4   /   R O L L B A C K   N E T P L A Y", font=small, fill=EMBER)
    draw.rectangle((84, 380, 84 + 96, 384), fill=EMBER)
    draw.text((84, 410), "Rollback netplay rooms, public rooms", font=body, fill=IVORY)
    draw.text((84, 462), "and tournaments for Ultra Street Fighter IV", font=body, fill=MUTED)

    out = asset("og.jpg")
    canvas.convert("RGB").save(out, "JPEG", quality=86, optimize=True, progressive=True)
    print(f"{out}: {os.path.getsize(out)} bytes")


if __name__ == "__main__":
    main()
