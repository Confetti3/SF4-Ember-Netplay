#pragma once

// Locale and Script, generated from locales/locales.json. Locale indexes the
// locale table, whose rows are generated from the same file.
#include "LocaleIds.hxx"

#include <fmt/format.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sf4e { namespace loc {

using Catalog = std::unordered_map<std::string, std::string>;

// An explicit tag wins. Otherwise USF4's language (a game code from
// GameLanguage.hxx, empty when unknown) unless it is ENG, which the game also
// uses for every language it lacks, then the Windows UI languages, then English.
Locale ResolveLocale(std::string_view preference, std::string_view gameLanguage,
    const std::vector<std::string>& windowsLanguages);
bool ValidPreference(std::string_view preference);
// Steps through "auto" and the locale tags. A valid preference in gives a
// valid preference out; anything else restarts at "auto".
std::string_view NextPreference(std::string_view preference, int delta);
void SetActive(Locale locale);
Locale Active();
// Whether `count` takes a catalog's singular ("_one") string in the active
// locale. French and Brazilian Portuguese say 0 in the singular too (CLDR's
// "one" category); the others' catalogs word their counts so that only 1 is.
bool SingularCount(unsigned long long count);
const char* T(const char* id);
const char* NativeName(Locale locale);
const char* Tag(Locale locale);
Script ScriptOf(Locale locale);
// Every translated string of a catalog run together, which is the text the UI
// fonts must be able to draw.
std::string DisplayText(Locale locale);
// DisplayText without the country names (country.XX), the text a font atlas
// bakes for the interface. Country names reach the atlas as players' names do,
// once they are on screen (Theme.hxx: NoteUserText), since a CJK catalog's 249
// of them would not fit its budget.
std::string InterfaceText(Locale locale);

namespace detail {
std::string Format(const char* translated, const char* id, fmt::format_args arguments);
}

template<class... A> std::string Tf(const char* id, A&&... a) {
    return detail::Format(T(id), id, fmt::make_format_args(a...));
}

namespace testing {
bool ParsePo(std::string_view source, Catalog& entries, std::string& error);
void SetPseudoActive();
}

} } // namespace sf4e::loc
