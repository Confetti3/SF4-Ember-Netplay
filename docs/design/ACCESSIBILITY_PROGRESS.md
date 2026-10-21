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

## Steam Deck / Proton (2026-10-20)

6. **Linux backend** (`platform/SpeechLinux.hxx`, `Speech.cxx` `RunLinux`): under
   Wine (ntdll `wine_get_version`) SAPI is never touched. Ember starts one
   python3 relay per session on the host through `__wine_unix_spawnvp` +
   the host's busctl (host loader from `/run/host`) asking the host's systemd
   for a transient unit `ember-speech-<pid>.service` (`KillMode=process`).
   The relay connects to speech-dispatcher, then makes a FIFO in the
   prefix's Temp folder; Ember writes SSIP to it (`SsipMessage`, tested in
   `SpeechLinux`). `CANCEL SELF` only, so Orca / Steam's reader is never cut.
7. **Notice** when speech is on but nothing can speak, per cause
   (`speech.no_windows_voice`, `speech.no_speech_dispatcher`).
8. Log lines in sf4e.log: `Speech: speech-dispatcher through a relay ...;
   connected|the relay did not answer`, `Speech: SAPI ...`.

Findings worth keeping (details in the spec's Linux section):
- SAPI under Proton 11: `Speak` fails SPERR_NOT_FOUND and a try stalled the
  Deck on Steam's loading screen. Never call it under Wine.
- Wine: no AF_UNIX (WSAEAFNOSUPPORT); CreateProcess of an ELF fails (193);
  `__wine_unix_spawnvp` works; `GetFileAttributesW` on a FIFO fails (203)
  while `CreateFileW` + `WriteFile` on it work; `wine_get_unix_file_name`
  works for folders, not for a FIFO.
- The sniper container shares the session bus (`/run/pressure-vessel/bus`),
  pipewire/pulse and home, not `$XDG_RUNTIME_DIR/speech-dispatcher`.
- Killing an spd-say client does not stop its speech; `spd-say -C` cancels
  everyone's (a screen reader's too).
- Steam's screen reader is Orca on the same speech-dispatcher; its switch is
  `config/config.vdf` "Accessibility" "ScreenReaderEnabled" "1"/"0".
- Deck test rules: no Steam app ids on probes (Game Mode shows a loading
  screen), `timeout -k 5 30` on every probe, short test lines (the user hears
  the Deck), never `pkill -f` a pattern that matches the ssh command itself.
