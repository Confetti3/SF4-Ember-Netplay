#include "../launcher/GameLocator.hxx"

#include <string>
#include <vector>

#include "test_support.hxx"

using sf4e::launcher::GameExecutable;
using sf4e::launcher::LibraryCandidates;
using sf4e::launcher::LocateGame;
using sf4e::launcher::ParseLibraryFolders;
using sf4e::launcher::RememberedFolder;
using sf4e::launcher::ShadowingRuntimeLibraries;
using Paths = std::vector<std::wstring>;

// The current format, as Steam writes it, with the Steam folder as "0".
static const std::string Nested = R"vdf("libraryfolders"
{
	"TimeNextStatsReport"		"1695600000"
	"ContentStatsID"		"-4467011487932427389"
	"0"
	{
		"path"		"C:\\Program Files (x86)\\Steam"
		"label"		""
		"contentid"		"1234567890"
		"totalsize"		"0"
		"apps"
		{
			"228980"		"123456"
		}
	}
	"1"
	{
		"path"		"D:\\SteamLibrary"
		"apps"
		{
			"45760"		"9876543210"
		}
	}
	"2"
	{
		"path"		"E:\\Games\\Steam"
		"apps"
		{
		}
	}
}
)vdf";

// The older format lists each library as a numbered value.
static const std::string Flat = R"vdf("LibraryFolders"
{
	"TimeNextStatsReport"		"1695600000"
	"ContentStatsID"		"-4467011487932427389"
	"1"		"D:\\SteamLibrary"
	"2"		"F:\\More Games"
}
)vdf";

