#pragma once

#include <fmt/format.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sf4e { namespace loc {

// Indexes the locale table in Localization.cxx, which is the only place a
// locale is declared. En must lead: it is the fallback catalog.
enum class Locale { En, PtBR, Es419, Count };

using Catalog = std::unordered_map<std::string, std::string>;

Locale ResolveLocale(std::string_view preference,
    const std::vector<std::string>& windowsLanguages);
bool ValidPreference(std::string_view preference);
// Steps through "auto" and the locale tags. A valid preference in gives a
// valid preference out; anything else restarts at "auto".
std::string_view NextPreference(std::string_view preference, int delta);
void SetActive(Locale locale);
Locale Active();
const char* T(const char* id);
const char* NativeName(Locale locale);
const char* Tag(Locale locale);

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
