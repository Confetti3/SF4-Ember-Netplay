#include "GameLanguage.hxx"
#include "SteamKeyValues.hxx"

#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

namespace sf4e { namespace loc {
namespace {
// SSFIV.exe's code table (0xA70808) without its ENG filler rows, in the game's
// order, so the first match is the one the game's strncmp loop finds.
constexpr std::array<const char*, 13> GameCodes = {
    "JPN", "ENG", "FRA", "ITA", "GER", "SPA", "KOR", "RUS", "POL", "DUT", "CHI", "CZE", "BRA"};

// The Steam names the game maps, byte-exact. Every other name is ENG.
constexpr std::pair<const char*, const char*> SteamNames[] = {
    {"japanese", "JPN"}, {"french", "FRA"}, {"italian", "ITA"}, {"german", "GER"},
    {"spanish", "SPA"}, {"koreana", "KOR"}, {"russian", "RUS"}, {"polish", "POL"},
    {"dutch", "DUT"}, {"schinese", "CHI"}, {"tchinese", "CHI"}, {"czech", "CZE"},
    {"brazilian", "BRA"}};

bool ReadFile(const std::filesystem::path& path, std::string& contents) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    contents.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

}

std::string GameLanguageFromConfig(std::string_view languageCfg) {
    if (languageCfg.size() < 3) return {};
    for (const char* code : GameCodes)
        if (languageCfg.substr(0, 3) == code) return code;
    return {};
}

std::string GameLanguageFromSteamName(std::string_view steamLanguage) {
    for (const auto& name : SteamNames)
        if (steamLanguage == name.first) return name.second;
    return "ENG";
}

std::string SteamLanguageFromManifest(std::string_view appmanifest) {
    using steam::Token;
    std::vector<Token> tokens;
    // A damaged manifest reads as having no language rather than a guess.
    if (!steam::Tokenize(std::string(appmanifest), tokens)) return {};
    // One root section, AppState. Inside it, section names down to the current
    // depth; a key is text followed by an opening brace (a section) or by text
    // (a value). The language counts only once the whole file has parsed.
    if (tokens.size() < 2 || tokens[0].kind != Token::Text || !EqualsIgnoreCase(tokens[0].text, "AppState") ||
        tokens[1].kind != Token::Open) return {};
    std::vector<std::string> path{tokens[0].text};
    std::string language;
    std::size_t i = 2;
    for (; i < tokens.size() && !path.empty(); ++i) {
        if (tokens[i].kind == Token::Close) { path.pop_back(); continue; }
        if (tokens[i].kind != Token::Text || i + 1 >= tokens.size()) return {};
        const auto& key = tokens[i].text;
        const auto& value = tokens[++i];
        if (value.kind == Token::Open) { path.push_back(key); continue; }
        if (value.kind != Token::Text) return {};
        if (path.size() == 2 && EqualsIgnoreCase(path[1], "UserConfig") && EqualsIgnoreCase(key, "language"))
            language = value.text;
    }
    return path.empty() && i == tokens.size() ? language : std::string();
}

std::string DetectGameLanguage(const std::filesystem::path& gameDirectory,
    std::string_view steamClientLanguage) {
    if (gameDirectory.empty()) return {};
    std::string contents;
    if (ReadFile(gameDirectory / "language.cfg", contents)) {
        auto code = GameLanguageFromConfig(contents);
        if (!code.empty()) return code;
    }
    // <library>\steamapps\common\<installdir>: the manifest sits in steamapps.
    auto directory = gameDirectory;
    if (!directory.has_filename()) directory = directory.parent_path();
    const auto manifest = directory.parent_path().parent_path() / "appmanifest_45760.acf";
    if (ReadFile(manifest, contents)) {
        const auto steamLanguage = SteamLanguageFromManifest(contents);
        if (!steamLanguage.empty()) return GameLanguageFromSteamName(steamLanguage);
    }
    if (!steamClientLanguage.empty()) return GameLanguageFromSteamName(steamClientLanguage);
    return {};
}

} } // namespace sf4e::loc
