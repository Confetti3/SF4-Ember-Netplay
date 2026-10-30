# V2 menus audit: navigation edge cases (2026-10-30)

Scope: the Nightly snapshot at fcd33f73 (sf4-nightly-audit), V2 paths only. Anything inside a `!v2::On()` branch is ignored because Classic is being removed in parallel. Findings name the file and function, never a line, so they survive that removal.

Covered here: Home, the header and its Back, right-click Back, tabs, Settings and its panels, value rows and their choice panels, popouts, notices, toasts, the on-screen keyboard, text rows, the Describe button, readers, the first-run welcome, News, Notifications, Awards, the country picker, fighter select, PressGate, repeat, Reconcile and MenuWalk.

Each finding was traced through the code. Where a trace could not be finished it says "unverified". Nothing here was run in the game.

Severity: P1 blocks landing V2 as the only look, P2 should be fixed before or soon after, P3 is polish.

## Summary

| # | Sev | Title | Where |
| --- | --- | --- | --- |
| 1 | P1 | A press that answers a toast acts again on the menu behind it | GameMenu.cxx `GameMenu::Draw`; ApplicationShell.cxx `ApplicationShell::Draw`; ApplicationShell__Toasts.cxx `AnswerToast` |
| 2 | P1 | A DirectInput stick cannot change tabs, so most of Settings is out of reach | ControllerNavigation.hxx `ControllerButtons`; GameMenuV2.cxx `GameMenu::DrawV2`; V2.cxx `v2::Tabs` |
| 3 | P1 | Settings, Player, Button layout does nothing when selected | ApplicationShell__ButtonLayout.cxx `ControllerActivate`; ApplicationShell.cxx `HandleActivate` |
| 4 | P2 | Clicking a tab while a reader is open leaves the old tab on the stack | ApplicationShell.cxx `ApplicationShell::Draw` (Shortcut branch); MenuNavigation.hxx `Return` |
| 5 | P2 | The header's Back and the hint bar's Cancel never close an open value panel or popout | GameMenuV2.cxx `GameMenu::DrawV2` |
| 6 | P2 | The V2 hint bar ignores open dialogs and readers; some hints are buttons that do nothing | GameMenuV2.cxx `GameMenu::DrawV2`; GameMenu.cxx `GameMenu::Draw`, `DialogLegend` |
| 7 | P2 | Toast focus takes Enter from the chat box, and a toast that came during typing takes focus afterwards | ApplicationShell.cxx `ApplicationShell::Draw`; ApplicationShell__Toasts.cxx `DrawToasts`; GameMenu.cxx `KeyboardMenuBits` |
| 8 | P2 | The on-screen keyboard does not fit 1280x720 at 1.5x or 200%; it and the toasts ignore TV mode's safe area | MenuDialogs.cxx `DrawEditModal`, `DrawKeyboard`; ApplicationShell__Toasts.cxx `DrawToasts` |
| 9 | P2 | Tabs stop at the ends (Discord asks them to wrap), with the end rule written three times | GameMenu.cxx `GameMenu::Draw`; GameMenuV2.cxx `GameMenu::DrawV2`; V2.cxx `v2::Tabs` |
| 10 | P2 | The mouse cannot change the Send reports switch: a click opens its reader | GameMenuV2.cxx `MenuValueStep`, `PressEntry`; MenuNavigation.hxx `MenuSelectOpens` |
| 11 | P2 | Country board: moving the pointer over the letters swaps the country list under it | CountryPicker.cxx `CountryPicker::BoardRows`, `DrawBoard`; GameMenuV2.cxx `PressEntry` |
| 12 | P3 | Select steps a word value but the hint bar offers no Select | MenuNavigation.hxx `MenuNavigation::Choose`, `MenuSelectOpens`; MenuPresentation.hxx `MenuPrimaryHint` |
| 13 | P3 | A choice panel on a disabled option still says Choose | GameMenuV2.cxx `GameMenu::DrawV2` |
| 14 | P3 | Welcome: Back on step 1 moves the focus; Down from Next goes left to Skip | ApplicationShell__FirstRun.cxx `WelcomeActivate`, `WelcomeRows` |
| 15 | P3 | Closing a panel opened from the rail focuses the panel opened first | ApplicationShell.cxx `ApplicationShell::Draw` (Shortcut branch); ApplicationShell__Settings.cxx `DrawSettingsBoard` |
| 16 | P3 | Held directions do not repeat in choice panels, notices or the on-screen keyboard | MenuNavigation.hxx `MenuNavigation::Update`; GameMenu.cxx `GameMenu::Draw`; MenuDialogs.cxx `ReadEditInput` |
| 17 | P3 | On-screen keyboard: hints name X, Y and LB on sticks that lack them; shift and the key position carry over between fields | MenuDialogs.cxx `DrawKeyboard`, `ReadEditInput` |
| 18 | P3 | A passphrase copy is left unwiped by the on-screen keyboard | MenuDialogs.cxx `GameMenu::PressKey` |
| 19 | P3 | RT, Start or a right click lose or bypass a focused toast | GameMenu.cxx `GameMenu::Draw`; GameMenuV2.cxx `GameMenu::DrawV2`; ApplicationShell.cxx `ApplicationShell::Draw` |
| 20 | P3 | Toasts are not drawn on fighter select, post-match and report screens | ApplicationShell.cxx `ApplicationShell::Draw` |
| 21 | P3 | Fighter board: Up from Costume jumps to the last fighter, and Left never returns to the grid | FighterSelector.cxx `FighterSelector` rows (board), `DrawBoard`; MenuNavigation.hxx `Update` |
| 22 | P3 | Country board, narrow: Down from the letters does not reach the countries under them | CountryPicker.cxx `CountryPicker::BoardRows` |
| 23 | P3 | Home, narrow: Right jumps to the player card at the bottom of the list | ApplicationShell.cxx `BuildRows` (home) |
| 24 | P3 | A tab that disappears leaves no current tab; RB then goes to the first | ApplicationShell.cxx `ApplicationShell::Draw`; GameMenu.cxx `GameMenu::Draw` |
| 25 | P3 | A body that forgets `bodyChoices` gets an invisible choice | GameMenuV2.cxx `GameMenu::DrawV2` (`Values`); GameMenu.cxx `Finish` |
| 26 | P3 | A value panel does not close on an outside click (a popout does) | GameMenuV2.cxx `GameMenu::DrawV2` (`Values`) |
| 27 | P3 | Awards and toasts use the display, not the window, for the narrow test | ApplicationShell__Awards.cxx `AwardColumns`, `DrawAwardsBoard` |
| 28 | P3 | Notice owners track "a notice is open", not their notice (unverified reach) | ApplicationShell__PadSetup.cxx `UpdatePadSetup`; ApplicationShell.cxx `AnswerDialog` |

