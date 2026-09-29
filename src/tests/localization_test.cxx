#include "../common/Localization.hxx"
#include "../ui/Theme.hxx"
#include "EmbeddedFonts.hxx"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <cstdlib>
#include <regex>

// ImGui's own copy, private to this test, to read the embedded fonts' cmaps.
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <imstb_truetype.h>

#include "test_support.hxx"

using sf4e::loc::Locale;

static std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    CHECK(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

static std::set<unsigned> Placeholders(const std::string& value) {
    std::set<unsigned> result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '{' || i + 1 >= value.size() || value[i + 1] == '{') continue;
        std::size_t end = i + 1;
        unsigned index = 0;
        bool any = false;
        while (end < value.size() && value[end] >= '0' && value[end] <= '9') {
            any = true; index = index * 10 + unsigned(value[end++] - '0');
        }
        if (any && end < value.size() && (value[end] == '}' || value[end] == ':')) result.insert(index);
    }
    return result;
}

static std::vector<unsigned> Codepoints(const std::string& text) {
    std::vector<unsigned> result;
    for (std::size_t i = 0; i < text.size();) {
        unsigned char first = static_cast<unsigned char>(text[i++]);
        unsigned value = first; int remaining = 0;
        if ((first & 0xe0) == 0xc0) { value = first & 0x1f; remaining = 1; }
        else if ((first & 0xf0) == 0xe0) { value = first & 0x0f; remaining = 2; }
        else if ((first & 0xf8) == 0xf0) { value = first & 0x07; remaining = 3; }
        for (int n = 0; n < remaining; ++n) {
            CHECK(i < text.size()); const auto next = static_cast<unsigned char>(text[i++]);
            CHECK((next & 0xc0) == 0x80); value = (value << 6) | (next & 0x3f);
        }
        result.push_back(value);
    }
    return result;
}

static bool HasGlyph(const unsigned char* font, unsigned codepoint) {
    stbtt_fontinfo info;
    CHECK(stbtt_InitFont(&info, font, stbtt_GetFontOffsetForIndex(font, 0)));
    return stbtt_FindGlyphIndex(&info, static_cast<int>(codepoint)) != 0;
}

// Whether the atlas a locale builds can draw a codepoint, judged by the font
// bytes that ship: a codepoint in Inter's baked ranges that both Inter weights
// have, or one the embedded font for the locale's script has.
static bool Covered(Locale locale, unsigned codepoint) {
    namespace fonts = sf4e::ui::fonts;
    for (const ImWchar* range = sf4e::ui::UiGlyphRanges; range[0] && range[1]; range += 2)
        if (codepoint >= range[0] && codepoint <= range[1] &&
            HasGlyph(fonts::Body, codepoint) && HasGlyph(fonts::Heading, codepoint)) return true;
    for (const auto& font : fonts::ScriptFonts)
        if (font.script == sf4e::loc::ScriptOf(locale)) return HasGlyph(font.data, codepoint);
    return false;
}