int main() {
    CHECK((ParseLibraryFolders(Nested) == Paths{L"C:\\Program Files (x86)\\Steam", L"D:\\SteamLibrary", L"E:\\Games\\Steam"}));
    CHECK((ParseLibraryFolders(Flat) == Paths{L"D:\\SteamLibrary", L"F:\\More Games"}));
    CHECK((ParseLibraryFolders("\xEF\xBB\xBF" + Flat) == Paths{L"D:\\SteamLibrary", L"F:\\More Games"}));

    // Steam writes UTF-8, so accented folders must survive the conversion.
    const std::string accented = "\"libraryfolders\"\n{\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"E:\\\\Jogos Instala\xc3\xa7\xc3\xa3o\\\\SteamLibrary\"\n\t}\n}\n";
    CHECK((ParseLibraryFolders(accented) == Paths{L"E:\\Jogos Instala\u00e7\u00e3o\\SteamLibrary"}));

    // More than the old limit of eight, in numeric rather than text order.
    std::string many = "\"libraryfolders\"\n{\n";
    Paths expected;
    for (int i = 1; i <= 10; ++i) {
        many += "\t\"" + std::to_string(i) + "\"\n\t{\n\t\t\"path\"\t\t\"X:\\\\Library" + std::to_string(i) + "\"\n\t}\n";
        expected.push_back(L"X:\\Library" + std::to_wstring(i));
    }
    many += "}\n";
    CHECK(ParseLibraryFolders(many) == expected);

    // Unreadable text yields nothing rather than an exception or a crash.
    CHECK(ParseLibraryFolders("").empty());
    CHECK(ParseLibraryFolders("this is not a vdf file {").empty());
    CHECK(ParseLibraryFolders("\"path\" \"D:\\\\Lib\"").empty());
    CHECK(ParseLibraryFolders("}\"libraryfolders\" { }").empty());
    CHECK(ParseLibraryFolders("\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"").empty());
    for (std::size_t length = 0; length < Nested.rfind('}'); ++length)
        CHECK(ParseLibraryFolders(Nested.substr(0, length)).empty());
    CHECK(ParseLibraryFolders(R"vdf("libraryfolders" { "1" "D:\\Lib)vdf").empty());
    CHECK(ParseLibraryFolders(R"vdf("libraryfolders" { "1" "D:\\Lib\)vdf").empty());
    CHECK(ParseLibraryFolders(R"vdf("libraryfolders" { "1" "D:\\Lib\")vdf").empty());
    CHECK(ParseLibraryFolders(R"vdf("libraryfolders" { "1" "D:\\Lib" } })vdf").empty());
    CHECK(ParseLibraryFolders(R"vdf("libraryfolders" { "1" "D:\\Lib" } "extra" { })vdf").empty());
    CHECK(ParseLibraryFolders(R"vdf("libraryfolders" { "1" "D:\\Lib" } / not a comment)vdf").empty());
    CHECK(ParseLibraryFolders(R"vdf("libraryfolders" { "1" D:\\Lib })vdf").empty());

    // Comments are skipped before the root block and inside a block.
    const std::size_t second = Nested.find("\t\"1\"");
    const std::string commented = "// Written by Steam\n" + Nested.substr(0, second) +
        "\t// A second drive\n" + Nested.substr(second) + "// trailing note";
    CHECK((ParseLibraryFolders(commented) == Paths{L"C:\\Program Files (x86)\\Steam", L"D:\\SteamLibrary", L"E:\\Games\\Steam"}));
    CHECK((ParseLibraryFolders(R"vdf("libraryfolders" { // no newline after this comment)vdf").empty()));

    // Escaped quotes and backslashes are decoded.
    const std::string escaped = R"vdf("libraryfolders" { "1" { "path" "D:\\Games\\\"Quoted\"\\Lib" } })vdf";
    CHECK((ParseLibraryFolders(escaped) == Paths{L"D:\\Games\\\"Quoted\"\\Lib"}));

    // Includes are not followed, so a file that uses one is refused.
    CHECK(ParseLibraryFolders("#base \"other.vdf\"\n" + Nested).empty());
    CHECK(ParseLibraryFolders("#include \"other.vdf\"\n" + Flat).empty());

    // The Steam folder comes first, and repeats of it in any spelling are dropped.
    const std::string repeats = R"vdf("libraryfolders"
{
	"0" { "path" "C:\\Program Files (x86)\\Steam" }
	"1" { "path" "D:\\SteamLibrary" }
	"2" { "path" "C:\\Program Files (x86)\\Steam\\" }
	"3" { "path" "E:\\Games" }
}
)vdf";
    CHECK((LibraryCandidates(L"c:/program files (x86)/steam", repeats) ==
        Paths{L"c:\\program files (x86)\\steam", L"D:\\SteamLibrary", L"E:\\Games"}));
    CHECK((LibraryCandidates(L"C:\\Program Files (x86)\\Steam\\", repeats) ==
        Paths{L"C:\\Program Files (x86)\\Steam\\", L"D:\\SteamLibrary", L"E:\\Games"}));
    CHECK((LibraryCandidates(L"c:/program files (x86)/steam", "") == Paths{L"c:\\program files (x86)\\steam"}));
    CHECK((LibraryCandidates(L"", Flat) == Paths{L"D:\\SteamLibrary", L"F:\\More Games"}));

    // The executable is joined to the folder with exactly one separator.
    Paths asked;
    const auto present = [&asked](const std::wstring& path) { asked.push_back(path); return true; };
    const auto absent = [](const std::wstring&) { return false; };
    CHECK(GameExecutable(L"D:\\Games\\Ultra", present) == L"D:\\Games\\Ultra\\SSFIV.exe");
    CHECK(GameExecutable(L"D:\\Games\\Ultra\\", present) == L"D:\\Games\\Ultra\\SSFIV.exe");
    CHECK((asked == Paths{L"D:\\Games\\Ultra\\SSFIV.exe", L"D:\\Games\\Ultra\\SSFIV.exe"}));
    CHECK(GameExecutable(L"D:\\Games\\Ultra", absent).empty());
    CHECK(GameExecutable(L"", present).empty());

    // A folder counts as saved only after the write succeeds, so a failed
    // write is attempted again on the next round.
    Paths written;
    bool failNext = true;
    const auto writer = [&](const std::wstring& folder) {
        written.push_back(folder);
        const bool ok = !failNext;
        failNext = false;
        return ok;
    };
    const std::wstring f = L"D:\\Games\\Ultra", g = L"E:\\Ultra";
    RememberedFolder remembered{L""};
    CHECK(!remembered.Remember(f, writer));
    CHECK(remembered.Persisted().empty());
    CHECK((written == Paths{f}));
    CHECK(remembered.Remember(f, writer));
    CHECK(remembered.Persisted() == f);
    CHECK((written == Paths{f, f}));
    CHECK(remembered.Remember(f, writer));
    CHECK((written == Paths{f, f}));
    // Other spellings of the saved folder are not written again.
    CHECK(remembered.Remember(L"d:\\games\\ultra\\", writer));
    CHECK(remembered.Remember(L"D:/Games/Ultra/", writer));
    CHECK(remembered.Remember(L"", writer));
    CHECK((written == Paths{f, f}));
    CHECK(remembered.Persisted() == f);
    CHECK(remembered.Remember(g, writer));
    CHECK(remembered.Persisted() == g);
    CHECK((written == Paths{f, f, g}));
    RememberedFolder fresh{L""};
    CHECK(fresh.Remember(L"", writer));
    CHECK((written == Paths{f, f, g}));

    // The search is a fallback for a saved folder that no longer holds the
    // game, and never replaces a folder picked in recovery.
    int searches = 0;
    std::wstring searchResult = L"F:\\Found";
    const auto search = [&]() { ++searches; return searchResult; };
    const auto onlyFound = [](const std::wstring& path) { return path == L"F:\\Found\\SSFIV.exe"; };
    const auto onlySaved = [](const std::wstring& path) { return path == L"D:\\Games\\Ultra\\SSFIV.exe"; };

    auto location = LocateGame(L"", f, onlyFound, search);
    CHECK(searches == 1);
    CHECK(location.directory == L"F:\\Found");
    CHECK(location.executable == L"F:\\Found\\SSFIV.exe");
    CHECK(!location.fromRecovery);

    searches = 0;
    location = LocateGame(L"", f, onlySaved, search);
    CHECK(searches == 0);
    CHECK(location.directory == f);
    CHECK(location.executable == L"D:\\Games\\Ultra\\SSFIV.exe");
    CHECK(!location.fromRecovery);

    location = LocateGame(g, f, onlySaved, search);
    CHECK(searches == 0);
    CHECK(location.directory.empty());
    CHECK(location.executable.empty());
    CHECK(location.fromRecovery);

    location = LocateGame(L"F:\\Found", f, onlyFound, search);
    CHECK(searches == 0);
    CHECK(location.directory == L"F:\\Found");
    CHECK(location.executable == L"F:\\Found\\SSFIV.exe");
    CHECK(location.fromRecovery);

    searchResult.clear();
    location = LocateGame(L"", L"", absent, search);
    CHECK(searches == 1);
    CHECK(location.directory.empty());
    CHECK(location.executable.empty());
    CHECK(!location.fromRecovery);

    // A runtime library beside the game or in a system folder is reported
    // with its full path, in folder then library order, because the game
    // loads it ahead of the package copy.
    const std::wstring package = L"C:\\Ember";
    const auto stale = [](const std::wstring& path) {
        return path == L"D:\\Games\\Ultra\\GGPO.dll" || path == L"C:\\Windows\\SysWOW64\\zlib1.dll" ||
            path == L"D:\\Games\\Ultra\\fmt.dll" || path == L"C:\\Ember\\GGPO.dll";
    };
    CHECK((ShadowingRuntimeLibraries(package, {L"D:\\Games\\Ultra", L"C:\\Windows\\SysWOW64", L"C:\\Windows"}, stale) ==
        Paths{L"D:\\Games\\Ultra\\GGPO.dll", L"D:\\Games\\Ultra\\fmt.dll", L"C:\\Windows\\SysWOW64\\zlib1.dll"}));
    CHECK((ShadowingRuntimeLibraries(package, {L"D:\\Games\\Ultra\\"}, stale) ==
        Paths{L"D:\\Games\\Ultra\\GGPO.dll", L"D:\\Games\\Ultra\\fmt.dll"}));
    CHECK(ShadowingRuntimeLibraries(package, {L"D:\\Games\\Ultra"}, absent).empty());
    // The package extracted into the game folder is found first by the game,
    // so it is not shadowing itself, in any spelling of that folder, and a
    // system copy after it is never reached. Empty folders are skipped.
    CHECK(ShadowingRuntimeLibraries(package, {L"c:/ember/", L"C:\\Windows\\SysWOW64"}, stale).empty());
    CHECK(ShadowingRuntimeLibraries(L"C:\\Ember\\", {L"", L"C:\\Ember", L"C:\\Windows\\SysWOW64"}, stale).empty());
    CHECK((ShadowingRuntimeLibraries(L"C:\\Windows\\SysWOW64", {L"D:\\Games\\Ultra", L"C:\\Windows\\SysWOW64", L"C:\\Windows"}, stale) ==
        Paths{L"D:\\Games\\Ultra\\GGPO.dll", L"D:\\Games\\Ultra\\fmt.dll"}));
    CHECK(ShadowingRuntimeLibraries(package, {}, stale).empty());
    // The launcher's DLL directory is handed to the game and searched right
    // after the game folder, so a copy that exists only in a system folder
    // does not block a package elsewhere, while a game-folder copy still does.
    const auto systemOnly = [](const std::wstring& path) { return path == L"C:\\Windows\\SysWOW64\\GGPO.dll"; };
    const Paths gameOrder{L"D:\\Games\\Ultra", package, L"C:\\Windows\\SysWOW64", L"C:\\Windows\\System", L"C:\\Windows"};
    CHECK(ShadowingRuntimeLibraries(package, gameOrder, systemOnly).empty());
    CHECK((ShadowingRuntimeLibraries(package, gameOrder, stale) == Paths{L"D:\\Games\\Ultra\\GGPO.dll", L"D:\\Games\\Ultra\\fmt.dll"}));
    // Every library Sidecar imports by name is checked.
    Paths probed;
    const auto record = [&probed](const std::wstring& path) { probed.push_back(path); return false; };
    CHECK(ShadowingRuntimeLibraries(package, {L"D:\\Games\\Ultra"}, record).empty());
    CHECK((probed == Paths{L"D:\\Games\\Ultra\\GGPO.dll", L"D:\\Games\\Ultra\\spdlog.dll", L"D:\\Games\\Ultra\\fmt.dll", L"D:\\Games\\Ultra\\zlib1.dll"}));
    return 0;
}