- Lab: `lab\speechprobe\` (probe exe + `deck-probe.sh`), `lab\deck-a11y-install.sh`
  (stage-a11y into the prefix's C:\Ember, old install kept as C:\Ember.bak,
  speech turned on in settings.json, old file kept as settings.json.bak-a11y),
  `lab\wav-peak.py` (level of a pw-record capture, per window).

Relay lifetime (fixed after the first Deck run, where a relay waited 1h13m
on a FIFO Ember never opened): the relay opens the FIFO without blocking,
leaves 15 s after start if Ember never writes, and on EOF once Ember closes
it or dies. Checked on the Deck host: no writer, exit after 16 s; writer
killed with SIGKILL, exit at once; FIFO removed both times.

## Quieter narration and the Piper voice (2026-10-20, after the user's first Deck test)

9. Narration as Tokon: a move says name and value (and Unavailable); a new
   screen its title and row count; detail once, 1.5 s after the stop; hints
   off by default; position behind Read position (off).
10. Utterances in parts; `speech::Say(parts)`, `speech::Render(parts)`.
11. `speech_relay.py` shipped beside Sidecar (PackageInventory), Ember's own
   line protocol (`SayLine`, `RenderLine`, `ClipKey`, tested in SpeechLinux).
   Piper + medium voice downloaded once, SHA-256 checked, clip cache in
   `~/.cache/ember-speech/clips/<voice>/<key>.wav`, pre-render at nice 10.
   Status file read by Ember: Voice row on the Accessibility page plus
   notices (download started, ready, failed).
12. Deck host test of the relay (no game): espeak spoke while downloading,
   status went downloading 0, 2, 63, natural; Piper spoke; clips made for
   SAY and RENDER parts; relay left and removed its files at EOF.
13. pw-play cannot play raw PCM from stdin; the relay plays WAV clips with
   paplay.

## Tokon-strict defaults (after the user's second Deck test: "still reads everything")

14. A move says the row's name; its value only for rows the player changes
    (adjustable or with choices); no "Unavailable", no row count. A new
    screen says its title. Descriptions only on F2, or after a 1.5 s stop
    when "Read descriptions automatically" is on (off by default). A notice
    that opens by itself stays quiet until the player moves in it or presses
    F2. Pad describe button: open (no free menu button yet; X/Y/View are
    screen shortcuts).

What makes Ember speak, all of it (grep `speech::` outside tests):
- `GameMenu::Narrate` (src/ui/GameMenu.cxx), called from `GameMenu::Draw`
  for every Ember menu: `speech::Say` for a focus move, a value change the
  player made, a dialog the player opened or moves in, F2, and descriptions
  after a stop when wanted; `speech::Render` (silent, prepares clips) when a
  screen opens; `speech::Stop` on F1.
- Nothing else calls Say: no header, player card, news, status line, toast
  or chat is spoken.

## Accessibility button, intro, notice chime (2026-10-20, user decisions)

15. RT (pads, XInput and DirectInput sticks, Xbox-layout 4th bottom button)
    and F2 = `MenuInput::Describe`: tap reads the description or a notice's
    text and buttons; tap again on the same row stops. F1 also stops.
16. Turning Speech on shows and reads "Speech is on. Move to hear each item.
    Press <RT/R2/F2> ..." once (a notice marked as asked by the player).
17. Notice that opens by itself: chime (winmm PlaySound, also under Proton,
    so no CHIME message to the relay) + "Notice. Press RT to hear it.".
    Chime on/off and volume rows. Accessibility page moved to
    ApplicationShell__Accessibility.cxx (shell back to 945 lines).
18. Deck install resets speech settings to defaults with speech on.

## Deck crash 15:21 (32e453a2) and fix

- Crash at the first menu frame: Sidecar!speech::UnixPath+0x47 on the speech
  thread, writing a std::string over a return address. Wine exports
  `wine_get_unix_file_name` as cdecl (`_wine_get_unix_file_name`, no @4);
  Ember declared it WINAPI, so each call left ESP 4 bytes off. Fixed in
  b4ebe12b4. Symbolized with `lab\symbolize-dump.ps1` (cdb + stage PDB).
- `SpeechWine` test (skipped off Wine) runs speech end to end under Proton:
  `lab/speech-wine-test.sh`. With the old declaration it crashed (exit 5,
  0xC0000005); fixed, exit 0.
- The relay left Piper and paplay running after it ended; it now kills them
  (d9b376b5a). Its unit goes once the speech-dispatcher it started idles out.
- 2026-10-20 ~16:00, d9b376b5a: 12 tests (SpeechWine skipped on Windows)
  pass; stage Sidecar md5 a4d5c766; SpeechWine on the Deck exit 0.

## Discoverability, free voices, guide (2026-10-20 evening)

19. Discoverability (46a540af7): intro at the first menu with speech on
    (introHeard), "RT for more" until 3 descriptions read (describeUsed,
    Remind setting Auto/On/Off), then a tick; screen help once per session.
20. Licences (29077ddde, spec table): lessac out (research only); English
    ljspeech (PD, user's pick by ear over kristin and cori), ru denis (CC0),
    zh on speech-dispatcher. Only PD/CC0/CC BY voices.
21. Guide docs/guides/ACCESSIBILITY.md with Opus recordings per section and
    chime/tick samples (b4b27f5cc); scripts/guide-audio/make-guide-audio.sh
    regenerates them (6394b26be). Todo: the guide and recordings in the
    other 13 languages.
22. Deck: 00c4793f installed (SpeechWine exit 0 first).

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
- 2026-10-20 14:10, 819c0c94e: SpeechLinux, MenuSpeech, ShellJourney,
  Localization, SettingsStore, GamepadPrompts, ControllerNavigation: 7/7
  passed. Stage built (Sidecar.dll md5 8fac5e6b...). On the Deck: the 12:50
  build (md5 145ccceb..., FIFO fix, without the relay lifetime fix) is
  installed; the 14:10 build waits for the user's game to close.

## State and open points (2026-10-21, before the session was cleared)

Branch fraccie/a11y-speech-core (pushed to the fork by the lead, history rewritten to the noreply email
without trailers; nothing pushed since). Steam Deck: C:\Ember has the stage of 2026-10-20 (Sidecar
00c4793f) plus speech_relay.py d742006c (packs for en, es-ES, es-419, fr, it, pt-BR, ja, zh-Hans); its
speech settings were reset to {enabled: true}.

Voice packs (lab\voice-packs, README there; render host = test PC D:\ember-voice):
- Hosted on the Nightly repo's "voice-packs" pre-release and in PACKS: en (Kokoro heart), es-ES, es-419
  (ef_dora), fr (ff_siwis), it (if_sara), pt-BR (pf_dora), ja (jf_alpha), zh-Hans (zf_xiaobei). Real
  downloads checked on the Deck for en, ja, es-ES (silent).
- MOSS-TTS-Nano packs, all voices approved (provisional): de (kerstin) stopped at 1717 of about 2330 WAVs
  in D:\ember-voice\de\wav (render_moss.py resumes); then pl (gosia), ru (denis), nl-BE (nathalie),
  nl-NL (ronnie), ko (Zeroth-Korean speaker, CC BY 4.0 credit in manifest/README), cs (Common Voice
  speaker). The lead is moving the queue into a standalone script on the test PC.
- Uploads only through the lead after the user agrees; then add each pack to PACKS (version, url, size, sha).

Open, code:
1. Dutch: map the interface language nl to the nl-NL or nl-BE pack by the OS region, and the "Dutch voice"
   row (spec, "Dutch: two voices"), once both packs exist.
2. Windows packs: download and unzip in Ember (the zlib extractor of PR #109 / fraccie/linux-install is
   not in nightly yet), Opus decoding with opusfile from vcpkg (approved; check the x86 triplet, the package
   inventory and the installer), PlaySound from memory. The Pack rows are hidden on Windows until then.
3. Speech under Wine without spawnvp, and hosts with another loader layout: the notice only (spec, Risks).
4. A miss in a pack (player and room names, chat, numbers with words) uses Piper only for English; other
   languages fall back to speech-dispatcher.
5. Pad describe is RT; rebinding it, a first-run speech prompt, the Steam overlay/keyboard quiet rule and
   "Steam reader on" detection (config.vdf ScreenReaderEnabled) are not built.
6. Piper's non-English voices carry the lessac licence risk (spec); with the packs they are only reached
   for misses in languages that have both.
7. The guide's recordings are in ljspeech; regenerate them with heart from the pack pipeline.
8. Nothing has gone to Katie: before any upstream PR, run the katie-bar gate, the katie-reviewer, and a
   full in-game test on Windows and the Deck.

Lab scripts are under ember-build\lab, which is not a repository of its own (it sits in the user's home
folder repository), so they are kept as files there, not committed.