## P1

### 1. A press that answers a toast acts again on the menu behind it

- Severity: P1.
- Where: GameMenu.cxx `GameMenu::Draw` (the `toastFocused` branch and `toastGate_`); ApplicationShell.cxx `ApplicationShell::Draw` (where `toastPressed` clears `toastFocus_`); ApplicationShell__Toasts.cxx `ApplicationShell::AnswerToast`.
- Repro (Xbox pad, or keyboard Escape): open Ember on Home with an update offer due, so the Update toast (Update now, Later) arrives and takes focus. Press B for Later the way a person does, holding it for a few frames.
- What happens: the toast closes, and on the next frame Ember hides as if B had been pressed at Home. On any other screen the same press also goes back a screen; on the room board it would leave your seat. Select has the same problem whenever `AnswerToast` does not push a screen: the Discord invitation screen is pushed when its toast appears, so Join only calls `Prefer("invite-switch")`, and the still-held A then selects the focused row. With `discordConfirm` false that row is Cancel invitation (the switch row does not exist), so Join cancels the invitation.
- Why: while the toast holds focus, `GameMenu::Draw` zeroes `menuInput.held` before `navigation.Update`, so the navigation's `PressGate` records nothing held. When the shell drops `toastFocus_`, nothing rearms the navigation, so the next frame's still-held button reads as a fresh press. The notice path does not have this problem because `DrawNoticeModal` calls `navigation.Rearm()` when a notice closes. The harness's `Press` holds a button for one frame, which is why the journeys never see it.
- Expected: the press that answers a toast is spent on the toast.
- Fix: rearm the navigation when the toast takes a press, exactly as a closing notice does. Better still, drop `toastGate_` and let the toast read the navigation's own gate, so there is one record of what is held.

