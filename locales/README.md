# Ember localization catalogs

Ember uses monolingual gettext catalogs whose `msgid` is a stable identifier, not the English source sentence. `en.po` is the base catalog and owns the product's English copy. The catalogs are compiled into each consuming binary; they are not package payload files.

Ember offers the languages USF4 itself supports, plus Latin American Spanish:

| Tag | Language | USF4 code |
|---|---|---|
| `en` | English | ENG |
| `pt-BR` | Português (Brasil) | BRA |
| `es-419` | Español (Latinoamérica) | none |
| `es-ES` | Español (España) | SPA |
| `fr` | Français | FRA |
| `it` | Italiano | ITA |
| `de` | Deutsch | GER |
| `nl` | Nederlands | DUT |
| `pl` | Polski | POL |
| `cs` | Čeština | CZE |
| `ru` | Русский | RUS |
| `ja` | 日本語 | JPN |
| `ko` | 한국어 | KOR |
| `zh-Hans` | 简体中文 | CHI |

## Adding text

- Use a dotted ID that describes ownership and purpose, such as `room.copy_invitation` or `updates.downloaded_mb`.
- Add the same ID with a non-empty `msgstr` to `en.po` and to every other catalog here, in the same position.
- Reference IDs as string literals in `T("area.key")` or `Tf("area.key", ...)` so the source-coverage test can find them.
- Use positional `{0}`, `{1}`, and later placeholders. Every translation must preserve the English placeholder set. Do not use printf placeholders.
- Keep stable ImGui identity after translated visible text with `###StableIdentity`.
- Do not add plurals or `msgctxt` until the parser and validation tests deliberately support them. Russian, Polish and Czech phrase counts neutrally, such as `Игроков: {0}`.
- After changing `ja.po`, `ko.po` or `zh-Hans.po`, run `python scripts/subset-cjk-fonts.py`. Those languages draw from Noto Sans CJK subsets that hold the characters the catalogs use plus the characters people type into names and chat (see "Player-written text"), and the Localization test names any character a subset lacks.
- Name a control in a sentence with the words of its own label (`home.settings` in `room.controller_required`). The Localization test checks the pairs it lists; Polish, Czech and Russian inflect the label and are exempt.

## Adding a language

Drop the catalog here and add its entry to `locales.json`: the `Locale` enumerator, the tag, the native name, the script its text needs and the USF4 language code that selects it (or `null`). That file is the only declaration of a language. The build generates the `Locale` and `Script` enums, the locale table and the embedded catalogs from it, the tests and render sweeps iterate the result, and `scripts/subset-cjk-fonts.py` reads it. A new script needs an entry under `scripts` with the font that draws it.

## Choosing a language

The Language setting is either a tag above or `auto`. `auto` follows USF4's own language when it is not English: `language.cfg` in the game folder, then Steam's language for the game, read the way `SSFIV.exe` reads them. The game uses English for every language it lacks, so English falls through to the Windows display languages, then to English. The game's one Spanish is Castilian, so a Latin American Windows keeps `es-419`.

## Player-written text

Display names, room names and chat can hold any character. Inter draws Latin, Latin Extended-A and Cyrillic. The three Noto Sans CJK subsets also hold the level-1 kanji and kana of JIS X 0208, the 2,350 Hangul syllables of KS X 1001 and the level-1 hanzi of GB2312, so those characters draw whatever language the interface uses. They reach the glyph atlas only once some text on screen uses them (`NoteUserText` in `Theme.cxx`), which rebuilds the atlas at most four times a second. A character in none of these sets (rare kanji, Traditional-only hanzi that Japanese kanji do not share, emoji) draws as the fallback glyph. The subsets add about 1.65 MB to each binary that embeds the fonts (Launcher.exe and Sidecar.dll).

## Text that remains English

Developer overlays, logs, CLI11 help, diagnostic exports, machine-readable support artifacts, Discord presence, game-owned fighter/stage/move names, device tokens such as `LP`, `LK`, and `Enter`, persisted or transmitted defaults, and stable ImGui identities are not localized.

## Weblate

Create one monolingual gettext component with `locales/en.po` as the base-language file and `locales/*.po` as the translation file mask. Keep the language codes exactly as the tags above, preserve source-defined positional placeholders, and do not enable automatic source-string-to-msgid rewriting.

Every catalog other than English is an implementation draft that still requires review by native speakers. The drafts follow USF4's own localized terminology where the game has a term. Their presence must not be advertised as complete language support until that review is accepted.
