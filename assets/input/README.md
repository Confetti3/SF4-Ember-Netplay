# Input glyphs

Original, unmodified 128-pixel PNG assets from **Kenney Input Prompts 1.5A**, distributed under CC0. See `License.txt`.

Source: https://kenney.nl/assets/input-prompts

Archive downloaded 2026-09-08: `kenney_input-prompts_1.5.zip`.
SHA-256: `AC2FCF599080B0F3BA2D174C9474DB6DF1A0E96FF0662580E2DA79A122AB78A1`.

Selected assets come from `Xbox Series/Double`, `Keyboard & Mouse/Double`, `Generic/Double`, `PlayStation Series/Double`, `Nintendo Switch/Double`, `Steam Controller/Double`, and `Flairs/Double`. The selected Xbox Series, PlayStation Series and Nintendo Switch assets also include the left and right stick-click prompts. The runtime uses the original sprites; no replacement button shapes are drawn in code. Xbox menu defaults are physical A for Select and B for Back, independently of fighting mappings. Unidentified DirectInput shapes use the generic sprite with an explicit mapped LP/LK action label.

## Ember's own mark

`mark_developer.png` is not from Kenney: it is Ember's developer mark, drawn beside a developer's name in place of a class (`src/ui/NameMarks.cxx`). It is the project's flame "DEV" lettering (2508 x 627 on black) cut out with its glow as alpha (alpha from the brightest channel, colour unpremultiplied), cropped to the glow and resized to 256 x 116 RGBA. It sits in this folder because a 1.3 updater accepts new PNGs here by pattern (`PackageInventory.hxx`), so the update needs no reinstall.

SHA-256: `B3D6CAA086242F85BA3DDFFB5DE7E71CDE61A520168B50ECDA75FFED8CEBC675`.