### 2. A DirectInput stick cannot change tabs, so most of Settings is out of reach

- Severity: P1.
- Where: ControllerNavigation.hxx `ControllerButtons` (PrevTab and NextTab are set only under `deviceType==input::PadXInput`); GameMenuV2.cxx `GameMenu::DrawV2` (tab hints); V2.cxx `v2::Tabs` (the LB and RB caps).
- Repro (DirectInput arcade stick, V2): Home, Settings. Try to reach Accessibility, Interface or About.
- What happens: the stick produces no tab press, and no row leads from one Settings tab to another (the rail lists only the current tab's panels). Matches, Interface, Accessibility (speech), Online and About can only be reached with the keyboard's Page Up and Page Down or the mouse. The same holds for Awards' Icons tab, the News filters, Replays' Sets and Export video, and the Rankings board. The hint bar and the strip's caps still show LB and RB, which the stick does not have. Classic reached these pages as rows; V2 alone does not.
- Expected: every tab is reachable with every device that can drive the menus.
- Fix: carry the two tab buttons in `input::MenuButtons` the way Menu confirm and Menu back are carried (bindable per device, with the stick's defaults), so `ControllerButtons` has one path for all devices instead of an XInput-only branch. Until a stick has them, the hint bar must not name LB and RB for it.

### 3. Settings, Player, Button layout does nothing when selected

- Severity: P1.
- Where: ApplicationShell__ButtonLayout.cxx `ApplicationShell::ControllerActivate`; ApplicationShell.cxx `ApplicationShell::HandleActivate`.
- Repro (any device, V2): Home, Settings, Player tab, open the Player panel, select Button layout.
- What happens: nothing. `ControllerActivate` only opens the layout for `"controls"` when `screen=="player"`, but in V2 the row lives on `tab-player/0`. `HandleActivate` has no other branch for `"controls"`, so the press is dropped without a word. Button layout stays reachable only through Controllers and a pad's row.
- Expected: it opens the gameplay pad's Button layout, as in Classic.
- Fix: decide on the row id alone (`action.id=="controls"`), without the screen test. Note: once this works, `DrawButtonLayout` and `DrawPadSetup` return before the per-frame `menu_.tabs.clear()`, `eyebrow` and `statusLeads` reset in `ApplicationShell::Draw`. The Settings tab strip, its LB and RB hints and the eyebrow from the previous frame would then show on Button layout and do nothing. Move that reset ahead of the two early returns at the same time.

## P2

### 4. Clicking a tab while a reader is open leaves the old tab on the stack

- Severity: P2.
- Where: ApplicationShell.cxx `ApplicationShell::Draw`, the Shortcut branch (`if(page!=screen)nav.Return(); if(SettingsTabOf(a.id)==a.id)nav.Return(); nav.Push(a.id);`); MenuNavigation.hxx `MenuNavigation::Return`; GameMenuV2.cxx `GameMenu::DrawV2` (tabs and hint bar guarded by `modalAtStart`, which leaves out `Reading()`).
- Repro (mouse): Home, News, select a release (it opens in a reader). Click the Updates tab, or the RB hint. Then press Back.
- What happens: `Return()` with a modal open only cancels the modal, so the first Return closes the reader and the stack keeps "news" under the newly pushed "news-updates". Back returns to the All tab instead of Home. Each such click adds a screen. The same happens on Settings, About (the licences and credits are readers: two Returns become reader-close plus one pop, leaving `tab-about` under the new tab), and on Rankings (How rankings work). A pad cannot do this, since `Update` returns early while reading.
- Expected: a tab replaces the screen it is on, whatever is open on it.
- Fix: `nav.Cancel()` before the Returns in the Shortcut branch; or give `MenuNavigation` one call that swaps the top screen, so the shell stops counting Returns.

### 5. The header's Back and the hint bar's Cancel never close an open value panel or popout

- Severity: P2.
- Where: GameMenuV2.cxx `GameMenu::DrawV2` (the `v2::Header` result and the `HintBar` hit are both guarded by `!modalAtStart`).
- Repro (mouse): Settings, Accessibility, open Text and language, click Language's value so its choice panel opens. Click the header's Back, or the hint bar's Cancel.
- What happens: nothing. `modalAtStart` is true whenever a choice is open, so the `if (navigation.Asking()) navigation.Cancel()` branch in the header and the `if (choosing) navigation.Cancel()` branch in the hint bar can never run. A popout still closes because a click outside it sets `popoutClose_`, but a value panel has no outside-click close (finding 26). Only B, Escape, right click or picking a choice close it, while V2_MENUS.md says the header's Back closes an open panel first and the hint bar shows Cancel.
- Expected: both Back controls close the open question first, as right click does.
- Fix: drop the `modalAtStart` guard from the two Back controls (keep the Editing and notice guards), so they take the same path as the right-click branch below them.

### 6. The V2 hint bar ignores open dialogs and readers; some hints are buttons that do nothing

- Severity: P2.
- Where: GameMenuV2.cxx `GameMenu::DrawV2` (hint bar); GameMenu.cxx `GameMenu::Draw` (computes `dialogSelect` and `dialogBack` through `DialogLegend` but does not hand them to `DrawV2`).
- Repro (pad): Settings, About, select Font licence. The reader opens.
- What happens: the hint bar still shows the row's hints: A "Read" (Select does nothing in a reader), B "Back" (it closes the reader; Classic says Close), and LB/RB "Pages" (ignored while reading). Speech is right, because `Narrate` gets the dialog legend; only the bar is wrong. The same holds under the edit and confirmation popups. Separately, the room shortcut hints (X, Y, View), Refresh on Public rooms and Rankings, and Adjust are pushed with `does.push_back(0)`, so clicking them does nothing, while V2_MENUS.md says each hint is also a button.
- Expected: the bar names what the buttons do now, and every hint shown as a button acts.
- Fix: pass the dialog legend into `DrawV2` as Classic's `MenuLegend` receives it, and give the shortcut and Adjust hints the same press they stand for (a Shortcut with that button, an Adjust of +1), or draw them as plain labels.

### 7. Toast focus takes Enter from the chat box, and a toast that came during typing takes focus afterwards

- Severity: P2.
- Where: ApplicationShell.cxx `ApplicationShell::Draw` (`menu_.toastFocused` checks Editing, Reading, Confirming, Choosing and the notice, but not `menu_.typesText`); ApplicationShell__Toasts.cxx `ApplicationShell::DrawToasts` (`toastFocus_` is set when a toast arrives, whatever the menu is doing); GameMenu.cxx `KeyboardMenuBits` (Enter stays in the `always` set when `typesText`).
- Repro A (keyboard, in a room): open Chat and type a message. A Discord invitation arrives (its toast has Join and Later). Press Enter to send.
- What happens: Enter is Select, the focused toast takes it as Join, the message is not sent, and the player is taken to the invitation screen. Escape likewise answers the toast. The landing row is a confirmation and the push rearms, so nothing destructive follows, hence P2.
- Repro B (pad): edit your player name with the on-screen keyboard; an update toast arrives meanwhile. Press Start to finish. The toast, which the player may not have noticed, now holds focus, and the next A meant for the list answers it.
- Expected: a toast takes focus only when it arrives over a free menu, and never while text is being typed.
- Fix: decide focus once, when the toast is taken, with one "menu is free" test that includes `typesText` (the shell already has every input for it), and drop the per-frame recheck in `Draw`.

### 8. The on-screen keyboard does not fit 1280x720 at 1.5x or 200%; it and the toasts ignore TV mode's safe area

- Severity: P2 (the height is an estimate from the constants; no render covers the keyboard).
- Where: MenuDialogs.cxx `GameMenu::DrawEditModal` (window position and `NextPopupSize`) and `GameMenu::DrawKeyboard`; ApplicationShell__Toasts.cxx `ApplicationShell::DrawToasts`.
- Repro (pad): 1280x720, Interface size 1.50x (or 200% DPI, one of UiRenderTest's sizes). Edit Player name with the pad.
- What happens: at scale s the popup needs about 44s padding + 24s heading + 52s field + 4s + five key rows of 58s + the 50s hint row, plus item spacing, and 48s more when the field has Recent names. That is about 730 px at s = 1.5 (about 800 with Recent) against a cap of 720 - 40s = 660 px; at s = 2, about 980 px against 640. The Space and Done row and the hint row are cut off or scrolled away, and nothing scrolls them back into view (the keys have no `SetScrollHereY`, and the popup is `NoNavInputs`). Start still accepts, but the player cannot see Done or Cancel. Separately, in TV mode the shell window sits inside a 5% safe area, but the keyboard is placed 24s above the viewport's bottom edge and the toasts at the viewport's right edge minus the side margin (about 30 to 50 px at 1080p, against the 96 px the safe area keeps). An overscanning TV crops both.
- Expected: the keyboard fits every supported size and stays inside the safe area, as the rest of V2 does.
- Fix: size the keys from the height left (as the width already is) and place the keyboard and the toasts from the shell window's rectangle instead of the main viewport. Add the keyboard to the lab's render cases.

### 9. Tabs stop at the ends (Discord asks them to wrap), with the end rule written three times

- Severity: P2 (a community request; also a duplication the quality bar rejects).
- Where: GameMenu.cxx `GameMenu::Draw` (LB/RB to `tabs[next]` only when `next` is in range); GameMenuV2.cxx `GameMenu::DrawV2` (hint bar LB/RB clicks, the same check); V2.cxx `v2::Tabs` (the caps, the same check plus their dimming).
- Repro (pad): Settings, press RB five times to About, press RB again.
- What happens: nothing; LB on Player does nothing either. The caps dim at the ends.
- Expected (Discord feedback): RB on the last tab goes to the first, LB on the first to the last.
- Fix: one stepping helper (current, delta, count, wrapping) used by all three places; with wrap the caps never dim. Note MenuWalk's seek (`MenuWalk::Frame`) only ever presses NextTab, so wrap also lets it find rows on earlier tabs.

What was checked and holds: `v2::Tabs` slides the strip so the current tab is always drawn whole (its right edge is placed `pad` inside the strip), so it is always highlighted. Only a single label wider than the whole strip could be skipped, which no supported size and language reaches. The caps and LB/RB step to tabs that are not drawn, because they step by index.

### 10. The mouse cannot change the Send reports switch: a click opens its reader

- Severity: P2.
- Where: GameMenuV2.cxx `MenuValueStep` (a switch never steps) and `PressEntry`; MenuNavigation.hxx `MenuSelectOpens` (a reader wins over everything); ApplicationShell.cxx `BuildRows` (problem-reports: the `send-reports` switch is `reading`).
- Repro (mouse): Settings, About, open Problem reports, click the Send reports switch.
- What happens: the reader with what a report holds opens. A switch has no arrow zones, so a click is always Select, and Select on this row is the reader. Only Left and Right change it, so a mouse-only player cannot. The hint bar says Read on a row drawn as a switch.
- Expected: clicking a switch flips it (V2_MENUS.md, Values).
- Fix: make the switch's own box a step zone in `MenuValueStep` (as a number's halves are), so a click on the switch flips it and a click on the label reads. Or move "what a report holds" to a row of its own, so no switch is also a reader.

### 11. Country board: moving the pointer over the letters swaps the country list under it

- Severity: P2.
- Where: CountryPicker.cxx `CountryPicker::BoardRows` (the letter shown follows the focus) and `CountryPicker::DrawBoard`; GameMenuV2.cxx `PressEntry` (a moving pointer focuses what it is over).
- Repro (mouse, wide window): Settings, Player, Country. Point at B, then move right towards the country list.
- What happens: the pointer crosses C, D, E, F and G on the way, each focuses, and `BoardRows` rebuilds the list for the last letter crossed. The list shows G's countries by the time the pointer gets there. Only clicking a letter (which moves the focus to its first country) keeps a letter's list.
- Expected: pointing at a list does not change it on the way.
- Fix: let a letter's list follow a selected letter only (Select or click, which already `Prefer`s its first country), not a hovered one.

## P3

### 12. Select steps a word value but the hint bar offers no Select

Where: MenuNavigation.hxx `MenuNavigation::Choose` (`selectSteps`), `MenuSelectOpens`; MenuPresentation.hxx `MenuPrimaryHint`. A word value with no choices (rows that `OfferValues` does not reach, such as room and fighter-select words like a named custom color) is stepped +1 by Select in V2, but `MenuSelectOpens` says Nothing, so the hint bar and speech offer no Select. A switch without choices is set On by Select (AdjustOnOff), so Select on an On switch does nothing. This breaks the "decided in one place so navigation and the legend agree" rule that `MenuSelectOpens` states. Fix: make stepping a `SelectOpens` kind and drop the `selectSteps` special case from `Choose`.

### 13. A choice panel on a disabled option still says Choose

Where: GameMenuV2.cxx `GameMenu::DrawV2` (the hint bar uses `menu.choose` whenever choosing). `DialogLegend` already leaves Select out for a disabled option; the V2 bar does not read it. If every option is disabled, Select silently does nothing while the bar says Choose. Fixed by finding 6.

### 14. Welcome: Back on step 1 moves the focus; Down from Next goes left to Skip

Where: ApplicationShell__FirstRun.cxx `ApplicationShell::WelcomeActivate`, `WelcomeRows`. On step 1 Back is hidden (`backInert`) but `ownsBack` still delivers B and Escape as Back, and `WelcomeActivate` clamps the step to 0 and `Prefer`s Speech on, so B on Skip or Next jumps the focus to Speech on. Also, Next and Skip are side by side (Skip left), but they are the last two list rows, so Down from Next goes to Skip and Up from Skip to Next. Fix: Back on step 0 does nothing; Next's `down` points at itself and Skip's `up` at the last answer row, so Up and Down move between the answers and the buttons, and Left and Right between Skip and Next.

### 15. Closing a panel opened from the rail focuses the panel opened first

Where: ApplicationShell.cxx `ApplicationShell::Draw` (Shortcut branch); ApplicationShell__Settings.cxx `DrawSettingsBoard`. With panel 1 open, clicking panel 3's head returns to the tab and pushes `tab-x/3`, but the tab's focus stays on panel 1's head. Back from panel 3 lands on panel 1. Fix: `Prefer` the head before pushing its panel.

### 16. Held directions do not repeat in choice panels, notices or the on-screen keyboard

Where: MenuNavigation.hxx `MenuNavigation::Update` (Choosing and Confirming read `pressed` only); GameMenu.cxx `GameMenu::Draw` (notice buttons); MenuDialogs.cxx `GameMenu::ReadEditInput` (`input.held&~keyHeld_`). CONTROLLER_MENUS.md says direction holds repeat. Language has 15 choices in three columns and the keyboard rows ten keys. Fix: one repeat source (the navigation's) that the choice, notice and keyboard read.

### 17. On-screen keyboard: hints name X, Y and LB on sticks that lack them; shift and the key position carry over between fields

Where: MenuDialogs.cxx `GameMenu::DrawKeyboard` (fixed glyphs "X", "Y", "LB", "Start"), `ReadEditInput` (Fighter, Options and PrevTab come only from XInput, finding 2). The keys themselves are on the board, so nothing is unreachable, but the hints lie on a stick. `capitals_`, `keyRow_` and `keyColumn_` persist, so a new field can open in capitals on a key far from the letters.

### 18. A passphrase copy is left unwiped by the on-screen keyboard

Where: MenuDialogs.cxx `GameMenu::PressKey`. It copies `navigation.Draft()` into a local string; on Done and on any key that does not change the draft (shift, a full field), the local is destroyed without `WipeText`. Every other secret path wipes. Fix: wipe the local on every return, or edit the draft in place.

### 19. RT, Start or a right click lose or bypass a focused toast

Where: GameMenu.cxx `GameMenu::Draw` (`toastPressed` includes Describe and Start); ApplicationShell.cxx `ApplicationShell::Draw` (any `toastPressed` clears `toastFocus_`); GameMenuV2.cxx `GameMenu::DrawV2` (right click goes back on the screen). A speech user who presses RT to hear more drops the toast's focus. A right click navigates the screen behind a focused toast, which keeps focus. Leave Describe out of what counts as "another press", and let right click answer Later as B does.

### 20. Toasts are not drawn on fighter select, post-match and report screens

Where: ApplicationShell.cxx `ApplicationShell::Draw` returns early for "selection", "post-match" and "report" before `DrawToasts`. A notice said there becomes a toast only after leaving, aged from then.

### 21. Fighter board: Up from Costume jumps to the last fighter, and Left never returns to the grid

Where: FighterSelector.cxx (board rows), `FighterSelector::DrawBoard`; MenuNavigation.hxx `Update` (footer Up goes to `cells-1`). Right from the grid's edge reaches Costume, but Left on Costume adjusts it, and Up from Costume lands on the last fighter rather than the one the player came from. Ready's `left` already names the saved fighter; give the pane's first row the same `up`.

### 22. Country board, narrow: Down from the letters does not reach the countries under them

Where: CountryPicker.cxx `CountryPicker::BoardRows`. A letter's `down` is itself on the last row, and a first-row country's `up` is Hidden. In the narrow layout the countries sit under the letters, so Down does nothing there; Right from the last column is the only way in.

### 23. Home, narrow: Right jumps to the player card at the bottom of the list

Where: ApplicationShell.cxx `BuildRows` (home: every row's `right` is "awards"). When the window is narrow the card is the last list row, not beside the list, so Right is a jump to the bottom and Left from it a jump to the top.

### 24. A tab that disappears leaves no current tab; RB then goes to the first

Where: ApplicationShell.cxx `ApplicationShell::Draw` (`menu_.tab` stays -1 when the screen is not in `tabs`, as when Replays' Sets empties or News' Updates goes); GameMenu.cxx `GameMenu::Draw` (`tab+1` is then 0). Nothing is highlighted, LB does nothing and RB lands on the first tab.

### 25. A body that forgets `bodyChoices` gets an invisible choice

Where: GameMenuV2.cxx `GameMenu::DrawV2` (`Values` returns false when `bodyChoices.card` is empty) and GameMenu.cxx `GameMenu::Finish` (skips `DrawChoiceModal` whenever there is a body). The choice is then open but drawn nowhere: inputs go to it, the bar says Choose and Cancel. Every body in this audit's scope sets it; RoomPanel sets it for popouts only (unverified whether a room row has non-popout choices, or whether the room board draws such a choice itself, as the seat chooser may). Fix, once the room side is checked: either every body that holds choice rows sets `bodyChoices`, or `Finish` falls back to `DrawChoiceModal` when no panel drew; the fallback is only safe if no body draws its own choices in place.

### 26. A value panel does not close on an outside click (a popout does)

Where: GameMenuV2.cxx `GameMenu::DrawV2` (`Values`: `closeOutside` only for `e.popout`). Clicks on the board under it are swallowed. With finding 5 fixed the header and hint bar still close it; consider one outside-click rule for both.

### 27. Awards and toasts use the display, not the window, for the narrow test

Where: ApplicationShell__Awards.cxx `AwardColumns` and `DrawAwardsBoard` read `ImGui::GetIO().DisplaySize`; V2_KIT.md rule 5 is `v2::Narrow(ImGui::GetWindowWidth(), s)`. In TV mode the window is 90% of the display, so the two disagree near 900 logical px. Navigation and drawing both use `AwardColumns`, so focus stays consistent; only the layout choice differs.

### 28. Notice owners track "a notice is open", not their notice (unverified reach)

Where: ApplicationShell__PadSetup.cxx `UpdatePadSetup` (the new-pad prompt waits while `NoticeOpen()` and then calls `CloseNotice()`); ApplicationShell.cxx `AnswerDialog` (the update prompt finishes on whichever notice closes). `ShowNotice` replaces an open notice silently. Most callers guard with `NoticeOpen()`, but `ShowError(v.readyFailure)` and the game settings card do not. If one replaced the pad prompt, the next press on the new pad would close the replacement unread; if one replaced the update prompt, closing it would settle the offer as Later. I could not find a sequence that reaches either in practice (the update prompt needs no room, the ready failure needs one), so this is unverified.

## Checked and found sound

- PressGate on opening: `GameMenu::Draw` rearms when a frame was skipped, and `Push`, `Return`, `Home` and closing a modal rearm, so the press that opens a screen, panel or tab never acts on it. A notice's gate rearms while none is open, and `DrawNoticeModal` rearms the navigation when one closes. A notice replacing another keeps the gate armed, so a held A does not answer the second.
- Repeat and diagonals: Up with Down, and Left with Right, cancel; a diagonal moves vertically (`Update` takes the vertical bits first); a new direction moves at once, then after 0.35 s every 0.085 s.
- Reconcile: a vanished focus moves to the row now at its index; a modal whose row vanished, changed kind or (for choices) changed option ids closes; a draft survives a pending checkpoint.
- A choice that changes while open: the `#n` step choices from `OfferSteps` compare by id, so new labels with the same count stay open, and a pick is turned into steps from the current value in `Finish`, which matches what is shown.
- Back keeps the old value: browsing a choice changes nothing until Select.
- A Settings panel with zero rows cannot occur: `SettingsTabRows` makes panels from rows, and `DrawSettingsBoard` returns early for an empty tab.
- Narrow Settings: the rail and the open panel each take the width in turn; Back closes the panel.
- The popout opens at the click (the press and `Values` run in the same frame, so `IsMouseReleased` is true) or beside the row for the pad, turns back at the window's edges, and closes on right click, B and an outside click.
- Notices: Back always declines (`Dismissed`); Left/Up and Right/Down move between buttons in either layout; the update question opens on Later.
- Text rows: `secret` opens empty, masks, never enters Recent, and its buffers are wiped on close (except finding 18); `textLimit` caps both the field and the keyboard; multiline accepts only with Ctrl+Enter and never shows the on-screen keyboard.
- The on-screen keyboard: every key is reachable from every other with the D-pad, an empty Recent row is skipped, Done, Start and Enter all accept through `AcceptText`, B, Escape and the Cancel hint all discard the draft, and a physical key hides the keyboard and keeps the field focused.
- Describe (RT, F2) is read before a notice or toast takes the input, and F2 stays live while typing.
- Two toasts with buttons: only the newest card shows its buttons and holds focus; the earlier one keeps its actions as a row in Notifications. This is by design, not a defect.
- A toast over a match: `Background` calls `DrawToasts(now,true)`, which sets no focus and opens no button window, so a toast that arrives during a fight never takes a game button and does not take focus when the menu opens later.
- Readers scroll with held Up and Down and the wheel, open at their top, and close on B, Escape, right click and the header's Back.

## MenuWalk coverage

`MenuWalk` (MenuWalk.cxx `Steps`) walks Home, Settings to the Accessibility tab's Text and language panel, the Language choice, back to Home, Online play, Create, a room, a member's popout, Leave room and Training, with pad presses held four frames. It does not touch: LB (it only seeks with NextTab, and tabs do not wrap), a reader, a notice, a toast, the on-screen keyboard, the welcome, News, Notifications, Awards and its Icons tab, the country board, the fighter board, a switch, a number, or any mouse or keyboard path. Because each press is held for four frames and the walk never meets a toast, it cannot see finding 1.

## Missing tests

1. `ToastBackHeldDoesNotGoBack`: a focused toast, B held for several frames: Later is taken and the screen and Ember stay (pins finding 1). The same with Select on the invitation toast while its screen is open.
2. `DirectInputReachesEveryTab`: a DirectInput sample walks from Settings, Player to every other Settings tab and to Awards' Icons (pins finding 2).
3. `SettingsButtonLayoutFromTab`: V2, Settings, Player, select Button layout: the screen is `button-layout`, and the Settings tabs are not drawn on it (pins finding 3 and its follow-up).
4. `TabClickWhileReadingKeepsStack`: News, open a release, click the Updates tab, Back: Home (pins finding 4). Repeat for About's Font licence.
5. `BackControlsCloseValuePanel`: open a value panel, click the header's Back, and separately the hint bar's Cancel: the panel closes and the screen stays (pins finding 5).
6. `HintBarFollowsDialogs`: with a reader, an edit and a confirmation open, the V2 bar names Close, Accept or the confirmation's button, not the row's (pins findings 6 and 13).
7. `ToastNeverTakesChatEnter`: Chat with a draft, an invitation toast arrives, Enter: the message is sent and the toast keeps waiting (pins finding 7).
8. `KeyboardFitsEverySize`: a lab render case of the on-screen keyboard at 1280x720 at 150% and 200% and in TV mode: Done and the hint row are inside the window and the safe area (pins finding 8).
9. `TabsWrapAtEnds`: RB on the last tab reaches the first, LB on the first the last, by pad, keys, caps and hints alike (pins finding 9).
10. `SwitchFlipsByMouse` and `CountryBoardHoverKeepsList`: a click on Send reports' switch changes it; moving the pointer across letters toward the list leaves the clicked letter's countries shown (pins findings 10 and 11).
