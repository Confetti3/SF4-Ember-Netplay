#include "../common/FighterCatalog.hxx"
#include "../common/InstalledContent.hxx"
#include "../common/StageCatalog.hxx"
#include "../common/SelectionAssetPath.hxx"
#include <climits>
#include <cstdlib>
#include <iterator>
#include <iostream>
#include <set>
#include <string>

#include "test_support.hxx"

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
    for (auto invalid : {INT64_MIN, std::int64_t(-1), std::int64_t(22), std::int64_t(23), std::int64_t(30), std::int64_t(255), INT64_MAX})
        CHECK(!FindStage(invalid) && NormalizeStage(invalid) == 0);
    // Random is a local choice: it survives NormalizeStageChoice but never
    // FindStage or NormalizeStage, and ResolveStage turns it into a catalog stage.
    CHECK(IsRandomStage(RandomStageId) && IsRandomStage(255) && !IsRandomStage(0) && !IsRandomStage(24));
    for (const auto& stage : StageList()) CHECK(NormalizeStageChoice(stage.id) == stage.id);
    CHECK(NormalizeStageChoice(RandomStageId) == RandomStageId);
    for (auto invalid : {INT64_MIN, std::int64_t(-1), std::int64_t(22), std::int64_t(23), std::int64_t(30), std::int64_t(254), INT64_MAX})
        CHECK(NormalizeStageChoice(invalid) == 0);
    std::set<int> resolved;
    for (std::uint32_t roll = 0; roll <= 1000; ++roll) {
        const int stage = ResolveStage(RandomStageId, roll);
        CHECK(FindStage(stage) != nullptr);
        resolved.insert(stage);
        CHECK(ResolveStage(11, roll) == 11 && ResolveStage(22, roll) == 0);
    }
    CHECK(resolved == stageIds);
    CHECK(FindStage(ResolveStage(RandomStageId, UINT32_MAX)) != nullptr);
    // Stages taken out of the Random pool never come up, a fixed choice ignores the pool, and a pool can't take every
    // game stage out: the last is refused, and a saved list that takes them all out puts them all back.
    const RandomPool noTrainingOrSkyscraper(std::vector<std::int64_t>{0, 15});
    CHECK(RandomPool().Count({}) == VersusStageCount && noTrainingOrSkyscraper.Count({}) == VersusStageCount - 2);
    CHECK(!noTrainingOrSkyscraper.Contains(0) && noTrainingOrSkyscraper.Contains(1) && !RandomPool().Contains(22));
    std::vector<std::int64_t> allButJurassic, everyStage;
    for (const auto& stage : StageList()) { everyStage.push_back(stage.id); if (stage.id != 29) allButJurassic.push_back(stage.id); }
    const RandomPool onlyJurassic(allButJurassic);
    std::set<int> pooled;
    for (std::uint32_t roll = 0; roll <= 1000; ++roll) {
        pooled.insert(ResolveStage(RandomStageId, roll, noTrainingOrSkyscraper));
        CHECK(ResolveStage(0, roll, noTrainingOrSkyscraper) == 0);
        CHECK(ResolveStage(RandomStageId, roll, onlyJurassic) == 29);
    }
    CHECK(pooled.size() == static_cast<std::size_t>(VersusStageCount - 2) && !pooled.count(0) && !pooled.count(15));
    CHECK(RandomPool(everyStage).Excluded().empty() && RandomPool(everyStage).Count({}) == VersusStageCount);
    CHECK(RandomPool(std::vector<std::int64_t>{22, std::int64_t(1) << 40, 0, 0, -3}).Excluded() == std::vector<int>{0});
    RandomPool lastLeft(allButJurassic);
    CHECK(!lastLeft.CanToggle(29) && !lastLeft.Toggle(29) && lastLeft.Contains(29) && !lastLeft.Toggle(22));
    CHECK(lastLeft.Toggle(1) && lastLeft.Contains(1) && lastLeft.CanToggle(29) && lastLeft.Toggle(29) && !lastLeft.Contains(29));
    CHECK(RandomPool() == RandomPool() && noTrainingOrSkyscraper != RandomPool());
    CHECK(std::string(FindStage(24)->code) == "DET" && std::string(FindStage(29)->code) == "JUR");
    CHECK(IsSelectionAssetPath(L"assets\\selection\\stage-sources.json"));
    CHECK(IsSelectionAssetPath(L"assets\\selection\\ultra-sources.json"));
    CHECK(IsSelectionAssetPath(L"assets\\selection\\color-sources.json"));
    CHECK(IsSelectionAssetPath(L"assets\\selection\\alt-color-sources.json"));
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
    // The roster's display order is a permutation of the native IDs, 15 to a row.
    {
        std::set<int> shown(std::begin(RosterDisplayOrder), std::end(RosterDisplayOrder));
        CHECK(shown.size() == static_cast<std::size_t>(FighterCount) && *shown.begin() == 0 && *shown.rbegin() == FighterCount - 1);
        CHECK(RosterGridColumns == 15 && RosterDisplayOrder[0] == 43 && RosterDisplayOrder[14] == 40 && RosterDisplayOrder[15] == 35);
        CHECK(RosterDisplayOrder[RosterGridColumns * 2] == 37 && RosterDisplayOrder[FighterCount - 1] == 42);
    }
    // Akuma's Ultra I is two light punches, forward, a light kick and a heavy punch.
    CHECK(std::string(UltraCommands(17, 0, 14)[0].symbols) == "LP LP 6 LK HP");
    CHECK(UltraNotation("LP LP 6 LK HP") == "LP, LP, F, LK, HP");
    CHECK(UltraNotation("236 236 + PPP") == "QCF x2 + PPP");
    CHECK(UltraNotation("214 214 + KKK") == "QCB x2 + KKK");
    CHECK(UltraNotation("63214 63214 + KKK") == "HCB x2 + KKK");
    CHECK(UltraNotation("360 360 + PPP") == "360 x2 + PPP");
    CHECK(UltraNotation("~4 6 4 6 + PPP") == "Charge B, F, B, F + PPP");
    CHECK(UltraNotation("~1 3 1 9 + KKK") == "Charge DB, DF, DB, UF + KKK");
    CHECK(UltraNotation("~1 6 4 6 + KKK") == "Charge DB, F, B, F + KKK");
    CHECK(UltraNotation("8 8 + KKK") == "U, U + KKK");
    CHECK(UltraNotation("2 2 2 + KKK") == "D, D, D + KKK");
    CHECK(UltraNotation("") == "");
    // Every input in the catalog turns into words; only the 360 motion keeps its number.
    for (int fighter = 0; fighter < FighterCount; ++fighter)
        for (int edition : AllowedEditions(fighter, true))
            for (int ultra : {0, 1})
                for (const auto& command : UltraCommands(fighter, ultra, edition)) {
                    std::string notation = UltraNotation(command.symbols);
                    CHECK(!notation.empty());
                    for (const char* kept : {"360 x2", " x2"})
                        for (std::size_t at; (at = notation.find(kept)) != std::string::npos;) notation.erase(at, std::string(kept).size());
                    CHECK(notation.find_first_of("0123456789~") == std::string::npos);
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
    Availability sparse; sparse.personalActions = 1; sparse.ready = true; sparse.costumes = 1u | (1u << 1) | (1u << 6);
    sparse.colors[1] = (1u << 0) | (1u << 9); sparse.colors[6] = (1u << 12) | (1u << 21);
    CHECK(AllowedCostumes(0, sparse) == (std::vector<int>{0, 1, 6}));
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

    // Custom costumes: slots 8-99 (indices 7-98) of every fighter, ten colors each, valid picks on the network and
    // listed only once installed.
    CHECK(IsCustomCostume(7) && IsCustomCostume(98) && !IsCustomCostume(6) && !IsCustomCostume(99));
    Pick custom; custom.costume = 11; custom.color = 9;
    CHECK(Valid(custom, true) && !Normalize(custom, true) && ColorCount(0, 11) == 10);
    custom.color = 10;
    CHECK(!Valid(custom, true));
    // The custom content installed on this PC comes in beside the native availability, which stays the game's own.
    Availability owned; owned.ready = true; owned.costumes = 1u;
    InstalledContent content; content.costumes[0] = {11}; content.costumes[40] = {11};
    CHECK(AllowedCostumes(0, owned, &content) == (std::vector<int>{0, 11}) && AllowedColors(0, 11, owned, &content).size() == 10);
    CHECK(AllowedCostumes(0, owned) == std::vector<int>{0} && AllowedColors(0, 11, owned).empty());
    Availability unowned = owned; unowned.costumes = 0;   // a licence-locked fighter (35-43) has no costumes at all
    CHECK(AllowedCostumes(40, unowned, &content).empty() && AllowedCostumes(40, owned, &content) == (std::vector<int>{0, 11}));
    CHECK(AllowedCostumes(0, sparse) == (std::vector<int>{0, 1, 6}));
    Pick customPick; customPick.costume = 11; customPick.personalAction = 255;
    owned.personalActions = 1;
    CHECK(Available(customPick, true, owned, &content) && !Available(customPick, true, owned));

    // Custom colors: colors 30-99 (indices 29-98) of any costume, the game's or a custom one, valid on the network and
    // listed after the costume's own colors once installed.
    CHECK(IsCustomColor(29) && IsCustomColor(98) && !IsCustomColor(28) && !IsCustomColor(22) && !IsCustomColor(99));
    CHECK(ColorInRange(0, 1, 30) && !ColorInRange(0, 1, 15) && !ColorInRange(0, 1, 99) && !ColorInRange(0, 11, 22));
    CHECK(ColorInRange(0, 11, 29) && ColorInRange(0, 11, 98) && !ColorInRange(0, 11, 10));
    Pick extra; extra.costume = 1; extra.color = 29;
    CHECK(Valid(extra, true) && !Normalize(extra, true));
    extra.costume = 11;
    CHECK(Valid(extra, true));
    extra.color = 10;
    CHECK(!Valid(extra, true));
    Availability colored; colored.ready = true; colored.costumes = 1u; colored.colors[0] = 1u;
    InstalledContent colors; colors.colors[std::make_pair(0, 0)] = {29, 40};
    CHECK(AllowedColors(0, 0, colored, &colors) == (std::vector<int>{0, 29, 40}));
    colors.costumes[0] = {11}; colors.colors[std::make_pair(0, 11)] = {35};
    CHECK(AllowedColors(0, 11, colored, &colors) == (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 35}));
    CHECK(colors.HasColor(0, 0, 40) && !colors.HasColor(0, 0, 41) && !colors.HasColor(1, 0, 40) && colors.HasCostume(0, 11) && !colors.HasCostume(0, 12));

    // Custom stages: any three capital letters or digits that aren't a game stage's code; the code is the id, so a
    // custom stage travels as one number. Checking one allocates nothing and needs nothing installed; they fall back
    // to a stock stage by rule; Random takes only the installed ones it is given.
    const int c12 = CustomStageId("C12"), d12 = CustomStageId("D12"), zzz = CustomStageId("ZZZ");
    CHECK(c12 == ('C' << 16 | '1' << 8 | '2') && IsCustomStage(c12) && IsCustomStage(d12) && IsCustomStage(zzz));
    for (const char* bad : {"CHN", "GAS", "SCX", "JUR", "c12", "C1", "C123", "C-2", ""}) CHECK(CustomStageId(bad) == -1);
    CHECK(!IsCustomStage(1) && !IsCustomStage(RandomStageId) && !IsCustomStage('C' << 16 | 'H' << 8 | 'N'));
    CHECK(!FindStage(c12) && ValidStage(c12) && ValidStage(1) && !ValidStage(22) && !ValidStage(RandomStageId) && !ValidStage(-1));
    CHECK(StageCode(c12) == "C12" && StageCode(24) == "DET" && StageCode(22).empty() && NormalizeStage(c12) == c12);
    CHECK(CustomStageFallback(c12) == 16 && CustomStageFallback(d12) == 16);              // the 12th fallback, CNX
    CHECK(CustomStageFallback(CustomStageId("C05")) == 6 && CustomStageFallback(CustomStageId("C19")) == 1);
    CHECK(FindStage(CustomStageFallback(zzz)) && !IsCustomStage(CustomStageFallback(zzz)));
    CHECK(ResolveStage(c12, 0) == c12);
    for (std::uint32_t roll = 0; roll <= 100; ++roll) CHECK(!IsCustomStage(ResolveStage(RandomStageId, roll)));
    // Installed custom stages join the game's in Random: with one game stage left, rolls land on it or the custom one.
    std::set<int> withCustom;
    for (std::uint32_t roll = 0; roll <= 1000; ++roll) withCustom.insert(ResolveStage(RandomStageId, roll, onlyJurassic, {c12}));
    CHECK(withCustom == (std::set<int>{29, c12}));
    CHECK(ResolveStage(RandomStageId, VersusStageCount, RandomPool(), {c12}) == c12 && ResolveStage(5, 0, RandomPool(), {c12}) == 5);
    // Taken out of Random and put back by id, any number of them: nothing taken out ever comes back on its own.
    RandomPool out;
    CHECK(out.Toggle(c12) && out.Toggle(d12) && !out.Contains(c12) && !out.Contains(d12) && out.Contains(zzz));
    CHECK(out.Stages({c12, d12, zzz}).back() == zzz && out.Count({c12, d12, zzz}) == VersusStageCount + 1);
    CHECK(out.Toggle(c12) && out.Contains(c12));
    for (int n = 0; n < 100; ++n) {
        const char code[4] = {'E', static_cast<char>('0' + n / 10), static_cast<char>('0' + n % 10), 0};
        CHECK(out.Toggle(CustomStageId(code)));
    }
    CHECK(!out.Contains(d12) && !out.Contains(CustomStageId("E00")) && !out.Contains(CustomStageId("E99")) && out.Excluded().size() == 101);
    CHECK(RandomPool(std::vector<std::int64_t>(out.Excluded().begin(), out.Excluded().end())) == out);
    // A stage taken out that isn't installed (any more) changes nothing in Random, and keeps its place for later.
    CHECK(out.Count({}) == VersusStageCount && out.Count({zzz}) == VersusStageCount + 1);

    // Names the setup program gives custom content: display only, kept apart per costume, color and stage.
    InstalledContent named;
    named.names[std::make_tuple(0, 70, -1)] = "Monster Hunter"; named.names[std::make_tuple(0, 1, 29)] = "Steel Blue";
    named.stageNames[d12] = "Testing Stage";
    CHECK(named.Name(0, 70) == "Monster Hunter" && named.Name(0, 1, 29) == "Steel Blue" && named.Name(0, 1).empty() && named.Name(1, 70).empty());
    CHECK(named.StageName(d12) == "Testing Stage" && named.StageName(c12).empty() && named.StageName(1).empty());

    // What each PC shows for a match's two picks, from what it has installed (here: Ryu's custom costume 12, his
    // Original's custom color 30, and color 36 of costume 12).
    InstalledContent here;
    here.costumes[0] = {11}; here.colors[std::make_pair(0, 0)] = {29}; here.colors[std::make_pair(0, 11)] = {35};
    {   // mixed: Ryu's costume 12 (installed) plays as his Original, Ken's costume 12 (not installed) falls back
        const Look picks[2] = {Look(0, 11, 3), Look(1, 11, 3)};
        const Appearance a = ResolveAppearance(picks, here);
        CHECK(a.shown[0] == Look(0, 0, 3) && a.costumes[0].fighter == 0 && a.costumes[0].standIn == 0 && a.costumes[0].custom == 11);
        CHECK(a.shown[1] == Look(1, 0, 0) && a.costumes[1].custom == -1 && a.colors[1].custom == -1 && a.colors[0].custom == -1);
    }
    {   // a custom color it doesn't have becomes color 1; the second side then takes color 2, not to look the same
        const Look picks[2] = {Look(0, 0, 0), Look(0, 0, 40)};
        const Appearance a = ResolveAppearance(picks, here);
        CHECK(a.shown[0] == Look(0, 0, 0) && a.shown[1] == Look(0, 0, 1) && a.colors[1].custom == -1);
    }
    {   // a mirror match on one custom slot and color: the second side takes color 2, and the sides play the slot as
        // different stand-ins (the file-name swap tells them apart by costume)
        const Look picks[2] = {Look(0, 11, 0), Look(0, 11, 0)};
        const Appearance a = ResolveAppearance(picks, here);
        CHECK(a.shown[0] == Look(0, 0, 0) && a.shown[1] == Look(0, 1, 1));
        CHECK(a.costumes[0].custom == 11 && a.costumes[0].standIn == 0 && a.costumes[1].custom == 11 && a.costumes[1].standIn == 1);
    }
    {   // the same on the game's own costumes: color 1 against color 1 makes the second color 2; color 2 against color 2
        // makes it color 1; any other color the same way, color 1
        Look picks[2] = {Look(5, 2, 0), Look(5, 2, 0)};
        CHECK(ResolveAppearance(picks, here).shown[1] == Look(5, 2, 1) && ResolveAppearance(picks, here).shown[0] == Look(5, 2, 0));
        picks[0].color = picks[1].color = 1;
        CHECK(ResolveAppearance(picks, here).shown[1] == Look(5, 2, 0) && ResolveAppearance(picks, here).shown[0] == Look(5, 2, 1));
        picks[0].color = picks[1].color = 6;
        CHECK(ResolveAppearance(picks, here).shown[1] == Look(5, 2, 0));
        picks[1].color = 7;   // different colors stay as picked
        CHECK(ResolveAppearance(picks, here).shown[1] == Look(5, 2, 7));
    }
    {   // a mirror on one custom color the PC has: the second side takes color 1, the first plays the custom color as color 2
        const Look picks[2] = {Look(0, 0, 29), Look(0, 0, 29)};
        const Appearance a = ResolveAppearance(picks, here);
        CHECK(a.shown[1] == Look(0, 0, 0) && a.colors[1].custom == -1);
        CHECK(a.shown[0] == Look(0, 0, 1) && a.colors[0].standIn == 1 && a.colors[0].custom == 29);
    }
    {   // a custom color on a custom costume: the costume's stand-in first, then the color on it
        const Look picks[2] = {Look(0, 11, 35), Look(1, 0, 0)};
        const Appearance a = ResolveAppearance(picks, here);
        CHECK(a.shown[0] == Look(0, 0, 0) && a.costumes[0].custom == 11);
        CHECK(a.colors[0].fighter == 0 && a.colors[0].costume == 0 && a.colors[0].standIn == 0 && a.colors[0].custom == 35);
    }
    {   // a custom color against the same costume's color 1: the next color stands in
        const Look picks[2] = {Look(0, 0, 29), Look(0, 0, 0)};
        const Appearance a = ResolveAppearance(picks, here);
        CHECK(a.shown[0] == Look(0, 0, 1) && a.colors[0].standIn == 1 && a.colors[0].custom == 29 && a.shown[1] == Look(0, 0, 0));
    }
    {   // nothing installed: both fall back, and the second side still takes color 2, as on a PC that has them
        const Look picks[2] = {Look(0, 11, 35), Look(0, 11, 35)};
        const Appearance a = ResolveAppearance(picks, InstalledContent());
        CHECK(a.shown[0] == Look(0, 0, 0) && a.shown[1] == Look(0, 0, 1) && a.costumes[0].custom == -1 && a.costumes[1].custom == -1);
    }
    std::cout << "44-fighter catalog, edition restrictions, palette bounds, sparse unlocks, saved-choice repair, Random pool, custom costumes, stages, names and appearance passed\n";
}
