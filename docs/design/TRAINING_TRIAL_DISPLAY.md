# Training trial display

How a combo trial is shown over the fight. This is the display combo trials must
use if they come back to Nightly from the combo branch. It follows Under Night
In-Birth's mission mode: the recipe is one line across the top of the screen, the
current step is marked, and long recipes are paged instead of wrapped.

## Placement

- One line, top centre, under the timer. Its box is at most x=280 to 1000 and
  y=144 to 166 in 1280x720 game units. The box is content sized, so a short recipe
  gets a short box.
- Convert design units to the backbuffer with the split HUD's game image
  transform (`MatchHud.cxx`). Do not use viewport percentages. At 1920x1080 the
  box is at most 1080x33 pixels.
- The timer box ends near y=132 and the life bars near y=125. USF4's damage panel
  starts near y=185, and the name logos sit on either side. The box fits between
  all of these. If another Ember HUD part reports overlapping bounds, the strip
  gets narrower and pages sooner. It never gets a second row.

```text
1280x720
| Portrait  PLAYER 1        K.O.        PLAYER 2  Portrait |
|      [ Life bar ]       [timer]       [ Life bar ]       |
| Sakura    [v 2LP > v 2LP > (5MP) > 236+HP > ...  3/5]    Hakan |  y 144-166
| (native inputs)                     (damage panel)       |
```

A measured mock over a real 1080p frame was produced while this spec was being
written (`trial-strip-mock.png`, kept out of the repository).

## Steps

- Body font at 18 units, with a floor of 16 for one hard step. Direction and
  button icons are 18 units. CJK text uses the merged font. Notation keeps its
  syntax in every language; move names are not added to each step.
- Steps are 6 units apart, and links or cancels show their separator (`>`, `xx`).
  A step is never split. A wide step uses compact notation, for example
  `236236PPP`.
- Each state can be read without colour:

| State | Look |
|---|---|
| Current | Ember outline, plus a small marker under the step |
| Done | Muted green, with a check |
| Still to come | Ivory, buttons keep their strength colours |
| Out (move issued, contact not yet seen) | Amber outline |
| Not checked by Ember | `?` after the step. Never shown as done |

- Arrows face the side chosen at the start of the attempt (`PlayerOneOnRight`).
  A cross-up does not flip them during the attempt.

## Long recipes

- About 64 units at the right end hold the paging marks (`<` `>`) and
  `current/total`.
- The window shows whole steps around the current one and moves only when the
  current step would leave it. When there is room, one completed step stays in
  view. The strip never scrolls like a marquee.
- The book allows 400 steps. Nothing is cut off without a mark: the counter and
  the paging marks always say that more steps exist.

## Results and details

- A drop or a completion shows briefly in the same row. It replaces the counter,
  never adds a line.
- Attempt counts, per-step drops, the full recipe, timing notes and the recipe's
  name are in the F6 Training menu. A name typed by a player is registered with
  `NoteUserText`.

## Visibility

- The strip is shown only while a trial runs. Previewing a selected or edited
  recipe is an F6 option, off by default.
- There is one trial display at a time. USF4's vertical task widget and Ember's
  side list stay off.
- It follows the passive overlay rule. It is hidden while the game's pause menu,
  the F6 controls or the Ember menu are open, and when F5 hides the HUD.
  Tooltips may still show.

## Review gate

A change that brings trials back to Nightly is reviewed against this page. The
thermo-nuclear review scope includes it, and the UI render test must assert:

- the strip's box is inside the bounds above at 1280x720 and 1920x1080;
- the strip has one row for a 400-step recipe;
- the strip is hidden while the native pause flag is set.
