#include "Localization.hxx"
#include "EmbeddedLocales.hxx"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <iterator>
#include <mutex>

namespace sf4e { namespace loc {
namespace {
// gameCode is the USF4 language that selects the row, or null when the game
// has no such language.
struct Entry {
    const char* tag; const char* nativeName; const unsigned char* po; Script script; const char* gameCode;
    // The embedded catalog is a NUL-terminated UTF-8 byte array.
    const char* Source() const { return reinterpret_cast<const char*>(po); }
};

// Locale is an index into this table. Its rows and the enum are generated
// from locales/locales.json, the only declaration of a language.
constexpr Entry Table[] = {
#include "LocaleTable.inc"
};
constexpr std::size_t Count = std::size(Table);
static_assert(Count == static_cast<std::size_t>(Locale::Count),
    "Locale and the locale table disagree.");

// Trailing catalog slot. Pseudo is English with the letters transformed, built
// only when a render test asks for it, never in a shipping process.
constexpr std::size_t PseudoIndex = Count;

std::once_flag catalogOnce, pseudoOnce;
std::array<Catalog, Count + 1> catalogs;
std::atomic<std::size_t> activeIndex{0};

std::size_t IndexOf(Locale locale) {
    const auto index = static_cast<std::size_t>(locale);
    return index < Count ? index : 0;
}

const Entry* FindTag(std::string_view tag) {
    for (const auto& entry : Table) if (tag == entry.tag) return &entry;
    return nullptr;
}

Locale LocaleOf(const Entry* entry) { return static_cast<Locale>(entry - Table); }

std::string Normalize(std::string language) {
    std::transform(language.begin(), language.end(), language.begin(), [](unsigned char c) {
        return c == '_' ? '-' : static_cast<char>(std::tolower(c));
    });
    return language;
}

std::string_view Primary(std::string_view language) { return language.substr(0, language.find('-')); }

const Entry* FindNormalized(std::string_view language) {
    for (const auto& entry : Table) if (Normalize(entry.tag) == language) return &entry;
    return nullptr;
}

// Spanish as spoken in Latin America and the US, which es-419 serves; every
// other Spanish region is served by the Spain catalog.
bool LatinAmericanSpanish(std::string_view language) {
    const auto dash = language.rfind('-');
    if (Primary(language) != "es" || dash == std::string_view::npos) return false;
    const auto region = language.substr(dash + 1);
    constexpr std::string_view regions[] = {"419", "ar", "bo", "cl", "co", "cr", "cu", "do", "ec", "gt",
        "hn", "mx", "ni", "pa", "pe", "pr", "py", "sv", "us", "uy", "ve"};
    return std::find(std::begin(regions), std::end(regions), region) != std::end(regions);
}

// The row serving one normalized Windows language tag, or null.
const Entry* MatchWindowsLanguage(std::string_view language) {
    if (const auto* exact = FindNormalized(language)) return exact;
    if (Primary(language) == "es") {
        if (const auto* spanish = FindNormalized(LatinAmericanSpanish(language) ? "es-419" : "es-es")) return spanish;
    }
    // First row with the same language subtag, so table order decides which
    // regional catalog serves a neighbouring region.
    for (const auto& entry : Table)
        if (Primary(Normalize(entry.tag)) == Primary(language)) return &entry;
    return nullptr;
}

bool DecodeQuoted(std::string_view text, std::string& output) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') return false;
    for (std::size_t i = 1; i + 1 < text.size(); ++i) {
        char c = text[i];
        if (c != '\\') { output.push_back(c); continue; }
        // The escaped character must sit inside the closing quote, so a
        // trailing backslash is an unterminated literal, not an escape.
        if (++i + 1 >= text.size()) return false;
        switch (text[i]) {
        case 'n': output.push_back('\n'); break;
        case '"': output.push_back('"'); break;
        case '\\': output.push_back('\\'); break;
        default: return false;
        }
    }
    return true;
}

bool ParsePoImpl(std::string_view source, Catalog& entries, std::string& error) {
    entries.clear(); error.clear();
    // None: nothing pending. Id: msgid read. String: msgid and msgstr read.
    enum class Field { None, Id, String } field = Field::None;
    std::string id, value;
    const auto finish = [&]() -> bool {
        if (field == Field::None) return true;
        if (field != Field::String) { error = "Truncated PO entry."; return false; }
        if (!id.empty() && !entries.emplace(id, value).second) {
            error = "Duplicate PO msgid: " + id; return false;
        }
        id.clear(); value.clear(); field = Field::None;
        return true;
    };
    std::size_t offset = 0;
    while (offset <= source.size()) {
        const auto end = source.find('\n', offset);
        std::string_view line = source.substr(offset, end == std::string_view::npos ? source.size() - offset : end - offset);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        offset = end == std::string_view::npos ? source.size() + 1 : end + 1;
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line.empty()) { if (!finish()) return false; continue; }
        if (line.front() == '#') continue;
        if (line.rfind("msgid ", 0) == 0) {
            if (!finish()) return false;
            field = Field::Id;
            if (!DecodeQuoted(line.substr(6), id)) { error = "Invalid PO msgid."; return false; }
        } else if (line.rfind("msgstr ", 0) == 0) {
            if (field != Field::Id) { error = "Unexpected PO msgstr."; return false; }
            field = Field::String;
            if (!DecodeQuoted(line.substr(7), value)) { error = "Invalid PO msgstr."; return false; }
        } else if (line.front() == '"') {
            if (field == Field::None || !DecodeQuoted(line, field == Field::Id ? id : value)) {
                error = "Invalid PO continuation."; return false;
            }
        } else {
            error = "Unsupported PO directive."; return false;
        }
    }
    return finish();
}

std::string Pseudo(std::string_view text) {
    std::string output;
    output.reserve(text.size() * 3 / 2 + 8);
    bool format = false;
    for (char c : text) {
        if (c == '{') format = true;
        if (!format) {
            switch (c) {
            case 'a': output += "á"; break; case 'A': output += "Á"; break;
            case 'e': output += "é"; break; case 'E': output += "É"; break;
            case 'i': output += "í"; break; case 'I': output += "Í"; break;
            case 'o': output += "ó"; break; case 'O': output += "Ó"; break;
            case 'u': output += "ú"; break; case 'U': output += "Ú"; break;
            case 'n': output += "ñ"; break; case 'N': output += "Ñ"; break;
            default: output.push_back(c); break;
            }
        } else output.push_back(c);
        if (c == '}') format = false;
    }
    const std::size_t padding = text.size() * 3 / 10;
    if (padding) { output += " ["; output.append(padding, '~'); output += ']'; }
    return output;
}

void EnsureCatalogs() {
    std::call_once(catalogOnce, [] {
        std::string error;
        for (std::size_t i = 0; i < Count; ++i)
            if (!ParsePoImpl(Table[i].Source(), catalogs[i], error)) catalogs[i].clear();
    });
}

const char* Lookup(const Catalog& catalog, const char* id) {
    const auto found = catalog.find(id ? id : "");
    return found == catalog.end() || found->second.empty() ? nullptr : found->second.c_str();
}

const char* EnglishText(const char* id) {
    EnsureCatalogs();
    if (const auto* value = Lookup(catalogs[0], id)) return value;
    return id ? id : "";
}
}