int main(int argc, char** argv) {
    using namespace sf4e::loc;
    CHECK(ResolveLocale("en", "", {"pt-BR"}) == Locale::En);
    CHECK(ResolveLocale("pt-BR", "JPN", {"en-US"}) == Locale::PtBR);
    CHECK(ResolveLocale("es-419", "", {}) == Locale::Es419);
    CHECK(ResolveLocale("auto", "", {"xx-XX", "es-MX", "en-US"}) == Locale::Es419);
    CHECK(ResolveLocale("auto", "", {"PT_br"}) == Locale::PtBR);
    CHECK(ResolveLocale("auto", "", {"EN-us"}) == Locale::En);
    CHECK(ResolveLocale("invalid", "", {"xx-XX", "pt-PT"}) == Locale::PtBR);
    CHECK(ResolveLocale("auto", "", {}) == Locale::En);
    // The game's language leads, except ENG, which the game also uses for
    // every language it lacks.
    CHECK(ResolveLocale("auto", "BRA", {"en-US"}) == Locale::PtBR);
    CHECK(ResolveLocale("auto", "ENG", {"pt-BR"}) == Locale::PtBR);
    CHECK(ResolveLocale("auto", "XYZ", {"pt-BR"}) == Locale::PtBR);
    // One Spanish in the game: Latin American Windows keeps es-419.
    CHECK(ResolveLocale("auto", "SPA", {"es-MX"}) == Locale::Es419);
    CHECK(ResolveLocale("auto", "SPA", {"en-US", "es-419"}) == Locale::Es419);
    // Spanish regions: the primary subtag alone is not enough.
    CHECK(ResolveLocale("auto", "", {"es-US"}) == Locale::Es419);
    CHECK(ResolveLocale("auto", "", {"esx-MX"}) == Locale::En);
    CHECK(ResolveLocale("auto", "", {"e"}) == Locale::En);
    CHECK(ResolveLocale("auto", "", {"es-ES"}) == Locale::EsES);
    CHECK(ResolveLocale("auto", "", {"es"}) == Locale::EsES);
    CHECK(ResolveLocale("auto", "", {"es-GQ"}) == Locale::EsES);
    CHECK(ResolveLocale("auto", "SPA", {"en-US"}) == Locale::EsES);
    CHECK(ResolveLocale("auto", "SPA", {}) == Locale::EsES);
    // Every USF4 language but English selects its catalog.
    const std::pair<const char*, Locale> games[] = {{"JPN", Locale::Ja}, {"FRA", Locale::Fr}, {"ITA", Locale::It},
        {"GER", Locale::De}, {"SPA", Locale::EsES}, {"KOR", Locale::Ko}, {"RUS", Locale::Ru}, {"POL", Locale::Pl},
        {"DUT", Locale::Nl}, {"CHI", Locale::ZhHans}, {"CZE", Locale::Cs}, {"BRA", Locale::PtBR}};
    for (const auto& game : games) CHECK(ResolveLocale("auto", game.first, {"en-US"}) == game.second);
    // The game's single Chinese is Simplified; it serves every Chinese Windows.
    CHECK(ResolveLocale("auto", "", {"zh-TW"}) == Locale::ZhHans);
    CHECK(ResolveLocale("auto", "", {"zh-Hant-HK"}) == Locale::ZhHans);
    CHECK(ResolveLocale("auto", "", {"zh-CN"}) == Locale::ZhHans);
    CHECK(ResolveLocale("auto", "", {"ja-JP"}) == Locale::Ja);
    CHECK(ResolveLocale("auto", "", {"fr-CA"}) == Locale::Fr);
    CHECK(ResolveLocale("auto", "", {"nl-BE"}) == Locale::Nl);
    CHECK(ResolveLocale("auto", "", {"xx-XX", "de-AT", "ru-RU"}) == Locale::De);
    CHECK(ScriptOf(Locale::Ru) == sf4e::loc::Script::Cyrillic && ScriptOf(Locale::Ko) == sf4e::loc::Script::Korean);
    for (int i = 0; i < static_cast<int>(Locale::Count); ++i) {
        const auto locale = static_cast<Locale>(i);
        CHECK(ResolveLocale(Tag(locale), "", {}) == locale);
        CHECK(ResolveLocale("auto", "", {Tag(locale)}) == locale);
    }
    CHECK(ValidPreference("auto") && ValidPreference("en") && ValidPreference("pt-BR") && ValidPreference("es-419"));
    CHECK(!ValidPreference("PT-br") && !ValidPreference("") && !ValidPreference("xx") && !ValidPreference("zh"));
    CHECK(Active() == Locale::En);

    // Cycling visits every tag in table order in both directions and wraps at "auto".
    std::string_view preference = "auto";
    for (int i = 0; i < static_cast<int>(Locale::Count); ++i) {
        preference = NextPreference(preference, 1);
        CHECK(preference == Tag(static_cast<Locale>(i)));
    }
    CHECK(NextPreference(preference, 1) == "auto");
    CHECK(NextPreference("en", -1) == "auto");
    CHECK(NextPreference("auto", -1) == Tag(static_cast<Locale>(static_cast<int>(Locale::Count) - 1)));
    CHECK(NextPreference("xx", 1) == "en");

    Catalog parsed;
    std::string error;
    CHECK(testing::ParsePo("# comment\nmsgid \"one\"\nmsgstr \"line 1\\n\"\n\"line \\\"2\\\" \\\\\"\n", parsed, error));
    CHECK(parsed["one"] == "line 1\nline \"2\" \\");
    CHECK(!testing::ParsePo("msgid \"cut\"\n", parsed, error));
    CHECK(!testing::ParsePo("msgctxt \"x\"\nmsgid \"a\"\nmsgstr \"b\"\n", parsed, error));
    CHECK(!testing::ParsePo("msgid \"same\"\nmsgstr \"a\"\n\nmsgid \"same\"\nmsgstr \"b\"\n", parsed, error));
    // A trailing backslash leaves the literal unterminated, it is not an escaped quote.
    CHECK(!testing::ParsePo("msgid \"a\"\nmsgstr \"cut\\\"\n", parsed, error));

    CHECK(argc >= 2);
    const auto root = std::filesystem::path(argv[1]);
    constexpr std::size_t count = static_cast<std::size_t>(Locale::Count);
    Catalog catalogs[count];
    for (std::size_t i = 0; i < count; ++i) {
        const auto locale = static_cast<Locale>(i);
        const auto source = Read(root / "locales" / (std::string(Tag(locale)) + ".po"));
        CHECK(testing::ParsePo(source, catalogs[i], error));
        CHECK(!catalogs[i].empty());
        for (const auto& entry : catalogs[i]) CHECK(!entry.first.empty() && !entry.second.empty());
    }
    for (std::size_t i = 1; i < count; ++i) {
        CHECK(catalogs[i].size() == catalogs[0].size());
        for (const auto& entry : catalogs[0]) {
            if (!catalogs[i].count(entry.first)) {
                std::cerr << Tag(static_cast<Locale>(i)) << " lacks " << entry.first << '\n'; std::exit(1);
            }
            CHECK(Placeholders(entry.second) == Placeholders(catalogs[i][entry.first]));
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto locale = static_cast<Locale>(i);
        std::vector<unsigned> text = Codepoints(NativeName(locale));
        for (const auto& entry : catalogs[i]) for (const auto codepoint : Codepoints(entry.second)) text.push_back(codepoint);
        for (const auto codepoint : text) {
            if (codepoint < 0x20 || Covered(locale, codepoint)) continue;
            std::cerr << Tag(locale) << " uses U+" << std::hex << codepoint << " that its fonts lack; "
                "run scripts/subset-cjk-fonts.py for CJK catalogs\n";
            std::exit(1);
        }
    }

    // An instruction that sends the player to a control names it with the words
    // the control's own label uses. Polish, Czech and Russian inflect the label
    // inside a sentence, and French and German phrase the connection check
    // without naming the row.
    {
        struct Reference { const char* label; std::vector<const char*> uses; std::vector<Locale> exempt; };
        const std::vector<Locale> inflected = {Locale::Pl, Locale::Cs, Locale::Ru};
        const std::vector<Reference> references = {
            {"home.settings", {"room.controller_required", "room.reject.name_taken"}, inflected},
            {"screen.player", {"room.controller_required"}, inflected},
            {"home.fighter_select", {"runtime.selection_unavailable", "runtime.ready.stage_unavailable",
                "runtime.ready.fighter_unavailable"}, inflected},
            {"connection.check", {"room.apply_recommendation.check_first"}, {Locale::Pl, Locale::Cs, Locale::Ru, Locale::Fr, Locale::De}},
            {"room.abandon_result", {"room.result_unresolved.detail"}, inflected},
        };
        for (const auto& reference : references)
            for (std::size_t i = 0; i < count; ++i) {
                const auto locale = static_cast<Locale>(i);
                if (std::find(reference.exempt.begin(), reference.exempt.end(), locale) != reference.exempt.end()) continue;
                const auto& label = catalogs[i][reference.label];
                for (const auto* use : reference.uses)
                    if (catalogs[i][use].find(label) == std::string::npos) {
                        std::cerr << Tag(locale) << ": " << use << " does not name \"" << label << "\" (" << reference.label << ")\n";
                        std::exit(1);
                    }
            }
        // A row and the screen it opens are one thing to the player.
        for (std::size_t i = 0; i < count; ++i) CHECK(catalogs[i]["room.settings"] == catalogs[i]["screen.room_settings"]);
    }
    // The room's error ids and the session client's stand in the catalogs, so a
    // failure the client reports is a sentence and never an internal code.
    for (const char* id : {"runtime.room_request_failed", "runtime.build_mismatch", "room.catching_up", "room.control_recovering"})
        for (std::size_t i = 0; i < count; ++i) CHECK(catalogs[i].count(id));

    // Names, room names and chat are drawn by the embedded fonts whatever the
    // interface language, so each script font holds the characters people type.
    {
        const auto has = [&](sf4e::loc::Script script, unsigned codepoint) {
            for (const auto& font : sf4e::ui::fonts::ScriptFonts)
                if (font.script == script) return HasGlyph(font.data, codepoint);
            return false;
        };
        // Chinese names, Japanese names in kanji and kana, Korean names in Hangul.
        for (const unsigned codepoint : {0x5F20u, 0x4F1Fu, 0x738Bu, 0x674Eu, 0x5218u})
            CHECK(has(sf4e::loc::Script::SimplifiedChinese, codepoint));
        for (const unsigned codepoint : {0x7530u, 0x4E2Du, 0x592Au, 0x90CEu, 0x82B1u, 0x5B50u, 0x3042u, 0x30A2u, 0x30FCu})
            CHECK(has(sf4e::loc::Script::Japanese, codepoint));
        for (const unsigned codepoint : {0xAE40u, 0xBBFCu, 0xC218u, 0xC774u, 0xBC15u, 0xD55Cu})
            CHECK(has(sf4e::loc::Script::Korean, codepoint));
    }

    std::string sources;
    for (const auto& item : std::filesystem::recursive_directory_iterator(root / "src")) {
        if (!item.is_regular_file()) continue;
        const auto extension = item.path().extension().string();
        if (extension == ".cxx" || extension == ".hxx" || extension == ".cpp" || extension == ".h")
            sources += Read(item.path());
    }
    const std::regex lookup("\\b(?:T|Tf)\\(\\s*\"([^\"]+)\"");
    for (std::sregex_iterator it(sources.begin(), sources.end(), lookup), end; it != end; ++it) {
        const auto id = (*it)[1].str();
        if (id == "missing.test.id") continue;
        if (!catalogs[0].count(id)) { std::cerr << "Missing catalog id: " << id << '\n'; std::exit(1); }
    }
    for (const auto& entry : catalogs[0])
        CHECK(sources.find("\"" + entry.first + "\"") != std::string::npos);

    int value = 7;
    const auto args = fmt::make_format_args(value);
    CHECK(detail::Format("{0} frames", "connection.frames", args) == "7 frames");
    CHECK(detail::Format("broken {", "connection.frames", args) == "7 frames");
    CHECK(DisplayText(Locale::PtBR).find("Idioma") != std::string::npos);
    CHECK(DisplayText(Locale::Ja).find("settings.language") == std::string::npos);
    SetActive(Locale::PtBR); CHECK(std::string(T("settings.language")) == "Idioma");
    SetActive(Locale::Es419); CHECK(std::string(T("settings.language")) == "Idioma");
    SetActive(Locale::En); CHECK(std::string(T("missing.test.id")) == "missing.test.id");
    CHECK(std::string(Tag(Locale::PtBR)) == "pt-BR" && std::string(NativeName(Locale::Es419)) == "Español (Latinoamérica)");
    std::cout << "Localization resolution, parser, catalogs and formatting passed\n";
}
