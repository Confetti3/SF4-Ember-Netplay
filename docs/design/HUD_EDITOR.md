# HUD layout editor (Remote tab)

Branch `fraccie/hud-editor` = `fraccie/menu-looks` (in-game themes) + `fraccie/training-remote`.
Mockup: https://claude.ai/artifact/BA7EFyoCVLsauBemdFZ5eM (desktop editor, phone notice, Auto-apply toggle).
Reference implementation (Python, lab only): `D:\ember-build\lab\cyberpunk\editor3.py`, `editor3.html`, `ems.py`, `texio.py`, `layout.py`, `cluster.py`.

## How it works
- A theme keeps its original `battle\hud\CMN\*.ems.emz`. The editor saves `layout.json` beside it
  (per element: x, y, rotation, scale, hide, mirror). The theme is never rewritten.
- Applying = Ember reads the theme's EMS, applies `layout.json`, writes the result into
  `ember\theme\battle\hud` (the active folder `platform::game_themes::Use` already swaps in), then reloads the battle HUD (`GameThemes::Preview`).
- Auto-apply on: every change applies within a second. Off: changes queue, one Apply button sends all. Setting is per browser, off by default. Apply with no changes switches to the theme as it is.
- Names, portraits, labels, ultra name load at game start only (restart needed). Cockpit loads per battle.
- Desktop only: tab hidden and `/api/hud/*` refused for phones/touch-only (UA + `pointer:coarse` in page, UA check on server). Same PIN, LAN only, same enable switch as the remote plus a HUD checkbox.
- Writes limited to `ember\themes\*`.

## Calibration (measured in game, Cyberpunk-Calibration theme)
HUD drawn 21 file px higher than file y. Super/ultra/revenge/sc roots also offset (-59.2, +55.6) for P1, x mirrored for P2. Life bar none.
EMS: 1280x720, G1 pos/size-rot keys, G2 sprite key (tex index + uv rect, negative width flips), G3 child transform, `+4000` pos x hides, flag 0x200 = additive.

## Phases
1. C++ EMZ + EMS codec and layout patcher, golden tests against Python output (`src/tests`).
2. `layout.json` read/apply in `GameThemes`, Remote endpoints (`/api/hud/themes`, `/layout`, `/apply`, `/texture`).
3. Page tab: stage, drag, zoom, arrows, snap, inspector, auto-apply; sprites rendered from texture PNGs.
4. Image replace/export (DDS decode/encode), Save as new theme, tests, draft PR.
