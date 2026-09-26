#include "../launcher/GameLocator.hxx"

#include <string>
#include <vector>

#include "test_support.hxx"

using sf4e::launcher::GameExecutable;
using sf4e::launcher::LibraryCandidates;
using sf4e::launcher::ParseLibraryFolders;
using sf4e::launcher::RememberedFolder;
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

    // A folder counts as saved only after the settings write succeeds.
    RememberedFolder remembered;
    CHECK(remembered.NeedsSave(L"D:\\Games\\Ultra"));
    CHECK(!remembered.NeedsSave(L""));
    // A failed write leaves persisted as it was, so the next round tries again.
    CHECK(remembered.NeedsSave(L"D:\\Games\\Ultra"));
    remembered.persisted = L"D:\\Games\\Ultra";
    CHECK(!remembered.NeedsSave(L"D:\\Games\\Ultra"));
    CHECK(!remembered.NeedsSave(L"d:\\games\\ultra\\"));
    CHECK(!remembered.NeedsSave(L"D:/Games/Ultra/"));
    CHECK(remembered.NeedsSave(L"E:\\Ultra"));

    // A saved folder whose game is gone does not resolve, so the launcher
    // falls through to the automatic search, and a later recovery choice
    // that does resolve still needs saving.
    const auto onlyOther = [](const std::wstring& path) { return path == L"E:\\Ultra\\SSFIV.exe"; };
    const RememberedFolder saved{L"D:\\Games\\Ultra"};
    CHECK(GameExecutable(saved.persisted, onlyOther).empty());
    CHECK(GameExecutable(L"E:\\Ultra", onlyOther) == L"E:\\Ultra\\SSFIV.exe");
    CHECK(saved.NeedsSave(L"E:\\Ultra"));
    return 0;
}
