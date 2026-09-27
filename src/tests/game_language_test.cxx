#include "../common/GameLanguage.hxx"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>

#include "test_support.hxx"

using namespace sf4e::loc;
namespace fs = std::filesystem;

// Trimmed from a real manifest: UserConfig follows InstalledDepots, and
// MountedConfig carries a language too that must not be read instead.
static const std::string Manifest = R"acf("AppState"
{
	"appid"		"45760"
	"name"		"Ultra Street Fighter IV"
	"installdir"		"Super Street Fighter IV - Arcade Edition"
	"InstalledDepots"
	{
		"45761"
		{
			"manifest"		"7494827315436345520"
			"language"		"german"
		}
	}
	"UserConfig"
	{
		"language"		"koreana"
	}
	"MountedConfig"
	{
		"language"		"french"
	}
}
)acf";

static void Write(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << contents;
    CHECK(file.good());
}

int main() {
    // language.cfg: the first three bytes, compared exactly as the game does.
    CHECK(GameLanguageFromConfig("JPN") == "JPN");
    CHECK(GameLanguageFromConfig("RUS\r\n") == "RUS");
    CHECK(GameLanguageFromConfig("CHINESE") == "CHI");
    CHECK(GameLanguageFromConfig("ENG") == "ENG");
    CHECK(GameLanguageFromConfig("jpn").empty());
    CHECK(GameLanguageFromConfig(" JPN").empty());
    CHECK(GameLanguageFromConfig("\xEF\xBB\xBFJPN").empty());
    CHECK(GameLanguageFromConfig("JP").empty());
    CHECK(GameLanguageFromConfig("").empty());
    CHECK(GameLanguageFromConfig("XYZ").empty());

    // Steam names, including the ones the game folds into ENG or CHI.
    CHECK(GameLanguageFromSteamName("koreana") == "KOR");
    CHECK(GameLanguageFromSteamName("schinese") == "CHI");
    CHECK(GameLanguageFromSteamName("tchinese") == "CHI");
    CHECK(GameLanguageFromSteamName("brazilian") == "BRA");
    CHECK(GameLanguageFromSteamName("czech") == "CZE");
    CHECK(GameLanguageFromSteamName("portuguese") == "ENG");
    CHECK(GameLanguageFromSteamName("korean") == "ENG");
    CHECK(GameLanguageFromSteamName("German") == "ENG");
    CHECK(GameLanguageFromSteamName("") == "ENG");

    // Manifest parsing reads only AppState/UserConfig/language.
    CHECK(SteamLanguageFromManifest(Manifest) == "koreana");
    CHECK(SteamLanguageFromManifest("\xEF\xBB\xBF" + Manifest) == "koreana");
    std::string crlf;
    for (char c : Manifest) { if (c == '\n') crlf += '\r'; crlf += c; }
    CHECK(SteamLanguageFromManifest(crlf) == "koreana");
    CHECK(SteamLanguageFromManifest("\"AppState\"\n{\n\t\"usercONFIG\"\n\t{\n\t\t\"Language\"\t\t\"polish\"\n\t}\n}\n") == "polish");
    CHECK(SteamLanguageFromManifest("\"AppState\"\n{\n\t\"MountedConfig\"\n\t{\n\t\t\"language\"\t\t\"french\"\n\t}\n}\n").empty());
    CHECK(SteamLanguageFromManifest("\"AppState\"\n{\n\t\"language\"\t\t\"french\"\n}\n").empty());
    CHECK(SteamLanguageFromManifest("\"AppState\" { \"UserConfig\" { \"language\" ").empty());
    // Cut off after the value, or followed by a second root: the file is damaged.
    CHECK(SteamLanguageFromManifest("\"AppState\" { \"UserConfig\" { \"language\" \"polish\"").empty());
    CHECK(SteamLanguageFromManifest("\"AppState\" { \"UserConfig\" { \"language\" \"polish\" }").empty());
    CHECK(SteamLanguageFromManifest("\"AppState\" { \"UserConfig\" { \"language\" \"polish\" } } \"x\" { }").empty());
    CHECK(SteamLanguageFromManifest("\"AppState\" { \"UserConfig\" { \"language\" \"polish\" } } }").empty());
    CHECK(SteamLanguageFromManifest("\"Other\" { \"UserConfig\" { \"language\" \"polish\" } }").empty());
    // A quoted brace is text, never structure.
    CHECK(SteamLanguageFromManifest("\"AppState\"\n{\n\t\"name\"\t\t\"{\"\n\t\"UserConfig\"\n\t{\n\t\t\"language\"\t\t\"polish\"\n\t}\n}\n") == "polish");
    CHECK(SteamLanguageFromManifest("\"AppState\"\n{\n\t\"UserConfig\"\t\"}\"\n\t\"language\"\t\t\"polish\"\n}\n").empty());
    CHECK(SteamLanguageFromManifest("not a manifest").empty());
    CHECK(SteamLanguageFromManifest("").empty());

    // A library laid out as Steam does, in a scratch folder.
    const auto root = fs::temp_directory_path() / ("sf4e-game-language-" + std::to_string(std::random_device{}()));
    const auto game = root / "steamapps" / "common" / "Super Street Fighter IV - Arcade Edition";
    fs::create_directories(game);
    CHECK(DetectGameLanguage(game, "").empty());
    CHECK(DetectGameLanguage(game, "russian") == "RUS");
    CHECK(DetectGameLanguage(game, "thai") == "ENG");
    Write(root / "steamapps" / "appmanifest_45760.acf", Manifest);
    CHECK(DetectGameLanguage(game, "russian") == "KOR");
    // A trailing separator names the same folder.
    CHECK(DetectGameLanguage(game / "", "") == "KOR");
    Write(game / "language.cfg", "XYZ");
    CHECK(DetectGameLanguage(game, "") == "KOR");
    Write(game / "language.cfg", "JPN");
    CHECK(DetectGameLanguage(game, "") == "JPN");
    Write(game / "language.cfg", "ENG");
    CHECK(DetectGameLanguage(game, "") == "ENG");
    CHECK(DetectGameLanguage(fs::path(), "german").empty());
    std::error_code ignored;
    fs::remove_all(root, ignored);

    std::cout << "Game language detection passed\n";
}
