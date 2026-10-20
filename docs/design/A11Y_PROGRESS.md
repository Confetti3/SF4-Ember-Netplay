# Accessibility: progress log

Branch `fraccie/a11y-speech-core` (worktree `~/SF4-Ember-Netplay-a11y`), off
`nightly` af047e444. Tracks [ACCESSIBILITY_SPEC.md](ACCESSIBILITY_SPEC.md)
phase 0, speech part only. Local only: not pushed, no PR.

## Done (2026-10-20)

1. **Speech preferences** (`PlayerPreferences::speech`, `netplay/SpeechPreferenceJson.hxx`):
   on/off, rate (-10..10, SAPI's scale), volume (10..100 in tens), read details,
   read button hints. One `"speech"` object in the netplay section of
   `settings.json`. Speech defaults to off; details and hints default to on.
2. **Speech service** (`platform/Speech.{hxx,cxx}`): `speech::Say(text, interrupt, rate, volume)`
   and `speech::Stop()`. SAPI 5 `ISpVoice` made and used on its own thread
   (MTA), started by the first call; the caller only moves a request under a
   lock. Interrupt clears the queue and speaks with `SPF_PURGEBEFORESPEAK`.
3. **Composer** (`ui/MenuSpeech.hxx`, `MenuNarrator`): pure, no voice, unit
   tested (`MenuSpeech` test). Title on screen change, then row
   "label, value[, Unavailable], n of m"; detail + button hint after 400 ms
   (non-interrupting); a changed value alone; dialogs say question then the
   highlighted button, and the row again when they close.
4. **Hook** (`GameMenu::Narrate`, called from `GameMenu::Draw`): every Ember
   menu (shell, fighter select, training flyout) feeds its narrator each frame;
   with speech on, the result goes to `speech::Say`. F1 stops speech while a
   menu shows. Button hint uses `SpokenButton` (`MenuGlyphs.hxx`): the player's
   real button as their pad prints it (Cross, R1, ZL, LK, Enter).
   The shell passes its own (possibly unsaved) speech settings with
   `SetMenuSpeech` right before drawing, so turning speech on says "On".
5. **Settings page** Settings > Accessibility (today's structure, not V2):
   Speech, Speech rate, Speech volume, Read details, Read button hints. The
   last four are unavailable while speech is off. Strings in all 14 catalogs;
   every non-English string is a machine draft (README says all are drafts).
   CJK subsets rerun: no new characters needed.

## Decisions

- **Tolk not added now.** It needs a new x86 DLL (Tolk.dll plus the NVDA
  controller client) in the package inventory and the installer. SAPI alone
  proves the narration path. Tolk goes behind the same `Say`/`Stop` in
  `Speech.cxx` (prefer it when `Tolk_DetectScreenReader` finds one, SAPI
  otherwise); rate then follows the screen reader.
- **Narrator per GameMenu**, not one global: each menu knows its own title,
  focus and dialog. An embedded menu (fighter select inside the shell) speaks
  after its parent in the same frame, and its interrupt wins.
- **The narrator runs even with speech off**, so its memory of the screen is
  current the moment speech turns on.
- **Joining words** (", " and ". ") is fixed punctuation, not a catalog
  string; "n of m" and "Left or Right to change" are catalog strings.
- **Journey test does not turn speech on**: that would speak through the PC's
  voice during ctest. The JSON round trip is in `SettingsStore`, the words in
  `MenuSpeech`.
- Commits carry a `Co-Authored-By` line on the team lead's request; strip
  before anything goes upstream (Katie's bar and the GitHub posting rule).

## Next

- First-run speech prompt (spec: speech on before the first screen, welcome
  line, "press F1 / Select to turn speech off"). Needs a first-run flag.
- Stop-speech on a pad (Select/View), and the "Stop-speech key" row (V2 §6).
- Notices: their text is the question today; check every notice reads well.
  Choice dialogs do not yet say "n of m" for their options.
- Edit fields: say the typed text / characters; reader: read the full text.
- New notices and incoming chat spoken (phase 0 list).
- Tolk backend; voice matching the UI language for SAPI.
- Sound cues (miniaudio, `PlayCue`) are the other half of phase 0.
- V2 Settings: the page becomes the Accessibility tab's Speech panel.

## Test results

Lab script `ember-build\lab\build-a11y.sh` (copy `D:\ember-build\src-a11y`,
junction `ember-build\src-a11y`, build `build\tests`), log `lab\logs\build-a11y.log`.

- 2026-10-20, 8baff8bd1: built MenuSpeechTest, GamepadPromptsTest,
  SettingsStoreTest, ControllerNavigationTest, ShellJourneyTest,
  LocalizationTest and Sidecar.dll. ctest: MenuSpeech, GamepadPrompts,
  SettingsStore, ControllerNavigation, ShellJourney, Localization, 6/6 passed.
  Not run in the game yet.
