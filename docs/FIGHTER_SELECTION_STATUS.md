# Arcade fighter selector

The shared selector replaces raw character bytes with a 44-fighter roster,
individual costume and color cards, named Ultra choices, and edition options
filtered for the selected fighter. It is used in the room and offline versus UI.

## Selection rules

`FighterCatalog` owns supported combinations. `Dimps__Selection` reads the local
game's costume, color, and personal-action availability. The UI normalizes saved
choices and prevents Ready until the game reports an available combination.
The session server validates supported combinations independently and freezes
each player's selection after Ready. A peer's DLC is never checked against the
receiving player's DLC ownership.

| Choice | Supported range |
| --- | --- |
| Fighter | 44 native IDs, 0–43 |
| Edition | SFIV (13), SSFIV (1), AE (2), AE 2012 (4), USFIV (14), Omega (16); filtered by fighter |
| Ultra | SFIV: I; USFIV: I, II, Double; other supported editions: I, II |
| Costume | Original plus six alternates for the original 25 fighters; original plus five for later fighters; filtered by local availability |
| Color | 01–12 on earlier outfits, 01–22 on Vacation/Wild/Horror; filtered by local availability |
| Personal action | None or actions 1–10; filtered by local unlocks |
| Win quote | Random or quotes 1–11 |
| Handicap | Normal, one hit, 25%, 50%, 75% |
| Stage | 28 versus arenas, native IDs 0–21 and 24–29; host controls the room stage |

Wire bytes are parsed as integers in 0–255 before narrowing. Negative, fractional,
boolean, string, null, and oversized values are rejected. None/Random use the
native byte sentinel 255. The unused reserved byte remains a byte, not a UI choice.

## Reference images and masking

