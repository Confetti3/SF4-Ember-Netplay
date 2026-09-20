# Ember localization catalogs

Ember uses monolingual gettext catalogs whose `msgid` is a stable identifier, not the English source sentence. `en.po` is the base catalog and owns the product's English copy. The catalogs are compiled into each consuming binary; they are not package payload files.

## Adding text

- Use a dotted ID that describes ownership and purpose, such as `room.copy_invitation` or `updates.downloaded_mb`.
- Add the same ID with a non-empty `msgstr` to `en.po`, `pt-BR.po`, and `es-419.po`.
- Reference IDs as string literals in `T("area.key")` or `Tf("area.key", ...)` so the source-coverage test can find them.
- Use positional `{0}`, `{1}`, and later placeholders. Every translation must preserve the English placeholder set. Do not use printf placeholders.
- Keep stable ImGui identity after translated visible text with `###StableIdentity`.
- Do not add plurals or `msgctxt` until the parser and validation tests deliberately support them.

## Adding a language

Drop the catalog here, add an `sf4e_embed_locale` line to `cmake/sf4e_locales.cmake`, and add the row to the locale table at the top of `src/common/Localization.cxx` along with its `Locale` enumerator. Nothing else dispatches on a locale, and a static assertion fails if the enum and the table drift apart.

## Text that remains English

Developer overlays, logs, CLI11 help, diagnostic exports, machine-readable support artifacts, Discord presence, game-owned fighter/stage/move names, device tokens such as `LP`, `LK`, and `Enter`, persisted or transmitted defaults, and stable ImGui identities are not localized.

## Weblate

Create one monolingual gettext component with `locales/en.po` as the base-language file and `locales/*.po` as the translation file mask. Keep the language codes exactly `en`, `pt-BR`, and `es-419`, preserve source-defined positional placeholders, and do not enable automatic source-string-to-msgid rewriting.

The Brazilian Portuguese and Latin American Spanish catalogs are implementation drafts that still require review by native speakers. Their presence must not be advertised as complete language support until that review is accepted.
