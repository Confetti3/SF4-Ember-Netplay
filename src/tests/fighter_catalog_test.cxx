#include "../common/FighterCatalog.hxx"
#include "../common/SelectionAssetPath.hxx"
#include <climits>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

#define CHECK(c) do { if (!(c)) { std::cerr << "Catalog check failed at " << __LINE__ << '\n'; std::exit(1); } } while (false)

int main() {
    using namespace sf4e::selection;
    CHECK(StageList().size() == 28);
    std::set<int> stageIds;
    std::set<std::string> stageCodes;
    for (const auto& stage : StageList()) {
        CHECK(FindStage(stage.id) == &stage && NormalizeStage(stage.id) == stage.id);
        CHECK(stageIds.insert(stage.id).second && stageCodes.insert(stage.code).second);
        CHECK(*stage.name);
        const auto stem = L"assets\\selection\\stages\\" + std::wstring(stage.code, stage.code + 3);
        CHECK(IsSelectionAssetPath(stem + L".jpg") && IsSelectionAssetPath(stem + L".png"));
        CHECK(!IsSelectionAssetPath(stem + L".jpg.exe") && !IsSelectionAssetPath(stem + L".jpg:stream"));
    }
    for (auto invalid : {INT64_MIN, std::int64_t(-1), std::int64_t(22), std::int64_t(23), std::int64_t(30), INT64_MAX})
        CHECK(!FindStage(invalid) && NormalizeStage(invalid) == 0);
    CHECK(std::string(FindStage(24)->code) == "DET" && std::string(FindStage(29)->code) == "JUR");
    CHECK(IsSelectionAssetPath(L"assets\\selection\\stage-sources.json"));
    CHECK(IsSelectionAssetPath(L"assets\\selection\\ultra-sources.json"));
    CHECK(IsSelectionAssetPath(L"assets\\selection\\color-sources.json"));
    CHECK(!IsSelectionAssetPath(L"assets\\selection\\stages\\GAS.jpg"));
    CHECK(!IsSelectionAssetPath(L"assets\\selection\\stages\\SCX.png"));
    CHECK(!IsSelectionAssetPath(L"assets\\selection\\stages\\..\\TRN.jpg"));
    std::set<std::string> codes;
    int original = 0, later = 0;
    for (int fighter = 0; fighter < FighterCount; ++fighter) {
        const auto* metadata = FindFighter(fighter);
        CHECK(metadata && *metadata->name && *metadata->ultras[0] && *metadata->ultras[1]);
        CHECK(codes.insert(metadata->code).second);
        CHECK(AllowedEditions(fighter, false) == std::vector<int>{14});
        if (EditionAllowed(fighter, 13, true)) ++original; else ++later;
        Availability all; all.personalActions = 0x3ff; all.ready = true; all.costumes = UINT32_MAX; all.colors.fill(UINT32_MAX);
        Pick pick; pick.fighter = fighter;
        for (int edition : AllowedEditions(fighter, true)) {
            pick.edition = edition;
            for (int ultra : AllowedUltras(fighter, edition)) {
                pick.ultra = ultra;
                for (int costume : AllowedCostumes(fighter, all)) {
                    pick.costume = costume;
                    for (int color : AllowedColors(fighter, costume, all)) {
                        pick.color = color;
                        CHECK(Available(pick, true, all));
                        CHECK(!Normalize(pick, true, &all));
                    }
                    pick.color = ColorCount(fighter, costume);
                    CHECK(!Valid(pick, true));
                }
            }
        }
        CHECK(CostumeCount(fighter) == (EditionAllowed(fighter, 13, true) ? 7 : 6));
        CHECK(ColorCount(fighter, 0) == 12);
        CHECK(ColorCount(fighter, CostumeCount(fighter) - 1) == 22);
    }
    for (int fighter = 0; fighter < FighterCount; ++fighter)
        for (int edition : AllowedEditions(fighter, true))
            for (int ultra : AllowedUltras(fighter, edition)) {
                const auto commands = UltraCommands(fighter, ultra, edition);
                CHECK(ultra == 2 ? commands.empty() : !commands.empty());
                for (const auto& command : commands) CHECK(command.symbols && *command.symbols);
            }
    CHECK(UltraCommands(-1, 0, 14).empty());
    CHECK(UltraCommands(0, 1, 13).empty());
    CHECK(UltraCommands(43, 0, 1).empty());
    CHECK(std::string(UltraCommands(11, 1, 1)[0].symbols) == "236 236 + PPP");
    CHECK(std::string(UltraCommands(11, 1, 2)[0].symbols) == "~4 6 4 6 + PPP");
    CHECK(std::string(UltraCommands(43, 1, 16)[1].symbols) == "214 214 + KKK");
    CHECK(std::string(UltraCommands(43, 1, 14)[1].symbols) == "~1 3 1 9 + KKK");
    CHECK(UltraCommands(25, 1, 14).size() == 2);
    CHECK(std::string(UltraCommands(25, 1, 14)[1].condition) == "Crane stance / in air");
    CHECK(original == 25 && later == 19);
    for (const wchar_t* path : {L"assets\\selection\\README.md", L"assets\\selection\\sources.json",
        L"assets\\selection\\RYU\\costume-6\\color-21-cutout.png",
        L"assets\\selection\\DCP\\costume-5\\color-21.jpg", L"assets\\selection\\BSN\\ultra-1.png"})
        CHECK(IsSelectionAssetPath(path));
    for (const wchar_t* path : {L"assets\\selection\\..\\Sidecar.dll", L"assets\\selection\\RYU\\costume-7\\color-0.png",
        L"assets\\selection\\DCP\\costume-6\\color-0.png", L"assets\\selection\\RYU\\costume-0\\color-12.png",
        L"assets\\selection\\RYU\\costume-6\\color-22.png", L"assets\\selection\\RYU\\costume-6\\color-0.png.exe",
        L"assets\\selection\\RYU\\costume-6\\color-0.png:stream", L"assets\\selection\\BAD\\portrait.png",
        L"assets\\selection\\RYU\\ultra-2.png", L"assets\\selection\\RYU\\costume-06\\color-0.png"})
        CHECK(!IsSelectionAssetPath(path));
    CHECK(AllowedEditions(43, true) == (std::vector<int>{14, 16}));
    CHECK(AllowedEditions(35, true) == (std::vector<int>{2, 4, 14, 16}));
    CHECK(AllowedUltras(0, 13) == std::vector<int>{0});
    CHECK(AllowedUltras(0, 16) == (std::vector<int>{0, 1}));
    CHECK(std::string(FindFighter(8)->name) == "Balrog");
    CHECK(std::string(FindFighter(9)->name) == "Vega");
    CHECK(std::string(FindFighter(11)->name) == "M. Bison");
    for (int bad : {-1, 44, 255, INT_MIN, INT_MAX}) {
        CHECK(!FindFighter(bad)); CHECK(AllowedEditions(bad, true).empty());
        CHECK(!EditionAllowed(0, bad, true));
        Pick pick; pick.fighter = bad; pick.costume = bad; pick.color = bad; pick.ultra = bad; pick.edition = bad;
        CHECK(!Valid(pick, true)); CHECK(Normalize(pick, true)); CHECK(Valid(pick, true));
    }
    // Unlocks can have holes: selecting by a count would expose locked options.
    Availability sparse; sparse.personalActions = 1; sparse.ready = true; sparse.costumes = (1u << 1) | (1u << 6);
    sparse.colors[1] = (1u << 0) | (1u << 9); sparse.colors[6] = (1u << 12) | (1u << 21);
    CHECK(AllowedCostumes(0, sparse) == (std::vector<int>{1, 6}));
    CHECK(AllowedColors(0, 6, sparse) == (std::vector<int>{12, 21}));
    Pick pick; pick.costume = 6; pick.color = 20;
    CHECK(!Available(pick, true, sparse)); CHECK(Normalize(pick, true, &sparse));
    CHECK(pick.costume == 6 && pick.color == 12 && Available(pick, true, sparse));
    pick.personalAction = 9;
    CHECK(!Available(pick, true, sparse)); CHECK(Normalize(pick, true, &sparse)); CHECK(pick.personalAction == 255);
    CHECK(AllowedPersonalActions(sparse) == (std::vector<int>{255, 0}));
    pick.winQuote = 11; pick.handicap = 5;
    CHECK(!Valid(pick, true)); CHECK(Normalize(pick, true)); CHECK(pick.winQuote == 255 && pick.handicap == 0);
    Availability unknown;
    CHECK(!Normalize(pick, true, &unknown)); CHECK(!Available(pick, true, unknown));
    pick.fighter = 43; pick.edition = 13; pick.costume = 6; pick.color = 21; pick.ultra = 2;
    CHECK(Normalize(pick, true));
    CHECK(pick.edition == 14 && pick.costume == 0 && pick.color == 0 && pick.ultra == 2);
    pick.edition = 16; CHECK(Normalize(pick, true)); CHECK(pick.ultra == 0);
    CHECK(Normalize(pick, false)); CHECK(pick.edition == 14);
    std::cout << "44-fighter catalog, edition restrictions, palette bounds, sparse unlocks and saved-choice repair passed\n";
}
