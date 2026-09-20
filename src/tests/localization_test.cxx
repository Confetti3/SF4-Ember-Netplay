#include "../common/Localization.hxx"
#include "../ui/Theme.hxx"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <cstdlib>
#include <regex>

#define CHECK(c) do { if (!(c)) { std::cerr << "Check failed at " << __LINE__ << ": " #c << '\n'; std::exit(1); } } while (false)

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

static bool Covered(unsigned codepoint) {
    for (const ImWchar* range = sf4e::ui::UiGlyphRanges; range[0] && range[1]; range += 2)
        if (codepoint >= range[0] && codepoint <= range[1]) return true;
    return false;
}

int main(int argc, char** argv) {
    using namespace sf4e::loc;
    CHECK(ResolveLocale("en", {"pt-BR"}) == Locale::En);
    CHECK(ResolveLocale("pt-BR", {"en-US"}) == Locale::PtBR);
    CHECK(ResolveLocale("es-419", {}) == Locale::Es419);
    CHECK(ResolveLocale("auto", {"fr-FR", "es-MX", "en-US"}) == Locale::Es419);
    CHECK(ResolveLocale("auto", {"PT_br"}) == Locale::PtBR);
    CHECK(ResolveLocale("auto", {"EN-us"}) == Locale::En);
    CHECK(ResolveLocale("invalid", {"fr-FR", "pt-PT"}) == Locale::PtBR);
    CHECK(ResolveLocale("auto", {}) == Locale::En);
    CHECK(ValidPreference("auto") && ValidPreference("en") && ValidPreference("pt-BR") && ValidPreference("es-419"));
    CHECK(!ValidPreference("PT-br") && !ValidPreference("") && !ValidPreference("fr"));
    CHECK(Active() == Locale::En);

    // Cycling stays inside the table in both directions and wraps at "auto".
    std::string_view preference = "auto";
    for (const char* expected : {"en", "pt-BR", "es-419", "auto", "en"}) {
        preference = NextPreference(preference, 1);
        CHECK(preference == expected);
    }
    CHECK(NextPreference("en", -1) == "auto");
    CHECK(NextPreference("auto", -1) == "es-419");
    CHECK(NextPreference("fr", 1) == "en");

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
    Catalog catalogs[3];
    const char* files[] = {"en.po", "pt-BR.po", "es-419.po"};
    for (int i = 0; i < 3; ++i) {
        CHECK(testing::ParsePo(Read(root / "locales" / files[i]), catalogs[i], error));
        CHECK(!catalogs[i].empty());
        for (const auto& entry : catalogs[i]) CHECK(!entry.first.empty() && !entry.second.empty());
    }
    CHECK(catalogs[0].size() == catalogs[1].size() && catalogs[0].size() == catalogs[2].size());
    for (const auto& entry : catalogs[0]) {
        CHECK(catalogs[1].count(entry.first) && catalogs[2].count(entry.first));
        CHECK(Placeholders(entry.second) == Placeholders(catalogs[1][entry.first]));
        CHECK(Placeholders(entry.second) == Placeholders(catalogs[2][entry.first]));
    }
    for (const auto& catalog : catalogs) for (const auto& entry : catalog)
        for (const auto codepoint : Codepoints(entry.second)) CHECK(codepoint < 0x20 || Covered(codepoint));

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
    SetActive(Locale::PtBR); CHECK(std::string(T("settings.language")) == "Idioma");
    SetActive(Locale::Es419); CHECK(std::string(T("settings.language")) == "Idioma");
    SetActive(Locale::En); CHECK(std::string(T("missing.test.id")) == "missing.test.id");
    CHECK(std::string(Tag(Locale::PtBR)) == "pt-BR" && std::string(NativeName(Locale::Es419)) == "Español (Latinoamérica)");
    std::cout << "Localization resolution, parser, catalogs and formatting passed\n";
}