bool ValidPreference(std::string_view preference) {
    return preference == "auto" || FindTag(preference) != nullptr;
}

Locale ResolveLocale(std::string_view preference, std::string_view gameLanguage,
    const std::vector<std::string>& windowsLanguages) {
    if (const auto* entry = FindTag(preference)) return LocaleOf(entry);
    std::vector<std::string> languages;
    for (const auto& language : windowsLanguages) languages.push_back(Normalize(language));
    if (!gameLanguage.empty() && gameLanguage != "ENG") {
        // The game has one Spanish; a Latin American Windows keeps es-419.
        const auto* latin = FindTag("es-419");
        if (gameLanguage == "SPA" && latin &&
            std::any_of(languages.begin(), languages.end(), [](const std::string& language) { return LatinAmericanSpanish(language); }))
            return LocaleOf(latin);
        for (const auto& entry : Table)
            if (entry.gameCode && gameLanguage == entry.gameCode) return LocaleOf(&entry);
    }
    for (const auto& language : languages)
        if (const auto* entry = MatchWindowsLanguage(language)) return LocaleOf(entry);
    return Locale::En;
}

std::string_view NextPreference(std::string_view preference, int delta) {
    const std::size_t total = Count + 1;
    const auto* entry = FindTag(preference);
    const std::size_t current = entry ? static_cast<std::size_t>(entry - Table) + 1 : 0;
    const std::size_t next = (current + (delta > 0 ? 1 : total - 1)) % total;
    return next == 0 ? std::string_view("auto") : std::string_view(Table[next - 1].tag);
}

void SetActive(Locale locale) {
    EnsureCatalogs();
    activeIndex.store(IndexOf(locale), std::memory_order_release);
}

Locale Active() {
    // Pseudo is the English catalog transformed, so it reports as English.
    const auto index = activeIndex.load(std::memory_order_acquire);
    return index < Count ? static_cast<Locale>(index) : Locale::En;
}

const char* T(const char* id) {
    EnsureCatalogs();
    const auto index = activeIndex.load(std::memory_order_acquire);
    if (const auto* value = Lookup(catalogs[index], id)) return value;
    if (index != 0) if (const auto* value = Lookup(catalogs[0], id)) return value;
    return id ? id : "";
}

const char* NativeName(Locale locale) { return Table[IndexOf(locale)].nativeName; }
const char* Tag(Locale locale) { return Table[IndexOf(locale)].tag; }
Script ScriptOf(Locale locale) { return Table[IndexOf(locale)].script; }
std::string DisplayText(Locale locale) {
    EnsureCatalogs();
    std::string text;
    for (const auto& entry : catalogs[IndexOf(locale)]) text += entry.second;
    return text;
}

namespace detail {
// The catalog test proves every translation keeps the English placeholder
// indexes, so the fallback only fires when a translation alters a format spec
// such as {0:.2f}. English is looked up on that path only.
std::string Format(const char* translated, const char* id, fmt::format_args arguments) {
    try { return fmt::vformat(translated ? translated : "", arguments); }
    catch (const fmt::format_error&) {}
    const char* english = EnglishText(id);
    try { return fmt::vformat(english, arguments); }
    catch (const fmt::format_error&) { return english; }
}
}

namespace testing {
bool ParsePo(std::string_view source, Catalog& entries, std::string& error) {
    return ParsePoImpl(source, entries, error);
}
void SetPseudoActive() {
    EnsureCatalogs();
    std::call_once(pseudoOnce, [] {
        for (const auto& entry : catalogs[0]) catalogs[PseudoIndex].emplace(entry.first, Pseudo(entry.second));
    });
    activeIndex.store(PseudoIndex, std::memory_order_release);
}
}

} } // namespace sf4e::loc
