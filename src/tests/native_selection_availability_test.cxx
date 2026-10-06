#include "../Dimps/Dimps__Selection.hxx"
#include "../ui/FighterSelector.hxx"
#include "../ui/Theme.hxx"
#include "imgui_test_support.hxx"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
sf4e::selection::Availability NativeAvailability(std::uint32_t flags) {
    sf4e::selection::Availability result;
    result.ready = true;
    result.costumes = Dimps::Selection::CostumeAvailabilityMask(flags);
    result.colors.fill(UINT32_MAX);
    result.personalActions = 0x3ff;
    return result;
}
}

int main() try {
    using namespace sf4e;
    using namespace sf4e::selection;
    using namespace sf4e::ui;
    // Read-only capture from Steam 1.05 SHA256 5d724595...b9eb, 2026-09-10.
    // Bits 16-23 are the Steam licence (EXCOSFLAG and its data, read by the
    // native costume menu at 69FF90). The low byte is installed data alone,
    // which every install has for every pack.
    Check(AllowedCostumes(0, NativeAvailability(0x000f007f)) ==
        (std::vector<int>{0, 1, 2, 3}), "Installed but unowned Ryu costumes became selectable");
    Check(AllowedCostumes(43, NativeAvailability(0x0003003f)) ==
        (std::vector<int>{0, 1}), "Installed but unowned later-roster costumes became selectable");
    Check(AllowedCostumes(0, NativeAvailability(0x0001007f)) ==
        (std::vector<int>{0}), "Installed data exposed an unowned costume");
    Check(AllowedCostumes(0, NativeAvailability(0x007f0001)).size() == 7, "Owned DLC costumes disappeared from selectable choices");
    Check(AllowedCostumes(0, NativeAvailability(0x00c100ff)) ==
        (std::vector<int>{0, 6}), "Sparse ownership or reserved costume bounds were lost");
    Check(AllowedCostumes(43, NativeAvailability(0x00ff00ff)).size() == 6,
        "Reserved native slots became selectable costumes");
    Check(AllowedCostumes(0, NativeAvailability(0)).empty(), "Empty native ownership invented costumes");
    // The title grants costume 1 to 39-43 whether or not the fighter is owned.
    Check(FighterLocked(43, NativeAvailability(0x0002003f)) && AllowedCostumes(43, NativeAvailability(0x0002003f)).empty(),
        "A costume grant made an unowned fighter selectable");
    Check(!FighterLocked(43, Availability{}), "Unknown availability locked a fighter");
    // The base roster is never licence-gated, whatever its bit 16 says.
    for (int fighter = 0; fighter < 35; ++fighter)
        Check(!FighterLocked(fighter, NativeAvailability(0x0000000f)), "A base roster fighter was locked");
    {
        Pick unowned; unowned.fighter = 43; unowned.costume = 1;
        const auto locked = NativeAvailability(0x0002003f);
        Check(!Available(unowned, true, locked), "An unowned fighter passed the Ready check");
        Check(Normalize(unowned, true, &locked) && unowned.fighter == 0 && unowned.costume == 0,
            "A saved unowned fighter was not replaced with Ryu");
    }

    HeadlessImGui imgui; auto& io = imgui.io;
    std::vector<MenuEntry> rows;
    SetMenuEntriesProbe([&](const std::vector<MenuEntry>& entries) { rows = entries; });
    for (const int fighter : {0, 43}) {
        FighterSelector selector;
        Pick pick; pick.fighter = fighter;
        const auto availability = NativeAvailability(fighter == 0 ? 0x007f007f : 0x003f003f);
        const auto frame = [&](unsigned buttons = 0, bool editable = true) {
            SetMenuInput({buttons, 0}); ImGui::NewFrame();
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("Selection regression");
            // No artwork: ownership must still yield working gallery choices.
            selector.Draw(pick, true, nullptr, [&](int) { return availability; }, nullptr, editable);
            ImGui::End(); ImGui::Render();
        };
        selector.Navigation().Push("costumes"); frame(); frame();
        Check(rows.size() == CostumeCount(fighter), "Missing artwork hid owned costume cards");
        for (int costume = BaseCostumeCount(fighter); costume < CostumeCount(fighter); ++costume) {
            const auto id = "costume-" + std::to_string(costume);
            const auto found = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& row) { return row.id == id; });
            Check(found != rows.end() && found->enabled, "Owned DLC gallery card is not selectable");
            selector.Navigation().Focus(id, rows); frame(MenuInput::Select); frame();
            Check(pick.costume == costume, "Selecting a DLC card did not save its native ID");
            frame(0, false); frame();
            Check(pick.costume == costume && Available(pick, true, availability),
                "Locking or refreshing selection reset an owned DLC costume");
            // A saved costume goes on to its colors; come back for the next card.
            Check(selector.Navigation().Screen() == "colors", "Saving a DLC costume did not go on to its colors");
            selector.Navigation().Return(); frame(); frame();
        }
    }
    {
        // An unowned fighter keeps its card in the roster, which cannot be saved.
        FighterSelector selector;
        Pick pick;
        const auto reader = [](int fighter) { return NativeAvailability(fighter == 43 ? 0x0002003f : 0x000f007f); };
        const auto frame = [&](unsigned buttons = 0) {
            SetMenuInput({buttons, 0}); ImGui::NewFrame();
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("Roster ownership regression");
            selector.Draw(pick, true, nullptr, reader, nullptr, true);
            ImGui::End(); ImGui::Render();
        };
        selector.Navigation().Push("roster"); frame(); frame();
        const auto row = [&](const char* id) {
            const auto found = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& entry) { return entry.id == id; });
            Check(found != rows.end(), "A fighter card left the roster");
            return *found;
        };
        Check(rows.size() == FighterCount, "The roster lost a card");
        Check(!row("fighter-43").enabled && row("fighter-0").enabled, "Roster cards ignored fighter ownership");
        selector.Navigation().Focus("fighter-43", rows); frame(MenuInput::Select); frame();
        Check(pick.fighter == 0, "Selecting an unowned fighter's card saved it");
    }
    SetMenuEntriesProbe({});
    std::cout << "Native ownership, DLC gallery, missing art and saved costume regressions passed.\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
