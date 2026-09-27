#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace sf4e { namespace loc {

// USF4's language choice, reproduced from SSFIV.exe (0x6B1B70) so the launcher
// can know it before the game runs. The game reads the first three bytes of
// language.cfg against its code table, then asks Steam for the game language,
// and treats anything else as ENG. Results are the game's codes: JPN, ENG, FRA,
// ITA, GER, SPA, KOR, RUS, POL, DUT, CHI, CZE, BRA.

// The code for language.cfg's contents, or empty when the game would move on
// to Steam. Byte-exact like the game: no trimming, case or BOM handling.
std::string GameLanguageFromConfig(std::string_view languageCfg);
// The code for a Steam language API name such as "koreana". Unknown names are
// ENG, as in the game.
std::string GameLanguageFromSteamName(std::string_view steamLanguage);
// UserConfig.language from an appmanifest, or empty when absent.
std::string SteamLanguageFromManifest(std::string_view appmanifest);
// The game's choice for an install folder. Steam's game language comes from
// the library's appmanifest_45760.acf, then from steamClientLanguage (the
// client's own setting, which Steam uses when the game has none). Empty when
// no source is available, so the caller can fall back to Windows.
std::string DetectGameLanguage(const std::filesystem::path& gameDirectory,
    std::string_view steamClientLanguage);

} } // namespace sf4e::loc
