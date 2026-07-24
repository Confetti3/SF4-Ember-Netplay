# SF4 Netplay Launcher v0.6.4-rc1 (testing)

> **Experimental unofficial test build** — not production-ready software.
> Both players must install the same complete release zip.

This release candidate moves match settings out of the launcher and into the
in-game overlay lobby, so round count and timer can be changed between matches
in a set, and reworks the launcher's Simple/Advanced screens.

**The netplay match-settings path has not been tested against a second
instance.** See [Known untested](#known-untested) before relying on this for a
real session.

## Install

1. Download `sf4-netplay-launcher-*-0.6.4.zip` from this release.
2. Extract the complete zip into a new folder.
3. Run `preflight.cmd`, then `Launcher.exe`.
4. Confirm both players show **v0.6.4**.

Mixed builds are rejected by the room compatibility check.

## Match settings move into the lobby

Round count, round time and edition select are now set by the host in the
overlay's network lobby — the screen where you pick your character online —
instead of being fixed before launch.

- Only the host can change them, and only between matches. Guests see the same
  controls disabled, showing the host's current choices.
- Both players read the same replicated lobby settings when a match starts, so
  the two sides cannot disagree.
- Round count offers 1/3/5/7 plus 15 and 99; round time offers 30/60/99 plus
  300 and 9999. The last entries in each list are how you set up an endless
  sparring session now.

The launcher's **Training room (endless sparring)** checkbox is removed. It
could only be chosen before launch, and picking 99 rounds / 9999s in the lobby
does the same thing while staying changeable mid-set.

There is also a **Cancel ready** button once you have readied up. It clears
both players' ready flags — the protocol has no per-player unready — so the
label says so.

Note this is not the game's built-in Training Mode: health, meter, supers and
ultras are unaffected. Only the round and clock limits change.

## Launcher changes

- **Advanced is now additive.** It reveals extra fields instead of swapping in
  a second copy of the basics. Display name and input delay previously existed
  twice and were hand-synced on every toggle; because that sync skipped empty
  values, clearing your name in Advanced left the old one in Simple. Fixed.
- **Host page scrolling.** The scroll area collapsed to its minimum, clipping
  "Host settings" and making everything below the share card unreachable while
  a large gap sat under Start game.
- **Connection method moved above the share cards**, since it decides which of
  those cards matter. The cards now follow the selected method instead of
  showing an empty relay code during Direct IP.
- **Session port** used Qt's native spin arrows, which render as an unusable
  sliver; it now uses the same stepper as input delay.
- **Join**: a Paste button, and pasted codes are cleaned up (whitespace
  stripped, `sf4-xxxx` uppercased). Connection method gains an Auto-detect
  default matching what Simple mode already inferred.
- **Back buttons** were low-contrast text with no hit target; they now have a
  real surface, and Escape works.
- Spacing, control heights, scrollbars, combo popups and tooltips were either
  cramped or unstyled and falling back to Qt defaults.
- Matchmaking and the room browser entry points are hidden until that feature
  is finished. The screen and its plumbing are unchanged underneath.

## Known untested

These need two game instances and have not been exercised:

- Round count and timer actually applying at match start, including the 99
  rounds / 9999s entries. This path changed shape: the values now come straight
  from the lobby rather than being substituted from a mode flag.
- Guests seeing the host's setting changes mirrored, and the timing of that.
- The Cancel ready button, particularly a guest pressing it, which routes
  through the server rather than the host's in-process path.
- Any rollback or desync behaviour across a full set.

Direct-IP requirements are unchanged from v0.6.2.

## Validation

- Native x86 RelWithDebInfo build of Launcher, Sidecar and session targets.
- Launcher smoke test: process starts, both UI modes render.
- Launcher IPC payload keys verified unchanged against v0.6.3 except the
  removed `trainingMode`, which the controller already defaulted to false.
- Settings persistence is additive; older `overlay_prefs.json` and launcher
  `config.json` files load with defaults for the new keys.

Live two-PC play remains the outstanding release-candidate check.
