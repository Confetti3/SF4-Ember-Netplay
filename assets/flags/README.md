# Country flags

`flags.png` holds the 4x3 flag of every ISO 3166-1 alpha-2 country in `scripts/countries.json`, one 32x24 cell each with 2 transparent pixels around it, 16 cells to a row, in code order. `flags.json` maps each code to its cell and records the layout. `src/common/Countries.hxx` is written from the same manifest, so a code's index there is its cell here.

Source: [flag-icons](https://github.com/lipis/flag-icons) v7.5.0 by Panayiotis Lipiridis, licensed MIT. The distributed licence is `LICENSE`. The release archive is `https://github.com/lipis/flag-icons/archive/refs/tags/v7.5.0.zip`, SHA-256 `7eafedfc07e16ce640dd53309fbe147be0dcfd36d027646cf3798aea79a18913`; the release publishes no checksum of its own. Only `flags/4x3/*.svg`, `country.json` and `LICENSE` were read from it.

To rebuild from an extracted release:

```
node scripts/build-flag-atlas.mjs --flags <flag-icons-7.5.0 folder>
python -P scripts/check-flag-atlas.py
```

The embernetplay.link rankings page serves a copy, `server/ember-short/deploy/site/assets/flags.png` with this licence beside it as `flags-LICENSE.txt`, and lists the codes in cell order in `assets/rankings.js` (`FLAGS`). Copy both files and update that list whenever the atlas is rebuilt.

The atlas is rendered by headless Edge at device scale 1. Each flag is drawn several times over itself so that stripes meeting inside a pixel do not leave a partly transparent seam; Nepal's flag is the one that is not a rectangle.
