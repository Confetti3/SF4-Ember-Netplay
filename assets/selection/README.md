# Selection reference images

Game imagery is copyright Capcom. These reference photos are separate from the
launcher's MIT-licensed source code. Gallery photographs are credited to
Street Fighter Galleries / slateman, with the Horror pack from Capcom's official
2015 announcement. Exact source URLs, hashes, and option mappings are recorded in
`sources.json`.

`CODE/costume-N/color-N.jpg` or `.png` is the preserved photograph. Indices in
paths are native zero-based values. A sibling `color-N-cutout.png` contains the
same photograph locally masked, cropped, and proportionally resized onto a
256×384 transparent canvas, stored as a 256-color palette PNG. `cutouts.json`
records processing details and hashes; cutouts first made at 512×768 were
halved (premultiplied, then quantized) and keep the original hash in
`resampled_from_sha256`. Only cutouts ship in packages; the preserved
photographs stay in the repository as masking inputs.
No character details or palette colors are generated or recolored.

The runtime prefers cutouts, then PNGs, then JPEGs. It reads installed-game
portraits when an original-costume reference is absent. Other absent previews
remain explicit; an image for one numbered color does not represent another.

The local ONNX model, Python environment, and intermediate masks are build tools
under `build/fighter-selection/masking`; they are not runtime dependencies.

Stage cards use 28 authentic USFIV screenshots from
[SuperSoluce / Pierre Durden's June 16, 2014 stage guide](https://www.supersoluce.com/soluce/ultra-street-fighter-iv/stages).
`stages/CODE.jpg` is a uniform 640×360 image. The game HUD and source watermark
are preserved. `stage-sources.json` records the URLs, original/output hashes,
and native IDs. Original 1280×720 downloads stay in
`build/fighter-selection/stages/originals`. Reproduce with
`python scripts/fetch-stage-references.py` using an environment with Pillow.

The versus catalog includes IDs 0–21 and 24–29. Native table entries 22 and 23
are Car Destruction and Barrel Destruction bonus rounds and are not versus
choices. Gallery image numbering diverges from its headings after image 018;
the fetch script pins the actual links rather than assuming sequential numbers.

Ultra photographs collected during research are retained with provenance in
`ultra-sources.json`, but the selector now uses input symbols instead of photos.
The archive contains 79 photographs: 70 iPlayWinner SSFIV references, four from
Capcom's 2013 showcase, one GamesRadar Decapre reference, and four Capcom / 4Gamer
AE references from 2010-2011. `scripts/fetch-ultra-references.py` reproduces them
with Pillow; two original promotional-video frames additionally use FFmpeg.

The EventHubs SFIV/SSFIV color sheets supply 713 additional numbered photographs.
`color-sources.json` supplements the original gallery manifest. The pinned
`scripts/selection-color-sheets.json` records all 69 original hashes and the
752 crop rectangles; existing default-color gallery photographs take precedence.
`python scripts/fetch-color-references.py` reproduces the RGB-preserving PNG
crops. Apply the same local mask pipeline using `--source-manifest color-sources.json`.
These are authentic photographed palettes, including the stylized colors 11 and
12 where the source contains them. Coverage varies by fighter and costume.

The remaining alternate-costume palettes come from the Street Fighter Wiki's
[Alternate Costumes/Street Fighter IV series](https://streetfighter.fandom.com/wiki/Alternate_Costumes/Street_Fighter_IV_series)
page, which shows every Alternate 1 through Horror costume as one sheet of
in-game screenshots: the two stylized colors, then the numbered colors in order.
`python scripts/fetch-alt-color-sheets.py` downloads the original sheets
(verifying each against the SHA-1 the wiki reports), cuts each into one image
per palette under `build/fighter-selection/alt-crops`, and records the sheet,
crop box and credit for every palette in `alt-color-sources.json`. Palettes
that already have a photograph keep it. These crops are masking inputs only;
the package ships just their cutouts, made with the same pipeline at the
same 256×384 canvas as 256-color palette PNGs (about 20 KB each):
`mask-selection-photos.py assets/selection --source-manifest alt-color-sources.json
--source-root build/fighter-selection/alt-crops --canvas 256x384 --margin 16 --colors 256`.
The fit and margin scale with the canvas, so the framing matches the
original 512×768 cutouts.