- The installed game supplies 44 native transparent roster portraits at runtime.
- [Street Fighter Galleries](https://www.sfgalleries.net/art/sf4/usf4/alt/)
  supplies 201 alternate-outfit photographs.
- [Capcom's October 2015 Horror announcement](https://news.capcomusa.com/lets/browse/beware-ultra-street-fighter-iv-horror-costumes-are-coming)
  supplies 44 final-pack photographs.
- EventHubs' 2010 SFIV/SSFIV color sheets supply 713 additional numbered
  photographs across 69 costume sets. `color-sources.json` records each cell's
  source-sheet hash and crop rectangle. Cropped PNGs retain the original decoded
  RGB pixels; no colors are synthesized. Existing default-color photos take
  precedence over 39 overlapping sheet cells.
- Ultra selection uses motion arrows and button symbols beneath each move name.
  Double shows both commands. The 79 previously collected Ultra photos are
  retained as reference assets and are not displayed in the selector.
- `assets/selection/sources.json` records each photograph's exact URL, native
  fighter/costume/color mapping, credit, and SHA-256. Two unavailable full-size
  images (Elena alternate 3 and T. Hawk alternate 4) use the gallery thumbnail;
  their manifest entries state that resolution limitation.
- Gallery boss filenames use Japanese names: `mbison` is Boxer (`BSN`), `balrog`
  is Claw (`BLR`), and `vega` is Dictator (`VEG`). English UI names differ.

The user approved local masking and cropping after image generation altered
details and failed to produce transparency. `scripts/mask-selection-photos.py`
uses local BiRefNet segmentation for alpha only. The photograph supplies RGB;
the pipeline crops and proportionally resizes it into a transparent 512×768 PNG
with 32-pixel margins and bottom alignment at y=736. Wide poses naturally occupy
less height. Source photographs remain unchanged alongside `color-0-cutout.png`.
The image loader prefers those cutouts. `cutouts.json` records output hashes,
source bounds, scale, offsets, and review state. Model weights and intermediate
masks remain under `build/fighter-selection/masking`.

Reproduction from the repository root:

```powershell
python -m venv build/fighter-selection/masking-venv
& build/fighter-selection/masking-venv/Scripts/python.exe -m pip install -r scripts/selection-masking-requirements.txt
& build/fighter-selection/masking-venv/Scripts/python.exe scripts/mask-selection-photos.py assets/selection
& build/fighter-selection/masking-venv/Scripts/python.exe scripts/fetch-color-references.py
& build/fighter-selection/masking-venv/Scripts/python.exe scripts/mask-selection-photos.py assets/selection --source-manifest color-sources.json
& build/fighter-selection/masking-venv/Scripts/python.exe scripts/review-selection-cutouts.py assets/selection --complete
```

The validator checks original hashes, output hashes, RGBA, fixed canvas size,
transparency, margins, and bottom alignment. Contact sheets are for visual review;
passing numeric checks alone does not establish segmentation quality.

The initial 245-image masking pass is complete. All nine contact sheets were visually
reviewed. Eight source-bound alpha corrections remove attached spectators or
stage lines and restore Ryu's sword hilt; their polygons and reasons are in
`scripts/selection-mask-overrides.json`. The three boss mappings were corrected
without changing photo bytes; the previous mapping is backed up in
`build/fighter-selection/boss-mapping-before.zip`. Some Horror source photographs
crop limbs or use very wide poses; their cutouts preserve that source framing.

Builds copy the assets beside Sidecar, CMake installs the `SelectionReferences`
component, and team packaging includes installed assets and notices. The updater
accepts only known selection metadata and exact fighter/costume/color image paths.
Its path tests reject unsupported option indices, traversal, alternate streams,
and executable suffixes.

At completion of the initial masking pass: 245/245 RGBA cutouts, zero source/hash/framing errors;
all 494 asset files match byte-for-byte in both the Sidecar build directory and
the isolated CMake package preview. The final renderer run loaded all 44 native
portraits, all 245 cutouts, and the original-outfit fallback, then passed 570
frames across five viewport/DPI configurations. The x86 Sidecar and Updater build
passed, as did the expanded catalog/path checks and six related UI/session tests.

## Evidence and validation

The stage picker adds a host-only **Stage** tab beside Fighter, Appearance, and
Ultra Combo. Offline versus setup has a separate STAGE tab shared by P1/P2.
Every option has a 640×360 screenshot, an orange selected border, and a larger
selected-stage preview. The responsive grid supports the same ImGui keyboard
and gamepad navigation as the costume cards.

The 28 versus stages use native IDs 0–21 and 24–29 from code-table RVA 0x66b678.
Car Destruction (22/GAS) and Barrel Destruction (23/SCX) are bonus rounds and
are excluded. Stage names correct the old native table's abbreviations/typos.
The photos come from SuperSoluce's June 16, 2014 USFIV stage guide; source links,
hashes, executable identity, and mappings are in `assets/selection/stage-sources.json`.
All 28 were checked on a contact sheet. The gallery's file-number gaps after
Cruise Ship Stern are explicitly mapped in `scripts/fetch-stage-references.py`.
Original downloads are preserved under `build/fighter-selection/stages/originals`.

Preferences repair invalid/bonus stages to Training Stage. Network messages
reject malformed JSON before integer narrowing, unsupported stage IDs, and
non-host changes. P1's Ready freezes the stage; P2 may ready first because the
existing protocol sends P1's stage immediately before P1's Ready. An active
match authority also blocks stage changes. The game-facing consumer checks
the received 64-bit stage ID before indexing the native stage table.

Initial stage validation: all 28 images decoded and uploaded to DX9; 645 render frames
passed at five viewport/DPI settings, including actual stage-card activation
to native ID 29 and removal of the host tab on switching to P2. Catalog/path,
strict-byte, client/server transport, legacy transport, and legacy session
integration checks passed. Server cases cover all 28 IDs, invalid/overflow/type
inputs, P2 rejection, P2-ready-first ordering, and P1 Ready freezing. The x86
Sidecar and Updater builds pass. That stage-only asset snapshot totaled 523 files,
including 28 stage photos and their source manifest. These are local build and
hidden-render checks; native stage loading and controller input in the game
still need a gameplay test.

Native analysis targets Steam USFIV 1.05 `SSFIV.exe`, SHA-256
`5d724595a8ab3c6c6d6f4959187f756f5be35bb497e51e5233c4e73b18b0b9eb`.
Local evidence is under `build/fighter-selection`, including option-evidence.json
and saved IDA decompilations. The edition matrix is RVA 0x539ba8; earlier costume
counts are RVA 0x566158. Ember costume availability uses the low ownership byte
tested by VA 0x69ffd0, matching native saved/match-choice validation. The high
menu mask tested by VA 0x69ff90 can omit owned DLC at the main menu and must not
filter Ember's gallery. See [the costume and menu repair evidence](COSTUME_MENU_TRAINING_FIX.md). Ultra
counts are confirmed in MovieUCWindow initialization at VA 0x4339e0. Personal
action and win-quote wrapping are confirmed in their native menu functions.

The x86 build and catalog, strict-byte, UI, client/server transport, legacy
transport, and legacy session integration tests have passed. The expanded server
test also checks malformed bytes, unsupported combinations, slot isolation, and
Ready freezing. `UiRenderTest` renders the real ImGui/DX9 code in a hidden window
at five viewport/DPI settings. Its optional game-root argument verifies all 44
native portrait decodes; its costume check verifies every alternate image through
the actual file decoder and DX9 uploader. This does not launch the game.

## Color and Ultra expansion validation

The 713-photo color masking batch is complete, with source-traced floor-line
corrections. There are 958 preserved photos and 958 transparent 512×768 cutouts.
The color contact sheets were visually inspected, and the validator reports
zero source/hash/transparency/framing errors. An independent pixel comparison
also confirms that all 713 PNG crops exactly preserve their source sheets'
decoded RGB pixels. CUDA is an optional local build tool; the default reproduction
uses CPU and the launcher has no ONNX/CUDA runtime dependency. The validator
also rejects a cutout whose recorded source hash no longer matches its photo.

The expanded renderer passed 735 frames across five viewport/DPI settings,
loading all 44 native portraits, 245 alternate-outfit cutouts, 28 stage photos,
75 Ultra photos, and 757 PNG-backed appearance cutouts (713 new colors plus
44 Horror defaults). It exercises the real decoder/cache/DX9 upload in bounded
batches and activates Color 12, Ultra Double, and a stage through actual UI
cards. Selected cards use the large preview without an overlapping duplicate
zoom tooltip. The latest x86 builds and six catalog/value/session checks passed.

The final local build and isolated `package-preview` each contain all 2,026
selection asset files. Every relative path and SHA-256 matches the source asset
directory, with no missing, extra, or changed files. Evidence is recorded in
`build/fighter-selection/package-validation.json`; the final 958-cutout report
is `build/fighter-selection/masking/final-review/validation.json`.

## Remaining work

- Of 4,788 supported costume/color combinations, 958 have matching photographs
  and 14 further default combinations use native portrait fallback. The other
  3,816 combinations have no matching preview. Every costume has a default
  preview, but this is not complete color-photo coverage. Local unlock/DLC
  availability further filters which combinations a player can select.
- Ultra photos have been replaced by vector command symbols at the user's
  request; incomplete Ultra photo coverage is no longer a UI requirement.
- Verify selection availability and confirmed choices in the native game,
  including controller navigation, two-player/offline selection, and Ready/rematch.
  Build and hidden-render success are not native gameplay evidence.

The existing checkout had unrelated Iroh/ImGui changes. Before this selector work,
its files and diff were backed up to `build/fighter-selection/before-selection.zip`
and `before-selection.patch`. No selector commit, publication, or game deployment
has been performed.

## Ultra symbol selection update

The Ultra page shows named moves with vector motion arrows and arcade button
symbols, using EventHubs notation as the visual reference. Edition restrictions
still control which choices appear. Ultra Double includes both commands.
Charge, stance, air and proximity conditions are explicit. Standard/Omega Decapre
and SSFIV/AE Bison use different inputs. The fighter overview uses the native
portrait, and Ultra move photos are unused. See `ULTRA_INPUTS.md` for sources.

The retained reference archive increased from 75 to 79 Ultra photos before this
design change. Source and output hashes remain recorded in the manifest.

The updated Sidecar and UiRenderTest builds passed. FighterCatalogTest passed
the command coverage, invalid-combination and edition-exception checks. The
final renderer passed 825 DX9 frames at five viewport/DPI configurations,
including Abel, Guile, Gen, Oni and standard/Omega Decapre command layouts.
The local build and isolated package-preview each contain 2,030 selection files,
with matching relative paths and SHA-256 hashes. Gameplay remains untested.

## Completion audit

The symbol implementation and its tests were concrete progress. The user has
explicitly deferred additional palette captures: missing images display
"Preview not available", while valid choices stay selectable. The photo count is
reproducible with `python scripts/audit-selection-coverage.py`; its output lists
each missing numbered color separately in `build/fighter-selection/coverage-audit.json`.

| Requirement | Current evidence | Status |
| --- | --- | --- |
| Arcade character and edition selection | Shared native UI, 44 portraits, edition matrix, catalog and render tests | Implemented; native interaction unverified |
| Individual costumes and colors | 289 costume choices and bounded numbered color cards | Implemented; missing previews explicitly deferred by the user |
| Original photos masked to equal size | 958 transparent 512x768 cutouts; final source/hash/framing report | Verified for supplied photos |
| Stages with photos | 28 versus IDs and mapped stage photos; actual UI stage activation test | Implemented; native stage loading unverified |
| Ultra inputs instead of photos | Both commands for 44 fighters, edition exceptions, symbol renderer, Double activation test | Implemented; native move execution unverified |
| Bound ranges to selectable options | Catalog, local availability reader, strict byte parsing, server Ready freeze tests | Automated checks passed; live unlock/DLC checks unverified |

An additional source search located a 2014 all-color Vacation playlist and a
2015-era numbered costume gallery. Both lead to hosts already inaccessible in
this session; no new photographs were imported or existing palettes recolored.
The user initially chose installed-game captures, then cancelled that work and
requested the unavailable-preview label instead. No new game captures were
imported. Native gameplay validation remains a known limitation of this local
implementation; it is not evidence of completed gameplay testing.

## Solo selection preview

Home now offers **Preview character selection**, with **Selection preview** also
available in the top navigation. It uses its own fighter, costume, color, Ultra,
edition and stage state. It requires no room, network helper or opponent, and
never submits session commands. All supported catalog choices are browseable;
the actual room and offline match selectors retain native unlock/DLC checks.
Preview choices persist across page changes within the current game session.
See `SELECTION_PREVIEW_TEST.md` for the test-build entry point.
